#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../runtime/settings.h"
const char *const g_tunic_colours[N_TUNIC_COLOURS]={[0 ... N_TUNIC_COLOURS-1]="Default"};
const char *const g_heart_colours[N_HEART_COLOURS]={[0 ... N_HEART_COLOURS-1]="Default"};
bool bind_settings_load(const char*k,const char*v){return false;}
bool cheatvm_settings_load(const char*k,const char*v){return false;}
void bind_settings_save(FILE*f){}
void cheatvm_settings_save(FILE*f){}
static void check(const char *exe, const char *expected, const char *combo) {
    pid_t p=fork();assert(p>=0);
    if(!p){execl(exe,exe,expected,combo,NULL);_exit(127);}
    int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
int main(int argc,char **argv) {
    if(argc==3){settings_load();assert(!strcmp(settings_language(),argv[1]));assert(g_settings.pad_menu_combo==atoi(argv[2]));settings_save();return 0;}
    char exe[4096];assert(realpath(argv[0],exe));
    char original[4096];assert(getcwd(original,sizeof original));
    char d[]="/tmp/opt5-settings-XXXXXX";assert(mkdtemp(d));assert(!chdir(d));
    FILE *f=fopen("fe3hrecomp.ini","wb");assert(f);
    fputs("\357\273\277language = en\r\npad_menu_combo = 0\r\nresolution = 1x\r\nantialias = off  \r\n",f);assert(!fclose(f));
    check(exe,"en-US","0");check(exe,"en-US","0");
    f=fopen("fe3hrecomp.ini","wb");fputs("language = en-GB\r\npad_menu_combo = 1\r\n",f);fclose(f);check(exe,"en-GB","1");
    f=fopen("fe3hrecomp.ini","wb");fputs("language = es\r\n",f);fclose(f);check(exe,"es","1");
    unlink("fe3hrecomp.ini");assert(!chdir(original));rmdir(d);
    puts("PASS settings: BOM, CRLF, trailing whitespace, legacy en, en-GB, Spanish and pad_menu_combo persistence across process restarts");
}
