// Exercise the real texture allocator and preservation policy, without game data.
// Vulkan command mocks maintain pixel contents, so resize/initialization regressions
// fail on data, not just on an expected sequence of function calls.
#include <assert.h>
#include "../runtime/nvn/vulkan/textures.c"

VkG g_vk;
static uint64_t frame = 1;
static int replacements, blits, clears;
typedef struct { uint32_t w, h; uint32_t pixels[]; } TestImage;
static TestImage *img(VkImage i) { return (TestImage *)(uintptr_t)i; }
uint64_t renderer_present_count(void) { return frame; }
uint64_t vk_pending_seq(void) { return 1; }
uint64_t vk_completed_seq(void) { return 1; }
VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
VkCommandBuffer vk_upload_cmd(void) { return VK_NULL_HANDLE; }
void vk_full_barrier(VkCommandBuffer cb) { (void)cb; }
void vk_effect_image_replaced(NvnTexture *t) { (void)t; replacements++; }
void vk_alias_texture_destroyed(NvnTexture *t) { (void)t; }
void rt_fatal_trace(void) { abort(); }
void vk_device_fault_report(void) { abort(); }
void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice d, VkFormat f, VkFormatProperties *p) {
    (void)d; (void)f; *p = (VkFormatProperties){.optimalTilingFeatures = UINT32_MAX};
}
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *c, const VmaAllocationCreateInfo *ai,
                        VkImage *out, VmaAllocation *m, VmaAllocationInfo *info) {
    (void)a; (void)ai; (void)info;
    assert(c->mipLevels == 1 && c->arrayLayers == 1);
    TestImage *p = malloc(sizeof *p + c->extent.width * c->extent.height * 4);
    p->w = c->extent.width; p->h = c->extent.height;
    memset(p->pixels, 0xcd, p->w * p->h * 4);
    *out = (VkImage)(uintptr_t)p; *m = (VmaAllocation)p; return VK_SUCCESS;
}
void vmaDestroyImage(VmaAllocator a, VkImage i, VmaAllocation m) { (void)a; (void)m; free(img(i)); }
void vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }
void vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo *d) { (void)c; (void)d; }
void vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l,
                                const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    (void)cb; (void)i; (void)l; (void)v; (void)n; (void)r; assert(!"unexpected depth test image");
}
void vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *v,
                          uint32_t n, const VkImageSubresourceRange *r) {
    (void)cb; (void)l; (void)r; assert(n == 1 && v->uint32[0] == 0);
    TestImage *p = img(i); memset(p->pixels, 0, p->w * p->h * 4); clears++;
}
void vkCmdBlitImage(VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl,
                    uint32_t n, const VkImageBlit *r, VkFilter f) {
    (void)cb; (void)sl; (void)dl; assert(n == 1 && f == VK_FILTER_NEAREST);
    TestImage *s = img(si), *d = img(di);
    assert(r->srcOffsets[1].x == (int)s->w && r->dstOffsets[1].y == (int)d->h);
    for (uint32_t y = 0; y < d->h; y++) for (uint32_t x = 0; x < d->w; x++)
        d->pixels[y*d->w+x] = s->pixels[((2*y+1)*s->h/(2*d->h))*s->w + (2*x+1)*s->w/(2*d->w)];
    blits++;
}
static void test_content(void) {
    NvnTexture t = {0}; TexBackend b = {0}; t.backend = &b;
    assert(!vk_effect_preserve(&t, 0));
    vk_effect_written(&t, 0); assert(vk_effect_preserve(&t, 0));
    assert(!vk_effect_preserve(&t, 1));
    vk_effect_written(&t, 2); assert(vk_effect_preserve(&t, 0) && vk_effect_preserve(&t, 2));
    frame++; assert(!vk_effect_preserve(&t, 0));
    vk_effect_written(&t, 1); assert(vk_effect_preserve(&t, 1) && !vk_effect_preserve(&t, 2));
    // A partial write must preserve both the written rectangle and previous pixels
    // outside it. The policy suppresses a whole-attachment implicit clear.
    uint32_t pixels[8] = {9,9,9,9,9,9,9,9}; pixels[2] = pixels[3] = 42;
    if (!vk_effect_preserve(&t, 1)) memset(pixels, 0, sizeof pixels);
    assert(pixels[0] == 9 && pixels[2] == 42 && pixels[7] == 9);
}
static void test_resize(void) {
    NvnTexture t = {.width=65, .height=36, .target=1, .format=0x25, .flags=8, .levels=1, .samples=1};
    renderer_set_scale(2.0f/3, 2.0f/3);
    TexBackend *b = tex_backend(&t);
    assert(b->scalable && b->width == 43 && b->height == 24);
    assert(img(b->image)->pixels[0] == 0); // freshly allocated render target initialized
    for (uint32_t i=0; i<b->width*b->height; i++) img(b->image)->pixels[i] = 0x12345678;
    b->gpu_owned = true; b->uploaded = true; b->wgen = 87; b->data_hash = 123; b->checked_frame = 99;
    renderer_set_scale(1,1); b = tex_backend(&t);
    assert(b->width == 65 && b->height == 36 && !b->uploaded && b->wgen == 0 && b->data_hash == 0 && b->checked_frame == 0);
    assert(vk_effect_preserve(&t, 0));
    for (uint32_t i=0; i<b->width*b->height; i++) assert(img(b->image)->pixels[i] == 0x12345678);
    renderer_set_scale(2.0f/3,2.0f/3); b = tex_backend(&t);
    assert(b->width == 43 && b->height == 24);
    for (uint32_t i=0; i<b->width*b->height; i++) assert(img(b->image)->pixels[i] == 0x12345678);
    assert(replacements == 2 && blits == 2 && clears == 3);
    backend_texture_destroyed(&t); vk_collect_dead();
}
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        setenv("FE3H_EFFECT_SCALE_FIX", "0", 1);
        NvnTexture t={0}; TexBackend b={0}; t.backend=&b;
        assert(!vk_effect_scale_fix()); vk_effect_written(&t,0);
        assert(!vk_effect_preserve(&t,0) && b.effect_content.levels==0);
        puts("effect content: legacy switch passed"); return 0;
    }
    unsetenv("FE3H_EFFECT_SCALE_FIX"); test_content(); test_resize();
    free(g_live); free(dead);
    puts("effect content: copy/partial writes, mip isolation, frame expiry, zero initialization, odd-size match/x1/match resize passed");
    return 0;
}
