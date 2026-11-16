#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/nvn/maxwell/shader.h"
int main(void) {
    uint8_t blob[0xc0]={0};uint64_t code[]={0,0xf0c8000000070000ull | (0x12ull<<20),0xe30000000007000full,0};
    memcpy(blob+0x80,code,sizeof code);
    for(int legacy=0;legacy<2;legacy++)for(int flip=0;flip<2;flip++) {
        ShaderKey key={.glsl=1,.flip_y=flip};fe3h_ydir_fix=!legacy;
        ShaderInfo *si=maxwell_translate(blob,0x80+24,STAGE_FRAGMENT,&key);
        if(si && !si->msl)fprintf(stderr,"translation: %s\n",si->error);
        assert(si && si->msl);assert(strstr(si->msl,legacy || flip?"r0 = 0xbf800000u":"r0 = 0x3f800000u"));maxwell_free(si);
    }
    puts("PASS: SR_Y_DIRECTION for both origins and legacy comparison switch");
}
