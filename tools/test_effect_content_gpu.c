// Real Vulkan test of the production texture allocator, without launching FE3H.
#include <assert.h>
#include "../runtime/nvn/vulkan/textures.c"
VkG g_vk;
uint64_t prof_views_created;
static uint64_t frame = 1, completed;
static VkCommandBuffer commands;
static bool recording;
static unsigned errors;
uint64_t renderer_present_count(void) { return frame; }
uint64_t vk_pending_seq(void) { return completed + 1; }
uint64_t vk_completed_seq(void) { return completed; }
void rt_fatal_trace(void) { abort(); }
void vk_device_fault_report(void) { abort(); }
void vk_alias_texture_destroyed(NvnTexture *t) { (void)t; }
void vk_end_pass(void) {}
void vk_effect_image_replaced(NvnTexture *t) { (void)t; }
VkCommandBuffer vk_cmd(void) {
    if (!recording) {
        VK_CHECK(vkResetCommandBuffer(commands, 0));
        VkCommandBufferBeginInfo b = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(commands, &b)); recording = true;
    }
    return commands;
}
VkCommandBuffer vk_upload_cmd(void) { return vk_cmd(); }
void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 m = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount=1, .pMemoryBarriers=&m};
    vkCmdPipelineBarrier2(cb, &d);
}
static void submit(void) {
    if (!recording) return;
    VK_CHECK(vkEndCommandBuffer(commands));
    VkSubmitInfo s = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&commands};
    VK_CHECK(vkQueueSubmit(g_vk.queue, 1, &s, VK_NULL_HANDLE)); VK_CHECK(vkQueueWaitIdle(g_vk.queue));
    recording=false; completed++; vk_collect_dead();
}
static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT s,
        VkDebugUtilsMessageTypeFlagsEXT t, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)s; (void)t; (void)u; errors++; fprintf(stderr, "validation: %s\n", d->pMessage); return VK_FALSE;
}
static VkDebugUtilsMessengerEXT messenger;
static void init(void) {
    const char *layer="VK_LAYER_KHRONOS_validation", *ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo a={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo i={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&a,
        .enabledLayerCount=1, .ppEnabledLayerNames=&layer, .enabledExtensionCount=1, .ppEnabledExtensionNames=&ext};
    bool validation=vkCreateInstance(&i,NULL,&g_vk.inst)==VK_SUCCESS;
    if (!validation) { i.enabledLayerCount=i.enabledExtensionCount=0; VK_CHECK(vkCreateInstance(&i,NULL,&g_vk.inst)); }
    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT m={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback=message};
        PFN_vkCreateDebugUtilsMessengerEXT f=(void*)vkGetInstanceProcAddr(g_vk.inst,"vkCreateDebugUtilsMessengerEXT");
        VK_CHECK(f(g_vk.inst,&m,NULL,&messenger));
    }
    uint32_t n=1; VK_CHECK(vkEnumeratePhysicalDevices(g_vk.inst,&n,&g_vk.phys));
    vkGetPhysicalDeviceProperties(g_vk.phys,&g_vk.props);
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&nq,NULL);
    VkQueueFamilyProperties *qp=calloc(nq,sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&nq,qp);
    bool found=false; for (uint32_t q=0;q<nq;q++) if (qp[q].queueFlags&VK_QUEUE_GRAPHICS_BIT) { g_vk.qfam=q;found=true;break; }
    free(qp); assert(found); float priority=1;
    VkDeviceQueueCreateInfo q={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=g_vk.qfam,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceVulkan13Features f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.synchronization2=true,.dynamicRendering=true};
    VkDeviceCreateInfo d={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f,.queueCreateInfoCount=1,.pQueueCreateInfos=&q};
    VK_CHECK(vkCreateDevice(g_vk.phys,&d,NULL,&g_vk.dev)); vkGetDeviceQueue(g_vk.dev,g_vk.qfam,0,&g_vk.queue);
    VmaAllocatorCreateInfo va={.instance=g_vk.inst,.physicalDevice=g_vk.phys,.device=g_vk.dev,.vulkanApiVersion=VK_API_VERSION_1_3};
    VK_CHECK(vmaCreateAllocator(&va,&g_vk.vma));
    VkCommandPoolCreateInfo p={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev,&p,NULL,&g_vk.pool));
    VkCommandBufferAllocateInfo c={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=g_vk.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VK_CHECK(vkAllocateCommandBuffers(g_vk.dev,&c,&commands));
    fprintf(stderr,"GPU %s; validation %s\n",g_vk.props.deviceName,validation?"enabled":"unavailable");
}
static uint32_t *pixels(TexBackend *b) {
    VkBuffer buf; VmaAllocation mem;
    VkDeviceSize size=(VkDeviceSize)b->width*b->height*4;
    VkBufferCreateInfo c={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo a={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT};
    VK_CHECK(vmaCreateBuffer(g_vk.vma,&c,&a,&buf,&mem,NULL));
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={b->width,b->height,1}};
    vk_full_barrier(vk_cmd()); vkCmdCopyImageToBuffer(vk_cmd(),b->image,VK_IMAGE_LAYOUT_GENERAL,buf,1,&copy);
    submit(); void *mapped; VK_CHECK(vmaMapMemory(g_vk.vma,mem,&mapped));
    VK_CHECK(vmaInvalidateAllocation(g_vk.vma,mem,0,VK_WHOLE_SIZE));
    uint32_t *out=malloc(size); memcpy(out,mapped,size); vmaUnmapMemory(g_vk.vma,mem); vmaDestroyBuffer(g_vk.vma,buf,mem); return out;
}
static void fill(TexBackend *b, float r, float g, float blue) {
    VkClearColorValue color={.float32={r,g,blue,1}};
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vk_full_barrier(vk_cmd()); vkCmdClearColorImage(vk_cmd(),b->image,VK_IMAGE_LAYOUT_GENERAL,&color,1,&range);
}
int main(void) {
    unsetenv("FE3H_EFFECT_SCALE_FIX"); init();
    NvnTexture t={.width=65,.height=36,.target=1,.format=0x25,.flags=8,.levels=1,.samples=1};
    NvnTexture source=t;
    renderer_set_scale(2.0f/3,2.0f/3); TexBackend *b=tex_backend(&t), *s=tex_backend(&source);
    uint32_t *p=pixels(b); for (uint32_t i=0;i<b->width*b->height;i++) assert(p[i]==0); free(p);
    b->gpu_owned=true; s->gpu_owned=true; fill(b,0,0,1); fill(s,0,1,0);
    VkImageCopy region={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstOffset={2,2,0},.extent={4,4,1}};
    vk_full_barrier(vk_cmd()); vkCmdCopyImage(vk_cmd(),s->image,VK_IMAGE_LAYOUT_GENERAL,b->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    vk_effect_written(&t,0); assert(vk_effect_preserve(&t,0));
    VkComponentMapping sw={0};
    VkImageView view=vk_image_view(b,VK_IMAGE_VIEW_TYPE_2D,b->format,0,1,0,1,sw,VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderingAttachmentInfo ca={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
        .loadOp=vk_effect_preserve(&t,0)?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{b->width,b->height}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&ca};
    vk_full_barrier(vk_cmd()); vkCmdBeginRendering(vk_cmd(),&ri);
    VkClearAttachment at={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.clearValue.color.float32={1,0,0,1}};
    VkClearRect rect={.rect={{0,0},{2,2}},.layerCount=1}; vkCmdClearAttachments(vk_cmd(),1,&at,1,&rect); vkCmdEndRendering(vk_cmd());
    p=pixels(b); assert(p[0]==0xff0000ff && p[2*b->width+2]==0xff00ff00 && p[b->width*b->height-1]==0xffff0000); free(p);
    // Read the preserved image through both reallocations, including odd-size rounding.
    renderer_set_scale(1,1); b=tex_backend(&t); assert(b->width==65 && b->height==36);
    p=pixels(b); assert(p[0]==0xff0000ff && p[b->width*b->height-1]==0xffff0000); free(p);
    frame++; renderer_set_scale(2.0f/3,2.0f/3); b=tex_backend(&t);
    p=pixels(b); assert(p[0]==0xff0000ff && p[b->width*b->height-1]==0xffff0000); free(p);
    renderer_set_scale(5.0f/6,5.0f/6); b=tex_backend(&t); // 900p scale
    assert(b->width==54 && b->height==30);
    p=pixels(b); assert(p[0]==0xff0000ff && p[b->width*b->height-1]==0xffff0000); free(p);
    backend_texture_destroyed(&t); backend_texture_destroyed(&source); submit(); completed++; vk_collect_dead();
    free(g_live); free(dead); vmaDestroyAllocator(g_vk.vma); vkDestroyCommandPool(g_vk.dev,g_vk.pool,NULL); vkDestroyDevice(g_vk.dev,NULL);
    if (messenger) { PFN_vkDestroyDebugUtilsMessengerEXT f=(void*)vkGetInstanceProcAddr(g_vk.inst,"vkDestroyDebugUtilsMessengerEXT"); f(g_vk.inst,messenger,NULL); }
    vkDestroyInstance(g_vk.inst,NULL); assert(errors==0);
    puts("Vulkan effect content: zero initialization, partial copy/LOAD/partial clear, match/x1/match/900p resize passed");
}
