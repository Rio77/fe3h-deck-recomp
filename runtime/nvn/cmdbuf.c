// NVN command buffers: host CmdRec storage isolates async Vulkan execution from guest
// command/control pool reuse while preserving guest memory callback budgets.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"

#define JUMP_RESERVE (sizeof(CmdRec) + 8)
#define CONTROL_PER_RECORDING 64

typedef struct CommandCopy {
    void *data;
    uint64_t size, mem_lo, mem_hi, ctrl_lo, ctrl_hi;
    unsigned readers;
    bool retired;
    struct CommandCopy *next;
} CommandCopy;
static CommandCopy *command_copies;
static pthread_mutex_t command_copies_lock = PTHREAD_MUTEX_INITIALIZER;

static void copies_collect(void) {
    CommandCopy **p = &command_copies;
    while (*p) {
        CommandCopy *entry = *p;
        if (!entry->retired || entry->readers) { p = &entry->next; continue; }
        *p = entry->next; free(entry->data); free(entry);
    }
}
static void copies_reuse(uint64_t lo, uint64_t size, bool is_ctrl) {
    if (!size || lo + size < lo) return;
    uint64_t hi = lo + size;
    pthread_mutex_lock(&command_copies_lock);
    for (CommandCopy *p = command_copies; p; p = p->next) {
        if (is_ctrl) {
            if (p->ctrl_hi && p->ctrl_lo < hi && lo < p->ctrl_hi) p->retired = true;
        } else {
            if (p->mem_hi && p->mem_lo < hi && lo < p->mem_hi) p->retired = true;
        }
    }
    copies_collect();
    pthread_mutex_unlock(&command_copies_lock);
}
const void *nvn_command_acquire(uint64_t handle, uint64_t *size) {
    const void *data = NULL;
    pthread_mutex_lock(&command_copies_lock);
    for (CommandCopy *p = command_copies; p; p = p->next)
        if ((uintptr_t)p->data == handle && !p->retired) {
            p->readers++; data = p->data; if (size) *size = p->size; break;
        }
    pthread_mutex_unlock(&command_copies_lock);
    return data;
}
void nvn_command_release(uint64_t handle) {
    pthread_mutex_lock(&command_copies_lock);
    for (CommandCopy *p = command_copies; p; p = p->next)
        if ((uintptr_t)p->data == handle) { if (p->readers) p->readers--; break; }
    copies_collect();
    pthread_mutex_unlock(&command_copies_lock);
}
RT_ON_RESET(command_copies_reset) {
    extern void vk_async_drain(void) __attribute__((weak));
    if (vk_async_drain) vk_async_drain();
    pthread_mutex_lock(&command_copies_lock);
    for (CommandCopy *p = command_copies; p; p = p->next) p->retired = true;
    copies_collect();
    pthread_mutex_unlock(&command_copies_lock);
}

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = nvn_host_calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) {
    NvnCmdBuf *b = cb_of(A(0));
    if (b) { free(b->records); nvn_host_free(b); }
    SET_HOST(A(0), 0);
}
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

NVN(nvnCommandBufferAddCommandMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t mem = HOST(NvnPool, A(1))->storage + A(2), size = A(3);
    copies_reuse(mem, size, false);
    cb->mem = mem;
    cb->size = size;
    cb->used = 0;
}
NVN(nvnCommandBufferAddControlMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    copies_reuse(A(1), A(2), true);
    cb->control = A(1);
    cb->control_size = A(2);
    cb->control_used = 0;
}
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->used); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->size > cb->used + JUMP_RESERVE ? cb->size - cb->used - JUMP_RESERVE : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->control_size ? cb->control_size : 0x10000);
}
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(cb_of(A(0))->control_used); }
NVN(nvnCommandBufferGetControlMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->control_size ? (cb->control_size > cb->control_used ? cb->control_size - cb->control_used : 0) : 0x10000);
}
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; }

// Reserve a record. May call back into the guest for more memory, so callers snapshot their
// arguments first.
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    uint32_t size = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    for (int tries = 0; !cb->mem || cb->used + size + JUMP_RESERVE > cb->size; tries++) {
        if (!cb->callback || tries == 2) FATAL("nvn: out of command memory (need %u)\n", size);
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->x[2] = size + JUMP_RESERVE;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    uint64_t need = cb->records_used + size;
    if (need > cb->records_cap) {
        uint64_t cap = cb->records_cap ? cb->records_cap : 4096;
        while (cap < need) cap *= 2;
        void *records = realloc(cb->records, cap);
        if (!records) FATAL("nvn: host command recording allocation failed\n");
        cb->records = records;
        cb->records_cap = cap;
    }
    CmdRec *r = raw_rec((uintptr_t)cb->records + cb->records_used, op, size);
    cb->records_used += size;
    cb->used += size;
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    cb->records_used = 0;
    cb->rec_mem = cb->mem + cb->used;
    rec(c, cb, CMD_BARRIER, 0);  // makes sure memory exists; the list starts here
    cb->start = cb->mem + cb->used - sizeof(CmdRec);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    uint64_t ctrl_lo = 0, ctrl_hi = 0;
    if (cb->control_size) {
        for (int tries = 0; cb->control_size - cb->control_used < CONTROL_PER_RECORDING; tries++) {
            if (!cb->callback || tries == 2) break;
            uint64_t saved[8];
            memcpy(saved, c->x, sizeof saved);
            c->x[0] = cb->guest;
            c->x[1] = 1;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY
            c->x[2] = 4096;
            c->x[3] = cb->callback_data;
            guest_call(c, cb->callback);
            memcpy(c->x, saved, sizeof saved);
        }
        if (cb->control_size - cb->control_used >= CONTROL_PER_RECORDING) {
            ctrl_lo = cb->control + cb->control_used;
            ctrl_hi = ctrl_lo + CONTROL_PER_RECORDING;
            cb->control_used += CONTROL_PER_RECORDING;
        }
    }
    CommandCopy *copy = calloc(1, sizeof *copy);
    if (!copy) FATAL("nvn: command handle allocation failed\n");
    copy->data = malloc(cb->records_used);
    copy->size = cb->records_used;
    if (!copy->data) FATAL("nvn: command copy allocation failed\n");
    memcpy(copy->data, cb->records, cb->records_used);
    copy->mem_lo = cb->rec_mem;
    copy->mem_hi = cb->mem + cb->used;
    if (copy->mem_hi < copy->mem_lo) copy->mem_hi = copy->mem_lo;
    copy->ctrl_lo = ctrl_lo;
    copy->ctrl_hi = ctrl_hi;
    pthread_mutex_lock(&command_copies_lock);
    copy->next = command_copies;
    command_copies = copy;
    pthread_mutex_unlock(&command_copies_lock);
    cb->recording = false;
    RET((uintptr_t)copy->data);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    // (cmd, program, stages): binds the program to those stages only (exec.c bind_program). A NULL program unbinds
    // them (FE3H unbinds tessellation, stages 0x8, before its draws).
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = s.x[1] ? (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]) : 0;  // host program
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffer) { (void)c; }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? (uint64_t)(uintptr_t)HOST(NvnTexture, colors[i]) : 0;
        if (views && views[i]) { rt.has_view[i] = 1; memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView)); }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; }
NVN(nvnCommandBufferSetDepthClamp) { (void)c; }
NVN(nvnCommandBufferSetRasterizerDiscard) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN(nvnCommandBufferSetPatchSize) { (void)c; }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; }
NVN(nvnCommandBufferSaveZCullData) { (void)c; }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("LARECOMP_TRACE_COUNT") ? atol(getenv("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); Snap s = snap(c); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { (void)c; }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { (void)c; }
NVN(nvnCommandBufferDrawTransformFeedback) { (void)c; }
NVN(nvnCommandBufferDrawTexture) { (void)c; }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)
NVN(nvnCommandBufferReportValue) { (void)c; }
NVN(nvnCommandBufferResetCounter) { (void)c; }
static void inline_commands(Cpu *c) {
    Snap s = snap(c);
    int count = (int)s.x[1];
    if (count <= 0 || count > 65536 || !s.x[2]) return;
    typedef struct { uint8_t *data; uint64_t size; } Part;
    Part *parts = calloc((size_t)count, sizeof *parts);
    if (!parts) FATAL("nvn: command inline allocation failed\n");
    for (int i = 0; i < count; i++) {
        uint64_t handle;
        memcpy(&handle, (const uint8_t *)(uintptr_t)s.x[2] + 8 * i, 8);
        uint64_t size = 0;
        const void *source = nvn_command_acquire(handle, &size);
        if (!source || size < sizeof(CmdRec)) {
            if (source) nvn_command_release(handle);
            static int nlog;
            if (nlog++ < 16) LOG("[nvn] skipped expired command handle %llx during inline copy\n", (unsigned long long)handle);
            continue;
        }
        parts[i].data = malloc(size);
        parts[i].size = size;
        if (!parts[i].data) FATAL("nvn: command inline snapshot failed\n");
        memcpy(parts[i].data, source, size);
        nvn_command_release(handle);
    }
    for (int i = 0; i < count; i++) {
        if (!parts[i].data) continue;
        uint64_t offset = 0;
        while (offset + sizeof(CmdRec) <= parts[i].size) {
            const CmdRec *src = (const CmdRec *)(parts[i].data + offset);
            if (src->size < sizeof *src || (src->size & 7) || src->size > parts[i].size - offset ||
                src->op >= CMD_COUNT || src->op == CMD_JUMP || src->op == CMD_CALL)
                FATAL("nvn: malformed command copy\n");
            if (src->op == CMD_END) break;
            CmdRec *dst = rec(c, cb_of(s.x[0]), src->op, src->size - sizeof *src);
            memcpy(dst, src, src->size);
            offset += src->size;
        }
        free(parts[i].data);
    }
    free(parts);
}
NVN(nvnCommandBufferCallCommands) { inline_commands(c); }
NVN(nvnCommandBufferCopyCommands) { inline_commands(c); }
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }
