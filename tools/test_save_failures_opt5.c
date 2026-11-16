#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int fault, writes, notices;
static int injected_open(const char *, int, ...);
static ssize_t injected_pwrite(int,const void *,size_t,off_t);
static int injected_fsync(int);
static int injected_rename(const char *,const char *);
#define open injected_open
#define pwrite injected_pwrite
#define fsync injected_fsync
#define rename injected_rename
#include "../runtime/services/fs.c"
#undef open
#undef pwrite
#undef fsync
#undef rename
const char *g_romfs_dir;
void rt_fatal_trace(void) { abort(); }
void plat_notice(const char *text) { assert(strstr(text,"could not be saved"));notices++; }
void save_editor_file_read(const char*p,uint64_t a,int64_t o,int64_t n) {(void)p;(void)a;(void)o;(void)n;}
void flags_ext_save_read(const char*p,uint64_t a,int64_t o,int64_t n) {(void)p;(void)a;(void)o;(void)n;}
void flags_ext_save_written(const char*p,uint64_t a,int64_t o,int64_t n) {(void)p;(void)a;(void)o;(void)n;}
static int injected_open(const char *p,int f,...) {
    int mode=0644;
    if (f&O_CREAT) {va_list a;va_start(a,f);mode=va_arg(a,int);va_end(a);}
    if (fault==5 && strstr(p,".pending") && (f&(O_WRONLY|O_RDWR))) {errno=EACCES;return -1;}
    return open(p,f,mode);
}
static ssize_t injected_pwrite(int fd,const void*p,size_t n,off_t o) {
    if (fault==2 && writes++) {errno=ENOSPC;return -1;}
    if ((fault==1 || fault==2) && n>3)n=3;
    return pwrite(fd,p,n,o);
}
static int injected_fsync(int fd) {if(fault==3){errno=ENOSPC;return -1;}return fsync(fd);}
static int injected_rename(const char*a,const char*b) {if(fault==4){errno=EACCES;return -1;}return rename(a,b);}
static void bytes(const char *path,const char *expected) {
    char b[32]={0};int fd=open(path,O_RDONLY);assert(fd>=0);assert(read(fd,b,sizeof b)>=0);close(fd);assert(!strcmp(b,expected));
}
static void reset(const char *p) {int f=open(p,O_CREAT|O_TRUNC|O_WRONLY,0600);assert(f>=0);assert(write(f,"old-save",8)==8);close(f);}
int main(void) {
    char dir[]="/tmp/opt5-save-XXXXXX";assert(mkdtemp(dir));char path[256];snprintf(path,sizeof path,"%s/slot00",dir);
    for(int mode=1;mode<=4;mode++) {
        reset(path);HostFs fs={.journaled=true};snprintf(fs.root,sizeof fs.root,"%s",dir);
        char work[1100];fault=0;int slot=pending_of(&fs,path,true,work,sizeof work);assert(slot==0);
        HostFile f={.fd=open(work,O_RDWR),.fs=&fs,.slot=slot,.gen=fs.gen};assert(f.fd>=0);snprintf(f.path,sizeof f.path,"%s",path);
        Service svc={.state=&f};uint64_t data[3]={0,0,8};char value[]="new-save";
        IpcReq req={.cmd=1,.data=(const uint8_t*)data,.na=1,.a={{(uint64_t)(uintptr_t)value,8}}};IpcResp resp={0};
        fault=mode;writes=0;file(&svc,&req,&resp);assert(resp.result==0);close(f.fd);commit(&fs);
        bytes(path,mode==1?"new-save":"old-save");assert(access(work,F_OK)!=0);fault=0;
    }
    reset(path);HostFs fs={.journaled=true};snprintf(fs.root,sizeof fs.root,"%s",dir);char work[1100];fault=5;
    assert(pending_of(&fs,path,true,work,sizeof work)==-1);assert(errno==EACCES);bytes(path,"old-save");
    int fd=scratch_copy(path);assert(fd>=0);assert(pwrite(fd,"scratch!",8,0)==8);close(fd);bytes(path,"old-save");fault=0;
    fs.npending=PENDING_MAX;assert(pending_of(&fs,path,true,work,sizeof work)==-1 && errno==ENOSPC);
    fs.npending=1;fs.gen=4;HostFile old={.fs=&fs,.slot=0,.gen=3};file_failed(&old,"old handle",ENOSPC);assert(!fs.failed[0]);
    // Retiring an unclosed service closes its descriptor without publishing
    // an uncommitted working copy or changing the confirmed save.
    fs.npending=0;fault=0;reset(path);
    assert(pending_of(&fs,path,true,work,sizeof work)==0);
    HostFile *open_file=calloc(1,sizeof *open_file);assert(open_file);
    open_file->fd=open(work,O_RDWR);assert(open_file->fd>=0);
    int old_fd=open_file->fd;assert(pwrite(old_fd,"pending!",8,0)==8);
    Service retired={.state=open_file};file_close(&retired);assert(!retired.state);
    errno=0;assert(fcntl(old_fd,F_GETFD)==-1 && errno==EBADF);
    bytes(path,"old-save");unlink(work);
    assert(notices==1);unlink(path);rmdir(dir);
    puts("PASS saves: short writes, disk full, fsync failure, replacement failure, permissions, scratch, generations");
}
