// HLE nn::nfp (nfp:user -> nfp:IUser) for Switch Amiibo emulation via <data_root>/Amiibo/*.bin.
#include "nfp.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hle.h"
#include "../ipc.h"
#include "../kernel.h"
#include "../paths.h"

// Error codes (Nintendo SDK Result module 115 = Nfc/Nfp)
#define NFP_ERR_DEVICE_NOT_FOUND   MAKE_RESULT(115, 64)
#define NFP_ERR_WRONG_ARGUMENT     MAKE_RESULT(115, 66)
#define NFP_ERR_INVALID_STATE      MAKE_RESULT(115, 96)
#define NFP_ERR_NEED_CREATE        MAKE_RESULT(115, 128)
#define NFP_ERR_NEED_FORMAT        MAKE_RESULT(115, 132)
#define NFP_ERR_NOT_SUPPORTED      MAKE_RESULT(115, 152)

typedef enum {
    NFP_STATE_NON_INITIALIZED = 0,
    NFP_STATE_INITIALIZED = 1,
} NfpState;

typedef enum {
    NFP_DEV_INITIALIZED = 0,
    NFP_DEV_SEARCHING = 1,
    NFP_DEV_TAG_FOUND = 2,
    NFP_DEV_TAG_REMOVED = 3,
    NFP_DEV_TAG_MOUNTED = 4,
    NFP_DEV_UNAVAILABLE = 5,
    NFP_DEV_FINALIZED = 6,
} NfpDeviceState;

enum {
    NFP_MODEL_AMIIBO = 0,
    NFP_MOUNT_ROM = 1,
    NFP_MOUNT_RAM = 2,
    NFP_MOUNT_ALL = 3,
};

#define NFP_DEVICE_ID 1ull

static pthread_mutex_t nfp_lock = PTHREAD_MUTEX_INITIALIZER;

// Scanned Amiibo catalogue (persists across RT_ON_RESET; refreshed on F1 open or initialize)
static NfpAmiiboInfo g_entries[NFP_MAX_ENTRIES];
static int g_nentries;
static int g_selected = -1;  // -1 = "None", 0..g_nentries-1 = selected entry
static uint64_t g_scan_gen = 1;

// Active per-boot NFP session state (reset in RT_ON_RESET)
static int g_nfp_state = NFP_STATE_NON_INITIALIZED;
static int g_dev_state = NFP_DEV_INITIALIZED;
static bool g_has_active_tag;
static bool g_tag_read_completed;
static NfpAmiiboInfo g_active_tag;
static KEvent *g_activate_ev;
static KEvent *g_deactivate_ev;
static KEvent *g_avail_ev;

static bool nfp_amiibo_enabled(void) {
    const char *e = getenv("FE3H_NFP_AMIIBO");
    return !(e && !strcmp(e, "0"));
}

// Allocate/reuse persistent KEvent* so svcCloseHandle on one visit doesn't free or break the next Attach*Event.
static KEvent *ensure_event_locked(KEvent **slot) {
    if (!*slot) {
        KEvent *ev = kernel_host_calloc(1, sizeof *ev);
        ev->type = KOBJ_EVENT;
        ev->refs = 1;  // owned by nfp service for the boot lifetime
        *slot = ev;
    }
    return *slot;
}

// Lock ordering rule: always acquire nfp_lock before g_klock, never reverse.
static void signal_event_locked(KEvent **slot) {
    KEvent *ev = ensure_event_locked(slot);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
}

static void clear_event_locked(KEvent *ev) {
    if (!ev) return;
    pthread_mutex_lock(&g_klock);
    ev->signaled = false;
    pthread_mutex_unlock(&g_klock);
}

static uint32_t attach_event_locked(KEvent **slot) {
    KEvent *ev = ensure_event_locked(slot);
    return kernel_new_handle(ev);
}

RT_ON_RESET(nfp_reset) {
    pthread_mutex_lock(&nfp_lock);
    g_nfp_state = NFP_STATE_NON_INITIALIZED;
    g_dev_state = NFP_DEV_INITIALIZED;
    g_has_active_tag = false;
    g_tag_read_completed = false;
    memset(&g_active_tag, 0, sizeof g_active_tag);
    // Old KEvents belong to the wiped boot's kernel handle table; kernel resets on reboot.
    g_activate_ev = NULL;
    g_deactivate_ev = NULL;
    g_avail_ev = NULL;
    pthread_mutex_unlock(&nfp_lock);
}

void nfp_test_reset_all(void) {
    pthread_mutex_lock(&nfp_lock);
    g_nentries = 0;
    g_selected = -1;
    g_scan_gen++;
    memset(g_entries, 0, sizeof g_entries);
    g_nfp_state = NFP_STATE_NON_INITIALIZED;
    g_dev_state = NFP_DEV_INITIALIZED;
    g_has_active_tag = false;
    g_tag_read_completed = false;
    memset(&g_active_tag, 0, sizeof g_active_tag);
    g_activate_ev = NULL;
    g_deactivate_ev = NULL;
    g_avail_ev = NULL;
    pthread_mutex_unlock(&nfp_lock);
}

// ------------------------------------------------------------------ .bin parser & scanner

bool nfp_parse_bin(const uint8_t *raw, size_t size, NfpAmiiboInfo *out) {
    if (!raw || !out || size < 532 || size > 1024) return false;
    // Unencrypted NTAG215 Amiibo model block sits at 0x54..0x5B with format marker 0x02 at 0x5B.
    if (raw[0x5B] != 0x02) return false;

    memset(out, 0, sizeof *out);
    out->uid[0] = raw[0x00];
    out->uid[1] = raw[0x01];
    out->uid[2] = raw[0x02];
    out->uid[3] = raw[0x04];  // raw[0x03] is BCC0
    out->uid[4] = raw[0x05];
    out->uid[5] = raw[0x06];
    out->uid[6] = raw[0x07];

    out->char_id[0] = raw[0x54];
    out->char_id[1] = raw[0x55];
    out->char_id[2] = raw[0x56];
    out->nfp_type = raw[0x57];
    out->numbering_id = (uint16_t)(((uint16_t)raw[0x58] << 8) | (uint16_t)raw[0x59]);
    out->series_id = raw[0x5A];
    return true;
}

static bool ends_with_bin_ci(const char *s) {
    size_t n = strlen(s);
    if (n < 4) return false;
    return tolower((unsigned char)s[n - 4]) == '.' && tolower((unsigned char)s[n - 3]) == 'b' &&
           tolower((unsigned char)s[n - 2]) == 'i' && tolower((unsigned char)s[n - 1]) == 'n';
}

static int cmp_entries(const void *a, const void *b) {
    const NfpAmiiboInfo *ea = (const NfpAmiiboInfo *)a;
    const NfpAmiiboInfo *eb = (const NfpAmiiboInfo *)b;
    return strcmp(ea->name, eb->name);
}

static void scan_dir_recursive(const char *abs_dir, const char *rel_prefix, int depth, NfpAmiiboInfo *tmp, int *count) {
    if (depth > 2 || *count >= NFP_MAX_ENTRIES) return;
    DIR *d = opendir(abs_dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && *count < NFP_MAX_ENTRIES) {
        if (de->d_name[0] == '.') continue;  // skip ".", "..", dotfiles

        char full[1280];
        int fn = snprintf(full, sizeof full, "%s/%s", abs_dir, de->d_name);
        if (fn < 0 || (size_t)fn >= sizeof full) continue;

        struct stat st;
#ifdef _WIN32
        if (stat(full, &st) != 0) continue;
#else
        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) continue;  // ignore symlinks
#endif

        char rel[512];
        int rn = rel_prefix[0]
            ? snprintf(rel, sizeof rel, "%s/%s", rel_prefix, de->d_name)
            : snprintf(rel, sizeof rel, "%s", de->d_name);
        if (rn < 0 || (size_t)rn >= sizeof rel) continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth < 2) scan_dir_recursive(full, rel, depth + 1, tmp, count);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !ends_with_bin_ci(de->d_name)) continue;
        if (st.st_size < 532 || st.st_size > 1024) continue;

        FILE *f = fopen(full, "rb");
        if (!f) continue;
        uint8_t buf[1024];
        size_t nr = fread(buf, 1, (size_t)st.st_size, f);
        fclose(f);

        NfpAmiiboInfo info;
        if (!nfp_parse_bin(buf, nr, &info)) continue;

        snprintf(info.rel_path, sizeof info.rel_path, "%s", rel);
        size_t rlen = strlen(rel);
        size_t stem = rlen >= 4 ? rlen - 4 : rlen;
        if (stem >= sizeof info.name) stem = sizeof info.name - 1;
        memcpy(info.name, rel, stem);
        info.name[stem] = 0;

        tmp[(*count)++] = info;
    }
    closedir(d);
}

void nfp_rescan_amiibos(void) {
    const char *root = rt_data_root();
    if (!root || !root[0]) root = ".";

    char amiibo_dir[1100];
    snprintf(amiibo_dir, sizeof amiibo_dir, "%s/Amiibo", root);

    NfpAmiiboInfo *tmp = calloc(NFP_MAX_ENTRIES, sizeof *tmp);
    int count = 0;
    if (tmp) {
        scan_dir_recursive(amiibo_dir, "", 0, tmp, &count);
        if (count > 1) qsort(tmp, (size_t)count, sizeof *tmp, cmp_entries);
    }

    pthread_mutex_lock(&nfp_lock);
    char prev_rel[512] = {0};
    if (g_selected >= 0 && g_selected < g_nentries)
        snprintf(prev_rel, sizeof prev_rel, "%s", g_entries[g_selected].rel_path);

    g_nentries = count;
    if (tmp && count > 0) memcpy(g_entries, tmp, (size_t)count * sizeof *tmp);
    else memset(g_entries, 0, sizeof g_entries);

    // Keep the user's armed selection if that file still exists across the rescan.
    g_selected = -1;
    if (prev_rel[0]) {
        for (int i = 0; i < g_nentries; i++) {
            if (strcmp(g_entries[i].rel_path, prev_rel) == 0) {
                g_selected = i;
                break;
            }
        }
    }
    g_scan_gen++;
    pthread_mutex_unlock(&nfp_lock);
    free(tmp);
}

// ------------------------------------------------------------------ detection & UI helpers

static void arm_active_tag_locked(void) {
    if (!nfp_amiibo_enabled()) return;
    if (g_selected < 0 || g_selected >= g_nentries) return;
    g_active_tag = g_entries[g_selected];
    g_has_active_tag = true;
    g_tag_read_completed = false;
    g_dev_state = NFP_DEV_TAG_FOUND;
    clear_event_locked(g_deactivate_ev);
    signal_event_locked(&g_activate_ev);
    LOG("[nfp] tag ready for Garreg Mach Monastery gazebo read: %s (char=%02x%02x%02x num=0x%04x)\n",
        g_active_tag.name, g_active_tag.char_id[0], g_active_tag.char_id[1],
        g_active_tag.char_id[2], g_active_tag.numbering_id);
}

// Reset UI selection to "None" ONLY when a real read actually took place and the user didn't swap afterward.
static void finish_completed_read_locked(bool signal_deactivate) {
    if (!g_tag_read_completed) return;
    LOG("[nfp] Garreg Mach Monastery gazebo read finished for '%s', returning selection to None\n", g_active_tag.name);
    if (g_selected >= 0 && g_selected < g_nentries &&
        strcmp(g_entries[g_selected].rel_path, g_active_tag.rel_path) == 0) {
        g_selected = -1;
    }
    g_has_active_tag = false;
    g_tag_read_completed = false;
    memset(&g_active_tag, 0, sizeof g_active_tag);
    if (signal_deactivate) {
        g_dev_state = NFP_DEV_TAG_REMOVED;
        clear_event_locked(g_activate_ev);
        signal_event_locked(&g_deactivate_ev);
    }
}

void nfp_ui_snapshot(NfpAmiiboInfo *out_entries, int max_entries, uint64_t *io_gen, NfpUiState *out_state) {
    pthread_mutex_lock(&nfp_lock);
    if (out_entries && max_entries > 0 && (!io_gen || *io_gen != g_scan_gen)) {
        int n = g_nentries < max_entries ? g_nentries : max_entries;
        if (n > 0) memcpy(out_entries, g_entries, (size_t)n * sizeof(NfpAmiiboInfo));
        if (io_gen) *io_gen = g_scan_gen;
    }
    if (out_state) {
        memset(out_state, 0, sizeof *out_state);
        out_state->count = g_nentries;
        out_state->selected_ui = (g_selected >= 0 && g_selected < g_nentries) ? (g_selected + 1) : 0;
        out_state->nfp_state = g_nfp_state;
        out_state->dev_state = g_dev_state;
        out_state->has_active_tag = g_has_active_tag;
        out_state->tag_read_completed = g_tag_read_completed;
        if (g_selected >= 0 && g_selected < g_nentries)
            out_state->active_tag = g_entries[g_selected];
    }
    pthread_mutex_unlock(&nfp_lock);
}

void nfp_ui_select(int ui_idx) {
    pthread_mutex_lock(&nfp_lock);
    int new_sel = (ui_idx >= 1 && ui_idx <= g_nentries) ? (ui_idx - 1) : -1;
    if (new_sel == g_selected) {
        pthread_mutex_unlock(&nfp_lock);
        return;
    }

    g_selected = new_sel;
    if (g_dev_state == NFP_DEV_SEARCHING) {
        // Gazebo is actively waiting for an amiibo: trigger detection immediately if one is chosen.
        if (new_sel >= 0 && nfp_amiibo_enabled()) {
            arm_active_tag_locked();
        }
    } else if (g_dev_state == NFP_DEV_TAG_FOUND || g_dev_state == NFP_DEV_TAG_MOUNTED) {
        // User removed or swapped while tag was active (even after read completion): notify tag removal.
        g_has_active_tag = false;
        g_tag_read_completed = false;
        memset(&g_active_tag, 0, sizeof g_active_tag);
        if (g_nfp_state == NFP_STATE_INITIALIZED) {
            g_dev_state = NFP_DEV_TAG_REMOVED;
            clear_event_locked(g_activate_ev);
            signal_event_locked(&g_deactivate_ev);
        }
    }
    // Outside the gazebo (Initialized / NonInitialized / TagRemoved), selection simply stays armed without events or reset.
    pthread_mutex_unlock(&nfp_lock);
}

// ------------------------------------------------------------------ IPC service handlers

static void fill_tag_info(const NfpAmiiboInfo *tag, uint8_t *dst, size_t len) {
    memset(dst, 0, len);
    memcpy(dst + 0x00, tag->uid, 7);
    dst[0x0A] = 7;                  // uid_length
    *(uint32_t *)(dst + 0x20) = 1;  // protocol: NfcProtocol_TypeA
    *(uint32_t *)(dst + 0x24) = 2;  // tag_type: NfcTagType_Type2 (NTAG215)
}

static void fill_model_info(const NfpAmiiboInfo *tag, uint8_t *dst, size_t len) {
    memset(dst, 0, len);
    dst[0x00] = tag->char_id[0];
    dst[0x01] = tag->char_id[1];
    dst[0x02] = tag->char_id[2];
    dst[0x03] = tag->series_id;
    dst[0x04] = (uint8_t)(tag->numbering_id & 0xFF);
    dst[0x05] = (uint8_t)((tag->numbering_id >> 8) & 0xFF);
    dst[0x06] = tag->nfp_type;
}

static bool check_dev_handle(IpcReq *req, uint32_t off) {
    if (req->data_size < off + 8) return false;
    uint64_t dev = IN(uint64_t, off);
    return dev == NFP_DEVICE_ID;
}

static void nfp_user_iface(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    pthread_mutex_lock(&nfp_lock);
    switch (req->cmd) {
    case 0: {  // Initialize(u64 aruid, u64 reserved, buffer<mcu_info>)
        g_nfp_state = NFP_STATE_INITIALIZED;
        g_dev_state = NFP_DEV_INITIALIZED;
        g_has_active_tag = false;
        g_tag_read_completed = false;
        memset(&g_active_tag, 0, sizeof g_active_tag);
        pthread_mutex_unlock(&nfp_lock);
        // Refresh catalogue outside IPC lock.
        nfp_rescan_amiibos();
        return;
    }
    case 1: {  // Finalize()
        finish_completed_read_locked(false);
        clear_event_locked(g_activate_ev);
        clear_event_locked(g_deactivate_ev);
        clear_event_locked(g_avail_ev);
        g_has_active_tag = false;
        g_tag_read_completed = false;
        memset(&g_active_tag, 0, sizeof g_active_tag);
        g_dev_state = NFP_DEV_INITIALIZED;
        g_nfp_state = NFP_STATE_NON_INITIALIZED;
        break;
    }
    case 2: {  // ListDevices() -> (s32 count, buffer<DeviceHandle>)
        IpcBuf *b = out_buf(req, 0);
        int32_t count = 0;
        if (b && b->addr && b->size >= sizeof(uint64_t)) {
            *(uint64_t *)(uintptr_t)b->addr = NFP_DEVICE_ID;
            count = 1;
        }
        OUT(int32_t, 0) = count;
        resp_size(resp, 4);
        break;
    }
    case 3: {  // StartDetection(DeviceHandle)
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        if (g_nfp_state != NFP_STATE_INITIALIZED ||
            (g_dev_state != NFP_DEV_INITIALIZED && g_dev_state != NFP_DEV_TAG_REMOVED)) {
            resp->result = NFP_ERR_INVALID_STATE;
            break;
        }
        clear_event_locked(g_deactivate_ev);
        clear_event_locked(g_activate_ev);
        g_has_active_tag = false;
        g_tag_read_completed = false;
        memset(&g_active_tag, 0, sizeof g_active_tag);
        g_dev_state = NFP_DEV_SEARCHING;
        if (g_selected >= 0 && g_selected < g_nentries && nfp_amiibo_enabled())
            arm_active_tag_locked();
        break;
    }
    case 4: {  // StopDetection(DeviceHandle)
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        finish_completed_read_locked(false);
        clear_event_locked(g_activate_ev);
        clear_event_locked(g_deactivate_ev);
        g_has_active_tag = false;
        g_tag_read_completed = false;
        memset(&g_active_tag, 0, sizeof g_active_tag);
        g_dev_state = NFP_DEV_INITIALIZED;
        break;
    }
    case 5: {  // Mount(DeviceHandle, ModelType, MountTarget)
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        if (req->data_size < 16) { resp->result = NFP_ERR_WRONG_ARGUMENT; break; }
        uint32_t model_type = IN(uint32_t, 8);
        uint32_t mount_target = IN(uint32_t, 12);
        if (!g_has_active_tag || (g_dev_state != NFP_DEV_TAG_FOUND && g_dev_state != NFP_DEV_TAG_MOUNTED)) {
            resp->result = NFP_ERR_INVALID_STATE;
            break;
        }
        if (model_type != NFP_MODEL_AMIIBO || mount_target == 0 || mount_target > NFP_MOUNT_ALL) {
            resp->result = NFP_ERR_WRONG_ARGUMENT;
            break;
        }
        g_dev_state = NFP_DEV_TAG_MOUNTED;
        break;
    }
    case 6: {  // Unmount(DeviceHandle): transition TagMounted -> TagFound without signaling deactivate or clearing selection
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        if (g_dev_state != NFP_DEV_TAG_MOUNTED) { resp->result = NFP_ERR_INVALID_STATE; break; }
        g_dev_state = NFP_DEV_TAG_FOUND;
        break;
    }
    case 7:   // OpenApplicationArea
    case 8:   // GetApplicationArea
    case 9:   // SetApplicationArea
    case 10:  // Flush
    case 11:  // Restore
    case 12:  // CreateApplicationArea
        resp->result = NFP_ERR_NEED_CREATE;
        break;
    case 13: {  // GetTagInfo(DeviceHandle) -> out 0x58
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        if (!g_has_active_tag || (g_dev_state != NFP_DEV_TAG_FOUND && g_dev_state != NFP_DEV_TAG_MOUNTED)) {
            resp->result = NFP_ERR_INVALID_STATE;
            break;
        }
        IpcBuf *b = out_buf(req, 0);
        if (!b || !b->addr || b->size < 0x58) { resp->result = NFP_ERR_WRONG_ARGUMENT; break; }
        fill_tag_info(&g_active_tag, (uint8_t *)(uintptr_t)b->addr, 0x58);
        if (g_dev_state == NFP_DEV_TAG_MOUNTED) g_tag_read_completed = true;
        break;
    }
    case 14: {  // GetRegisterInfo(DeviceHandle) -> out 0x100 (minimal stub for completeness)
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        if (!g_has_active_tag || g_dev_state != NFP_DEV_TAG_MOUNTED) { resp->result = NFP_ERR_INVALID_STATE; break; }
        IpcBuf *b = out_buf(req, 0);
        if (!b || !b->addr || b->size < 0x100) { resp->result = NFP_ERR_WRONG_ARGUMENT; break; }
        memset((void *)(uintptr_t)b->addr, 0, 0x100);
        break;
    }
    case 15: {  // GetCommonInfo(DeviceHandle) -> out 0x40 (minimal stub for completeness)
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        if (!g_has_active_tag || g_dev_state != NFP_DEV_TAG_MOUNTED) { resp->result = NFP_ERR_INVALID_STATE; break; }
        IpcBuf *b = out_buf(req, 0);
        if (!b || !b->addr || b->size < 0x40) { resp->result = NFP_ERR_WRONG_ARGUMENT; break; }
        memset((void *)(uintptr_t)b->addr, 0, 0x40);
        *(uint16_t *)((uint8_t *)(uintptr_t)b->addr + 0x06) = 0x00D8;  // application_area_size
        break;
    }
    case 16: {  // GetModelInfo(DeviceHandle) -> out 0x40
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        if (!g_has_active_tag || (g_dev_state != NFP_DEV_TAG_FOUND && g_dev_state != NFP_DEV_TAG_MOUNTED)) {
            resp->result = NFP_ERR_INVALID_STATE;
            break;
        }
        IpcBuf *b = out_buf(req, 0);
        if (!b || !b->addr || b->size < 0x40) { resp->result = NFP_ERR_WRONG_ARGUMENT; break; }
        fill_model_info(&g_active_tag, (uint8_t *)(uintptr_t)b->addr, 0x40);
        if (g_dev_state == NFP_DEV_TAG_MOUNTED) g_tag_read_completed = true;
        break;
    }
    case 17: {  // AttachActivateEvent(DeviceHandle) -> copy handle
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        resp->copy_handles[resp->ncopy++] = attach_event_locked(&g_activate_ev);
        break;
    }
    case 18: {  // AttachDeactivateEvent(DeviceHandle) -> copy handle
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        resp->copy_handles[resp->ncopy++] = attach_event_locked(&g_deactivate_ev);
        break;
    }
    case 19: {  // GetState() -> u32
        OUT(uint32_t, 0) = (uint32_t)g_nfp_state;
        resp_size(resp, 4);
        break;
    }
    case 20: {  // GetDeviceState(DeviceHandle) -> u32
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        OUT(uint32_t, 0) = (uint32_t)g_dev_state;
        resp_size(resp, 4);
        break;
    }
    case 21: {  // GetNpadId(DeviceHandle) -> u32
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        OUT(uint32_t, 0) = 0;  // NpadId::No1
        resp_size(resp, 4);
        break;
    }
    case 22: {  // GetApplicationAreaSize(DeviceHandle) -> u32
        if (!check_dev_handle(req, 0)) { resp->result = NFP_ERR_DEVICE_NOT_FOUND; break; }
        OUT(uint32_t, 0) = 0xD8;
        resp_size(resp, 4);
        break;
    }
    case 23: {  // AttachAvailabilityChangeEvent() -> copy handle
        resp->copy_handles[resp->ncopy++] = attach_event_locked(&g_avail_ev);
        break;
    }
    case 24:  // RecreateApplicationArea
        resp->result = NFP_ERR_NOT_SUPPORTED;
        break;
    default:
        LOG("[nfp] IUser cmd %u (ok)\n", req->cmd);
        resp_size(resp, 0x10);
        break;
    }
    pthread_mutex_unlock(&nfp_lock);
}

SERVICE("nfp:user", nfp_user) {
    (void)s;
    if (req->cmd == 0)
        ipc_return_object(resp, service_new("nfp:IUser", nfp_user_iface, NULL));
    else {
        LOG("[nfp] user static cmd %u (ok)\n", req->cmd);
        resp_size(resp, 0x10);
    }
}
