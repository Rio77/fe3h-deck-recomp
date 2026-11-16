// Test of mod stacking (runtime/mods.c, romfs_layout_stack in runtime/romfs_build.c) on made-up folders: no game data.
//   make test-mods        (exit 0: passed; works in build/test_mods_tmp, removed afterwards)
#include "mods.h"
#include "romfs_build.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <signal.h>
#endif

#include "platform.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static char T[512];

static void mk(const char *rel) {  // folders on the way to T/rel
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", T, rel);
    for (char *c = p + strlen(T) + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
}
static void put(const char *rel, const void *data, size_t n) {
    mk(rel);
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", T, rel);
    FILE *f = fopen(p, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}
static void puts_(const char *rel, const char *s) { put(rel, s, strlen(s)); }

// INFO0 record: id, size, size, flag, path
static void rec(uint8_t *r, uint64_t id, uint64_t size, const char *path) {
    memset(r, 0, 0x120);
    memcpy(r, &id, 8), memcpy(r + 8, &size, 8), memcpy(r + 16, &size, 8);
    strncpy((char *)r + 0x20, path, 0xFF);
}

// ---- reading the laid-out image back: path -> the host file whose data it serves
static const char *lookup(const RomfsLayout *L, const char *path) {
    uint64_t h[10];
    memcpy(h, L->header, sizeof h);
    const uint8_t *dm = L->tables + (h[3] - L->tables_off), *fm = L->tables + (h[7] - L->tables_off);
    uint32_t cur = 0;
    char buf[512];
    strncpy(buf, path, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *name = buf, *slash;
    while ((slash = strchr(name, '/'))) {  // folders
        *slash = 0;
        uint32_t c;
        memcpy(&c, dm + cur + 8, 4);
        while (c != 0xFFFFFFFFu) {
            uint32_t nl;
            memcpy(&nl, dm + c + 0x14, 4);
            if (nl == strlen(name) && !memcmp(dm + c + 0x18, name, nl)) break;
            memcpy(&c, dm + c + 4, 4);
        }
        if (c == 0xFFFFFFFFu) return NULL;
        cur = c, name = slash + 1;
    }
    uint32_t f;
    memcpy(&f, dm + cur + 12, 4);
    const char *prev = "";
    while (f != 0xFFFFFFFFu) {
        uint32_t nl;
        memcpy(&nl, fm + f + 0x1C, 4);
        char nm[256] = {0};
        memcpy(nm, fm + f + 0x20, nl < 255 ? nl : 255);
        CHECK(strcmp(prev, nm) < 0 || !prev[0], "files of a folder not in name order (%s, %s)", prev, nm);
        if (!strcmp(nm, name)) {
            uint64_t data;
            memcpy(&data, fm + f + 8, 8);
            for (int i = 0; i < L->nfiles; i++)
                if (L->files[i].start == ROMFS_DATA_START + data) return L->files[i].path;
            return NULL;
        }
        static char keep[256];
        strcpy(keep, nm), prev = keep;
        memcpy(&f, fm + f + 4, 4);
    }
    return NULL;
}

static void count_line(void *ctx, const char *text) {
    (*(int *)ctx)++;
    fprintf(stderr, "  conflict line: %s\n", text);
}

static int ends(const char *s, const char *t) { return s && strlen(s) >= strlen(t) && !strcmp(s + strlen(s) - strlen(t), t); }

int main(void) {
    snprintf(T, sizeof T, "build/test_mods_tmp");
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf \"%s\"", T);
    if (system(cmd)) {}  // (a run that stopped halfway)
    mkdir("build", 0755);
    mkdir(T, 0755);
    // the game: two data files, an update folder with its index (ids 0, 5, 9)
    puts_("romfs/nx/a.bin", "game a");
    puts_("romfs/patch4/nx/data/fixed.bin", "game fixed");
    uint8_t idx[3 * 0x120];
    rec(idx, 0, 6, "rom:/patch4/common/msg.bin");
    rec(idx + 0x120, 5, 10, "rom:/patch4/nx/data/fixed.bin");
    rec(idx + 0x240, 9, 4, "rom:/patch2/x.bin");
    put("romfs/patch4/INFO0.bin", idx, sizeof idx);
    uint64_t info2[2] = {3, 4062};
    put("romfs/patch4/INFO2.bin", info2, sizeof info2);
    puts_("romfs/patch4/common/msg.bin", "game msg");

    // mod A (an overhaul): replaces fixed.bin in place (its record: new size), msg.bin in place, adds id 7 -> mods/a/new.bai
    puts_("mods/Mod A/romfs/patch4/nx/data/fixed.bin", "A fixed longer");
    puts_("mods/Mod A/romfs/patch4/common/msg.bin", "A msg");
    puts_("mods/Mod A/romfs/mods/a/new.bai", "A new map");
    uint8_t ia[4 * 0x120];
    memcpy(ia, idx, sizeof idx);
    rec(ia + 0x120, 5, 14, "rom:/patch4/nx/data/fixed.bin");
    rec(ia + 0x360, 7, 9, "rom:/mods/a/new.bai");
    put("mods/Mod A/romfs/patch4/INFO0.bin", ia, sizeof ia);
    uint64_t ia2[2] = {4, 4062};
    put("mods/Mod A/romfs/patch4/INFO2.bin", ia2, sizeof ia2);
    mk("mods/Mod A/exefs/x");

    // mod B (a translation): points id 0 at mods/0 (hides A's msg.bin), adds id 12 -> mods/12; also ships mods/a/new.bai
    puts_("mods/Mod B/romfs/mods/0", "B text");
    puts_("mods/Mod B/romfs/mods/12", "B extra");
    puts_("mods/Mod B/romfs/mods/a/new.bai", "B map");
    uint8_t ib[4 * 0x120];
    memcpy(ib, idx, sizeof idx);
    rec(ib, 0, 6, "rom:/mods/0");
    rec(ib + 0x360, 12, 7, "rom:/mods/12");
    put("mods/Mod B/romfs/patch4/INFO0.bin", ib, sizeof ib);
    put("mods/Mod B/romfs/patch4/INFO2.bin", ia2, sizeof ia2);

    // ---- no load order: the game alone
    char base[600];
    snprintf(base, sizeof base, "%s/romfs", T);
    const char *const *dirs;
    int n = mods_romfs_stack(base, &dirs);
    CHECK(n == 1, "no load_order.txt: %d folders", n);

    // ---- both mods, B last (plus noise in the list)
    puts_("mods/load_order.txt", "\xEF\xBB\xBF# my mods\r\nMod A\r\n\r\nMissing Mod\r\n  Mod B  \r\nMod A\r\n");
    n = mods_romfs_stack(base, &dirs);
    CHECK(n == 4, "stack: %d folders (want the game, A, B, merged)", n);
    RomfsLayout L;
    char why[256];
    CHECK(romfs_layout_stack(dirs, n, &L, mods_note_file, NULL, why, sizeof why), "layout: %s", why);
    mods_report();
    CHECK(ends(lookup(&L, "nx/a.bin"), "/romfs/nx/a.bin"), "game file kept");
    CHECK(ends(lookup(&L, "patch4/nx/data/fixed.bin"), "Mod A/romfs/patch4/nx/data/fixed.bin"), "A's replacement");
    CHECK(ends(lookup(&L, "patch4/common/msg.bin"), "Mod A/romfs/patch4/common/msg.bin"), "A's msg");
    CHECK(ends(lookup(&L, "mods/a/new.bai"), "Mod B/romfs/mods/a/new.bai"), "same path in A and B: B (later) wins");
    CHECK(ends(lookup(&L, "mods/0"), "Mod B/romfs/mods/0"), "B's new file");
    CHECK(ends(lookup(&L, "patch4/INFO0.bin"), "mods/.merged/romfs/patch4/INFO0.bin"), "merged index on top");
    CHECK(lookup(&L, "patch4/nope.bin") == NULL, "missing file");

    // the merged index: ids 0 (B), 5 (A), 7 (A), 9 (game), 12 (B), sorted; INFO2 count 5, second value kept
    char p[700];
    snprintf(p, sizeof p, "%s/mods/.merged/romfs/patch4/INFO0.bin", T);
    FILE *f = fopen(p, "rb");
    uint8_t m[8 * 0x120];
    size_t got = f ? fread(m, 1, sizeof m, f) : 0;
    if (f) fclose(f);
    CHECK(got == 5 * 0x120, "merged index: %zu bytes", got);
    uint64_t want_id[5] = {0, 5, 7, 9, 12};
    const char *want_path[5] = {"rom:/mods/0", "rom:/patch4/nx/data/fixed.bin", "rom:/mods/a/new.bai", "rom:/patch2/x.bin", "rom:/mods/12"};
    for (int i = 0; i < 5 && got == 5 * 0x120; i++) {
        uint64_t id, size;
        memcpy(&id, m + i * 0x120, 8), memcpy(&size, m + i * 0x120 + 8, 8);
        CHECK(id == want_id[i], "record %d: id %llu", i, (unsigned long long)id);
        CHECK(!strcmp((char *)m + i * 0x120 + 0x20, want_path[i]), "record %d: %s", i, (char *)m + i * 0x120 + 0x20);
        if (id == 5) CHECK(size == 14, "id 5: A's size");
    }
    snprintf(p, sizeof p, "%s/mods/.merged/romfs/patch4/INFO2.bin", T);
    f = fopen(p, "rb");
    uint64_t i2[2] = {0};
    if (f) { if (fread(i2, 1, 16, f) != 16) i2[0] = 0; fclose(f); }
    CHECK(i2[0] == 5 && i2[1] == 4062, "merged INFO2: %llu %llu", (unsigned long long)i2[0], (unsigned long long)i2[1]);
    romfs_layout_free(&L);

    // DLC: A ships a replacement DATA0.bin for pack ...004, nothing for ...001
    puts_("dlc/010055D009F79004/romfs/DATA0.bin", "pack data0");
    puts_("dlc/010055D009F79004/romfs/DATA1.bin", "pack data1");
    puts_("mods/Mod A/dlc/010055D009F79004/romfs/DATA0.bin", "A pack data0");
    char pack[700];
    snprintf(pack, sizeof pack, "%s/dlc/010055D009F79004/romfs", T);
    n = mods_dlc_stack(pack, 0x010055D009F79004ull, &dirs);
    CHECK(n == 2, "DLC 004: %d folders", n);
    CHECK(romfs_layout_stack(dirs, n, &L, NULL, NULL, why, sizeof why), "DLC layout: %s", why);
    CHECK(ends(lookup(&L, "DATA0.bin"), "Mod A/dlc/010055D009F79004/romfs/DATA0.bin"), "A's pack file");
    CHECK(ends(lookup(&L, "DATA1.bin"), "/dlc/010055D009F79004/romfs/DATA1.bin"), "the pack's own file");
    romfs_layout_free(&L);
    CHECK(mods_dlc_stack(pack, 0x010055D009F79001ull, &dirs) == 1, "DLC 001: no mod files");

    // ---- only B: its index alone merged; A's files gone
    puts_("mods/load_order.txt", "Mod B\n");
    n = mods_romfs_stack(base, &dirs);
    CHECK(n == 3, "B alone: %d folders", n);
    CHECK(romfs_layout_stack(dirs, n, &L, mods_note_file, NULL, why, sizeof why), "layout: %s", why);
    CHECK(ends(lookup(&L, "patch4/nx/data/fixed.bin"), "/romfs/patch4/nx/data/fixed.bin") &&
          !strstr(lookup(&L, "patch4/nx/data/fixed.bin"), "Mod A"), "A off: game's file");
    romfs_layout_free(&L);

    // ---- the game alone through the stack = romfs_layout
    RomfsLayout a, b;
    CHECK(romfs_layout(base, &a, why, sizeof why) && romfs_layout_stack(dirs, 1, &b, NULL, NULL, why, sizeof why), "layouts");
    CHECK(a.size == b.size && a.tables_size == b.tables_size && !memcmp(a.tables, b.tables, a.tables_size) &&
          !memcmp(a.header, b.header, sizeof a.header), "one folder: the same image as before");
    romfs_layout_free(&a), romfs_layout_free(&b);

    // ---- the Mods tab's view: scan, save, conflicts
    puts_("mods/Mod B/romfs/nx/event/text/ENG_E/TEXT_1.bin", "B eu text");
    mk("mods/.merged/x");  // (not a mod)
    ModInfo mi[8];
    n = mods_scan(T, mi, 8);
    CHECK(n == 2, "scan: %d mods", n);
    if (n == 2) {
        CHECK(!strcmp(mi[0].name, "Mod B") && mi[0].place == 1 && !strcmp(mi[1].name, "Mod A") && mi[1].place == 0,
              "scan order: on first (%s %d, %s %d)", mi[0].name, mi[0].place, mi[1].name, mi[1].place);
        ModInfo *a = &mi[1], *bm = &mi[0];
        CHECK(a->has_romfs && a->has_exefs && a->dlc_packs == 1, "A: romfs %d exefs %d dlc %d", a->has_romfs, a->has_exefs, a->dlc_packs);
        CHECK(a->replaced_files == 2 && a->new_files == 1, "A files: %d replaced, %d new", a->replaced_files, a->new_files);
        CHECK(a->index_changed == 1 && a->index_added == 1 && a->text == MOD_TEXT_NONE, "A index %d/%d text %d",
              a->index_changed, a->index_added, a->text);
        CHECK(bm->new_files == 4 && bm->replaced_files == 0 && bm->index_changed == 1 && bm->index_added == 1,
              "B: %d new %d replaced, index %d/%d", bm->new_files, bm->replaced_files, bm->index_changed, bm->index_added);
        CHECK(bm->text == MOD_TEXT_EU, "B text %d (European English)", bm->text);
        // both on, A first: saved, read back
        mi[1].place = 1, mi[0].place = 2;
        CHECK(mods_save_order(T, mi, n), "save");
        ModInfo again[8];
        int m2 = mods_scan(T, again, 8);
        CHECK(m2 == 2 && !strcmp(again[0].name, "Mod A") && again[0].place == 1 && again[1].place == 2, "saved order");
        int lines = 0;
        int c = mods_check_conflicts(T, again, m2, count_line, &lines);
        CHECK(c == 2 && lines == 2, "conflicts: %d (%d lines)", c, lines);
        // the game reads the same order
        n = mods_romfs_stack(base, &dirs);
        CHECK(n == 4 && strstr(dirs[1], "Mod A") && strstr(dirs[2], "Mod B"), "stack from the saved order");
        // B off: no conflicts
        again[1].place = 0;
        lines = 0;
        CHECK(mods_check_conflicts(T, again, m2, count_line, &lines) == 0, "one mod: no conflicts");
    }

#ifndef _WIN32
    char broken[1024];
    snprintf(broken, sizeof broken, "%s/mods/Mod A/romfs/broken-file", T);
    CHECK(!symlink("/nonexistent-opt5-mod-fixture", broken), "create broken mod file");
    puts_("mods/load_order.txt", "Mod A\n");
    n = mods_romfs_stack(base, &dirs);
    CHECK(!romfs_layout_stack(dirs, n, &L, NULL, NULL, why, sizeof why), "unreadable mod file must not yield a partial layout");
    romfs_layout_free(&L);unlink(broken);
#endif
    puts_("mods/load_order.txt", "Mod A\n");
    puts_("romfs/patch4/INFO0.bin", "truncated-base-index");
    mods_romfs_stack(base, &dirs);
    CHECK(*mods_last_error(), "truncated base index must prevent an incomplete merge");
    put("romfs/patch4/INFO0.bin", idx, sizeof idx);
#ifndef _WIN32
    struct rlimit before, limited;
    CHECK(!getrlimit(RLIMIT_FSIZE, &before), "get size limit");
    limited = before; limited.rlim_cur = 1;
    void (*old_signal)(int) = signal(SIGXFSZ, SIG_IGN);
    CHECK(!setrlimit(RLIMIT_FSIZE, &limited), "set test size limit");
    mods_romfs_stack(base, &dirs);
    CHECK(*mods_last_error(), "failed merged-index write must prevent startup");
    CHECK(!setrlimit(RLIMIT_FSIZE, &before), "restore size limit");
    signal(SIGXFSZ, old_signal);
#endif
    uint8_t invalid[2 * 0x120] = {0};
    put("mods/Mod A/romfs/patch4/INFO0.bin", invalid, 0x120);
    mods_romfs_stack(base, &dirs);
    CHECK(*mods_last_error(), "aligned garbage must not be accepted as an index");
    rec(invalid, 7, 4, "rom:/a");rec(invalid + 0x120, 7, 4, "rom:/b");
    put("mods/Mod A/romfs/patch4/INFO0.bin", invalid, sizeof invalid);
    mods_romfs_stack(base, &dirs);
    CHECK(*mods_last_error(), "duplicate index ids must prevent an ambiguous merge");
    puts_("mods/Mod A/romfs/patch4/INFO0.bin", "invalid-index");
    mods_romfs_stack(base, &dirs);
    CHECK(*mods_last_error(), "malformed enabled mod must report a startup error");
    puts_("mods/load_order.txt", "");
    CHECK(mods_romfs_stack(base, &dirs) == 1 && !*mods_last_error(), "disabling mods clears the startup error");
    if (system(cmd)) {}
    fprintf(stderr, fails ? "test_mods: %d FAILED\n" : "test_mods: passed\n", fails);
    return fails != 0;
}
