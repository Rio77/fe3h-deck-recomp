// Mods (mods.h): the load order, the stack of folders the game's data image is laid out from, and the merged update
// indexes.
//
// Three Houses mods use the game's own update system. Each romfs/patchN/ of the game has an index, INFO0.bin: records
// of 0x120 bytes (an entry id, sizes, a flag, then a 0x100-byte "rom:/..." path), sorted by id; INFO2.bin holds their
// count. A mod ships a whole patchN/INFO0.bin: every record of the game's, some pointing at its own files instead, and
// new ones. Laid over each other, the last mod's index would hide every other mod's records, so when enabled mods
// ship an index, a merged one is written: the game's records, then each mod's differences from them in load order.
// It goes on top of the stack (<game files>/mods/.merged/romfs/patchN/), rebuilt at every start.
#include "mods.h"

#include "platform.h"  // (Windows: POSIX mkdir)

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG(...) fprintf(stderr, __VA_ARGS__)  // (as runtime/rt.h)

#define MAX_MODS 64
#define REC 0x120        // one INFO0.bin record
#define MAX_PATCH 9      // patch1 .. patch9
#define MAX_CONFLICT_LOG 20
static char g_mod_error[512];
const char *mods_last_error(void) { return g_mod_error; }

typedef struct {
    char name[256];
    char romfs[1400];  // <game files>/mods/<name>/romfs
    bool has_romfs;    // (false: only files for DLC packs)
    int new_files, replaced_game, replaced_mod;
} Mod;

static char g_root[1024];       // the game files folder
static char g_base[1100];       // its romfs
static char g_merged[1200];     // <game files>/mods/.merged/romfs
static Mod g_mods[MAX_MODS];
static int g_nmods;
static bool g_has_merged;
static const char *g_stack[MAX_MODS + 2];
static int g_stack_mod[MAX_MODS + 2];  // the mod each folder of the stack is (-1: the game's, the merged indexes)
static int g_conflicts;         // conflicts logged (the rest only counted)
static void (*g_sink)(void *ctx, const char *line);  // (mods_check_conflicts) conflicts go here instead of the log
static void *g_sink_ctx;
static bool g_quiet;            // (mods_check_conflicts) no log lines, nothing written

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void mkdirs(const char *path) {  // (each missing folder on the way)
    char p[1200];
    strlcpy(p, path, sizeof p);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char k = *c;
            *c = 0;
            mkdir(p, 0755);
            *c = k;
        }
    mkdir(p, 0755);
}

// A whole file into memory (NULL: missing / unreadable). *size: its size.
static uint8_t *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    *size = b ? (size_t)n : 0;
    return b;
}

static bool write_all(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

// ------------------------------------------------------------------ load order
// The folder names listed in <root>/mods/load_order.txt, in order (no duplicates, nothing that isn't a folder name).
static int read_order(const char *root, char (*names)[256], int max) {
    char path[1200], line[256];
    snprintf(path, sizeof path, "%s/mods/load_order.txt", root);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    bool first = true;
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        if (first && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s += 3;  // (UTF-8 mark)
        first = false;
        while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (!l || s[0] == '#') continue;
        if (s[0] == '.' || strchr(s, '/') || strchr(s, '\\')) {
            if (!g_quiet) LOG("[mods] \"%s\": not a mod folder name, skipped\n", s);
            continue;
        }
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= !strcmp(names[i], s);
        if (dup) continue;
        if (n == max) {
            if (!g_quiet) LOG("[mods] more than %d mods listed: the rest are skipped\n", max);
            break;
        }
        strlcpy(names[n++], s, 256);
    }
    fclose(f);
    return n;
}

// g_mods from a list of folder names (those that are mods: a romfs or a dlc folder)
static void set_mods(char (*names)[256], int n) {
    g_nmods = 0;
    for (int i = 0; i < n; i++) {
        const char *s = names[i];
        Mod *m = &g_mods[g_nmods];
        memset(m, 0, sizeof *m);
        strlcpy(m->name, s, sizeof m->name);
        snprintf(m->romfs, sizeof m->romfs, "%s/mods/%s/romfs", g_root, s);
        m->has_romfs = is_dir(m->romfs);
        char sub[1400];
        snprintf(sub, sizeof sub, "%s/mods/%s/dlc", g_root, s);
        if (!m->has_romfs && !is_dir(sub)) {
            if (!g_quiet) LOG("[mods] %s: no romfs or dlc folder in mods/%s, skipped\n", s, s);
            continue;
        }
        snprintf(sub, sizeof sub, "%s/mods/%s/exefs", g_root, s);
        if (is_dir(sub) && !g_quiet)
            LOG("[mods] %s: has an exefs folder (code changes): those can't apply to the recompiled game, only its data "
                "is used\n", s);
        g_nmods++;
    }
}

static void load_order(void) {
    static char names[MAX_MODS][256];
    set_mods(names, read_order(g_root, names, MAX_MODS));
}

// ------------------------------------------------------------------ merged update indexes
typedef struct { uint64_t id; const uint8_t *rec, *base; int owner; } Entry;  // owner: -1 the game's, else a mod

static int by_id(const void *a, const void *b) {
    uint64_t x = ((const Entry *)a)->id, y = ((const Entry *)b)->id;
    return x < y ? -1 : x > y;
}

static uint64_t rec_id(const uint8_t *r) { uint64_t v; memcpy(&v, r, 8); return v; }

static int by_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static bool valid_index(const uint8_t *data, size_t size) {
    if (!data || !size || size % REC) return false;
    size_t n = size / REC;
    uint64_t *ids = malloc(n * sizeof *ids);
    if (!ids) return false;
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *record = data + i * REC, *path = record + 0x20;
        ids[i] = rec_id(record);
        if (memcmp(path, "rom:/", 5) || !memchr(path, 0, 0x100)) ok = false;
    }
    qsort(ids, n, sizeof *ids, by_u64);
    for (size_t i = 1; i < n; i++) if (ids[i] == ids[i-1]) ok = false;
    free(ids);
    return ok;
}

static void conflict(const char *fmt, ...) {
    if (g_sink) {
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        g_conflicts++;
        g_sink(g_sink_ctx, line);
        return;
    }
    if (g_conflicts++ < MAX_CONFLICT_LOG) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "[mods] conflict: ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    } else if (g_conflicts == MAX_CONFLICT_LOG + 1) {
        LOG("[mods] (more conflicts: counted, not listed)\n");
    }
}

// Merges patchN/INFO0.bin of the game and the enabled mods that ship one. false: no mod ships one.
static bool merge_index(int pn) {
    char path[1600], merged0[1300], merged2[1300];
    snprintf(merged0, sizeof merged0, "%s/patch%d/INFO0.bin", g_merged, pn);
    snprintf(merged2, sizeof merged2, "%s/patch%d/INFO2.bin", g_merged, pn);
    uint8_t *mod_idx[MAX_MODS] = {0};
    size_t mod_size[MAX_MODS] = {0};
    int nidx = 0;
    for (int i = 0; i < g_nmods; i++) {
        snprintf(path, sizeof path, "%s/patch%d/INFO0.bin", g_mods[i].romfs, pn);
        if (!g_mods[i].has_romfs || !is_file(path)) continue;
        mod_idx[i] = read_all(path, &mod_size[i]);
        if (!valid_index(mod_idx[i], mod_size[i])) {
            snprintf(g_mod_error, sizeof g_mod_error, "%s: invalid or unreadable patch%d/INFO0.bin", g_mods[i].name, pn);
            if (!g_quiet) LOG("[mods] %s: patch%d/INFO0.bin isn't an index (%zu bytes), not merged\n", g_mods[i].name, pn, mod_size[i]);
            free(mod_idx[i]), mod_idx[i] = NULL;
            continue;
        }
        nidx++;
    }
    if (!nidx) {  // (a merged index from an earlier start, for mods no longer on: not in the stack, removed anyway)
        if (!g_quiet) remove(merged0), remove(merged2);
        return false;
    }
    size_t bsize = 0;
    snprintf(path, sizeof path, "%s/patch%d/INFO0.bin", g_base, pn);
    uint8_t *base = read_all(path, &bsize);
    if (is_file(path) && (!base || bsize % REC)) {
        snprintf(g_mod_error, sizeof g_mod_error, "Invalid or unreadable base patch%d/INFO0.bin; cannot safely merge mods", pn);
        free(base);
        for (int i = 0; i < g_nmods; i++) free(mod_idx[i]);
        return false;
    }
    size_t n = bsize / REC, cap = n + 64;
    Entry *e = malloc(sizeof *e * cap);
    for (size_t i = 0; i < n; i++) e[i] = (Entry){rec_id(base + i * REC), base + i * REC, base + i * REC, -1};
    qsort(e, n, sizeof *e, by_id);

    for (int m = 0; m < g_nmods; m++) {
        if (!mod_idx[m]) continue;
        int changed = 0, added = 0;
        size_t nb = n;  // (entries before this mod: looked up by id; this mod's own new ones come after)
        for (size_t r = 0; r < mod_size[m] / REC; r++) {
            const uint8_t *rec = mod_idx[m] + r * REC;
            Entry key = {rec_id(rec), NULL, NULL, 0};
            Entry *x = bsearch(&key, e, nb, sizeof *e, by_id);
            if (x) {
                if (x->base && !memcmp(rec, x->base, REC)) continue;  // the game's record: unchanged by this mod
                if (!memcmp(rec, x->rec, REC)) continue;
                if (x->owner >= 0)
                    conflict("patch%d index entry %llu: changed by %s and by %s (later in the load order: used)", pn,
                             (unsigned long long)key.id, g_mods[x->owner].name, g_mods[m].name);
                x->rec = rec, x->owner = m;
                changed++;
            } else {
                if (n == cap) e = realloc(e, sizeof *e * (cap *= 2));
                e[n++] = (Entry){key.id, rec, NULL, m};
                added++;
            }
        }
        qsort(e, n, sizeof *e, by_id);
        if (!g_quiet) LOG("[mods] %s: patch%d index: %d entr%s changed, %d added\n", g_mods[m].name, pn, changed, changed == 1 ? "y" : "ies", added);
    }

    // A mod that points an entry at its own file hides another mod's replacement of the game's file for that entry.
    for (size_t i = 0; i < n; i++) {
        if (e[i].owner < 0 || !e[i].base || !memcmp(e[i].rec + 0x20, e[i].base + 0x20, 0x100)) continue;  // (same file)
        char rel[0x101];
        memcpy(rel, e[i].base + 0x20, 0x100);
        rel[0x100] = 0;
        if (strncmp(rel, "rom:/", 5)) continue;
        for (int m = 0; m < g_nmods; m++) {
            if (m == e[i].owner || !g_mods[m].has_romfs) continue;
            snprintf(path, sizeof path, "%s/%s", g_mods[m].romfs, rel + 5);
            if (is_file(path))
                conflict("%s: %s changes this entry of the update index, so %s's copy of the file isn't used",
                         rel + 5, g_mods[e[i].owner].name, g_mods[m].name);
        }
    }

    if (g_quiet) {  // (a check only)
        free(e), free(base);
        for (int i = 0; i < g_nmods; i++) free(mod_idx[i]);
        return true;
    }
    uint8_t *out = malloc(n * REC + 1);
    for (size_t i = 0; i < n; i++) memcpy(out + i * REC, e[i].rec, REC);
    uint8_t info2[16] = {0};
    size_t s2 = 0;
    snprintf(path, sizeof path, "%s/patch%d/INFO2.bin", g_base, pn);
    uint8_t *b2 = read_all(path, &s2);
    for (int m = g_nmods - 1; !b2 && m >= 0; m--) {  // (the game has none for this folder: the last mod's)
        if (!g_mods[m].has_romfs) continue;
        snprintf(path, sizeof path, "%s/patch%d/INFO2.bin", g_mods[m].romfs, pn);
        b2 = read_all(path, &s2);
    }
    if (b2) memcpy(info2, b2, s2 < 16 ? s2 : 16);
    free(b2);
    uint64_t count = n;
    memcpy(info2, &count, 8);  // the entry count; the second value is kept as it is
    snprintf(path, sizeof path, "%s/patch%d", g_merged, pn);
    mkdirs(path);
    bool ok = write_all(merged0, out, n * REC) && write_all(merged2, info2, sizeof info2);
    if (ok) LOG("[mods] patch%d index merged: %zu entries (the game's: %zu)\n", pn, n, bsize / REC);
    else LOG("[mods] can't write the merged patch%d index in %s: mods' index changes may clash\n", pn, g_merged);
    free(out), free(e), free(base);
    for (int i = 0; i < g_nmods; i++) free(mod_idx[i]);
    if (!ok) snprintf(g_mod_error, sizeof g_mod_error, "Could not write merged patch%d indexes; check free space and permissions", pn);
    return ok;
}

// ------------------------------------------------------------------ the stack
static void set_root(const char *root) {
    strlcpy(g_root, root && root[0] ? root : ".", sizeof g_root);
    snprintf(g_base, sizeof g_base, "%s/romfs", g_root);
    snprintf(g_merged, sizeof g_merged, "%s/mods/.merged/romfs", g_root);
}

int mods_romfs_stack(const char *romfs_dir, const char *const **dirs) {
    g_nmods = 0, g_has_merged = false, g_conflicts = 0;
    g_mod_error[0] = 0;
    char root[1024];
    strlcpy(root, romfs_dir, sizeof root);
    size_t cut = strlen(root);
    while (cut && root[cut - 1] != '/' && root[cut - 1] != '\\') cut--;  // (drop the last component, "romfs")
    root[cut ? cut - 1 : 0] = 0;
    set_root(root);
    strlcpy(g_base, romfs_dir, sizeof g_base);

    int n = 0;
    g_stack_mod[n] = -1, g_stack[n++] = g_base;
    load_order();
    if (g_nmods) {
        char list[2048] = "";
        for (int i = 0; i < g_nmods; i++) {
            if (i) strlcat(list, ", ", sizeof list);
            strlcat(list, g_mods[i].name, sizeof list);
        }
        LOG("[mods] load order (later wins): %s\n", list);
        for (int i = 0; i < g_nmods; i++)
            if (g_mods[i].has_romfs) g_stack_mod[n] = i, g_stack[n++] = g_mods[i].romfs;
        for (int pn = 1; pn <= MAX_PATCH; pn++) g_has_merged |= merge_index(pn);
        if (g_has_merged) g_stack_mod[n] = -1, g_stack[n++] = g_merged;
    }
    *dirs = g_stack;
    return n;
}

int mods_dlc_stack(const char *dlc_romfs, uint64_t id, const char *const **dirs) {
    static char base[1200], mod[MAX_MODS][1400];
    static const char *stack[MAX_MODS + 1];
    strlcpy(base, dlc_romfs, sizeof base);
    int n = 0;
    stack[n++] = base;
    for (int i = 0; i < g_nmods; i++) {
        snprintf(mod[i], sizeof mod[i], "%s/mods/%.255s/dlc/%016llX/romfs", g_root, g_mods[i].name, (unsigned long long)id);
        if (!is_dir(mod[i]))  // (the folder name in lower case: case matters outside Windows)
            snprintf(mod[i], sizeof mod[i], "%s/mods/%.255s/dlc/%016llx/romfs", g_root, g_mods[i].name, (unsigned long long)id);
        if (!is_dir(mod[i])) continue;
        LOG("[mods] %s: DLC %016llX's data from mods/%s/dlc/%016llX\n", g_mods[i].name, (unsigned long long)id,
            g_mods[i].name, (unsigned long long)id);
        stack[n++] = mod[i];
    }
    *dirs = stack;
    return n;
}

static bool is_index(const char *rel) {  // patchN/INFO0.bin, patchN/INFO2.bin (merged, not laid over each other)
    const char *slash = strrchr(rel, '/');
    return slash && !strncmp(rel, "patch", 5) && (!strcmp(slash, "/INFO0.bin") || !strcmp(slash, "/INFO2.bin"));
}

void mods_note_file(void *ctx, int dir, const char *rel, int replaced) {
    (void)ctx;
    if (dir < 1 || dir > MAX_MODS + 1 || g_stack_mod[dir] < 0) return;  // (the merged indexes)
    if (is_index(rel)) return;  // (counted in the index lines instead)
    Mod *m = &g_mods[g_stack_mod[dir]];
    if (replaced < 0) m->new_files++;
    else if (replaced == 0 || g_stack_mod[replaced] < 0) m->replaced_game++;
    else {
        m->replaced_mod++;
        conflict("%s: in %s and in %s (later in the load order: used)", rel, g_mods[g_stack_mod[replaced]].name, m->name);
    }
}

void mods_report(void) {
    for (int i = 0; i < g_nmods; i++) {
        Mod *m = &g_mods[i];
        LOG("[mods] %s: %d new file%s, %d replacing the game's", m->name, m->new_files, m->new_files == 1 ? "" : "s", m->replaced_game);
        if (m->replaced_mod) LOG(", %d replacing earlier mods'", m->replaced_mod);
        LOG("\n");
    }
    if (g_conflicts) LOG("[mods] %d conflict%s between enabled mods (see above)\n", g_conflicts, g_conflicts == 1 ? "" : "s");
}

// ------------------------------------------------------------------ for the start screen's Mods tab
typedef void (*WalkFn)(void *ctx, const char *rel, const char *host);

static void walk(const char *dir, const char *rel, WalkFn fn, void *ctx, int depth) {
    if (depth > 32) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;  // (as the layout: ".", "..", hidden files)
        char host[1600], r[1024];
        snprintf(host, sizeof host, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(host, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(host, r, fn, ctx, depth + 1);
        else if (S_ISREG(st.st_mode)) fn(ctx, r, host);
    }
    closedir(d);
}

static int english_of(const char *s) {  // the English text a path belongs to: MOD_TEXT_US / _EU / NONE
    return strstr(s, "ENG_E") ? MOD_TEXT_EU : strstr(s, "ENG_U") ? MOD_TEXT_US : MOD_TEXT_NONE;
}

static void count_file(void *ctx, const char *rel, const char *host) {
    (void)host;
    ModInfo *m = ctx;
    if (is_index(rel)) return;
    char base[1600];
    snprintf(base, sizeof base, "%s/%s", g_base, rel);
    if (is_file(base)) m->replaced_files++;
    else m->new_files++;
    m->text |= english_of(rel);
}

// The mod's update-index changes against the game's (counts, and the English text the changed entries point at)
static void count_index(ModInfo *m, const char *romfs) {
    char path[2000];
    for (int pn = 1; pn <= MAX_PATCH; pn++) {
        size_t ms = 0, bs = 0;
        snprintf(path, sizeof path, "%s/patch%d/INFO0.bin", romfs, pn);
        uint8_t *mi = read_all(path, &ms);
        if (!mi) continue;
        snprintf(path, sizeof path, "%s/patch%d/INFO0.bin", g_base, pn);
        uint8_t *bi = read_all(path, &bs);
        size_t nb = bs / REC;
        Entry *e = malloc(sizeof *e * (nb + 1));
        for (size_t i = 0; i < nb; i++) e[i] = (Entry){rec_id(bi + i * REC), bi + i * REC, bi + i * REC, -1};
        qsort(e, nb, sizeof *e, by_id);
        for (size_t r = 0; r + REC <= ms; r += REC) {
            Entry key = {rec_id(mi + r), NULL, NULL, 0};
            Entry *x = bsearch(&key, e, nb, sizeof *e, by_id);
            if (x && !memcmp(x->base, mi + r, REC)) continue;
            if (x) m->index_changed++;
            else m->index_added++;
            char p[0x101];
            memcpy(p, mi + r + 0x20, 0x100), p[0x100] = 0;
            m->text |= english_of(p);
            if (x) {
                memcpy(p, x->base + 0x20, 0x100), p[0x100] = 0;
                m->text |= english_of(p);
            }
        }
        free(e), free(mi), free(bi);
    }
}

static int by_order(const void *a, const void *b) {  // on (by place) first, then off (by name)
    const ModInfo *x = a, *y = b;
    if (x->place && y->place) return x->place - y->place;
    if (x->place || y->place) return x->place ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

int mods_scan(const char *game_root, ModInfo *out, int max) {
    set_root(game_root);
    static char order[MAX_MODS][256];
    g_quiet = true;
    int norder = read_order(g_root, order, MAX_MODS);
    g_quiet = false;
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/mods", g_root);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;  // (.merged)
        char p[1500];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!is_dir(p)) continue;  // (load_order.txt)
        ModInfo *m = &out[n];
        memset(m, 0, sizeof *m);
        strlcpy(m->name, e->d_name, sizeof m->name);
        for (int i = 0; i < norder; i++)
            if (!strcmp(order[i], m->name)) m->place = i + 1;
        char romfs[1600];
        snprintf(romfs, sizeof romfs, "%s/romfs", p);
        m->has_romfs = is_dir(romfs);
        snprintf(romfs + strlen(p), sizeof romfs - strlen(p), "/exefs");
        m->has_exefs = is_dir(romfs);
        snprintf(romfs, sizeof romfs, "%s/dlc", p);
        DIR *dd = opendir(romfs);
        if (dd) {
            struct dirent *pk;
            while ((pk = readdir(dd))) {
                char pr[1900];
                snprintf(pr, sizeof pr, "%s/%s/romfs", romfs, pk->d_name);
                if (pk->d_name[0] != '.' && is_dir(pr)) m->dlc_packs++;
            }
            closedir(dd);
        }
        if (m->has_romfs) {
            snprintf(romfs, sizeof romfs, "%s/romfs", p);
            walk(romfs, "", count_file, m, 0);
            count_index(m, romfs);
        }
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof *out, by_order);
    int place = 0;  // (places 1, 2, 3... with no gaps: names listed but missing are dropped)
    for (int i = 0; i < n; i++)
        if (out[i].place) out[i].place = ++place;
    return n;
}

bool mods_save_order(const char *game_root, const ModInfo *mods, int n) {
    set_root(game_root);
    char path[1200];
    snprintf(path, sizeof path, "%s/mods", g_root);
    mkdirs(path);
    snprintf(path, sizeof path, "%s/mods/load_order.txt", g_root);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "# Mods that are on, one folder name per line. Where two change the same thing, the later one wins.\n"
               "# (The start screen's Settings > Mods writes this file.)\n");
    for (int place = 1; place <= n; place++)
        for (int i = 0; i < n; i++)
            if (mods[i].place == place) fprintf(f, "%s\n", mods[i].name);
    return fclose(f) == 0;
}

typedef struct { const char *rel; int mod; } Owned;
typedef struct { Owned *v; int n, cap, mod; const char *prefix; } OwnedList;

static void own_file(void *ctx, const char *rel, const char *host) {
    (void)host;
    OwnedList *l = ctx;
    if (!l->prefix[0] && is_index(rel)) return;  // (merged)
    if (l->n == l->cap) l->v = realloc(l->v, sizeof *l->v * (l->cap = l->cap ? l->cap * 2 : 1024));
    char *r = malloc(strlen(l->prefix) + strlen(rel) + 1);
    strcpy(r, l->prefix), strcat(r, rel);
    l->v[l->n++] = (Owned){r, l->mod};
}

static int by_rel(const void *a, const void *b) {
    const Owned *x = a, *y = b;
    int c = strcmp(x->rel, y->rel);
    return c ? c : x->mod - y->mod;
}

int mods_check_conflicts(const char *game_root, const ModInfo *mods, int n, void (*line)(void *ctx, const char *text),
                         void *ctx) {
    set_root(game_root);
    static char names[MAX_MODS][256];
    int k = 0;
    for (int place = 1; place <= n && k < MAX_MODS; place++)
        for (int i = 0; i < n; i++)
            if (mods[i].place == place) strlcpy(names[k++], mods[i].name, 256);
    g_quiet = true, g_sink = line, g_sink_ctx = ctx, g_conflicts = 0;
    set_mods(names, k);
    for (int pn = 1; pn <= MAX_PATCH; pn++) merge_index(pn);
    // the same file in two mods (the game's data, and each DLC pack's)
    OwnedList l = {0};
    for (int m = 0; m < g_nmods; m++) {
        char p[1400];
        l.mod = m, l.prefix = "";
        if (g_mods[m].has_romfs) walk(g_mods[m].romfs, "", own_file, &l, 0);
        snprintf(p, sizeof p, "%s/mods/%.255s/dlc", g_root, g_mods[m].name);
        DIR *dd = opendir(p);
        if (dd) {
            struct dirent *pk;
            while ((pk = readdir(dd))) {
                if (pk->d_name[0] == '.') continue;
                char pr[1700], pre[300];
                snprintf(pr, sizeof pr, "%s/%s/romfs", p, pk->d_name);
                snprintf(pre, sizeof pre, "DLC %s: ", pk->d_name);
                for (char *c = pre; *c; c++) *c = (char)toupper((unsigned char)*c);
                l.prefix = pre;
                walk(pr, "", own_file, &l, 0);
            }
            closedir(dd);
        }
        l.prefix = "";
    }
    qsort(l.v, l.n, sizeof *l.v, by_rel);
    for (int i = 0; i < l.n; i++) {
        int j = i;
        while (j + 1 < l.n && !strcmp(l.v[j + 1].rel, l.v[i].rel)) j++;
        if (j > i)  // (in load order: the last one is used)
            conflict("%s: in %s and in %s (later in the load order: used)", l.v[i].rel, g_mods[l.v[j - 1].mod].name,
                     g_mods[l.v[j].mod].name);
        i = j;
    }
    for (int i = 0; i < l.n; i++) free((char *)l.v[i].rel);
    free(l.v);
    int found = g_conflicts;
    g_quiet = false, g_sink = NULL, g_sink_ctx = NULL, g_conflicts = 0, g_nmods = 0;
    return found;
}
