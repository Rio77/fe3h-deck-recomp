// Synthetic unit tests for runtime/services/nfp.c (Amiibo .bin parser, scanner, and FE3H nfp:IUser state machine).
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            exit(1);                                                                        \
        }                                                                                   \
    } while (0)

#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#include "../runtime/kernel.h"
#include "../runtime/paths.h"
#include "../runtime/services/nfp.h"

// ---------------- Minimal host/kernel/IPC test harness ----------------

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KObject *g_handles[512];
static char g_test_root[512];

static void *g_host_allocs[256];
static size_t g_host_alloc_bytes[256];
static int g_nhost_allocs;

void rt_fatal_trace(void) { abort(); }

void *kernel_host_calloc(size_t count, size_t size) {
    CHECK(count > 0 && size > 0);
    void *p = calloc(count, size);
    CHECK(p != NULL);
    CHECK(g_nhost_allocs < (int)(sizeof g_host_allocs / sizeof g_host_allocs[0]));
    g_host_allocs[g_nhost_allocs] = p;
    g_host_alloc_bytes[g_nhost_allocs] = count * size;
    g_nhost_allocs++;
    return p;
}

static void free_tracked_host_allocs(void) {
    for (int i = 0; i < g_nhost_allocs; i++) {
        free(g_host_allocs[i]);
        g_host_allocs[i] = NULL;
        g_host_alloc_bytes[i] = 0;
    }
    g_nhost_allocs = 0;
}

const char *rt_data_root(void) { return g_test_root; }

KObject *handle_get(uint32_t h) {
    if (h < 0x101 || h >= 0x100 + 512) return NULL;
    return g_handles[h - 0x100];
}

uint32_t kernel_new_handle(KObject *o) {
    pthread_mutex_lock(&g_klock);
    uint32_t out = 0;
    for (int i = 1; i < 512; i++) {
        if (!g_handles[i]) {
            g_handles[i] = o;
            o->refs++;
            out = (uint32_t)(0x100 + i);
            break;
        }
    }
    pthread_mutex_unlock(&g_klock);
    CHECK(out != 0);
    return out;
}

static void test_close_handle(uint32_t h) {
    pthread_mutex_lock(&g_klock);
    KObject *o = handle_get(h);
    if (o) {
        g_handles[h - 0x100] = NULL;
        o->refs--;
    }
    pthread_mutex_unlock(&g_klock);
}

void kernel_signal(KObject *o) { o->signaled = true; }

Service *service_new(const char *name, ServiceHandler h, void *state) {
    Service *s = calloc(1, sizeof *s);
    CHECK(s != NULL);
    s->name = name;
    s->handler = h;
    s->state = state;
    return s;
}

void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }

extern const ServiceDef __start_services[];
extern const ServiceDef __stop_services[];
extern const ResetFn __start_resets[];
extern const ResetFn __stop_resets[];

static Service *find_nfp_user_service(void) {
    for (const ServiceDef *d = __start_services; d < __stop_services; d++) {
        if (d->name && strcmp(d->name, "nfp:user") == 0)
            return service_new(d->name, d->handler, NULL);
    }
    return NULL;
}

static void run_ipc(Service *svc, uint32_t cmd, const void *in, uint32_t in_sz, IpcBuf *outb, IpcResp *resp) {
    IpcReq req;
    memset(&req, 0, sizeof req);
    memset(resp, 0, sizeof *resp);
    req.cmd = cmd;
    req.data = (const uint8_t *)in;
    req.data_size = in_sz;
    if (outb) {
        req.c[0] = *outb;
        req.nc = 1;
    }
    svc->handler(svc, &req, resp);
}

static void make_synthetic_bin(uint8_t *buf, size_t len, const uint8_t uid[7], uint8_t c0, uint8_t c1, uint8_t c2,
                               uint8_t type, uint16_t numbering_id, uint8_t series_id) {
    memset(buf, 0, len);
    buf[0x00] = uid[0];
    buf[0x01] = uid[1];
    buf[0x02] = uid[2];
    buf[0x03] = 0x88;  // BCC0 should be skipped by GetTagInfo
    buf[0x04] = uid[3];
    buf[0x05] = uid[4];
    buf[0x06] = uid[5];
    buf[0x07] = uid[6];
    buf[0x54] = c0;
    buf[0x55] = c1;
    buf[0x56] = c2;
    buf[0x57] = type;
    buf[0x58] = (uint8_t)(numbering_id >> 8);
    buf[0x59] = (uint8_t)(numbering_id & 0xFF);
    buf[0x5A] = series_id;
    buf[0x5B] = 0x02;
}

static void write_file_bytes(const char *path, const uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(buf, 1, n, f) == n);
    fclose(f);
}

static void rm_tree_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char child[1280];
                snprintf(child, sizeof child, "%s/%s", path, de->d_name);
                rm_tree_recursive(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static bool is_signaled(uint32_t h) {
    pthread_mutex_lock(&g_klock);
    KObject *o = handle_get(h);
    bool s = o && o->signaled;
    pthread_mutex_unlock(&g_klock);
    return s;
}

static void clear_handles_table(void) {
    memset(g_handles, 0, sizeof g_handles);
}

// ---------------- Test cases ----------------

static void test_parser_and_all_fe3h_headers(void) {
    uint8_t buf[1024];
    const uint8_t uid[7] = {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    make_synthetic_bin(buf, sizeof buf, uid, 0x21, 0x08, 0x00, 0x00, 0x036F, 0x10);

    NfpAmiiboInfo info;
    CHECK(!nfp_parse_bin(NULL, 540, &info));
    CHECK(!nfp_parse_bin(buf, 531, &info));
    CHECK(!nfp_parse_bin(buf, 1025, &info));

    CHECK(nfp_parse_bin(buf, 540, &info));
    CHECK(memcmp(info.uid, uid, 7) == 0);
    CHECK(info.char_id[0] == 0x21 && info.char_id[1] == 0x08 && info.char_id[2] == 0x00);
    CHECK(info.numbering_id == 0x036F);
    CHECK(info.series_id == 0x10);
    CHECK(info.nfp_type == 0x00);

    // Boundary dump sizes 532, 572, 1024
    CHECK(nfp_parse_bin(buf, 532, &info));
    CHECK(nfp_parse_bin(buf, 572, &info));
    CHECK(nfp_parse_bin(buf, 1024, &info));

    // Wrong format byte at 0x5B rejected
    buf[0x5B] = 0x00;
    CHECK(!nfp_parse_bin(buf, 540, &info));

    // Verify synthetic headers for all 12 FE3H Fire Emblem Amiibos (11 unique char_ids)
    static const struct {
        const char *name;
        uint8_t c0, c1, c2;
        uint16_t num;
        uint8_t series;
    } fe3h_figures[12] = {
        {"Super Smash Bros/Marth",      0x21, 0x00, 0x00, 0x0010, 0x00},
        {"Super Smash Bros/Ike",        0x21, 0x01, 0x00, 0x0024, 0x00},
        {"Super Smash Bros/Lucina",     0x21, 0x02, 0x00, 0x0029, 0x00},
        {"Super Smash Bros/Robin",      0x21, 0x03, 0x00, 0x002A, 0x00},
        {"Super Smash Bros/Roy",        0x21, 0x04, 0x00, 0x0053, 0x00},
        {"Super Smash Bros/Corrin P1",  0x21, 0x05, 0x00, 0x025A, 0x00},
        {"Super Smash Bros/Corrin P2",  0x21, 0x05, 0x01, 0x0363, 0x00},
        {"Fire Emblem/Alm",             0x21, 0x06, 0x00, 0x034E, 0x10},
        {"Fire Emblem/Celica",          0x21, 0x07, 0x00, 0x034F, 0x10},
        {"Fire Emblem/Chrom",           0x21, 0x08, 0x00, 0x036F, 0x10},
        {"Super Smash Bros/Chrom",      0x21, 0x08, 0x00, 0x0388, 0x00},
        {"Fire Emblem/Tiki",            0x21, 0x09, 0x00, 0x0370, 0x10},
    };
    uint32_t seen_mask = 0;
    bool saw_corrin_p2 = false;
    for (int i = 0; i < 12; i++) {
        uint8_t fig[540];
        uint8_t fuid[7] = {0x04, (uint8_t)(0x10 + i), 0x22, 0x33, 0x44, 0x55, 0x66};
        make_synthetic_bin(fig, sizeof fig, fuid, fe3h_figures[i].c0, fe3h_figures[i].c1, fe3h_figures[i].c2,
                           0x00, fe3h_figures[i].num, fe3h_figures[i].series);
        CHECK(nfp_parse_bin(fig, sizeof fig, &info));
        CHECK(info.char_id[0] == 0x21 && info.char_id[1] == fe3h_figures[i].c1 && info.char_id[2] == fe3h_figures[i].c2);
        CHECK(info.numbering_id == fe3h_figures[i].num);
        CHECK(info.series_id == fe3h_figures[i].series);
        seen_mask |= 1u << info.char_id[1];
        if (info.char_id[1] == 0x05 && info.char_id[2] == 0x01) saw_corrin_p2 = true;
    }
    CHECK(seen_mask == 0x3FFu);  // 21 00 .. 21 09
    CHECK(saw_corrin_p2);        // + 21 05 01 = 11 unique char_ids
}

static void mkdir_p(const char *path) {
    char tmp[768];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void test_scanner_depth_and_rejections(void) {
    snprintf(g_test_root, sizeof g_test_root, "/tmp/fe3h_nfp_test_XXXXXX");
    CHECK(mkdtemp(g_test_root) != NULL);

    char p[768];
    snprintf(p, sizeof p, "%s/Amiibo/Fire Emblem", g_test_root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/Amiibo/Super Smash Bros", g_test_root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/Amiibo/d1/d2/d3", g_test_root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/Outside", g_test_root);
    mkdir_p(p);

    uint8_t marth[540], chrom_fe[540], chrom_ssb[540], tiki[540], bad[540];
    const uint8_t uid1[7] = {0x04, 1, 2, 3, 4, 5, 6};
    const uint8_t uid2[7] = {0x04, 2, 2, 3, 4, 5, 6};
    const uint8_t uid3[7] = {0x04, 3, 2, 3, 4, 5, 6};
    const uint8_t uid4[7] = {0x04, 4, 2, 3, 4, 5, 6};
    make_synthetic_bin(marth, sizeof marth, uid1, 0x21, 0x00, 0x00, 0, 0x0010, 0x00);
    make_synthetic_bin(chrom_fe, sizeof chrom_fe, uid2, 0x21, 0x08, 0x00, 0, 0x036F, 0x10);
    make_synthetic_bin(chrom_ssb, sizeof chrom_ssb, uid3, 0x21, 0x08, 0x00, 0, 0x0388, 0x00);
    make_synthetic_bin(tiki, sizeof tiki, uid4, 0x21, 0x09, 0x00, 0, 0x0370, 0x10);
    memset(bad, 0xFF, sizeof bad);

    snprintf(p, sizeof p, "%s/Amiibo/Marth.bin", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    snprintf(p, sizeof p, "%s/Amiibo/Fire Emblem/Chrom.bin", g_test_root);
    write_file_bytes(p, chrom_fe, sizeof chrom_fe);
    snprintf(p, sizeof p, "%s/Amiibo/Super Smash Bros/Chrom.BIN", g_test_root);
    write_file_bytes(p, chrom_ssb, sizeof chrom_ssb);
    snprintf(p, sizeof p, "%s/Amiibo/d1/d2/Tiki.bin", g_test_root);
    write_file_bytes(p, tiki, sizeof tiki);  // depth 2: included

    // Rejected: depth 3, corrupt magic, wrong size, dotfile, non-.bin, symlinks
    snprintf(p, sizeof p, "%s/Amiibo/d1/d2/d3/TooDeep.bin", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    snprintf(p, sizeof p, "%s/Amiibo/Bad.bin", g_test_root);
    write_file_bytes(p, bad, sizeof bad);
    snprintf(p, sizeof p, "%s/Amiibo/Short.bin", g_test_root);
    write_file_bytes(p, marth, 531);
    snprintf(p, sizeof p, "%s/Amiibo/.Hidden.bin", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    snprintf(p, sizeof p, "%s/Amiibo/readme.txt", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    snprintf(p, sizeof p, "%s/Outside/Target.bin", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    char sym[768];
    snprintf(sym, sizeof sym, "%s/Amiibo/SymLink.bin", g_test_root);
    CHECK(symlink(p, sym) == 0);
    char out_dir[768], sym_dir[768];
    snprintf(out_dir, sizeof out_dir, "%s/Outside", g_test_root);
    snprintf(sym_dir, sizeof sym_dir, "%s/Amiibo/SymDir", g_test_root);
    CHECK(symlink(out_dir, sym_dir) == 0);

    nfp_test_reset_all();
    nfp_rescan_amiibos();

    NfpAmiiboInfo entries[16];
    NfpUiState st;
    uint64_t gen = 0;
    nfp_ui_snapshot(entries, 16, &gen, &st);
    CHECK(st.count == 4);
    CHECK(st.selected_ui == 0);
    CHECK(strcmp(entries[0].name, "Fire Emblem/Chrom") == 0);
    CHECK(entries[0].numbering_id == 0x036F);
    CHECK(strcmp(entries[1].name, "Marth") == 0);
    CHECK(strcmp(entries[2].name, "Super Smash Bros/Chrom") == 0);
    CHECK(entries[2].numbering_id == 0x0388);
    CHECK(strcmp(entries[3].name, "d1/d2/Tiki") == 0);

    // Rescan preserves armed selection even when a new earlier-sorting entry shifts indices
    nfp_ui_select(2);  // "Marth"
    snprintf(p, sizeof p, "%s/Amiibo/AAA_First.bin", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    nfp_rescan_amiibos();
    nfp_ui_snapshot(entries, 16, &gen, &st);
    CHECK(st.count == 5 && st.selected_ui == 3 && strcmp(entries[2].name, "Marth") == 0);
    unlink(p);
    nfp_rescan_amiibos();
    nfp_ui_snapshot(entries, 16, &gen, &st);
    CHECK(st.count == 4 && st.selected_ui == 2 && strcmp(entries[1].name, "Marth") == 0);
    nfp_ui_select(0);
}

static void test_fe3h_gazebo_state_machine(void) {
    clear_handles_table();
    free_tracked_host_allocs();
    unsetenv("FE3H_NFP_AMIIBO");

    Service *root = find_nfp_user_service();
    CHECK(root != NULL);

    IpcResp resp;
    run_ipc(root, 0, NULL, 0, NULL, &resp);
    CHECK(resp.result == 0 && resp.nobjects == 1 && resp.objects[0] != NULL);
    Service *iuser = resp.objects[0];

    // 1. Initialize + ListDevices + GetNpadId + Attach*Event
    uint8_t init_in[16] = {0};
    run_ipc(iuser, 0, init_in, sizeof init_in, NULL, &resp);
    CHECK(resp.result == 0);

    uint64_t dev_list[4] = {0};
    IpcBuf dev_buf = {.addr = (uintptr_t)dev_list, .size = sizeof dev_list};
    run_ipc(iuser, 2, NULL, 0, &dev_buf, &resp);
    CHECK(resp.result == 0 && *(int32_t *)resp.data == 1 && dev_list[0] == 1ull);

    uint64_t dev = dev_list[0];
    run_ipc(iuser, 21, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && *(uint32_t *)resp.data == 0);  // NpadId::No1

    run_ipc(iuser, 23, NULL, 0, NULL, &resp);
    CHECK(resp.result == 0 && resp.ncopy == 1);
    uint32_t avail_h = resp.copy_handles[0];

    run_ipc(iuser, 17, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && resp.ncopy == 1);
    uint32_t act_h = resp.copy_handles[0];

    run_ipc(iuser, 18, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && resp.ncopy == 1);
    uint32_t deact_h = resp.copy_handles[0];
    CHECK(g_nhost_allocs == 3);  // kernel_host_calloc used for the 3 KEvents

    // 2. No Amiibo ("None"): StartDetection stays SearchingForTag, activate_ev NOT signaled
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 1);  // SearchingForTag
    CHECK(!is_signaled(act_h));

    run_ipc(iuser, 4, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    test_close_handle(act_h);
    test_close_handle(deact_h);
    test_close_handle(avail_h);
    run_ipc(iuser, 1, NULL, 0, NULL, &resp);
    CHECK(resp.result == 0);

    // 3. Cancel-before-read preservation: pre-select Marth (index 2), pre-mount GetTagInfo without Mount
    nfp_ui_select(2);  // "Marth"
    NfpUiState st;
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 2);

    run_ipc(iuser, 0, init_in, sizeof init_in, NULL, &resp);
    run_ipc(iuser, 23, NULL, 0, NULL, &resp);
    uint32_t avail_h2 = resp.copy_handles[0];
    run_ipc(iuser, 17, &dev, 8, NULL, &resp);
    uint32_t act_h2 = resp.copy_handles[0];
    run_ipc(iuser, 18, &dev, 8, NULL, &resp);
    uint32_t deact_h2 = resp.copy_handles[0];
    CHECK(handle_get(act_h2) != NULL && handle_get(deact_h2) != NULL && handle_get(avail_h2) != NULL);

    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && is_signaled(act_h2));
    uint8_t tag_info[0x58], model_info[0x40];
    IpcBuf tb = {.addr = (uintptr_t)tag_info, .size = sizeof tag_info};
    IpcBuf mb = {.addr = (uintptr_t)model_info, .size = sizeof model_info};
    // Pre-mount GetTagInfo must NOT mark tag_read_completed
    run_ipc(iuser, 13, &dev, 8, &tb, &resp);
    CHECK(resp.result == 0);
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(!st.tag_read_completed);

    run_ipc(iuser, 4, &dev, 8, NULL, &resp);  // StopDetection before Mount
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 2);  // Preserved!

    // 4. Exact FE3H Gazebo sequence:
    // StartDetection -> pre-mount GetTagInfo -> Mount(0,1) -> GetModelInfo -> Unmount -> StopDetection/Finalize
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && is_signaled(act_h2));
    run_ipc(iuser, 13, &dev, 8, &tb, &resp);  // pre-mount GetTagInfo
    CHECK(resp.result == 0);
    CHECK(tag_info[0] == 0x04 && tag_info[3] == 0x03 && tag_info[0x0A] == 7);  // BCC0 skipped!
    CHECK(*(uint32_t *)(tag_info + 0x20) == 1 && *(uint32_t *)(tag_info + 0x24) == 2);
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(!st.tag_read_completed);

    uint8_t mount_in[16] = {0};
    *(uint64_t *)(mount_in + 0) = dev;
    *(uint32_t *)(mount_in + 8) = 0;   // Amiibo
    *(uint32_t *)(mount_in + 12) = 1;  // Rom
    run_ipc(iuser, 5, mount_in, sizeof mount_in, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 4);  // NFP_DEV_TAG_MOUNTED

    run_ipc(iuser, 16, &dev, 8, &mb, &resp);  // GetModelInfo while MOUNTED
    CHECK(resp.result == 0);
    CHECK(model_info[0] == 0x21 && model_info[1] == 0x00 && model_info[2] == 0x00);
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.tag_read_completed);

    // CRITICAL FE3H Unmount check: TAG_MOUNTED -> TAG_FOUND (2), !is_signaled(deact_h2), selection preserved until Finalize/StopDetection
    run_ipc(iuser, 6, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    CHECK(!is_signaled(deact_h2));
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 2);  // NFP_DEV_TAG_FOUND
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 2);  // Still armed until Finalize / StopDetection

    run_ipc(iuser, 1, NULL, 0, NULL, &resp);  // Finalize resets to "None"
    CHECK(resp.result == 0);
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 0);

    // 5. Live F1 selection during SearchingForTag + consecutive multi-amiibo scans
    run_ipc(iuser, 0, init_in, sizeof init_in, NULL, &resp);
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);  // SearchingForTag with None
    CHECK(resp.result == 0 && !is_signaled(act_h2));
    nfp_ui_select(1);  // "Fire Emblem/Chrom" live from F1!
    CHECK(is_signaled(act_h2));
    run_ipc(iuser, 5, mount_in, sizeof mount_in, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 16, &dev, 8, &mb, &resp);
    CHECK(resp.result == 0);
    CHECK(model_info[0] == 0x21 && model_info[1] == 0x08);
    CHECK(*(uint16_t *)(model_info + 4) == 0x036F && model_info[3] == 0x10);
    run_ipc(iuser, 6, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && !is_signaled(deact_h2));
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 2);
    run_ipc(iuser, 4, &dev, 8, NULL, &resp);  // StopDetection resets to "None"
    CHECK(resp.result == 0);
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 0);

    // Consecutive scan #2: Super Smash Bros/Chrom (index 3)
    nfp_ui_select(3);
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && is_signaled(act_h2));
    run_ipc(iuser, 5, mount_in, sizeof mount_in, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 16, &dev, 8, &mb, &resp);
    CHECK(resp.result == 0);
    CHECK(*(uint16_t *)(model_info + 4) == 0x0388 && model_info[3] == 0x00);
    run_ipc(iuser, 6, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && !is_signaled(deact_h2));

    // 6. Live F1 swap AFTER read completion (TAG_FOUND -> TAG_REMOVED, signals deactivate, preserves new selection!)
    nfp_ui_select(4);  // Swap to "d1/d2/Tiki" while TAG_FOUND post-Unmount
    CHECK(is_signaled(deact_h2));
    CHECK(!is_signaled(act_h2));
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 3);  // NFP_DEV_TAG_REMOVED
    run_ipc(iuser, 4, &dev, 8, NULL, &resp);  // StopDetection must NOT wipe newly selected Tiki!
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 4);

    // Also test live F1 removal BEFORE read completion (TAG_FOUND -> TAG_REMOVED)
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && is_signaled(act_h2));
    nfp_ui_select(0);  // User selects "None" before read
    CHECK(is_signaled(deact_h2));
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 3);  // NFP_DEV_TAG_REMOVED
    run_ipc(iuser, 4, &dev, 8, NULL, &resp);

    // Also test live F1 swap WHILE TAG_MOUNTED post-read (TAG_MOUNTED -> TAG_REMOVED, preserves new selection)
    nfp_ui_select(2);
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && is_signaled(act_h2));
    run_ipc(iuser, 5, mount_in, sizeof mount_in, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 16, &dev, 8, &mb, &resp);
    CHECK(resp.result == 0);
    nfp_ui_select(1);  // Swap while mounted
    CHECK(is_signaled(deact_h2) && !is_signaled(act_h2));
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 3);  // NFP_DEV_TAG_REMOVED
    run_ipc(iuser, 6, &dev, 8, NULL, &resp);
    CHECK(resp.result == MAKE_RESULT(115, 96));  // Unmount after physical removal returns InvalidState
    run_ipc(iuser, 4, &dev, 8, NULL, &resp);
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 1);  // Newly selected Fire Emblem/Chrom preserved!

    // 7. A/B toggle FE3H_NFP_AMIIBO=0
    setenv("FE3H_NFP_AMIIBO", "0", 1);
    nfp_ui_select(2);
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 1);  // stays SearchingForTag
    CHECK(!is_signaled(act_h2));
    nfp_ui_select(3);  // live select while disabled must also stay SearchingForTag
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 1);
    CHECK(!is_signaled(act_h2));
    unsetenv("FE3H_NFP_AMIIBO");

    // 8. RT_ON_RESET hook clears session state but preserves Amiibo list and armed selection
    for (const ResetFn *fn = __start_resets; fn < __stop_resets; fn++) (*fn)();
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.count == 4 && st.selected_ui == 3 && st.nfp_state == 0 && st.dev_state == 0);
    CHECK(!st.has_active_tag && !st.tag_read_completed);

    // Re-initialize after RT_ON_RESET allocates fresh KEvents via kernel_host_calloc
    int prev_allocs = g_nhost_allocs;
    test_close_handle(act_h2);
    test_close_handle(deact_h2);
    test_close_handle(avail_h2);
    run_ipc(iuser, 0, init_in, sizeof init_in, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 17, &dev, 8, NULL, &resp);
    uint32_t act_h3 = resp.copy_handles[0];
    CHECK(g_nhost_allocs == prev_allocs + 1);
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && is_signaled(act_h3));
    test_close_handle(act_h3);

    free(iuser);
    free(root);
    free_tracked_host_allocs();
}

int main(void) {
    test_parser_and_all_fe3h_headers();
    test_scanner_depth_and_rejections();
    test_fe3h_gazebo_state_machine();

    if (strncmp(g_test_root, "/tmp/fe3h_nfp_test_", 19) == 0) {
        rm_tree_recursive(g_test_root);
    }
    printf("PASS test_nfp\n");
    return 0;
}
