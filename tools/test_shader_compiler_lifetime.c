#include <assert.h>
#include "../runtime/nvn/vulkan/shaders.c"
static void *compile_thread(void *arg) {
    (void)arg;
    const char *source="#version 450\nvoid main(){gl_Position=vec4(0.0,0.0,0.0,1.0);}";
    for (int i=0;i<2;i++) {
        size_t n=0;uint32_t *p=vk_glsl_to_spirv(source,STAGE_VERTEX,"lifetime-test",&n);
        assert(p && n>5 && p[0]==0x07230203);free(p);
    }
    return NULL;
}
int main(void) {
    setenv("LARECOMP_VK_NO_SPIRV_CACHE","1",1);
    for (int i=0;i<20;i++) {
        pthread_t t;assert(!pthread_create(&t,NULL,compile_thread,NULL));assert(!pthread_join(t,NULL));
        uint64_t created,freed;vk_shader_compiler_stats(&created,&freed);assert(created==(uint64_t)i+1 && freed==created);
    }
    puts("PASS shader compiler lifecycle: 20 threads, 40 real compilations, one context per thread, zero retired contexts");
}
