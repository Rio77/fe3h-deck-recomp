// Cheats: Atmosphere's cheat VM over cheats.txt (the project folder; Atmosphere's format, never committed).
//
// cheats.txt holds cheats as Atmosphere writes them: a [name] line, then the opcodes (8-digit hex words); {name} is a
// master code, always on. Lines starting with # under a name describe it (shown in the menu). The enabled ones run once a frame, as dmnt runs them on the console: memory writes and
// reads relative to the main module or the heap, conditionals, loops, registers, button conditions.
//
// A cheat that writes an instruction into the game's code (opcode 0, 4 bytes, inside main's .text) can't change a
// recompiled game by writing memory: the instruction is C by then. gen.py compiled such instructions in as variants
// (tools/cheats.py), and the generated code asks cheat_word() which one to run. The VM's code writes land in that
// table instead of memory, owned by the cheat that made them, and are undone when that cheat is switched off. The
// variants are compiled from the cheats.txt `make gen` saw (generated/main/cheat_variants.txt): one added since needs
// `make gen-main` and `make`, and the menu says so.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

#include "../hle.h"
#include "cheatvm.h"
#include "../gamebuild.h"

#define MAX_CHEATS 128
#define MAX_WORDS 1024
#define MAX_CODE 512
#define CHEATS_FILE "cheats.txt"
#define VARIANTS_FILE "generated/main/cheat_variants.txt"

typedef struct {
    char name[96];
    char desc[512];  // the "# ..." lines under its name (ours: Atmosphere has no descriptions)
    char key[96];  // the settings key: the name in lower case, other characters as '_'
    uint32_t *w;
    int n;
    bool master, on, bad;  // bad: an opcode the VM doesn't know (not run)
    bool needs_gen;        // writes an instruction that isn't compiled in
    uint64_t code[32];     // the instructions it patches: cheats patching the same one exclude each other (Exp x2 / x4)
    int ncode;
    char excludes[256];    // their names, for the menu
} Cheat;

static Cheat g_cheats[MAX_CHEATS];
static int g_ncheats = -1;  // -1: not loaded
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// code words written by cheats: address -> word, and the cheat that wrote it
static struct { uint64_t addr; uint32_t word; int owner; } g_code[MAX_CODE];
static int g_ncode;
// instructions compiled with variants (addr, word)
static struct { uint64_t addr; uint32_t word; } g_var[MAX_CODE];
static int g_nvar;

// settings read before the cheats were loaded (larecomp settings file: cheat.<key> = 1)
static char g_pending[MAX_CHEATS][96];
static int g_npending;

static const Module *main_module(void) {
    for (int i = 0; i < g_nmodules; i++)
        if (g_modules[i].name && !strcmp(g_modules[i].name, "main")) return &g_modules[i];
    return g_nmodules ? &g_modules[0] : NULL;
}

static bool in_main_text(uint64_t a) {
    const Module *m = main_module();
    return m && a >= m->base + m->text_off && a < m->base + m->text_off + m->text_size;
}

static bool compiled(uint64_t addr, uint32_t word) {
    for (int i = 0; i < g_nvar; i++)
        if (g_var[i].addr == addr && g_var[i].word == word) return true;
    return false;
}

static void make_key(const char *name, char *key, size_t cap) {
    size_t n = 0;
    if (cap > 57) cap = 57;  // (settings keys are read up to 63 characters, "cheat." included)
    for (const char *p = name; *p && n + 1 < cap; p++) key[n++] = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '_';
    key[n] = 0;
}

// both patch the same instruction of the game's code (only one can be on)
static bool conflict(const Cheat *a, const Cheat *b) {
    for (int i = 0; i < a->ncode; i++)
        for (int j = 0; j < b->ncode; j++)
            if (a->code[i] == b->code[j]) return true;
    return false;
}

static int op_len(uint32_t w) {  // words in the opcode starting with w (0: unknown)
    switch (w >> 28) {
    case 0: case 1: return ((w >> 24) & 0xF) == 8 ? 4 : 3;
    case 2: return 1;
    case 3: return ((w >> 24) & 0xF) == 0 ? 2 : 1;  // (300R0000 VVVVVVVV begins a loop, 310R0000 ends it)
    case 4: return 3;
    case 5: return 2;
    case 6: return 3;
    case 7: return 2;
    case 8: return 1;
    default: return 0;
    }
}

static void load(void) {
    if (g_ncheats >= 0) return;
    g_ncheats = 0;
    // the cheat patches the game code was built with: next to the library a start-screen build made (gamebuild.c),
    // else the source checkout's generated/ (make gen-main)
    extern const char *gamelib_path(void);
    char vf[1200] = "";
    if (gamelib_path()) snprintf(vf, sizeof vf, "%s.cheats", gamelib_path());
    FILE *f = vf[0] ? fopen(vf, "r") : NULL;
    if (!f) f = fopen(VARIANTS_FILE, "r");
    if (f) {
        unsigned long long off;
        unsigned w;
        while (g_nvar < MAX_CODE && fscanf(f, "%llx %x", &off, &w) == 2)
            g_var[g_nvar].addr = 0x7000000000ull + off, g_var[g_nvar++].word = w;
        fclose(f);
    }
    f = fopen(CHEATS_FILE, "r");
    if (!f) { LOG("[cheats] no %s: no cheats\n", CHEATS_FILE); return; }
    char line[1024];
    Cheat *c = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        char *e = p + strlen(p);
        while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
        if ((*p == '[' && e > p && e[-1] == ']') || (*p == '{' && e > p && e[-1] == '}')) {
            if (g_ncheats >= MAX_CHEATS) break;
            c = &g_cheats[g_ncheats++];
            memset(c, 0, sizeof *c);
            snprintf(c->name, sizeof c->name, "%.*s", (int)(e - p - 2), p + 1);
            make_key(c->name, c->key, sizeof c->key);
            c->master = *p == '{';
            c->on = c->master;
            c->w = calloc(MAX_WORDS, sizeof *c->w);
            continue;
        }
        if (!c) continue;
        if (*p == '#') {  // a description line
            p++;
            while (*p == ' ') p++;
            size_t l = strlen(c->desc);
            snprintf(c->desc + l, sizeof c->desc - l, "%s%s", l ? " " : "", p);
            continue;
        }
        for (char *t = strtok(p, " \t"); t; t = strtok(NULL, " \t"))
            if (strlen(t) == 8 && strspn(t, "0123456789abcdefABCDEF") == 8 && c->n < MAX_WORDS)
                c->w[c->n++] = (uint32_t)strtoul(t, NULL, 16);
    }
    fclose(f);
    // check each: known opcodes, and its code writes compiled in
    for (int i = 0; i < g_ncheats; i++) {
        Cheat *ch = &g_cheats[i];
        for (int k = 0; k < ch->n;) {
            int n = op_len(ch->w[k]);
            if (!n || k + n > ch->n) { ch->bad = true; break; }
            uint32_t w = ch->w[k];
            if ((w >> 28) == 0 && ((w >> 24) & 0xF) == 4 && ((w >> 20) & 0xF) == 0 && ((w >> 16) & 0xF) == 0) {
                uint64_t a = 0x7000000000ull + ((uint64_t)(w & 0xFF) << 32 | ch->w[k + 1]);
                if (in_main_text(a) && !compiled(a, ch->w[k + 2])) ch->needs_gen = true;
                if (in_main_text(a) && ch->ncode < 32) ch->code[ch->ncode++] = a;
            }
            k += n;
        }
        for (int j = 0; j < g_npending; j++)
            if (!strcmp(g_pending[j], ch->key)) ch->on = true;
        for (int j = 0; j < i && ch->on && !ch->master; j++)  // (two that exclude each other were on: the first stays)
            if (g_cheats[j].on && conflict(&g_cheats[j], ch)) ch->on = false;
        LOG("[cheats] %s%s%s%s\n", ch->name, ch->on ? " (on)" : "", ch->bad ? " (unsupported opcode: not run)" : "",
            !ch->needs_gen ? "" : gamebuild_available() ? " (its code patch isn't compiled in: Settings > Game Files > Rebuild game code)"
                                                        : " (its code patch isn't compiled in: make gen-main, then make)");
    }
    for (int i = 0; i < g_ncheats; i++)
        for (int j = 0; j < g_ncheats; j++)
            if (j != i && !g_cheats[j].master && conflict(&g_cheats[i], &g_cheats[j])) {
                size_t l = strlen(g_cheats[i].excludes);
                snprintf(g_cheats[i].excludes + l, sizeof g_cheats[i].excludes - l, "%s%s", l ? ", " : "", g_cheats[j].name);
            }
    LOG("[cheats] %d from %s, %d instruction variants compiled in\n", g_ncheats, CHEATS_FILE, g_nvar);
}

// ------------------------------------------------------------------ the generated code's side
uint32_t cheat_word(uint64_t addr) {
    // (no lock: a stale read for one frame is harmless; entries are written whole)
    for (int i = 0; i < g_ncode; i++)
        if (g_code[i].addr == addr) return g_code[i].word;
    return 0;
}

static void code_write(uint64_t addr, uint32_t word, int owner) {
    for (int i = 0; i < g_ncode; i++)
        if (g_code[i].addr == addr) { g_code[i].word = word, g_code[i].owner = owner; return; }
    if (g_ncode < MAX_CODE) {
        g_code[g_ncode].owner = owner;
        g_code[g_ncode].word = word;
        __atomic_store_n(&g_code[g_ncode].addr, addr, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_ncode, 1, __ATOMIC_RELEASE);
    }
}

static void code_release(int owner) {  // a cheat switched off: its instructions back to the originals
    for (int i = 0; i < g_ncode; i++)
        if (g_code[i].owner == owner) g_code[i].word = 0;
}

// ------------------------------------------------------------------ the VM
static uint64_t region_base(int mem) {
    const Module *m = main_module();
    switch (mem) {
    case 0: return m ? m->base : 0x7000000000ull;  // main
    case 1: return HEAP_BASE;                      // heap
    case 3: return GUEST_LO;                       // ASLR region
    default: return 0;                              // alias: not here
    }
}

static bool mem_ok(uint64_t a, int n) { return a >= GUEST_LO && a + (uint64_t)n <= GUEST_HI; }

static uint64_t rd(uint64_t a, int width) {
    if (!mem_ok(a, width)) return 0;
    uint64_t v = 0;
    memcpy(&v, (const void *)(uintptr_t)a, (size_t)width);
    return v;
}

static void wr(uint64_t a, uint64_t v, int width, int owner) {
    if (width == 4 && in_main_text(a)) { code_write(a, (uint32_t)v, owner); return; }
    if (!mem_ok(a, width) || (width != 1 && width != 2 && width != 4 && width != 8)) return;
    static int log = -1, nlog;
    if (log < 0) log = getenv("LARECOMP_CHEAT_LOG") != NULL;
    if (log && nlog < 500) {  // (debug) each memory write a cheat makes that changes something
        uint64_t old = 0;
        memcpy(&old, (void *)(uintptr_t)a, (size_t)width);
        if (old != (width == 8 ? v : v & ((1ull << (width * 8)) - 1))) {
            nlog++;
            LOG("[cheats] %s: %u bytes at %llx (heap +%llx): %llx -> %llx\n", g_cheats[owner].name, width, (unsigned long long)a,
                (unsigned long long)(a - HEAP_BASE), (unsigned long long)old, (unsigned long long)v);
        }
    }
    memcpy((void *)(uintptr_t)a, &v, (size_t)width);
}

static uint64_t mask(uint64_t v, int width) { return width >= 8 ? v : v & ((1ull << (width * 8)) - 1); }

// index of the instruction after the end (or else) matching the conditional at w[k]; nested conditionals skipped
static int skip_cond(const uint32_t *w, int n, int k, bool stop_at_else) {
    int depth = 0;
    for (k += op_len(w[k]); k < n; k += op_len(w[k])) {
        if (!op_len(w[k])) return n;
        uint32_t t = w[k] >> 28;
        if (t == 1 || t == 8) depth++;
        else if (t == 2) {
            bool is_else = ((w[k] >> 24) & 0xF) == 1;
            if (depth == 0 && (!is_else || stop_at_else)) return k + 1;
            if (!is_else) depth--;
        }
    }
    return n;
}

static void run(Cheat *c, int idx, uint64_t keys) {
    uint64_t reg[16] = {0};
    int loop_start[16] = {0};
    const uint32_t *w = c->w;
    int n = c->n, budget = 1 << 20;  // (a runaway loop stops instead of hanging the frame)
    for (int k = 0; k < n && budget-- > 0;) {
        uint32_t o = w[k];
        int t = o >> 28, len = op_len(o);
        int width = (o >> 24) & 0xF, mem = (o >> 20) & 0xF, r = (o >> 16) & 0xF;
        switch (t) {
        case 0: {  // store static: 0TMR00AA AAAAAAAA V(V)
            uint64_t a = region_base(mem) + ((uint64_t)(o & 0xFF) << 32 | w[k + 1]) + reg[r];
            uint64_t v = width == 8 ? (uint64_t)w[k + 2] << 32 | w[k + 3] : w[k + 2];
            wr(a, v, width, idx);
            break;
        }
        case 1: {  // conditional on memory: 1TMC00AA AAAAAAAA V(V)
            int cond = (o >> 16) & 0xF;
            uint64_t a = region_base(mem) + ((uint64_t)(o & 0xFF) << 32 | w[k + 1]);
            uint64_t v = width == 8 ? (uint64_t)w[k + 2] << 32 | w[k + 3] : w[k + 2], m = rd(a, width);
            bool ok = cond == 1 ? m > v : cond == 2 ? m >= v : cond == 3 ? m < v : cond == 4 ? m <= v
                    : cond == 5 ? m == v : cond == 6 ? m != v : false;
            if (!ok) { k = skip_cond(w, n, k, true); continue; }
            break;
        }
        case 2:  // end conditional (20000000); else (21000000): reached from the true branch -> skip to the end
            if (((o >> 24) & 0xF) == 1) { k = skip_cond(w, n, k, false); continue; }
            break;
        case 3: {  // loop: 300R0000 VVVVVVVV ... 310R0000
            if (((o >> 24) & 0xF) == 0) { reg[r] = w[k + 1]; loop_start[r] = k + len; }
            else if (--reg[r] != 0) { k = loop_start[r]; continue; }
            break;
        }
        case 4:  // load register static: 400R0000 VVVVVVVV VVVVVVVV
            reg[r] = (uint64_t)w[k + 1] << 32 | w[k + 2];
            break;
        case 5: {  // load register from memory: 5TMRI0AA AAAAAAAA
            int from = (o >> 12) & 0xF;
            uint64_t off = (uint64_t)(o & 0xFF) << 32 | w[k + 1];
            uint64_t a = from == 1 ? reg[r] + off : from == 2 ? region_base(mem) + reg[r] + off : region_base(mem) + off;
            reg[r] = rd(a, width);
            break;
        }
        case 6: {  // store static to register address: 6T0RIor0 VVVVVVVV VVVVVVVV
            int inc = (o >> 12) & 0xF, use_off = (o >> 8) & 0xF, ro = (o >> 4) & 0xF;
            uint64_t a = reg[r] + (use_off ? reg[ro] : 0);
            wr(a, (uint64_t)w[k + 1] << 32 | w[k + 2], width, idx);
            if (inc) reg[r] += (uint64_t)width;
            break;
        }
        case 7: {  // legacy arithmetic: 7T0RC000 VVVVVVVV
            int op = (o >> 12) & 0xF;
            uint64_t v = w[k + 1];
            uint64_t x = reg[r];
            x = op == 0 ? x + v : op == 1 ? x - v : op == 2 ? x * v : op == 3 ? x << v : op == 4 ? x >> v : x;
            reg[r] = mask(x, width);
            break;
        }
        case 8:  // buttons held: 8kkkkkkk
            if ((keys & (o & 0x0FFFFFFF)) != (o & 0x0FFFFFFF)) { k = skip_cond(w, n, k, true); continue; }
            break;
        default: return;
        }
        k += len;
    }
}

void cheatvm_frame(void) {
    pthread_mutex_lock(&g_lock);
    load();
    extern uint64_t input_buttons_now(void);
    uint64_t keys = 0;
    bool any = false;
    for (int i = 0; i < g_ncheats; i++) any |= g_cheats[i].on && !g_cheats[i].bad;
    if (any) keys = input_buttons_now();
    for (int i = 0; i < g_ncheats; i++)
        if (g_cheats[i].on && !g_cheats[i].bad) run(&g_cheats[i], i, keys);
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ the menu's side (runtime/ui/menu.mm)
int cheatvm_count(void) {
    pthread_mutex_lock(&g_lock);
    load();
    int n = g_ncheats;
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool cheatvm_info(int i, CheatInfo *out) {
    if (i < 0 || i >= g_ncheats) return false;
    out->name = g_cheats[i].name;
    out->on = g_cheats[i].on;
    out->master = g_cheats[i].master;
    out->unsupported = g_cheats[i].bad;
    out->needs_gen = g_cheats[i].needs_gen;
    out->desc = g_cheats[i].desc;
    out->excludes = g_cheats[i].excludes;
    return true;
}

void cheatvm_set(int i, bool on) {
    pthread_mutex_lock(&g_lock);
    if (i >= 0 && i < g_ncheats && !g_cheats[i].master && g_cheats[i].on != on) {
        if (on)  // the ones patching the same instruction go off (one multiplier at a time)
            for (int j = 0; j < g_ncheats; j++)
                if (j != i && g_cheats[j].on && !g_cheats[j].master && conflict(&g_cheats[i], &g_cheats[j])) {
                    g_cheats[j].on = false;
                    code_release(j);
                    LOG("[cheats] %s off (shares code with %s)\n", g_cheats[j].name, g_cheats[i].name);
                }
        g_cheats[i].on = on;
        if (!on) code_release(i);
        LOG("[cheats] %s %s\n", g_cheats[i].name, on ? "on" : "off");
    }
    pthread_mutex_unlock(&g_lock);
}

// settings file: cheat.<key> = 1 for each enabled cheat (runtime/settings.c)
bool cheatvm_settings_load(const char *key, const char *val) {
    if (strncmp(key, "cheat.", 6) || atoi(val) == 0) return !strncmp(key, "cheat.", 6);
    if (g_ncheats >= 0) {
        for (int i = 0; i < g_ncheats; i++)
            if (!strcmp(g_cheats[i].key, key + 6)) g_cheats[i].on = true;
    } else if (g_npending < MAX_CHEATS) {
        snprintf(g_pending[g_npending++], sizeof g_pending[0], "%s", key + 6);
    }
    return true;
}

void cheatvm_settings_save(FILE *f) {
    for (int i = 0; i < g_ncheats; i++)
        if (g_cheats[i].on && !g_cheats[i].master) fprintf(f, "cheat.%s = 1\n", g_cheats[i].key);
    if (g_ncheats < 0)  // (not loaded this run: keep what the file said)
        for (int i = 0; i < g_npending; i++) fprintf(f, "cheat.%s = 1\n", g_pending[i]);
}
