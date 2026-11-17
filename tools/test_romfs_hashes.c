#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "romfs_build.h"
int main(int argc, char **argv) {
    assert(argc == 3);
    char why[256];
    assert(romfs_check_structure(argv[1], why, sizeof why));
    bool clean = romfs_check_clean(argv[1], why, sizeof why);
    assert(clean == !strcmp(argv[2], "clean"));
    if (!clean) assert(strstr(why, "DATA0.bin"));
    puts("PASS native structural acceptance and advisory content check");
}
