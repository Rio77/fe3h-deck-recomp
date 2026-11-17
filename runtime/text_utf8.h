// Bounded name conversion and editing. SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static inline int text_utf8_count(const char *s) {
    int n=0; for (; *s; ++s) if (((unsigned char)*s & 0xc0)!=0x80) ++n; return n;
}
static inline void text_utf8_truncate(char *s, int max) {
    int n=0; for (; *s; ++s) if (((unsigned char)*s & 0xc0)!=0x80 && n++>=max) { *s=0; return; }
}
static inline void text_utf8_pop(char *s) {
    size_t n=strlen(s); if (!n) return;
    do { --n; } while(n && ((unsigned char)s[n]&0xc0)==0x80); s[n]=0;
}
static inline void text_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t used=0; if (!cap) return;
    for(size_t i=0; s && i<n && s[i]; ++i) {
        uint32_t cp=s[i];
        if(cp>=0xd800 && cp<=0xdbff && i+1<n && s[i+1]>=0xdc00 && s[i+1]<=0xdfff) {
            cp=0x10000+((cp-0xd800)<<10)+(s[i+1]-0xdc00); ++i;
        } else if(cp>=0xd800 && cp<=0xdfff) cp=0xfffd;
        char b[4]; size_t len;
        if(cp<0x80) {b[0]=cp;len=1;}
        else if(cp<0x800) {b[0]=0xc0|(cp>>6);b[1]=0x80|(cp&63);len=2;}
        else if(cp<0x10000) {b[0]=0xe0|(cp>>12);b[1]=0x80|((cp>>6)&63);b[2]=0x80|(cp&63);len=3;}
        else {b[0]=0xf0|(cp>>18);b[1]=0x80|((cp>>12)&63);b[2]=0x80|((cp>>6)&63);b[3]=0x80|(cp&63);len=4;}
        if(len>=cap-used) break; memcpy(out+used,b,len);used+=len;
    }
    out[used]=0;
}
