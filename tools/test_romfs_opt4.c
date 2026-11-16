#include <assert.h>
#include "../runtime/services/fs.c"
#include "../runtime/services/aoc.c"
bool romfs_build(const char *, const char *, void (*)(uint64_t,uint64_t),char *,size_t);
const char *g_romfs_dir;
void rt_fatal_trace(void) { abort(); }
static char fixture[1100];
bool rt_remove_game_file(const char *dir,const char *name) {
    assert(!strcmp(dir,fixture) && !strcmp(name,"romfs.bin"));
    char p[1200];snprintf(p,sizeof p,"%s/%s",dir,name);return unlink(p)==0;
}
static void compare(int fd,uint64_t off,size_t n) {
    uint8_t a[8192]={0},b[8192]={0};assert(n<=sizeof a);
    assert(pread(fd,a,n,off)>=0);assert(base_pread(b,n,off)==(ssize_t)n);assert(!memcmp(a,b,n));
}
int main(int argc,char **argv) {
    assert(argc==2);snprintf(fixture,sizeof fixture,"%s",argv[1]);
    char dir[1200],bin[1200],why[256];snprintf(dir,sizeof dir,"%s/romfs",fixture);snprintf(bin,sizeof bin,"%s/romfs.bin",fixture);
    assert(romfs_build(dir,bin,NULL,why,sizeof why));int ref=open(bin,O_RDONLY);assert(ref>=0);
    g_romfs_dir=dir;base_open();assert(g_base.nfiles>=1000 && access(bin,F_OK)!=0);
    for(uint64_t o=0;o<g_base.size+8192;o+=71)compare(ref,o,1024);
    for(int i=0;i<g_base.nfiles;i++) {
        uint64_t o=g_base.files[i].start;compare(ref,o?o-1:0,128);
        compare(ref,o+g_base.files[i].size,128);
    }
    compare(ref,0,0);compare(ref,g_base.tables_off-1,8192);
    int nonempty=0;while(!g_base.files[nonempty].size)nonempty++;
    char missing[1400];snprintf(missing,sizeof missing,"%s",g_base.files[nonempty].path);
    for(int k=0;k<BASE_FDS;k++)if(g_base_fds[k].fd>0)close(g_base_fds[k].fd);
    memset(g_base_fds,0,sizeof g_base_fds);char hidden[1450];snprintf(hidden,sizeof hidden,"%s.hidden",missing);assert(rename(missing,hidden)==0);
    uint8_t tmp[32];assert(base_pread(tmp,1,g_base.files[nonempty].start)==-1);assert(rename(hidden,missing)==0);
    base_close();g_base_fd=ref;g_base_open=true;compare(ref,0,512);g_base_fd=-1;g_base_open=false;
    assert(romfs_build(dir,bin,NULL,why,sizeof why));
    Dlc x={.fd=-1,.last_file=-1,.last_fd=-1};snprintf(x.dir,sizeof x.dir,"%s",fixture);
    assert(dlc_open(&x));assert(access(bin,F_OK)!=0);
    for(uint64_t o=0;o<x.size+8192;o+=71) {
        uint8_t a[1024]={0},b[1024]={0};assert(pread(ref,a,sizeof a,o)>=0);assert(dlc_read(&x,b,sizeof b,o));assert(!memcmp(a,b,sizeof a));
    }
    if(x.last_fd>=0)close(x.last_fd);romfs_layout_free(&x.lay);
    assert(romfs_build(dir,bin,NULL,why,sizeof why));char hold[1300];snprintf(hold,sizeof hold,"%s.hold",dir);assert(rename(dir,hold)==0);
    x=(Dlc){.fd=-1,.last_file=-1,.last_fd=-1};snprintf(x.dir,sizeof x.dir,"%s",fixture);assert(dlc_open(&x));assert(x.fd>=0 && access(bin,F_OK)==0);close(x.fd);
    assert(mkdir(dir,0755)==0);x=(Dlc){.fd=-1,.last_file=-1,.last_fd=-1};snprintf(x.dir,sizeof x.dir,"%s",fixture);
    assert(!dlc_open(&x));assert(access(bin,F_OK)==0);
    close(ref);puts("PASS: base/DLC byte equivalence, boundaries, padding, missing file error, validated deletion, binary fallback, empty DLC preserves binary");
}
