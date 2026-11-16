// aoc:u (add-on content, DLC) and the DLC data the game mounts (fsp-srv OpenDataStorageByDataId).
//
// Each DLC is a folder named by its content id under <game files>/dlc, holding the DLC's extracted romfs:
//   extracted/dlc/010055D009F79001/romfs/...
// A DLC's RomFS image is served from its romfs/ folder as the base game's is (runtime/services/fs.c): the header and
// tables laid out in memory, each file's bytes read from its own file - no image file kept next to it. A romfs.bin an
// older version built is used when the folder isn't there, and deleted when it is. The game sees each one as installed and owned: an add-on content id is the DLC base id
// (the application id + 0x1000) plus the DLC's index, and the game asks for the indices, then mounts each by id.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hle.h"
#include "../ipc.h"
#include "../mods.h"
#include "../romfs_build.h"

#define MAX_DLC 64

typedef struct {
    uint64_t id;
    char dir[1024];
    int fd;  // (an older version's romfs.bin; -1: the layout below)
    bool open;
    uint64_t size;
    RomfsLayout lay;
    int last_file, last_fd;  // (the file read last, kept open: reads come in runs within one file)
} Dlc;
static pthread_mutex_t dlc_lock = PTHREAD_MUTEX_INITIALIZER;
static Dlc dlcs[MAX_DLC];
static int ndlc = -1;  // -1: not scanned yet

static int by_id(const void *a, const void *b) {
    const Dlc *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id;
}

// <game files>/dlc: g_romfs_dir is <game files>/romfs.
static void dlc_root(char *out, size_t n) {
    const char *r = g_romfs_dir ? g_romfs_dir : "extracted/romfs";
    size_t l = strlen(r);
    while (l && (r[l - 1] == '/' || r[l - 1] == '\\')) l--;
    size_t cut = l;
    while (cut && r[cut - 1] != '/' && r[cut - 1] != '\\') cut--;  // (drop the last component, "romfs")
    snprintf(out, n, "%.*sdlc", (int)cut, r);
}

static void dlc_scan(void) {
    if (ndlc >= 0) return;
    ndlc = 0;
    char root[1024];
    dlc_root(root, sizeof root);
    DIR *d = opendir(root);
    if (!d) { LOG("[aoc] no DLC folder (%s): no add-on content\n", root); return; }
    struct dirent *e;
    while ((e = readdir(d)) && ndlc < MAX_DLC) {
        const char *nm = e->d_name;
        if (nm[0] == '.') continue;
        char *end;
        uint64_t id = strtoull(nm, &end, 16);
        if (strlen(nm) != 16 || *end || !id) {
            LOG("[aoc] %s/%s: not a 16-digit content id, skipped\n", root, nm);
            continue;
        }
        Dlc *x = &dlcs[ndlc];
        snprintf(x->dir, sizeof x->dir, "%s/%s", root, nm);
        // (offered to the game only with data: one it owns but can't mount aborts the game at start)
        char chk[1100];
        struct stat sb;
        char bin[1100];
        snprintf(bin, sizeof bin, "%s/romfs.bin", x->dir);
        bool has = stat(bin, &sb) == 0;
        snprintf(chk, sizeof chk, "%s/romfs", x->dir);
        bool folder = stat(chk, &sb) == 0;
        if (!has && !folder) {
            LOG("[aoc] %s: no romfs folder, left out\n", x->dir);
            continue;
        }
        if (folder && !has) {  // a packed DLC (.nsp / .nca / .xci) put where its extracted files belong: hung the game at start
            const char *packed = NULL;
            static char nmbuf[256];
            DIR *r = opendir(chk);
            if (r) {
                struct dirent *f;
                while ((f = readdir(r))) {
                    const char *dot = strrchr(f->d_name, '.');
                    if (dot && (!strcasecmp(dot, ".nsp") || !strcasecmp(dot, ".nca") || !strcasecmp(dot, ".xci") ||
                                !strcasecmp(dot, ".nsz"))) {
                        snprintf(nmbuf, sizeof nmbuf, "%s", f->d_name);
                        packed = nmbuf;
                        break;
                    }
                }
                closedir(r);
            }
            if (packed) {
                LOG("[aoc] %s: romfs holds %s, not the DLC's extracted files: left out (extract its romfs there)\n",
                    x->dir, packed);
                continue;
            }
        }
        x->id = id;
        x->fd = -1;
        x->open = false;
        x->last_file = -1;
        x->last_fd = -1;
        ndlc++;
    }
    closedir(d);
    qsort(dlcs, ndlc, sizeof *dlcs, by_id);
    for (int i = 0; i < ndlc; i++) {
        if (i && (dlcs[i].id & ~0xFFFull) != (dlcs[0].id & ~0xFFFull))
            LOG("[aoc] warning: %016llx belongs to another game than %016llx\n", (unsigned long long)dlcs[i].id,
                (unsigned long long)dlcs[0].id);
        LOG("[aoc] DLC %016llx (index %u)\n", (unsigned long long)dlcs[i].id, (unsigned)(dlcs[i].id & 0xFFF));
    }
}

static uint64_t base_id(void) { return ndlc > 0 ? dlcs[0].id & ~0xFFFull : 0; }

// The DLC's RomFS image: laid out from its romfs/ folder (an older version's romfs.bin if there's no folder). false: not
// available.
static bool dlc_open(Dlc *x) {
    if (x->open) return true;
    char src[1100], bin[1100], why[256] = "";
    snprintf(src, sizeof src, "%s/romfs", x->dir);
    struct stat sb;
    if (stat(src, &sb) == 0) {
        const char *const *stack;
        int n = mods_dlc_stack(src, x->id, &stack);  // (the pack's romfs, then enabled mods' files for it)
        if (!romfs_layout_stack(stack, n, &x->lay, NULL, NULL, why, sizeof why)) {
            LOG("[aoc] %s: %s\n", src, why);
            if (n > 1) FATAL("DLC mods could not be prepared: %s. Disable or fix the mod.\n", why);
            return false;
        }
        if (!x->lay.nfiles) {
            romfs_layout_free(&x->lay);
            LOG("[aoc] %s: empty layout, preserving any legacy image\n", src);
            return false;
        }
        x->size = x->lay.size;
        snprintf(bin, sizeof bin, "%s/romfs.bin", x->dir);
        struct stat old;
        if (lstat(bin, &old) == 0 && S_ISREG(old.st_mode) && unlink(bin) == 0)
            LOG("[aoc] removed %s after layout validation\n", bin);
    } else {
        snprintf(bin, sizeof bin, "%s/romfs.bin", x->dir);
        x->fd = open(bin, O_RDONLY);
        if (x->fd < 0) { LOG("[aoc] cannot open %s: %s\n", bin, strerror(errno)); return false; }
        fstat(x->fd, &sb);
        x->size = (uint64_t)sb.st_size;
    }
    x->open = true;
    return true;
}

// n bytes of the DLC's image at off (gaps and past the end: zeros). false: a file couldn't be read.
static bool dlc_read(Dlc *x, uint8_t *dst, uint64_t n, uint64_t off) {
    memset(dst, 0, n);
    if (x->fd >= 0) return off >= x->size || pread(x->fd, dst, n, (off_t)off) >= 0;
    const RomfsLayout *l = &x->lay;
    uint64_t end = off + n;
    if (off < ROMFS_DATA_START) memcpy(dst, l->header + off, (end < ROMFS_DATA_START ? end : ROMFS_DATA_START) - off);
    uint64_t ta = l->tables_off, tb = ta + l->tables_size;
    if (off < tb && end > ta) {
        uint64_t a = off > ta ? off : ta, b = end < tb ? end : tb;
        memcpy(dst + (a - off), l->tables + (a - ta), b - a);
    }
    int lo = 0, hi = l->nfiles - 1, first = l->nfiles;  // (the first file ending past off)
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (l->files[mid].start + l->files[mid].size > off) first = mid, hi = mid - 1;
        else lo = mid + 1;
    }
    bool ok = true;
    pthread_mutex_lock(&dlc_lock);
    for (int i = first; i < l->nfiles && l->files[i].start < end; i++) {
        const RomfsLayoutFile *f = &l->files[i];
        uint64_t a = off > f->start ? off : f->start, b = end < f->start + f->size ? end : f->start + f->size;
        if (a >= b) continue;
        if (x->last_file != i) {
            if (x->last_fd >= 0) close(x->last_fd);
            x->last_fd = open(f->path, O_RDONLY);
            x->last_file = i;
        }
        if (x->last_fd < 0 || pread(x->last_fd, dst + (a - off), b - a, (off_t)(a - f->start)) != (ssize_t)(b - a)) {
            LOG("[aoc] can't read %s\n", f->path);
            ok = false;
            break;
        }
    }
    pthread_mutex_unlock(&dlc_lock);
    return ok;
}

RT_ON_RESET(aoc_reset) {
    for (int i = 0; i < ndlc; i++) {
        Dlc *x = &dlcs[i];
        if (x->fd >= 0) close(x->fd), x->fd = -1;
        if (x->last_fd >= 0) close(x->last_fd), x->last_fd = -1;
        x->last_file = -1;
        if (x->open && x->lay.files) romfs_layout_free(&x->lay);
        x->open = false;
    }
}

// ------------------------------------------------------------------ IStorage of one DLC's RomFS
static void dlc_storage(Service *s, IpcReq *req, IpcResp *resp) {
    Dlc *x = s->state;
    switch (req->cmd) {
    case 0: {  // Read(offset, size) -> B buffer
        uint64_t off = IN(uint64_t, 0), size = IN(uint64_t, 8);
        IpcBuf *b = out_buf(req, 0);
        if (!b) { resp->result = MAKE_RESULT(2, 6001); break; }
        if (size > b->size) size = b->size;
        uint8_t *dst = (uint8_t *)(uintptr_t)b->addr;
        if (!dlc_read(x, dst, size, off)) LOG("[aoc] read %016llx at %llx failed\n", (unsigned long long)x->id, (unsigned long long)off);
        break;
    }
    case 4: OUT(uint64_t, 0) = x->size; resp_size(resp, 8); break;  // GetSize
    default: LOG("[aoc] IStorage cmd %u (ok)\n", req->cmd);
    }
}

// fsp-srv OpenDataStorageByDataId (services/fs.c): a DLC's data, or NULL when the id is no DLC here.
Service *aoc_open_storage(uint64_t id) {
    dlc_scan();
    for (int i = 0; i < ndlc; i++)
        if (dlcs[i].id == id) {
            if (!dlc_open(&dlcs[i])) return NULL;
            LOG("[aoc] mounted %016llx (%llu MB)\n", (unsigned long long)id, (unsigned long long)(dlcs[i].size >> 20));
            return service_new("IStorage(dlc)", dlc_storage, &dlcs[i]);
        }
    return NULL;
}

// ------------------------------------------------------------------ aoc:u
static uint32_t new_event(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// IPurchaseEventManager: no purchases happen here.
static void purchase_events(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 2: resp->copy_handles[resp->ncopy++] = new_event(); break;  // GetPurchasedEventReadableHandle
    case 3: case 4: resp->result = MAKE_RESULT(164, 400); break;     // PopPurchasedProductInfo: none available
    default: break;                                                    // SetDefaultDeliveryTarget, SetDeliveryTarget
    }
}

// The indices of the DLC, from `offset`, at most `count` and what fits in the out buffer.
static void list_indices(IpcReq *req, IpcResp *resp, uint32_t offset, uint32_t count) {
    IpcBuf *b = out_buf(req, 0);
    uint32_t cap = b ? (uint32_t)(b->size / 4) : 0, n = 0;
    for (int i = (int)offset; i < ndlc && n < count && n < cap; i++)
        ((uint32_t *)(uintptr_t)b->addr)[n++] = (uint32_t)(dlcs[i].id & 0xFFF);
    OUT(uint32_t, 0) = n;
    resp_size(resp, 4);
    LOG("[aoc] ListAddOnContent(%u, %u): %u\n", offset, count, n);
}

SERVICE("aoc:u", aoc_u) {
    (void)s;
    dlc_scan();
    switch (req->cmd) {
    case 0: case 2:  // CountAddOnContent(ByApplicationId)
        OUT(uint32_t, 0) = (uint32_t)ndlc;
        resp_size(resp, 4);
        LOG("[aoc] CountAddOnContent: %d\n", ndlc);
        break;
    case 1: case 3: list_indices(req, resp, IN(uint32_t, 0), IN(uint32_t, 4)); break;  // ListAddOnContent(ByApplicationId)
    case 4: case 5:  // GetAddOnContentBaseId(ByApplicationId)
        OUT(uint64_t, 0) = base_id();
        resp_size(resp, 8);
        break;
    case 6: case 7:  // PrepareAddOnContent(ByApplicationId)(index): everything is local
        LOG("[aoc] PrepareAddOnContent %u\n", IN(uint32_t, 0));
        break;
    case 8: case 10: resp->copy_handles[resp->ncopy++] = new_event(); break;  // GetAddOnContentListChangedEvent
    case 9: break;                                                           // GetAddOnContentLostErrorCode: none
    case 50: break;                                                          // CheckAddOnContentMountStatus: ok
    case 100: case 101:  // Create(Permanent)EcPurchasedEventManager
        ipc_return_object(resp, service_new("IPurchaseEventManager", purchase_events, NULL));
        break;
    default:
        LOG("[aoc] aoc:u cmd %u (ok)\n", req->cmd);
        resp_size(resp, 0x10);
    }
}
