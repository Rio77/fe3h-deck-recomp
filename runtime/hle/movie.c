// movie:: (nnMedia) implemented with FFmpeg: an Android MediaExtractor/MediaCodec-style API.
// The game owns the objects (vtable at +0); we keep host state at +8.
//
// Extractor slots: 4 GetTrackCount(int*), 5 GetTrackConfiguration(int, MediaData*), 6 SelectTrack(int),
//   9 GetSampleTrackIndex(size_t*), 10 Read(Buffer*), 11 Advance(), 12 GetSampleTime(int64*),
//   15 SetDataSource(DataSource*, const char* path).
// Decoder slots: 2 Configure(MediaData*), 3 RegisterClientEvents(EventType*[4]), 4 GetOutputFormat(MediaData*),
//   6 Start, 9 AcquireInputBufferIndex(int*, int*), 10 GetInputBuffer(int, Buffer*),
//   11 SendInputBufferForDecode(int, int off, int size, int64 time, uint32 flags),
//   12 AcquireOutputBufferIndex(int*, int64*, uint32*, int*), 13 GetOutputBuffer(int, Buffer*),
//   15 ReleaseOutputBufferIndex(int), 18 GetOutputBufferFormat(int, MediaData*).
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "movie_buffer_owners.h"

#define MOVIE_EOS 4u

// ------------------------------------------------------------------ MediaData (8-byte handle)
typedef struct { char *key; char *sval; int64_t ival; bool is_str, is_rect; int32_t rect[4]; } MdEntry;
typedef struct MediaData { MdEntry e[32]; int n; void *codecpar; struct MediaData *next; } MediaData;
static MediaData *g_media_data;
static pthread_mutex_t g_media_data_lock=PTHREAD_MUTEX_INITIALIZER;
static MediaData *md_new(void) {
    MediaData *m=calloc(1,sizeof *m);
    if (!m) FATAL("movie MediaData allocation failed\n");
    pthread_mutex_lock(&g_media_data_lock);m->next=g_media_data;g_media_data=m;
    pthread_mutex_unlock(&g_media_data_lock);return m;
}

static MediaData *md(uint64_t guest) {
    MediaData **p = (MediaData **)(uintptr_t)guest;
    if (!*p) *p = md_new();
    return *p;
}
static void md_clear(MediaData *m) {
    for (int i = 0; i < m->n; i++) { free(m->e[i].key); free(m->e[i].sval); }
    m->n = 0;
    m->codecpar = NULL;
}
static void md_drop(MediaData *m) {
    pthread_mutex_lock(&g_media_data_lock);
    MediaData **p=&g_media_data;while (*p && *p!=m) p=&(*p)->next;
    if (*p) { *p=m->next;md_clear(m);free(m); }
    pthread_mutex_unlock(&g_media_data_lock);
}
static MdEntry *md_slot(MediaData *m, const char *k) {
    for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i].key, k)) return &m->e[i];
    if (m->n == 32) return NULL;
    MdEntry *e = &m->e[m->n++];
    memset(e, 0, sizeof *e);
    e->key = strdup(k);
    return e;
}
static void md_set_int(MediaData *m, const char *k, int64_t v) { MdEntry *e = md_slot(m, k); if (e) { e->ival = v; e->is_str = e->is_rect = false; } }
static void md_set_rect(MediaData *m, const char *k, int32_t l, int32_t t, int32_t r, int32_t b) {
    MdEntry *e = md_slot(m, k);
    if (e) { e->is_str = false; e->is_rect = true; e->rect[0] = l; e->rect[1] = t; e->rect[2] = r; e->rect[3] = b; }
}
static void md_set_str(MediaData *m, const char *k, const char *v) {
    MdEntry *e = md_slot(m, k);
    if (e) { free(e->sval); e->sval = strdup(v); e->is_str = true; }
}

static void md_miss(const char *what, const char *k, Cpu *c) {
    static int n;
    if (n++ < 40) LOG("[movie] MediaData %s(\"%s\") not found (lr=%llx)\n", what, k ? k : "", (unsigned long long)c->x[30]);
}

HLE_FUNC("_ZN5movie9MediaDataC1Ev", md_ctor) { *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)(uintptr_t)md_new(); }
HLE_FUNC("_ZN5movie9MediaDataC1ERKS0_", md_copy) {
    MediaData *src = md(c->x[1]), *dst = md_new();
    for (int i = 0; i < src->n; i++) {
        dst->e[i].key = strdup(src->e[i].key);
        dst->e[i].sval = src->e[i].sval ? strdup(src->e[i].sval) : NULL;
        dst->e[i].ival = src->e[i].ival;
        dst->e[i].is_str = src->e[i].is_str;
        dst->e[i].is_rect = src->e[i].is_rect;
        memcpy(dst->e[i].rect, src->e[i].rect, sizeof dst->e[i].rect);
    }
    dst->n = src->n;
    dst->codecpar = src->codecpar;
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)(uintptr_t)dst;
}
HLE_FUNC("_ZN5movie9MediaDataD1Ev", md_dtor) {
    MediaData *m = *(MediaData **)(uintptr_t)c->x[0];
    if (m) md_drop(m);
    *(uint64_t *)(uintptr_t)c->x[0] = 0;
}
HLE_FUNC("_ZN5movie9MediaData5ClearEv", md_clear_hle) { md_clear(md(c->x[0])); }
// FindRect(name, &left, &top, &right, &bottom), e.g. "crop" (inclusive right / bottom)
HLE_FUNC("_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_", md_find_rect) {
    MediaData *m = md(c->x[0]);
    const char *k = (const char *)(uintptr_t)c->x[1];
    for (int i = 0; i < m->n; i++)
        if (m->e[i].is_rect && !strcmp(m->e[i].key, k)) {
            for (int j = 0; j < 4; j++) if (c->x[2 + j]) *(int32_t *)(uintptr_t)c->x[2 + j] = m->e[i].rect[j];
            c->x[0] = 1;
            return;
        }
    md_miss("FindRect", k, c);
    c->x[0] = 0;
}
HLE_FUNC("_ZN5movie9MediaData7SetRectEPKciiii", md_set_rect_hle) {
    md_set_rect(md(c->x[0]), (const char *)(uintptr_t)c->x[1], (int32_t)c->x[2], (int32_t)c->x[3], (int32_t)c->x[4],
                (int32_t)c->x[5]);
}
HLE_FUNC("_ZN5movie9MediaData8SetInt32EPKci", md_set_int32) {
    md_set_int(md(c->x[0]), (const char *)(uintptr_t)c->x[1], (int32_t)c->x[2]);
    if (getenv("LARECOMP_MOVIE_LOG")) LOG("[movie] MediaData SetInt32(\"%s\", %d)\n", (const char *)(uintptr_t)c->x[1], (int32_t)c->x[2]);
}
HLE_FUNC("_ZN5movie9MediaData8SetInt64EPKcl", md_set_int64) {
    md_set_int(md(c->x[0]), (const char *)(uintptr_t)c->x[1], (int64_t)c->x[2]);
}
HLE_FUNC("_ZN5movie9MediaData9SetStringEPKcS2_", md_set_string) {
    const char *v = (const char *)(uintptr_t)c->x[2];
    md_set_str(md(c->x[0]), (const char *)(uintptr_t)c->x[1], v ? v : "");
}
HLE_FUNC("_ZNK5movie9MediaData9FindInt64EPKcPl", md_find_int64) {
    MediaData *m = md(c->x[0]);
    const char *k = (const char *)(uintptr_t)c->x[1];
    for (int i = 0; i < m->n; i++)
        if (!m->e[i].is_str && !strcmp(m->e[i].key, k)) { *(int64_t *)(uintptr_t)c->x[2] = m->e[i].ival; c->x[0] = 1; return; }
    md_miss("FindInt64", k, c);
    c->x[0] = 0;
}
HLE_FUNC("_ZNK5movie9MediaData9FindInt32EPKcPi", md_find_int32) {
    MediaData *m = md(c->x[0]);
    const char *k = (const char *)(uintptr_t)c->x[1];
    for (int i = 0; i < m->n; i++)
        if (!m->e[i].is_str && !strcmp(m->e[i].key, k)) { *(int32_t *)(uintptr_t)c->x[2] = (int32_t)m->e[i].ival; c->x[0] = 1; return; }
    md_miss("FindInt32", k, c);
    c->x[0] = 0;
}
HLE_FUNC("_ZNK5movie9MediaData10FindStringEPKcPS2_", md_find_string) {
    MediaData *m = md(c->x[0]);
    const char *k = (const char *)(uintptr_t)c->x[1];
    for (int i = 0; i < m->n; i++)
        if (m->e[i].is_str && !strcmp(m->e[i].key, k)) { *(uint64_t *)(uintptr_t)c->x[2] = (uint64_t)(uintptr_t)m->e[i].sval; c->x[0] = 1; return; }
    md_miss("FindString", k, c);
    c->x[0] = 0;
}

// ------------------------------------------------------------------ Buffer (opaque, <= 0x40 bytes)
typedef struct {
    uint64_t base, capacity;
    int32_t offset, size;
    int64_t i64;
    uint32_t u32;
    int32_t i32;
} MBuffer;
_Static_assert(sizeof(MBuffer) <= 0x40, "movie::Buffer size");
#define BUF(x) ((MBuffer *)(uintptr_t)(x))
static MovieBufferOwners g_buffer_owners;
static pthread_mutex_t g_buffer_owners_lock = PTHREAD_MUTEX_INITIALIZER;

HLE_FUNC("_ZN5movie6BufferC1Ev", buf_ctor) {
    pthread_mutex_lock(&g_buffer_owners_lock);movie_owned_release(&g_buffer_owners,c->x[0]);
    memset(BUF(c->x[0]),0,sizeof(MBuffer));pthread_mutex_unlock(&g_buffer_owners_lock);
}
HLE_FUNC("_ZN5movie6BufferC1EPvm", buf_ctor_ext) {
    pthread_mutex_lock(&g_buffer_owners_lock);movie_owned_release(&g_buffer_owners,c->x[0]);
    MBuffer *b = BUF(c->x[0]);
    memset(b, 0, sizeof *b);
    b->base = c->x[1];
    b->capacity = c->x[2];
    b->size = (int32_t)c->x[2];
    pthread_mutex_unlock(&g_buffer_owners_lock);
}
HLE_FUNC("_ZN5movie6BufferC1Em", buf_ctor_alloc) {
    if (c->x[1]>SIZE_MAX-64) FATAL("movie Buffer capacity overflow\n");
    void *base=calloc(1,(size_t)c->x[1]+64);
    if (!base) FATAL("movie Buffer allocation failed\n");
    pthread_mutex_lock(&g_buffer_owners_lock);
    bool owned=movie_owned_add(&g_buffer_owners,c->x[0],base,(size_t)c->x[1]+64);
    if (!owned) { pthread_mutex_unlock(&g_buffer_owners_lock);free(base);FATAL("movie Buffer registry allocation failed\n"); }
    MBuffer *b = BUF(c->x[0]);
    memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)base;
    b->capacity = c->x[1];
    b->size = (int32_t)c->x[1];
    pthread_mutex_unlock(&g_buffer_owners_lock);
}
static void buffer_copy(Cpu *c) {
    pthread_mutex_lock(&g_buffer_owners_lock);
    bool ok=movie_owned_copy(&g_buffer_owners,c->x[0],c->x[1]);
    if (ok) *BUF(c->x[0])=*BUF(c->x[1]);
    pthread_mutex_unlock(&g_buffer_owners_lock);
    if (!ok) FATAL("movie Buffer copy allocation failed\n");
}
HLE_FUNC("_ZN5movie6BufferC1ERKS0_", buf_copy) { buffer_copy(c); }
HLE_FUNC("_ZN5movie6BufferaSERKS0_", buf_assign) { buffer_copy(c); }
HLE_FUNC("_ZN5movie6BufferD1Ev", buf_dtor) {
    pthread_mutex_lock(&g_buffer_owners_lock);movie_owned_release(&g_buffer_owners,c->x[0]);
    pthread_mutex_unlock(&g_buffer_owners_lock);
}
HLE_FUNC("_ZNK5movie6Buffer4BaseEv", buf_base) {
    static int n;
    if (getenv("LARECOMP_MOVIE_LOG") && n++ < 60) LOG("[movie] Buffer %llx Base -> %llx (lr=%llx)\n", (unsigned long long)c->x[0],
                                                     (unsigned long long)BUF(c->x[0])->base, (unsigned long long)c->x[30]);
    c->x[0] = BUF(c->x[0])->base;
}
HLE_FUNC("_ZNK5movie6Buffer4SizeEv", buf_size) { c->x[0] = (uint32_t)BUF(c->x[0])->size; }
HLE_FUNC("_ZNK5movie6Buffer6OffsetEv", buf_offset) { c->x[0] = (uint32_t)BUF(c->x[0])->offset; }
HLE_FUNC("_ZNK5movie6Buffer8CapacityEv", buf_capacity) { c->x[0] = BUF(c->x[0])->capacity; }
HLE_FUNC("_ZN5movie6Buffer8SetRangeEii", buf_set_range) { BUF(c->x[0])->offset = (int32_t)c->x[1]; BUF(c->x[0])->size = (int32_t)c->x[2]; }
HLE_FUNC("_ZN5movie6Buffer18SetDataAndCapacityEPvm", buf_set_data) {
    MBuffer *b = BUF(c->x[0]);
    b->base = c->x[1];
    b->capacity = c->x[2];
}
HLE_FUNC("_ZN5movie6Buffer12SetInt64DataEl", buf_set_i64) {
    if (getenv("LARECOMP_MOVIE_LOG")) LOG("[movie] Buffer %llx SetInt64Data(%llx) (lr=%llx)\n", (unsigned long long)c->x[0],
                                         (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
    BUF(c->x[0])->i64 = (int64_t)c->x[1];
}
HLE_FUNC("_ZNK5movie6Buffer12GetInt64DataEv", buf_get_i64) {
    if (getenv("LARECOMP_MOVIE_LOG")) LOG("[movie] Buffer %llx GetInt64Data -> %llx (lr=%llx)\n", (unsigned long long)c->x[0],
                                         (unsigned long long)BUF(c->x[0])->i64, (unsigned long long)c->x[30]);
    c->x[0] = (uint64_t)BUF(c->x[0])->i64;
}
HLE_FUNC("_ZN5movie6Buffer13SetUint32DataEj", buf_set_u32) { BUF(c->x[0])->u32 = (uint32_t)c->x[1]; }
HLE_FUNC("_ZNK5movie6Buffer13GetUint32DataEv", buf_get_u32) { c->x[0] = BUF(c->x[0])->u32; }
HLE_FUNC("_ZN5movie6Buffer12SetInt32DataEi", buf_set_i32) { BUF(c->x[0])->i32 = (int32_t)c->x[1]; }

HLE_FUNC("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", movie_set_allocator) { (void)c; }
HLE_FUNC("_ZN5movie17SetThreadPriorityEi", movie_set_thread_priority) { (void)c; }
HLE_FUNC("_ZN5movie11SetCoreMaskEm", movie_set_core_mask) { c->x[0] = 0; }
HLE_FUNC("_ZN5movie17GetThreadPriorityEv", movie_get_thread_priority) { c->x[0] = 16; }

// ------------------------------------------------------------------ helpers
static uint64_t make_vtable(HostFn *fns, int n, const char *prefix) {
    uint64_t *t = hle_alloc(64 * 8);
    char name[96];
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof name, "<%s slot %d>", prefix, i);
        t[i] = hle_register(name, i < n ? fns[i] : NULL);
    }
    return (uint64_t)(uintptr_t)t;
}

// The game reads the audio and video tracks from different threads; FFmpeg's demuxer is not thread-safe.
static pthread_mutex_t ex_lock = PTHREAD_MUTEX_INITIALIZER;

static void *self_state(Cpu *c) { return (void *)(uintptr_t)((uint64_t *)(uintptr_t)c->x[0])[1]; }

static void signal_guest_event(Cpu *c, uint64_t event) {
    if (!event) return;
    uint64_t saved[8];
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = event;
    guest_call(c, guest_sym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE"));
    memcpy(c->x, saved, sizeof saved);
}

static void ok(Cpu *c) { c->x[0] = 0; }

// ------------------------------------------------------------------ Extractor
typedef struct {
    AVFormatContext *fmt;
    int map[16], ntracks;  // exposed track -> stream
    uint64_t selected;  // bitmask of streams
    AVPacket *pkt;
    bool have_pkt, eof;
    bool skippable;  // the launch opening (forced on the first launch, no save yet): A / B / + ends it
} Extractor;

static bool g_skip_held;           // ex_fill: A / B held at the last look
static double g_skip_pressed_at;    // when they were pressed (0 = not yet)

static void ex_fill(Extractor *e) {
    if (!e->fmt || !e->pkt) { e->eof = true; return; }  // (no source: a failed open)
    if (e->skippable && !e->eof) {
        // the game skips the opening with Plus (once a save exists); A and B do too (we press Plus for it)
        extern uint64_t input_buttons_now(void);
        extern volatile int g_inject_plus;
        bool held = g_skip_held;
        double pressed_at = g_skip_pressed_at;
        uint64_t b = input_buttons_now();
        bool now = (b & ((1u << 0) | (1u << 1) | (1u << 10))) != 0;  // A / B / Plus
        if (now && !held) {
            LOG("[movie] opening: A/B/+ -> Plus (skip)\n");
            if (b & 3) g_inject_plus = 1;
            if (!pressed_at) pressed_at = av_gettime_relative() / 1e6;
        }
        held = now;
        // With no save file yet the game ignores Plus (first viewing can't be skipped): if the movie still plays
        // half a second after the press, jump to its last half second so it ends the normal way (ending the
        // stream outright makes the game restart it).
        if (pressed_at && av_gettime_relative() / 1e6 - pressed_at > 0.5 && e->fmt->duration > 1000000) {
            pressed_at = 0;
            int64_t to = e->fmt->duration - 500000;
            if (av_seek_frame(e->fmt, -1, to, AVSEEK_FLAG_BACKWARD) >= 0) {
                if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
                LOG("[movie] opening: still playing -> seek to the end\n");
            }
            e->skippable = false;  // (done: later presses on the title / file select are the menus' own)
        }
        g_skip_held = held, g_skip_pressed_at = pressed_at;
    }
    while (!e->have_pkt && !e->eof) {
        if (av_read_frame(e->fmt, e->pkt) < 0) { e->eof = true; break; }
        if (e->selected & (1ull << e->pkt->stream_index)) e->have_pkt = true;
        else av_packet_unref(e->pkt);
    }
}

static void ex_open_path(Cpu *c, const char *path);
static void ex_set_data_source(Cpu *c) { ex_open_path(c, (const char *)(uintptr_t)c->x[2]); }

static void ex_open_path(Cpu *c, const char *path) {
    Extractor *e = self_state(c);
    // Replacing a source drops the old one's file, last packet and track selection, also after a failed open (each
    // open used to leak the previous packet; upstream LAS Recomp ac3db2c, huangjianqin).
    if (e->pkt) av_packet_free(&e->pkt);
    if (e->fmt) avformat_close_input(&e->fmt);
    e->selected = 0;
    e->ntracks = 0;
    e->have_pkt = e->eof = e->skippable = false;
    if (!path || !*path) { c->x[0] = 1; return; }
    // "rom:/region_common/movie/title.webm" -> extracted/romfs/region_common/movie/title.webm
    const char *rel = path && strstr(path, ":/") ? strstr(path, ":/") + 2 : path;
    char host[1024];
    snprintf(host, sizeof host, "%s/%s", g_romfs_dir, rel ? rel : "");
    LOG("[movie] open %s -> %s\n", path ? path : "(null)", host);
    if (avformat_open_input(&e->fmt, host, NULL, NULL) < 0 || avformat_find_stream_info(e->fmt, NULL) < 0) {
        LOG("[movie] cannot open %s\n", host);
        if (e->fmt) avformat_close_input(&e->fmt);
        c->x[0] = 1;
        return;
    }
    e->pkt = av_packet_alloc();
    if (!e->pkt) { avformat_close_input(&e->fmt); c->x[0] = 1; return; }
    // the opening at launch can always be skipped, vanilla and randomizer (with no save yet the game forces it)
    e->skippable = rel && strstr(rel, "movie/opening.webm");
    if (e->skippable) g_skip_pressed_at = 0;  // (a press that ended the title's attract replay isn't for this one)
    for (unsigned i = 0; i < e->fmt->nb_streams && e->ntracks < 16; i++)
        if (e->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            (e->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && g_settings.movie_audio && !getenv("LARECOMP_NO_MOVIE_AUDIO")))
            e->map[e->ntracks++] = (int)i;
    c->x[0] = 0;
}

static void ex_get_track_count(Cpu *c) {
    Extractor *e = self_state(c);
    *(int32_t *)(uintptr_t)c->x[1] = e->fmt ? e->ntracks : 0;
    c->x[0] = 0;
}

static void ex_get_track_config(Cpu *c) {
    Extractor *e = self_state(c);
    int i = (int)c->x[1];
    MediaData *m = md(c->x[2]);
    md_clear(m);
    if (!e->fmt || i >= e->ntracks) { c->x[0] = 1; return; }
    AVStream *s = e->fmt->streams[e->map[i]];
    AVCodecParameters *p = s->codecpar;
    m->codecpar = p;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
        md_set_str(m, "mime", p->codec_id == AV_CODEC_ID_VP9 ? "video/x-vnd.on2.vp9" :
                              p->codec_id == AV_CODEC_ID_VP8 ? "video/x-vnd.on2.vp8" : "video/avc");
        md_set_int(m, "width", p->width);
        md_set_int(m, "height", p->height);
        int fps = s->avg_frame_rate.den ? s->avg_frame_rate.num / s->avg_frame_rate.den : 30;
        md_set_int(m, "frame-rate", fps ? fps : 30);
    } else {
        md_set_str(m, "mime", p->codec_id == AV_CODEC_ID_VORBIS ? "audio/vorbis" : "audio/mp4a-latm");
        md_set_int(m, "sample-rate", p->sample_rate);
        md_set_int(m, "channel-count", p->ch_layout.nb_channels);
    }
    if (e->fmt->duration > 0) md_set_int(m, "durationUs", e->fmt->duration);
    md_set_int(m, "max-input-size", p->codec_type == AVMEDIA_TYPE_VIDEO ? 4 << 20 : 256 << 10);  // = the decoder's in_cap
    c->x[0] = 0;
}

static void ex_select_track(Cpu *c) {
    Extractor *e = self_state(c);
    int i = (int)c->x[1];
    if (i >= 0 && i < e->ntracks) e->selected |= 1ull << e->map[i];
    c->x[0] = 0;
}

static void ex_get_sample_track(Cpu *c) {
    Extractor *e = self_state(c);
    ex_fill(e);
    if (!e->have_pkt) { c->x[0] = 1; return; }
    uint64_t t = 0;
    for (int i = 0; i < e->ntracks; i++) if (e->map[i] == e->pkt->stream_index) t = (uint64_t)i;
    *(uint64_t *)(uintptr_t)c->x[1] = t;
    c->x[0] = 0;
}

static void ex_read(Cpu *c) {
    Extractor *e = self_state(c);
    MBuffer *b = BUF(c->x[1]);
    ex_fill(e);
    if (!e->have_pkt) { c->x[0] = 1; return; }
    uint32_t n = (uint32_t)e->pkt->size;
    if (n > b->capacity) { LOG("[movie] packet %u > buffer %llu\n", n, b->capacity); n = (uint32_t)b->capacity; }
    memcpy((void *)(uintptr_t)b->base, e->pkt->data, n);
    b->offset = 0;
    b->size = (int32_t)n;
    c->x[0] = 0;
}

static void ex_advance(Cpu *c) {
    Extractor *e = self_state(c);
    if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
    c->x[0] = 0;
}

static void ex_get_sample_time(Cpu *c) {
    Extractor *e = self_state(c);
    ex_fill(e);
    if (!e->have_pkt) { c->x[0] = 1; return; }
    AVStream *s = e->fmt->streams[e->pkt->stream_index];
    int64_t ts = e->pkt->pts != AV_NOPTS_VALUE ? e->pkt->pts : e->pkt->dts;
    *(int64_t *)(uintptr_t)c->x[1] = av_rescale_q(ts, s->time_base, (AVRational){1, 1000000});
    c->x[0] = 0;
}

// slot 8: SeekTo(int64 timeUs)
static void ex_seek(Cpu *c) {
    Extractor *e = self_state(c);
    if (!e->fmt) { c->x[0] = 1; return; }
    if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
    e->eof = false;
    av_seek_frame(e->fmt, -1, (int64_t)c->x[1], AVSEEK_FLAG_BACKWARD);
    c->x[0] = 0;
}

// slot 14: GetCachedDuration(out1, int64* durationUs) -- local files are fully cached
static void ex_cached_duration(Cpu *c) {
    Extractor *e = self_state(c);
    int64_t dur = e->fmt && e->fmt->duration > 0 ? e->fmt->duration : INT64_MAX / 4;
    if (c->x[1]) *(int64_t *)(uintptr_t)c->x[1] = 1;
    if (c->x[2]) *(int64_t *)(uintptr_t)c->x[2] = dur;
    c->x[0] = 0;
}

// Movie objects of this boot (host side; a reboot mid-movie closes them): vtables live in the HLE heap.
static uint64_t g_ex_vt, g_dec_vt;
#define MAX_MOVIE_OBJS 32
static void *g_movie_objs[2][MAX_MOVIE_OBJS];  // [0] Extractor, [1] Decoder
static pthread_mutex_t g_movie_lock = PTHREAD_MUTEX_INITIALIZER;
// Destructors may leave guest-side references to host state. Keep a retirement
// list until the boot's threads have stopped, instead of losing the allocation.
typedef struct RetiredMovie { void *p; int kind; struct RetiredMovie *next; } RetiredMovie;
static RetiredMovie *g_retired_movies;
static void movie_track(int kind, void *p, bool add) {
    pthread_mutex_lock(&g_movie_lock);
    bool found = false;
    for (int i = 0; i < MAX_MOVIE_OBJS; i++)
        if (g_movie_objs[kind][i] == (add ? NULL : p)) {
            g_movie_objs[kind][i] = add ? p : NULL; found = true;
            if (!add) {
                RetiredMovie *r = malloc(sizeof *r);
                if (!r) FATAL("movie retirement allocation failed\n");
                *r = (RetiredMovie){p, kind, g_retired_movies}; g_retired_movies = r;
            }
            break;
        }
    pthread_mutex_unlock(&g_movie_lock);
    if (add && !found) FATAL("movie object registry exhausted\n");
}

static void ex_dtor(Cpu *c) {
    Extractor *e = self_state(c);
    if (e && e->pkt) av_packet_free(&e->pkt);
    if (e && e->fmt) avformat_close_input(&e->fmt);
    if (e) movie_track(0, e, false);
    c->x[0] = 0;
}

// Reads a NUL-terminated string at a guest address without faulting on a bad pointer (NULL if not text).
static const char *peek_str(uint64_t a, char *buf, size_t cap) {
    if (a < 0x7000000000ull || a >= 0x7800000000ull || (a >= HLE_BASE && a < TLS_BASE)) return NULL;
#ifdef _WIN32
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)a, buf, cap - 1, &got) || got == 0) {
        // (the string may end close to the end of a mapping: try a short read)
        if (!ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)a, buf, 16, &got) || got == 0) return NULL;
    }
    buf[got] = 0;
#elif defined(__linux__)
    // Kernel-checked reads also handle invalid or partially mapped guest pointers.
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    ssize_t got = pread(fd, buf, cap - 1, (off_t)a);
    if (got <= 0) got = pread(fd, buf, 16, (off_t)a);
    close(fd);
    if (got <= 0) return NULL;
    buf[got] = 0;
#else
    return NULL;
#endif
    size_t n = strnlen(buf, cap - 1);
    if (n < 4) return NULL;
    for (size_t i = 0; i < n; i++) if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7e) return NULL;
    return buf;
}

// Unknown extractor / decoder slots: log the slot and arguments; an extractor call given a movie path opens it.
static void movie_unknown(Cpu *c, int kind, int slot) {
    static int n;
    char sb[4][256];
    const char *str[4] = {0};
    for (int i = 0; i < 4; i++) str[i] = peek_str(c->x[1 + i], sb[i], sizeof sb[i]);
    if (n++ < 40)
        LOG("[movie] %s slot %d (lr=%llx) x1=%llx%s%s%s x2=%llx%s%s%s x3=%llx x4=%llx\n", kind ? "Decoder" : "Extractor",
            slot, (unsigned long long)c->x[30], (unsigned long long)c->x[1], str[0] ? " \"" : "", str[0] ? str[0] : "",
            str[0] ? "\"" : "", (unsigned long long)c->x[2], str[1] ? " \"" : "", str[1] ? str[1] : "", str[1] ? "\"" : "",
            (unsigned long long)c->x[3], (unsigned long long)c->x[4]);
    if (kind == 0)
        for (int i = 0; i < 2; i++)
            if (str[i] && (strstr(str[i], ".webm") || strstr(str[i], ".mp4") || strstr(str[i], "movie/"))) {
                LOG("[movie] treating Extractor slot %d as SetDataSource(path)\n", slot);
                pthread_mutex_lock(&ex_lock);
                ex_open_path(c, str[i]);
                pthread_mutex_unlock(&ex_lock);
                return;
            }
    c->x[0] = 0;
}
static void unk_0_0(Cpu *c) { movie_unknown(c, 0, 0); }
static void unk_0_1(Cpu *c) { movie_unknown(c, 0, 1); }
static void unk_0_2(Cpu *c) { movie_unknown(c, 0, 2); }
static void unk_0_3(Cpu *c) { movie_unknown(c, 0, 3); }
static void unk_0_4(Cpu *c) { movie_unknown(c, 0, 4); }
static void unk_0_5(Cpu *c) { movie_unknown(c, 0, 5); }
static void unk_0_6(Cpu *c) { movie_unknown(c, 0, 6); }
static void unk_0_7(Cpu *c) { movie_unknown(c, 0, 7); }
static void unk_0_8(Cpu *c) { movie_unknown(c, 0, 8); }
static void unk_0_9(Cpu *c) { movie_unknown(c, 0, 9); }
static void unk_0_10(Cpu *c) { movie_unknown(c, 0, 10); }
static void unk_0_11(Cpu *c) { movie_unknown(c, 0, 11); }
static void unk_0_12(Cpu *c) { movie_unknown(c, 0, 12); }
static void unk_0_13(Cpu *c) { movie_unknown(c, 0, 13); }
static void unk_0_14(Cpu *c) { movie_unknown(c, 0, 14); }
static void unk_0_15(Cpu *c) { movie_unknown(c, 0, 15); }
static void unk_0_16(Cpu *c) { movie_unknown(c, 0, 16); }
static void unk_0_17(Cpu *c) { movie_unknown(c, 0, 17); }
static void unk_0_18(Cpu *c) { movie_unknown(c, 0, 18); }
static void unk_0_19(Cpu *c) { movie_unknown(c, 0, 19); }
static void unk_0_20(Cpu *c) { movie_unknown(c, 0, 20); }
static void unk_0_21(Cpu *c) { movie_unknown(c, 0, 21); }
static void unk_0_22(Cpu *c) { movie_unknown(c, 0, 22); }
static void unk_0_23(Cpu *c) { movie_unknown(c, 0, 23); }
static void unk_0_24(Cpu *c) { movie_unknown(c, 0, 24); }
static void unk_0_25(Cpu *c) { movie_unknown(c, 0, 25); }
static void unk_0_26(Cpu *c) { movie_unknown(c, 0, 26); }
static void unk_0_27(Cpu *c) { movie_unknown(c, 0, 27); }
static void unk_0_28(Cpu *c) { movie_unknown(c, 0, 28); }
static void unk_0_29(Cpu *c) { movie_unknown(c, 0, 29); }
static void unk_0_30(Cpu *c) { movie_unknown(c, 0, 30); }
static void unk_0_31(Cpu *c) { movie_unknown(c, 0, 31); }
static const HostFn g_unk_0[32] = {unk_0_0, unk_0_1, unk_0_2, unk_0_3, unk_0_4, unk_0_5, unk_0_6, unk_0_7, unk_0_8, unk_0_9, unk_0_10, unk_0_11, unk_0_12, unk_0_13, unk_0_14, unk_0_15, unk_0_16, unk_0_17, unk_0_18, unk_0_19, unk_0_20, unk_0_21, unk_0_22, unk_0_23, unk_0_24, unk_0_25, unk_0_26, unk_0_27, unk_0_28, unk_0_29, unk_0_30, unk_0_31};
static void unk_1_0(Cpu *c) { movie_unknown(c, 1, 0); }
static void unk_1_1(Cpu *c) { movie_unknown(c, 1, 1); }
static void unk_1_2(Cpu *c) { movie_unknown(c, 1, 2); }
static void unk_1_3(Cpu *c) { movie_unknown(c, 1, 3); }
static void unk_1_4(Cpu *c) { movie_unknown(c, 1, 4); }
static void unk_1_5(Cpu *c) { movie_unknown(c, 1, 5); }
static void unk_1_6(Cpu *c) { movie_unknown(c, 1, 6); }
static void unk_1_7(Cpu *c) { movie_unknown(c, 1, 7); }
static void unk_1_8(Cpu *c) { movie_unknown(c, 1, 8); }
static void unk_1_9(Cpu *c) { movie_unknown(c, 1, 9); }
static void unk_1_10(Cpu *c) { movie_unknown(c, 1, 10); }
static void unk_1_11(Cpu *c) { movie_unknown(c, 1, 11); }
static void unk_1_12(Cpu *c) { movie_unknown(c, 1, 12); }
static void unk_1_13(Cpu *c) { movie_unknown(c, 1, 13); }
static void unk_1_14(Cpu *c) { movie_unknown(c, 1, 14); }
static void unk_1_15(Cpu *c) { movie_unknown(c, 1, 15); }
static void unk_1_16(Cpu *c) { movie_unknown(c, 1, 16); }
static void unk_1_17(Cpu *c) { movie_unknown(c, 1, 17); }
static void unk_1_18(Cpu *c) { movie_unknown(c, 1, 18); }
static void unk_1_19(Cpu *c) { movie_unknown(c, 1, 19); }
static void unk_1_20(Cpu *c) { movie_unknown(c, 1, 20); }
static void unk_1_21(Cpu *c) { movie_unknown(c, 1, 21); }
static void unk_1_22(Cpu *c) { movie_unknown(c, 1, 22); }
static void unk_1_23(Cpu *c) { movie_unknown(c, 1, 23); }
static void unk_1_24(Cpu *c) { movie_unknown(c, 1, 24); }
static void unk_1_25(Cpu *c) { movie_unknown(c, 1, 25); }
static void unk_1_26(Cpu *c) { movie_unknown(c, 1, 26); }
static void unk_1_27(Cpu *c) { movie_unknown(c, 1, 27); }
static void unk_1_28(Cpu *c) { movie_unknown(c, 1, 28); }
static void unk_1_29(Cpu *c) { movie_unknown(c, 1, 29); }
static void unk_1_30(Cpu *c) { movie_unknown(c, 1, 30); }
static void unk_1_31(Cpu *c) { movie_unknown(c, 1, 31); }
static const HostFn g_unk_1[32] = {unk_1_0, unk_1_1, unk_1_2, unk_1_3, unk_1_4, unk_1_5, unk_1_6, unk_1_7, unk_1_8, unk_1_9, unk_1_10, unk_1_11, unk_1_12, unk_1_13, unk_1_14, unk_1_15, unk_1_16, unk_1_17, unk_1_18, unk_1_19, unk_1_20, unk_1_21, unk_1_22, unk_1_23, unk_1_24, unk_1_25, unk_1_26, unk_1_27, unk_1_28, unk_1_29, unk_1_30, unk_1_31};

// LARECOMP_MOVIE_LOG: every Extractor / Decoder virtual call with its arguments and result
static HostFn g_vt_real[2][32];
static void vt_trace(Cpu *c, int k, int i) {
    static int on = -1, n;
    if (on < 0) on = getenv("LARECOMP_MOVIE_LOG") ? 1 : 0;
    uint64_t a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], lr = c->x[30];
    g_vt_real[k][i](c);
    if (on && n++ < 400)
        LOG("[movie] %s.%d(%llx, %llx, %llx) -> %llx (lr=%llx)\n", k ? "Decoder" : "Extractor", i, (unsigned long long)a1,
            (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)c->x[0], (unsigned long long)lr);
}
static void tr_0_0(Cpu *c) { vt_trace(c, 0, 0); }
static void tr_0_1(Cpu *c) { vt_trace(c, 0, 1); }
static void tr_0_2(Cpu *c) { vt_trace(c, 0, 2); }
static void tr_0_3(Cpu *c) { vt_trace(c, 0, 3); }
static void tr_0_4(Cpu *c) { vt_trace(c, 0, 4); }
static void tr_0_5(Cpu *c) { vt_trace(c, 0, 5); }
static void tr_0_6(Cpu *c) { vt_trace(c, 0, 6); }
static void tr_0_7(Cpu *c) { vt_trace(c, 0, 7); }
static void tr_0_8(Cpu *c) { vt_trace(c, 0, 8); }
static void tr_0_9(Cpu *c) { vt_trace(c, 0, 9); }
static void tr_0_10(Cpu *c) { vt_trace(c, 0, 10); }
static void tr_0_11(Cpu *c) { vt_trace(c, 0, 11); }
static void tr_0_12(Cpu *c) { vt_trace(c, 0, 12); }
static void tr_0_13(Cpu *c) { vt_trace(c, 0, 13); }
static void tr_0_14(Cpu *c) { vt_trace(c, 0, 14); }
static void tr_0_15(Cpu *c) { vt_trace(c, 0, 15); }
static void tr_0_16(Cpu *c) { vt_trace(c, 0, 16); }
static void tr_0_17(Cpu *c) { vt_trace(c, 0, 17); }
static void tr_0_18(Cpu *c) { vt_trace(c, 0, 18); }
static void tr_0_19(Cpu *c) { vt_trace(c, 0, 19); }
static void tr_0_20(Cpu *c) { vt_trace(c, 0, 20); }
static void tr_0_21(Cpu *c) { vt_trace(c, 0, 21); }
static void tr_0_22(Cpu *c) { vt_trace(c, 0, 22); }
static void tr_0_23(Cpu *c) { vt_trace(c, 0, 23); }
static void tr_0_24(Cpu *c) { vt_trace(c, 0, 24); }
static void tr_0_25(Cpu *c) { vt_trace(c, 0, 25); }
static void tr_0_26(Cpu *c) { vt_trace(c, 0, 26); }
static void tr_0_27(Cpu *c) { vt_trace(c, 0, 27); }
static void tr_0_28(Cpu *c) { vt_trace(c, 0, 28); }
static void tr_0_29(Cpu *c) { vt_trace(c, 0, 29); }
static void tr_0_30(Cpu *c) { vt_trace(c, 0, 30); }
static void tr_0_31(Cpu *c) { vt_trace(c, 0, 31); }
static const HostFn g_tr_0[32] = {tr_0_0, tr_0_1, tr_0_2, tr_0_3, tr_0_4, tr_0_5, tr_0_6, tr_0_7, tr_0_8, tr_0_9, tr_0_10, tr_0_11, tr_0_12, tr_0_13, tr_0_14, tr_0_15, tr_0_16, tr_0_17, tr_0_18, tr_0_19, tr_0_20, tr_0_21, tr_0_22, tr_0_23, tr_0_24, tr_0_25, tr_0_26, tr_0_27, tr_0_28, tr_0_29, tr_0_30, tr_0_31};
static void tr_1_0(Cpu *c) { vt_trace(c, 1, 0); }
static void tr_1_1(Cpu *c) { vt_trace(c, 1, 1); }
static void tr_1_2(Cpu *c) { vt_trace(c, 1, 2); }
static void tr_1_3(Cpu *c) { vt_trace(c, 1, 3); }
static void tr_1_4(Cpu *c) { vt_trace(c, 1, 4); }
static void tr_1_5(Cpu *c) { vt_trace(c, 1, 5); }
static void tr_1_6(Cpu *c) { vt_trace(c, 1, 6); }
static void tr_1_7(Cpu *c) { vt_trace(c, 1, 7); }
static void tr_1_8(Cpu *c) { vt_trace(c, 1, 8); }
static void tr_1_9(Cpu *c) { vt_trace(c, 1, 9); }
static void tr_1_10(Cpu *c) { vt_trace(c, 1, 10); }
static void tr_1_11(Cpu *c) { vt_trace(c, 1, 11); }
static void tr_1_12(Cpu *c) { vt_trace(c, 1, 12); }
static void tr_1_13(Cpu *c) { vt_trace(c, 1, 13); }
static void tr_1_14(Cpu *c) { vt_trace(c, 1, 14); }
static void tr_1_15(Cpu *c) { vt_trace(c, 1, 15); }
static void tr_1_16(Cpu *c) { vt_trace(c, 1, 16); }
static void tr_1_17(Cpu *c) { vt_trace(c, 1, 17); }
static void tr_1_18(Cpu *c) { vt_trace(c, 1, 18); }
static void tr_1_19(Cpu *c) { vt_trace(c, 1, 19); }
static void tr_1_20(Cpu *c) { vt_trace(c, 1, 20); }
static void tr_1_21(Cpu *c) { vt_trace(c, 1, 21); }
static void tr_1_22(Cpu *c) { vt_trace(c, 1, 22); }
static void tr_1_23(Cpu *c) { vt_trace(c, 1, 23); }
static void tr_1_24(Cpu *c) { vt_trace(c, 1, 24); }
static void tr_1_25(Cpu *c) { vt_trace(c, 1, 25); }
static void tr_1_26(Cpu *c) { vt_trace(c, 1, 26); }
static void tr_1_27(Cpu *c) { vt_trace(c, 1, 27); }
static void tr_1_28(Cpu *c) { vt_trace(c, 1, 28); }
static void tr_1_29(Cpu *c) { vt_trace(c, 1, 29); }
static void tr_1_30(Cpu *c) { vt_trace(c, 1, 30); }
static void tr_1_31(Cpu *c) { vt_trace(c, 1, 31); }
static const HostFn g_tr_1[32] = {tr_1_0, tr_1_1, tr_1_2, tr_1_3, tr_1_4, tr_1_5, tr_1_6, tr_1_7, tr_1_8, tr_1_9, tr_1_10, tr_1_11, tr_1_12, tr_1_13, tr_1_14, tr_1_15, tr_1_16, tr_1_17, tr_1_18, tr_1_19, tr_1_20, tr_1_21, tr_1_22, tr_1_23, tr_1_24, tr_1_25, tr_1_26, tr_1_27, tr_1_28, tr_1_29, tr_1_30, tr_1_31};

#define EX_LOCKED(fn) \
    static void fn##_l(Cpu *c) { pthread_mutex_lock(&ex_lock); fn(c); pthread_mutex_unlock(&ex_lock); }
EX_LOCKED(ex_dtor)
EX_LOCKED(ex_get_track_count)
EX_LOCKED(ex_get_track_config)
EX_LOCKED(ex_select_track)
EX_LOCKED(ex_seek)
EX_LOCKED(ex_get_sample_track)
EX_LOCKED(ex_read)
EX_LOCKED(ex_advance)
EX_LOCKED(ex_get_sample_time)
EX_LOCKED(ex_cached_duration)
EX_LOCKED(ex_set_data_source)

HLE_FUNC("_ZN5movie9ExtractorC1ENS_13ContainerTypeE", movie_extractor_ctor) {
    uint64_t vt = g_ex_vt;
    if (!vt) {
        HostFn f[32] = {ex_dtor_l, ex_dtor_l, NULL, NULL, ex_get_track_count_l,
                        ex_get_track_config_l, ex_select_track_l, NULL, ex_seek_l, ex_get_sample_track_l,
                        ex_read_l, ex_advance_l, ex_get_sample_time_l, NULL, ex_cached_duration_l,
                        ex_set_data_source_l};
        for (int i = 0; i < 32; i++) if (!f[i]) f[i] = g_unk_0[i];
        for (int i = 0; i < 32; i++) g_vt_real[0][i] = f[i], f[i] = g_tr_0[i];
        vt = g_ex_vt = make_vtable(f, 32, "Extractor");
    }
    uint64_t *self = (uint64_t *)(uintptr_t)c->x[0];
    self[0] = vt;
    Extractor *e = calloc(1, sizeof(Extractor));
    self[1] = (uint64_t)(uintptr_t)e;
    movie_track(0, e, true);
}

// ------------------------------------------------------------------ Decoder
#define NIN 4
#define NOUT 6
#define NPENDING 16

typedef struct {
    int idx;
    int64_t time;
    uint32_t flags;
} OutReady;

typedef struct {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    bool video;
    uint64_t events[4];  // input available, output available, format changed, error
    uint8_t *in_mem[NIN];
    uint32_t in_cap;
    bool in_busy[NIN];
    uint8_t *out_mem[NOUT];
    uint32_t out_cap, out_size[NOUT];
    bool out_busy[NOUT];
    OutReady ready[NOUT];
    int nready;
    int width, height, stride, sample_rate, channels;
    struct SwsContext *sws;
    uint8_t *rgba;
    AVFrame *pending[NPENDING];
    int64_t pending_time[NPENDING];
    int npending;
    bool eos_pending;
    int64_t eos_time;
    bool stopped;  // Stop / destroyed: no more events (see dec_stop)
    pthread_mutex_t lock;
} Decoder;

// Input / output buffers live in the HLE heap, which only a reboot frees: buffers of destroyed decoders are kept
// for the next movie (the title's attract loop replays the opening, ~37 MB of buffers per showing).
static struct { uint8_t *mem; uint32_t size; } g_buf_pool[64];
static int g_nbuf_pool;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *buf_get(uint32_t size) {
    pthread_mutex_lock(&g_pool_lock);
    for (int i = 0; i < g_nbuf_pool; i++)
        if (g_buf_pool[i].size == size) {
            uint8_t *m = g_buf_pool[i].mem;
            g_buf_pool[i] = g_buf_pool[--g_nbuf_pool];
            pthread_mutex_unlock(&g_pool_lock);
            return m;
        }
    pthread_mutex_unlock(&g_pool_lock);
    return hle_alloc(size);
}
static void buf_put(uint8_t *m, uint32_t size) {
    if (!m) return;
    pthread_mutex_lock(&g_pool_lock);
    if (g_nbuf_pool < 64) g_buf_pool[g_nbuf_pool++].mem = m, g_buf_pool[g_nbuf_pool - 1].size = size;
    pthread_mutex_unlock(&g_pool_lock);
}

// Signal one of the game's decoder events unless the decoder was stopped. Under the lock so a Stop on another
// thread can't slip between the check and the signal.
void rt_unwind_mutex_enter(pthread_mutex_t *m);
void rt_unwind_mutex_leave(pthread_mutex_t *m);
static void dec_signal(Cpu *c, Decoder *d, int i) {
    pthread_mutex_lock(&d->lock);
    rt_unwind_mutex_enter(&d->lock);
    if (!d->stopped) signal_guest_event(c, d->events[i]);
    rt_unwind_mutex_leave(&d->lock);
    pthread_mutex_unlock(&d->lock);
}

typedef struct NvnTexture NvnTexture;
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride);

static void dec_configure(Cpu *c) {
    Decoder *d = self_state(c);
    MediaData *m = md(c->x[1]);
    AVCodecParameters *p = m->codecpar;
    if (!p) { LOG("[movie] Configure without codec parameters\n"); c->x[0] = 1; return; }
    const AVCodec *codec = avcodec_find_decoder(p->codec_id);
    d->ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d->ctx, p);
    d->ctx->thread_count = 4;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) { LOG("[movie] cannot open decoder\n"); c->x[0] = 1; return; }
    d->frame = av_frame_alloc();
    d->pkt = av_packet_alloc();
    d->video = p->codec_type == AVMEDIA_TYPE_VIDEO;
    d->width = p->width;
    d->height = p->height;
    d->stride = (p->width + 255) & ~255;
    d->sample_rate = p->sample_rate;
    d->channels = p->ch_layout.nb_channels;
    d->in_cap = d->video ? 4 << 20 : 256 << 10;
    d->out_cap = d->video ? (uint32_t)(d->stride * ((d->height + 15) & ~15) * 3 / 2) : 64 << 10;
    for (int i = 0; i < NIN; i++) d->in_mem[i] = buf_get(d->in_cap);
    for (int i = 0; i < NOUT; i++) d->out_mem[i] = buf_get(d->out_cap);
    LOG("[movie] decoder %s configured: %dx%d %d Hz %d ch\n", codec->name, d->width, d->height, d->sample_rate, d->channels);
    c->x[0] = 0;
}

static void dec_register_events(Cpu *c) {
    Decoder *d = self_state(c);
    memcpy(d->events, (void *)(uintptr_t)c->x[1], sizeof d->events);
    LOG("[movie] decoder events: input %llx output %llx format %llx error %llx\n", (unsigned long long)d->events[0],
        (unsigned long long)d->events[1], (unsigned long long)d->events[2], (unsigned long long)d->events[3]);
    c->x[0] = 0;
}

static void dec_output_format(Decoder *d, MediaData *m) {
    md_clear(m);
    if (d->video) {
        md_set_str(m, "mime", "video/raw");
        md_set_int(m, "width", d->width);
        md_set_int(m, "height", d->height);
        md_set_int(m, "nv12-y-stride", d->stride);
        md_set_int(m, "nv12-y-offset", 0);
        md_set_int(m, "nv12-uv-offset", d->stride * ((d->height + 15) & ~15));
        md_set_int(m, "nv12-colorspace", 0);
        md_set_int(m, "stride", d->stride);
        md_set_int(m, "slice-height", (d->height + 15) & ~15);
        md_set_rect(m, "crop", 0, 0, d->width - 1, d->height - 1);
    } else {
        md_set_str(m, "mime", "audio/raw");
        md_set_int(m, "sample-rate", d->sample_rate);
        md_set_int(m, "channel-count", d->channels);
    }
}

static void dec_get_output_format(Cpu *c) { dec_output_format(self_state(c), md(c->x[1])); c->x[0] = 0; }
static void dec_get_output_buffer_format(Cpu *c) { dec_output_format(self_state(c), md(c->x[2])); c->x[0] = 0; }

static void dec_start(Cpu *c) {
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    d->stopped = false;
    pthread_mutex_unlock(&d->lock);
    // a hardware decoder reports its output format before the first frame: FE3H creates its video textures
    // (sized from GetOutputFormat's "crop") only on this event
    if (d->video) dec_signal(c, d, 2);
    dec_signal(c, d, 0);  // all input buffers are free
    c->x[0] = 0;
}

// slot 6: Stop. The game's player (0x7000912640) stops both decoders, stops its renderer threads, deletes the
// decoders and nulls its pointers to them, and only then tells its decoder thread (0x70009114b0) to exit: an event
// of ours still set by then wakes that thread into a virtual call through the null decoder (signal 11 after the
// title's attract movie, when the thread was slow to run). A stopped hardware decoder signals nothing, so stop
// signalling here and clear what is still set.
static void dec_stop(Cpu *c) {
    Decoder *d = self_state(c);
    static uint64_t clear_event;
    if (!clear_event) clear_event = guest_sym("_ZN2nn2os10ClearEventEPNS0_9EventTypeE");
    pthread_mutex_lock(&d->lock);
    rt_unwind_mutex_enter(&d->lock);
    if (!d->stopped) {
        d->stopped = true;
        LOG("[movie] decoder %s stopped\n", d->video ? "video" : "audio");
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        for (int i = 0; i < 4; i++)
            if (d->events[i]) { c->x[0] = d->events[i]; guest_call(c, clear_event); }
        memcpy(c->x, saved, sizeof saved);
    }
    rt_unwind_mutex_leave(&d->lock);
    pthread_mutex_unlock(&d->lock);
    c->x[0] = 0;
}

static uint64_t n_acq_in, n_send, n_acq_out, n_get_out, n_rel_out, n_frames;
static void movie_stats(void) {
    static uint64_t last;
    uint64_t now = n_acq_in + n_send + n_acq_out + n_rel_out;
    if (now / 200 != last / 200)
        LOG("[movie] acq_in %llu send %llu frames %llu acq_out %llu get_out %llu rel_out %llu\n", n_acq_in, n_send,
            n_frames, n_acq_out, n_get_out, n_rel_out);
    last = now;
}

static void dec_acquire_input(Cpu *c) {
    Decoder *d = self_state(c);
    n_acq_in++;
    movie_stats();
    pthread_mutex_lock(&d->lock);
    int out_free = 0;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) out_free++;
    if (out_free < 2 || d->npending) { pthread_mutex_unlock(&d->lock); c->x[0] = 1; return; }  // backpressure
    int idx = -1, free_n = 0;
    for (int i = 0; i < NIN; i++)
        if (!d->in_busy[i]) { if (idx < 0) idx = i; else free_n++; }
    if (idx >= 0) d->in_busy[idx] = true;
    pthread_mutex_unlock(&d->lock);
    if (idx < 0) { c->x[0] = 1; return; }
    *(int32_t *)(uintptr_t)c->x[1] = idx;
    if (c->x[2]) *(int32_t *)(uintptr_t)c->x[2] = free_n;
    c->x[0] = 0;
}

static void dec_get_input_buffer(Cpu *c) {
    Decoder *d = self_state(c);
    int i = (int)c->x[1];
    MBuffer *b = BUF(c->x[2]);
    if (i < 0 || i >= NIN) { c->x[0] = 1; return; }
    b->offset = b->size = 0;  // (the user data fields stay the game's)
    b->base = (uint64_t)(uintptr_t)d->in_mem[i];
    b->capacity = d->in_cap;
    c->x[0] = 0;
}

// Copy a decoded frame into an output slot; caller holds the lock.
static bool emit_frame(Decoder *d, AVFrame *f, int64_t time, uint32_t flags) {
    int slot = -1;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) { slot = i; break; }
    if (slot < 0) return false;
    uint8_t *o = d->out_mem[slot];
    uint32_t size = 0;
    if (flags & MOVIE_EOS) {
        size = 0;
    } else if (d->video) {
        int h = d->height, w = d->width, uv_off = d->stride * ((h + 15) & ~15);
        for (int y = 0; y < h; y++) memcpy(o + y * d->stride, f->data[0] + y * f->linesize[0], w);
        for (int y = 0; y < h / 2; y++) {  // interleave U and V into NV12
            uint8_t *uv = o + uv_off + y * d->stride;
            const uint8_t *u = f->data[1] + y * f->linesize[1], *v = f->data[2] + y * f->linesize[2];
            for (int x = 0; x < w / 2; x++) { uv[2 * x] = u[x]; uv[2 * x + 1] = v[x]; }
        }
        size = (uint32_t)(uv_off + d->stride * (h / 2));
    } else {
        // planar float -> interleaved s16
        int ch = f->ch_layout.nb_channels, n = f->nb_samples;
        int16_t *out = (int16_t *)o;
        if ((uint32_t)(n * ch * 2) > d->out_cap) n = (int)(d->out_cap / (ch * 2));
        // FFmpeg reorders Vorbis 5.1 to FL FR FC LFE BL BR; the game expects Vorbis order (FL FC FR BL BR LFE)
        // and reorders itself.
        static const int vorbis51[6] = {0, 2, 1, 4, 5, 3};
        bool native = ch == 6 && d->ctx->codec_id == AV_CODEC_ID_VORBIS;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++) {
                int src = native ? vorbis51[k] : k;
                float s = f->format == AV_SAMPLE_FMT_FLTP ? ((const float *)f->data[src])[i] : 0.0f;
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                out[i * ch + k] = (int16_t)(s * 32767.0f);
            }
        size = (uint32_t)(n * ch * 2);
    }
    d->out_busy[slot] = true;
    d->out_size[slot] = size;
    d->ready[d->nready++] = (OutReady){slot, time, flags};
    return true;
}

// Emit queued frames (and a pending end-of-stream marker) into free output slots.
static bool flush_pending(Decoder *d) {
    bool any = false;
    while (d->npending && emit_frame(d, d->pending[0], d->pending_time[0], 0)) {
        av_frame_free(&d->pending[0]);
        memmove(d->pending, d->pending + 1, (d->npending - 1) * sizeof d->pending[0]);
        memmove(d->pending_time, d->pending_time + 1, (d->npending - 1) * sizeof d->pending_time[0]);
        d->npending--;
        any = true;
    }
    if (!d->npending && d->eos_pending && emit_frame(d, NULL, d->eos_time, MOVIE_EOS)) {
        d->eos_pending = false;
        any = true;
    }
    return any;
}

static void dec_send_input(Cpu *c) {
    Decoder *d = self_state(c);
    n_send++;
    int idx = (int)c->x[1], off = (int)c->x[2], size = (int)c->x[3];
    int64_t time = (int64_t)c->x[4];
    uint32_t flags = (uint32_t)c->x[5];
    pthread_mutex_lock(&d->lock);
    if (size > 0 && idx >= 0 && idx < NIN) {
        av_packet_unref(d->pkt);
        d->pkt->data = d->in_mem[idx] + off;
        d->pkt->size = size;
        d->pkt->pts = time;
        avcodec_send_packet(d->ctx, d->pkt);
    }
    if (flags & MOVIE_EOS) avcodec_send_packet(d->ctx, NULL);
    bool produced = false;
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        int64_t t = d->frame->pts != AV_NOPTS_VALUE ? d->frame->pts : time;
        // keep order: queue behind anything already pending
        if (d->npending || !emit_frame(d, d->frame, t, 0)) {
            if (d->npending < NPENDING) {
                d->pending[d->npending] = av_frame_clone(d->frame);
                d->pending_time[d->npending++] = t;
            } else {
                LOG("[movie] output queue full, dropping frame\n");
            }
        }
        n_frames++;
        produced = true;
    }
    if (flags & MOVIE_EOS) { d->eos_pending = true; d->eos_time = time; }
    produced |= flush_pending(d);
    if (idx >= 0 && idx < NIN) d->in_busy[idx] = false;
    pthread_mutex_unlock(&d->lock);
    dec_signal(c, d, 0);
    if (produced) dec_signal(c, d, 1);
    c->x[0] = 0;
}

static void dec_acquire_output(Cpu *c) {
    Decoder *d = self_state(c);
    n_acq_out++;
    pthread_mutex_lock(&d->lock);
    if (!d->nready) { pthread_mutex_unlock(&d->lock); c->x[0] = 1; return; }
    OutReady r = d->ready[0];
    memmove(d->ready, d->ready + 1, --d->nready * sizeof *d->ready);
    int remaining = d->nready;
    pthread_mutex_unlock(&d->lock);
    *(int32_t *)(uintptr_t)c->x[1] = r.idx;
    if (c->x[2]) *(int64_t *)(uintptr_t)c->x[2] = r.time;
    if (c->x[3]) *(uint32_t *)(uintptr_t)c->x[3] = r.flags;
    if (c->x[4]) *(int32_t *)(uintptr_t)c->x[4] = remaining;
    c->x[0] = 0;
}

static void dec_get_output_buffer(Cpu *c) {
    Decoder *d = self_state(c);
    n_get_out++;
    int i = (int)c->x[1];
    MBuffer *b = BUF(c->x[2]);
    if (i < 0 || i >= NOUT) { c->x[0] = 1; return; }
    {
        static int n;
        if (n++ < 6)
            LOG("[movie] GetOutputBuffer(%d) %s in: base %llx (-> %llx) capacity %llx offset %d size %d i64 %llx u32 %x i32 %d\n",
                i, d->video ? "video" : "audio", (unsigned long long)b->base,
                b->base ? (unsigned long long)*(uint64_t *)(uintptr_t)b->base : 0ull, (unsigned long long)b->capacity,
                b->offset, b->size, (unsigned long long)b->i64, b->u32, b->i32);
    }
    if (d->video && b->base && b->capacity == 8) {
        // texture output mode: the buffer points at the game's NVNtexture; write RGBA into it
        NvnTexture *t = *(NvnTexture **)(uintptr_t)b->base;
        int w = d->width, h = d->height;
        if (!d->sws)
            d->sws = sws_getContext(w, h, AV_PIX_FMT_NV12, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        if (!d->rgba) d->rgba = malloc((size_t)w * h * 4);
        const uint8_t *src[2] = {d->out_mem[i], d->out_mem[i] + d->stride * ((h + 15) & ~15)};
        int src_stride[2] = {d->stride, d->stride};
        uint8_t *dst[1] = {d->rgba};
        int dst_stride[1] = {w * 4};
        sws_scale(d->sws, src, src_stride, 0, h, dst, dst_stride);
        renderer_upload_rgba(t, d->rgba, (uint32_t)w, (uint32_t)h, (uint32_t)w * 4);
        c->x[0] = 0;
        return;
    }
    if (b->base && b->capacity > 8) {
        // the game's own memory (FE3H): copy the decoded frame into it and keep the game's pointer
        uint32_t n = d->out_size[i];
        if (n > b->capacity) {
            static int warned;
            if (!warned++) LOG("[movie] output %u bytes > the game's buffer %llu\n", n, (unsigned long long)b->capacity);
            n = (uint32_t)b->capacity;
        }
        memcpy((void *)(uintptr_t)b->base, d->out_mem[i], n);
        b->offset = 0;
        b->size = (int32_t)n;
        c->x[0] = 0;
        return;
    }
    b->offset = 0;  // (the user data fields i64 / u32 / i32 stay the game's)
    b->base = (uint64_t)(uintptr_t)d->out_mem[i];
    b->capacity = d->out_cap;
    b->size = (int32_t)d->out_size[i];
    c->x[0] = 0;
}

static void dec_release_output(Cpu *c) {
    Decoder *d = self_state(c);
    n_rel_out++;
    dec_signal(c, d, 0);  // input may be accepted again
    int i = (int)c->x[1];
    pthread_mutex_lock(&d->lock);
    if (i >= 0 && i < NOUT) d->out_busy[i] = false;
    bool produced = flush_pending(d);
    pthread_mutex_unlock(&d->lock);
    if (produced) dec_signal(c, d, 1);
    c->x[0] = 0;
}

// Both destruction and a reboot release the host allocations (a reboot used to free only the codec: frames, packet,
// scaler and RGBA buffer leaked with each movie open at the time; upstream 7a6eb6e). The HLE input / output buffers
// are separate: destruction returns them to the pool; a reboot discards the whole HLE heap after guest threads stop.
static void dec_free(Decoder *d) {
    if (d->ctx) avcodec_free_context(&d->ctx);
    for (int i = 0; i < d->npending; i++) av_frame_free(&d->pending[i]);
    d->npending = d->nready = 0;
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    if (d->sws) sws_freeContext(d->sws), d->sws = NULL;
    free(d->rgba), d->rgba = NULL;
}

static void dec_dtor(Cpu *c) {
    Decoder *d = self_state(c);
    if (!d) { c->x[0] = 0; return; }
    dec_stop(c);
    pthread_mutex_lock(&d->lock);
    dec_free(d);
    for (int i = 0; i < NIN; i++) buf_put(d->in_mem[i], d->in_cap), d->in_mem[i] = NULL;
    for (int i = 0; i < NOUT; i++) buf_put(d->out_mem[i], d->out_cap), d->out_mem[i] = NULL;
    pthread_mutex_unlock(&d->lock);
    movie_track(1, d, false);  // (d itself stays: the game's object may still point at it)
    c->x[0] = 0;
}

// Reboot: close what a movie left open (decoder threads, the file), new vtables in the next boot's HLE heap.
RT_ON_RESET(movie_reset) {
    unsigned freed_ex = 0, freed_dec = 0;
    for (int i = 0; i < MAX_MOVIE_OBJS; i++) {
        Extractor *e = g_movie_objs[0][i];
        if (e && e->pkt) av_packet_free(&e->pkt);
        if (e && e->fmt) avformat_close_input(&e->fmt);
        if (e) { free(e); freed_ex++; }
        Decoder *d = g_movie_objs[1][i];
        if (d) { dec_free(d); pthread_mutex_destroy(&d->lock); free(d); freed_dec++; }
        g_movie_objs[0][i] = g_movie_objs[1][i] = NULL;
    }
    while (g_retired_movies) {
        RetiredMovie *r = g_retired_movies; g_retired_movies = r->next;
        if (r->kind == 1) {
            Decoder *d = r->p; dec_free(d); pthread_mutex_destroy(&d->lock); freed_dec++;
        } else freed_ex++;
        free(r->p); free(r);
    }
    LOG("[lifetime] movie reset freed %u extractors, %u decoders; retired objects 0\n", freed_ex, freed_dec);
    movie_owned_reset(&g_buffer_owners); // guest threads stopped, no further Buffer access
    unsigned metadata=0;while (g_media_data) { md_drop(g_media_data);metadata++; }
    LOG("[lifetime] movie reset freed %u remaining metadata objects\n",metadata);
    LOG("[lifetime] movie Buffers created %zu freed %zu live bytes %zu\n",
        g_buffer_owners.created,g_buffer_owners.freed,g_buffer_owners.bytes);
    g_ex_vt = g_dec_vt = 0;
    g_nbuf_pool = 0;  // (the HLE heap starts over)
    g_skip_held = false, g_skip_pressed_at = 0;
}

static void dec_get_counts(Cpu *c) {  // slot 14: (int*, int64*) queried after Configure
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = 0;
    if (c->x[2]) *(int64_t *)(uintptr_t)c->x[2] = 0;
    c->x[0] = 0;
}

HLE_FUNC("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", movie_decoder_ctor) {
    uint64_t vt = g_dec_vt;
    if (!vt) {
        HostFn f[32] = {dec_dtor, dec_dtor, dec_configure, dec_register_events, dec_get_output_format,
                        dec_start, dec_stop, ok, ok, dec_acquire_input, dec_get_input_buffer, dec_send_input,
                        dec_acquire_output, dec_get_output_buffer, dec_get_counts, dec_release_output, ok,
                        NULL, dec_get_output_buffer_format};
        for (int i = 0; i < 32; i++) if (!f[i]) f[i] = g_unk_1[i];
        for (int i = 0; i < 32; i++) g_vt_real[1][i] = f[i], f[i] = g_tr_1[i];
        vt = g_dec_vt = make_vtable(f, 32, "Decoder");
    }
    uint64_t *self = (uint64_t *)(uintptr_t)c->x[0];
    Decoder *d = calloc(1, sizeof *d);
    movie_track(1, d, true);
    pthread_mutex_init(&d->lock, NULL);
    self[0] = vt;
    self[1] = (uint64_t)(uintptr_t)d;
}
