// Branch graph of a Maxwell program for structured control flow (scf.h, step 1).
//
// The translator's switch loop runs the flow instructions with small per-invocation stacks: SSY / PBK / PCNT push a
// target, SYNC and BRK pop one and jump there, CONT jumps to the top PCNT entry without popping, CAL pushes its return
// point and jumps, PRET pushes, RET pops and jumps (exits when empty). For a structured program every such jump needs
// a fixed target, so the stacks are followed statically through the program: each instruction gets the stack contents
// it is reached with, and a program reaching some instruction with two different contents isn't handled (the caller
// keeps the switch loop). The pushes then translate to nothing and SYNC / BRK / CONT / RET become plain jumps.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scf.h"

#define STK 16  // the switch loop's stack size (deeper isn't handled)

typedef enum {
    OP_PLAIN,  // not a flow instruction (or one that never runs)
    OP_BRA, OP_SSY, OP_PBK, OP_PCNT, OP_PRET, OP_CAL, OP_SYNC, OP_BRK, OP_CONT, OP_RET, OP_EXIT,
    OP_UNSUPPORTED,
} Op;

typedef struct { uint8_t n[4]; uint32_t v[4][STK]; } Stacks;  // [0] ssy, [1] pbk, [2] pcnt, [3] cal

static int64_t sbits(uint64_t w, int lo, int n) {
    uint64_t v = (w >> lo) & ((1ull << n) - 1);
    return (int64_t)(v << (64 - n)) >> (64 - n);
}

static uint32_t norm(int64_t t) {  // (as translate.c norm_target: a target on a scheduling word means the next one)
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}

static Op decode(uint64_t w, uint32_t pc, uint32_t *target, int *pred, bool *neg) {
    uint64_t t12 = w & 0xfff0000000000000ull;
    bool cc = w & 0x20;  // (the condition-code forms aren't flow the translator knows)
    *pred = (int)((w >> 16) & 7);
    *neg = (w >> 19) & 1;
    *target = norm((int64_t)pc + 8 + sbits(w, 20, 24));
    if (t12 == 0xe290000000000000ull || t12 == 0xe2a0000000000000ull || t12 == 0xe2b0000000000000ull ||
        t12 == 0xe270000000000000ull) {  // SSY / PBK / PCNT / PRET take no predicate
        *pred = 7;
        *neg = false;
    }
    if (*pred == 7 && *neg) return OP_PLAIN;  // @!PT: never runs
    if (t12 == 0xe240000000000000ull) return cc ? OP_UNSUPPORTED : OP_BRA;
    if (t12 == 0xe290000000000000ull) return cc ? OP_UNSUPPORTED : OP_SSY;
    if (t12 == 0xe2a0000000000000ull) return cc ? OP_UNSUPPORTED : OP_PBK;
    if (t12 == 0xe2b0000000000000ull) return cc ? OP_UNSUPPORTED : OP_PCNT;
    if (t12 == 0xe270000000000000ull) return cc ? OP_UNSUPPORTED : OP_PRET;
    if (t12 == 0xe260000000000000ull) return cc ? OP_UNSUPPORTED : OP_CAL;
    if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) return OP_SYNC;
    if (t12 == 0xe340000000000000ull) return OP_BRK;
    if (t12 == 0xe350000000000000ull) return OP_CONT;
    if (t12 == 0xe320000000000000ull) return OP_RET;
    if (t12 == 0xe300000000000000ull) return OP_EXIT;
    // BRX / JMX / JMP and the rest of the e2xx..e3xx flow group, BAR (compute: the switch loop splits at barriers)
    if (t12 >= 0xe200000000000000ull && t12 <= 0xe350000000000000ull && t12 != 0xe330000000000000ull) return OP_UNSUPPORTED;
    if ((w & 0xfff8000000000000ull) == 0xf0a8000000000000ull) return getenv("FE3H_COMPUTE_SCF") ? OP_PLAIN : OP_UNSUPPORTED; // preserve BAR at its original position in structured compute
    return OP_PLAIN;
}

static size_t next_insn(size_t i) { i++; if (i % 4 == 0) i++; return i; }

typedef struct {
    const uint64_t *code;
    size_t end;
    Op *op;
    uint32_t *tgt;
    int *pr;
    bool *ng;
    Stacks *st;      // per instruction: the stacks it is reached with
    bool *seen;
    bool *leader;
    int *taken;      // per instruction: the instruction its jump goes to (-1: exit), for jumps
    size_t *work;
    size_t nwork;
    char *why;
    size_t whylen;
} B;

static bool fail(B *b, const char *fmt, unsigned a) {
    snprintf(b->why, b->whylen, fmt, a);
    return false;
}

// Reach instruction i (or the exit, i >= end) with stacks s.
static bool reach(B *b, size_t i, const Stacks *s) {
    if (i >= b->end) return true;  // (falling off the end, or a jump past it: the shader ends)
    if (i % 4 == 0) return fail(b, "jump to a scheduling word at %x", (unsigned)(i * 8));
    if (b->seen[i]) {
        if (memcmp(&b->st[i], s, sizeof *s)) return fail(b, "flow stacks differ at %x", (unsigned)(i * 8));
        return true;
    }
    b->seen[i] = true;
    b->st[i] = *s;
    b->work[b->nwork++] = i;
    return true;
}

static bool push(B *b, Stacks *s, int k, uint32_t v, size_t i) {
    if (s->n[k] >= STK) return fail(b, "flow stack overflow at %x", (unsigned)(i * 8));
    s->v[k][s->n[k]++] = v;
    return true;
}

bool scf_build_cfg(const uint64_t *code, size_t end, ScfCfg *g, char *why, size_t whylen) {
    memset(g, 0, sizeof *g);
    if (whylen) why[0] = 0;
    if (end < 2) { snprintf(why, whylen, "empty"); return false; }
    B b = {.code = code, .end = end, .why = why, .whylen = whylen};
    b.op = calloc(end, sizeof *b.op);
    b.tgt = calloc(end, sizeof *b.tgt);
    b.pr = calloc(end, sizeof *b.pr);
    b.ng = calloc(end, sizeof *b.ng);
    b.st = calloc(end, sizeof *b.st);
    b.seen = calloc(end, 1);
    b.leader = calloc(end + 1, 1);
    b.taken = calloc(end, sizeof *b.taken);
    b.work = calloc(end, sizeof *b.work);
    int *blk = calloc(end, sizeof *blk);
    bool ok = b.op && b.tgt && b.pr && b.ng && b.st && b.seen && b.leader && b.taken && b.work && blk;
    if (!ok) snprintf(why, whylen, "out of memory");
    for (size_t i = 0; ok && i < end; i++) {
        if (i % 4 == 0) continue;
        b.op[i] = decode(code[i], (uint32_t)(i * 8), &b.tgt[i], &b.pr[i], &b.ng[i]);
        if (b.op[i] == OP_UNSUPPORTED) ok = fail(&b, "flow instruction at %x", (unsigned)(i * 8));
    }
    // follow the stacks from the first instruction
    Stacks s0 = {0};
    if (ok) ok = reach(&b, 1, &s0);
    while (ok && b.nwork) {
        size_t i = b.work[--b.nwork];
        Stacks s = b.st[i];
        bool guarded = !(b.pr[i] == 7 && !b.ng[i]);
        size_t fall = next_insn(i);
        int64_t to = -2;  // the jump's destination: an instruction, -1 exit, -2 no jump
        switch (b.op[i]) {
        case OP_PLAIN: break;
        case OP_SSY: ok = push(&b, &s, 0, b.tgt[i], i); break;
        case OP_PBK: ok = push(&b, &s, 1, b.tgt[i], i); break;
        case OP_PCNT: ok = push(&b, &s, 2, b.tgt[i], i); break;
        case OP_PRET: ok = push(&b, &s, 3, b.tgt[i], i); break;
        case OP_BRA: to = b.tgt[i] / 8; break;
        case OP_EXIT: to = -1; break;
        case OP_CAL: case OP_SYNC: case OP_BRK: case OP_CONT: case OP_RET: {
            Stacks t = s;  // (the taken path's stacks; the untaken path keeps s)
            if (b.op[i] == OP_CAL) {
                ok = push(&b, &t, 3, (uint32_t)(fall * 8), i);
                to = b.tgt[i] / 8;
            } else {
                int k = b.op[i] == OP_SYNC ? 0 : b.op[i] == OP_BRK ? 1 : b.op[i] == OP_CONT ? 2 : 3;
                if (!t.n[k]) {
                    if (k == 3) to = -1;  // RET with nothing pushed: the shader ends
                    else ok = fail(&b, "pop from an empty flow stack at %x", (unsigned)(i * 8));
                } else {
                    to = t.v[k][t.n[k] - 1] / 8;
                    if (k != 2) t.n[k]--;  // (CONT leaves its entry)
                    t.v[k][t.n[k]] = k != 2 ? 0 : t.v[k][t.n[k]];  // (popped slots compare equal)
                }
            }
            if (ok) b.taken[i] = (int)to;
            if (ok && to >= 0) ok = reach(&b, (size_t)to, &t);
            if (ok && guarded) ok = reach(&b, fall, &s);
            continue;
        }
        default: ok = false; break;
        }
        if (!ok) break;
        if (to == -2) { ok = reach(&b, fall, &s); continue; }
        b.taken[i] = (int)to;
        if (to >= 0) ok = reach(&b, (size_t)to, &s);
        if (ok && guarded) ok = reach(&b, fall, &s);
    }
    // blocks: start at the first instruction, at jump destinations and after jumps
    bool any = false;
    for (size_t i = 1; ok && i < end; i = next_insn(i)) {
        if (!b.seen[i]) continue;
        bool jump = b.op[i] == OP_BRA || b.op[i] == OP_EXIT || b.op[i] == OP_CAL || b.op[i] == OP_SYNC ||
                    b.op[i] == OP_BRK || b.op[i] == OP_CONT || b.op[i] == OP_RET;
        if (!any) { b.leader[i] = true; any = true; }
        if (jump) {
            if (b.taken[i] >= 0) b.leader[b.taken[i]] = true;
            size_t f = next_insn(i);
            if (f < end) b.leader[f] = true;
        }
    }
    // an unreached instruction ends the block before it (whatever reaches past it reaches a leader)
    for (size_t i = 1; ok && i < end; i = next_insn(i))
        if (!b.seen[i]) { b.leader[i] = false; size_t f = next_insn(i); if (f < end && b.seen[f]) b.leader[f] = true; }
    int n = 0;
    for (size_t i = 1; ok && i < end; i = next_insn(i)) blk[i] = b.seen[i] && b.leader[i] ? n++ : -1;
    if (ok) {
        g->b = calloc((size_t)n, sizeof *g->b);
        if (!g->b) ok = fail(&b, "out of memory", 0);
    }
    if (ok) {
        g->n = n;
        int cur = -1;
        for (size_t i = 1; i < end; i = next_insn(i)) {
            if (!b.seen[i]) continue;
            if (blk[i] >= 0) { cur = blk[i]; g->b[cur].first = (uint32_t)i; }
            ScfBlock *k = &g->b[cur];
            k->last = (uint32_t)i;
            size_t f = next_insn(i);
            bool ends = f >= end || !b.seen[f] || blk[f] >= 0;
            bool jump = b.op[i] == OP_BRA || b.op[i] == OP_EXIT || b.op[i] == OP_CAL || b.op[i] == OP_SYNC ||
                        b.op[i] == OP_BRK || b.op[i] == OP_CONT || b.op[i] == OP_RET;
            if (!jump && !ends) continue;
            int fs = f < end && b.seen[f] ? blk[f] : SCF_EXIT;
            if (!jump) { k->term = SCF_GOTO; k->succ[0] = fs; continue; }
            int ts = b.taken[i] >= 0 ? blk[b.taken[i]] : SCF_EXIT;
            bool guarded = !(b.pr[i] == 7 && !b.ng[i]);
            if (!guarded) { k->term = SCF_GOTO; k->succ[0] = ts; continue; }
            k->term = SCF_COND;
            k->succ[0] = ts;
            k->succ[1] = fs;
            k->pred = b.pr[i];
            k->pred_neg = b.ng[i];
        }
    }
    free(b.op); free(b.tgt); free(b.pr); free(b.ng); free(b.st); free(b.seen); free(b.leader); free(b.taken);
    free(b.work); free(blk);
    if (!ok) { free(g->b); memset(g, 0, sizeof *g); }
    return ok;
}

// Whether instruction i of a block is its terminating jump (translated by the structure, not as an instruction):
// the last instruction of a block whose term came from a flow instruction. Pushes (SSY / PBK / PCNT / PRET) also
// translate to nothing.
bool scf_insn_is_flow(uint64_t w) {
    uint32_t t; int p; bool ng;
    Op o = decode(w, 0, &t, &p, &ng);
    return o != OP_PLAIN;
}
