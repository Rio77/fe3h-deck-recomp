#include <assert.h>
#include "../runtime/services/fs.c"
int main(int argc,char **argv) {
 assert(argc==2);char why[256],bin[1200];
 assert(romfs_layout(argv[1],&g_base,why,sizeof why));snprintf(bin,sizeof bin,"%s.bin",argv[1]);int fd=open(bin,O_RDONLY);assert(fd>=0);
 struct stat st;assert(fstat(fd,&st)==0);assert((uint64_t)st.st_size==g_base.size);
 uint64_t rng=12345;unsigned checks=0;
 for(int i=0;i<2000;i++) {
  rng=rng*6364136223846793005ull+1;uint64_t off=rng%g_base.size;
  uint8_t a[4096]={0},b[4096]={0};ssize_t n=pread(fd,a,sizeof a,off);assert(n>=0);assert(base_pread(b,sizeof b,off)==sizeof b);assert(!memcmp(a,b,sizeof b));checks++;
 }
 for(int i=0;i<g_base.nfiles;i++) {
  uint64_t off=g_base.files[i].start;uint8_t a[32]={0},b[32]={0};assert(pread(fd,a,sizeof a,off)>=0);assert(base_pread(b,sizeof b,off)==sizeof b);assert(!memcmp(a,b,sizeof b));checks++;
 }
 close(fd);printf("PASS real RomFS: %d files, %u sampled reads match existing binary (%llu bytes), no files modified\n",g_base.nfiles,checks,(unsigned long long)g_base.size);base_close();
}
