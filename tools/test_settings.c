#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/settings.h"
const char *const g_tunic_colours[N_TUNIC_COLOURS]={[0 ... N_TUNIC_COLOURS-1]="Default"};
const char *const g_heart_colours[N_HEART_COLOURS]={[0 ... N_HEART_COLOURS-1]="Default"};
bool bind_settings_load(const char *k,const char *v){return false;}
bool cheatvm_settings_load(const char *k,const char *v){return false;}
void bind_settings_save(FILE *f){}
void cheatvm_settings_save(FILE *f){}
int main(int argc,char **argv){
 assert(argc==2);int step=atoi(argv[1]);settings_load();
 assert(g_settings.pad_menu_combo==(step!=1));
 if(step<2){g_settings.pad_menu_combo=step==1;settings_save();}
 printf("PASS pad_menu_combo persistence step %d\n",step);
}
