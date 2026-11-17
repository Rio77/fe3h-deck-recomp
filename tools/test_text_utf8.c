#include <assert.h>
#include <string.h>
#include "text_utf8.h"
int main(void) {
    char text[96] = "José김";
    assert(text_utf8_count(text) == 5);
    text_utf8_pop(text); assert(!strcmp(text, "José"));
    text_utf8_truncate(text, 3); assert(!strcmp(text, "Jos"));
    unsigned short original[] = {0x00f1, 0xd83d, 0xde00, 0xd800};
    text_utf16_to_utf8(original, 4, text, sizeof text);
    assert(!strcmp(text, "ñ\xf0\x9f\x98\x80\xef\xbf\xbd"));
    text_utf16_to_utf8(original, 4, text, 4);
    assert(!strcmp(text, "ñ"));
    strcpy(text, "\xf0\x9f\x98\x80"); text_utf8_pop(text); assert(!text[0]);
    return 0;
}
