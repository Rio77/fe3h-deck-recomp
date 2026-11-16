#include <assert.h>
#include "../runtime/hle/movie.c"
const char *g_romfs_dir;
Settings g_settings={.movie_audio=true};
volatile int g_inject_plus;
uint64_t input_buttons_now(void){return 0;}
void rt_fatal_trace(void){abort();}
static void allocate_decoder(Decoder *d) {
    memset(d,0,sizeof *d);
    d->ctx=avcodec_alloc_context3(NULL);assert(d->ctx);
    d->frame=av_frame_alloc();d->pkt=av_packet_alloc();assert(d->frame && d->pkt);
    assert(!av_new_packet(d->pkt,4096));
    for(int i=0;i<4;i++) {
        d->pending[i]=av_frame_alloc();assert(d->pending[i]);
        d->pending[i]->width=512;d->pending[i]->height=512;d->pending[i]->format=AV_PIX_FMT_YUV420P;
        assert(av_frame_get_buffer(d->pending[i],32)>=0);d->npending++;
    }
    d->sws=sws_getContext(512,512,AV_PIX_FMT_YUV420P,512,512,AV_PIX_FMT_RGBA,SWS_BILINEAR,NULL,NULL,NULL);assert(d->sws);
    d->rgba=malloc(512*512*4);assert(d->rgba);memset(d->rgba,127,512*512*4);
}
static void released(Decoder *d) {
    assert(!d->ctx && !d->frame && !d->pkt && !d->sws && !d->rgba && !d->npending && !d->nready);
    for(int i=0;i<4;i++)assert(!d->pending[i]);
}
int main(int argc,char **argv) {
    assert(argc==3);g_romfs_dir=argv[1];Extractor e={0};uint64_t self[2]={0,(uint64_t)(uintptr_t)&e};Cpu c={0};
    for(int iteration=0;iteration<40;iteration++) {
        c.x[0]=(uint64_t)(uintptr_t)self;ex_open_path(&c,argv[2]);assert(!c.x[0] && e.fmt && e.pkt);
        e.selected=~0ull;ex_fill(&e);
        c.x[0]=(uint64_t)(uintptr_t)self;ex_open_path(&c,"rom:/missing-opt5-test.webm");assert(c.x[0] && !e.fmt && !e.pkt);
        ex_fill(&e);assert(e.eof);
        Decoder d;allocate_decoder(&d);dec_free(&d);released(&d);dec_free(&d);released(&d);
    }
    Decoder *d=calloc(1,sizeof *d);allocate_decoder(d);pthread_mutex_init(&d->lock,NULL);movie_track(1,d,true);
    Extractor *last=calloc(1,sizeof *last);self[1]=(uint64_t)(uintptr_t)last;
    c.x[0]=(uint64_t)(uintptr_t)self;ex_open_path(&c,argv[2]);assert(!c.x[0]);movie_track(0,last,true);
    MediaData *meta=md_new();md_set_str(meta,"test","owned metadata");
    assert(movie_owned_add(&g_buffer_owners,123,malloc(256),256));
    assert(movie_owned_copy(&g_buffer_owners,456,123));
    movie_reset();assert(!g_media_data && !g_retired_movies && !g_buffer_owners.bytes);
    assert(!g_movie_objs[0][0] && !g_movie_objs[1][0]);
    puts("PASS movies: real extractor reopen/failure, decoder buffers, repeated free and reboot cleanup");
}
