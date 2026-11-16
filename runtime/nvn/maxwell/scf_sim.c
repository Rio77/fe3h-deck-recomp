// Simulation of a ScfCfg and of a ScfNode tree with identical branch decisions (tests only; see scf.h).
#include <stdlib.h>

#include "scf.h"

static uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

bool scf_decide(uint64_t seed, int block, int k) {
    uint64_t h = splitmix(seed);
    h = splitmix(h ^ (uint64_t)(uint32_t)block);
    h = splitmix(h ^ ((uint64_t)(uint32_t)k << 17) ^ 0xA5A5A5A5ull);
    return (h & 1) != 0;
}

int scf_sim_cfg(const ScfCfg *g, uint64_t seed, int *trace, int max) {
    int *cnt = calloc(g->n > 0 ? (size_t)g->n : 1, sizeof *cnt);
    if (!cnt) return -1;
    int len = 0, cur = 0;
    while (cur != SCF_EXIT) {
        if (cur < 0 || cur >= g->n || len >= max) break;
        trace[len++] = cur;
        const ScfBlock *b = &g->b[cur];
        cur = b->term == SCF_GOTO ? b->succ[0] : b->succ[scf_decide(seed, cur, cnt[cur]++) ? 0 : 1];
    }
    free(cnt);
    return len;
}

typedef enum { SG_NORMAL, SG_BREAK, SG_CONTINUE, SG_EXIT, SG_FULL, SG_ERROR } SigKind;
typedef struct {
    SigKind k;
    int n;
} Sig;

typedef struct {
    const ScfCfg *g;
    uint64_t seed;
    int *trace, max, len;
    int *cnt;
} Sim;

static Sig sig(SigKind k, int n) { return (Sig){k, n}; }

static Sig exec(Sim *s, const ScfNode *p) {
    if (!p) return sig(SG_NORMAL, 0);
    switch (p->kind) {
    case SCF_N_SEQ: {
        Sig r = exec(s, p->a);
        return r.k != SG_NORMAL ? r : exec(s, p->b);
    }
    case SCF_N_CODE:
        if (p->block < 0 || p->block >= s->g->n) return sig(SG_ERROR, 0);
        if (s->len >= s->max) return sig(SG_FULL, 0);
        s->trace[s->len++] = p->block;
        return sig(SG_NORMAL, 0);
    case SCF_N_IF:
        if (p->block < 0 || p->block >= s->g->n) return sig(SG_ERROR, 0);
        return exec(s, scf_decide(s->seed, p->block, s->cnt[p->block]++) ? p->a : p->b);
    case SCF_N_LOOP:
        for (;;) {
            Sig r = exec(s, p->a);
            if (r.k == SG_CONTINUE && r.n == 0) continue;
            if (r.k == SG_BREAK && r.n == 0) return sig(SG_NORMAL, 0);
            if ((r.k == SG_BREAK || r.k == SG_CONTINUE) && r.n > 0) r.n--;
            return r.k == SG_NORMAL ? sig(SG_NORMAL, 0) : r;
        }
    case SCF_N_BLOCK: {
        Sig r = exec(s, p->a);
        if (r.k == SG_CONTINUE && r.n == 0) return sig(SG_ERROR, 0);  // a BLOCK is not a loop
        if (r.k == SG_BREAK && r.n == 0) return sig(SG_NORMAL, 0);
        if (r.k == SG_BREAK || r.k == SG_CONTINUE) r.n--;
        return r;
    }
    case SCF_N_BREAK: return p->depth < 0 ? sig(SG_ERROR, 0) : sig(SG_BREAK, p->depth);
    case SCF_N_CONTINUE: return p->depth < 0 ? sig(SG_ERROR, 0) : sig(SG_CONTINUE, p->depth);
    case SCF_N_EXIT: return sig(SG_EXIT, 0);
    }
    return sig(SG_ERROR, 0);
}

int scf_sim_tree(const ScfNode *root, const ScfCfg *g, uint64_t seed, int *trace, int max) {
    Sim s = {g, seed, trace, max, 0, calloc(g->n > 0 ? (size_t)g->n : 1, sizeof(int))};
    if (!s.cnt) return -1;
    Sig r = exec(&s, root);
    free(s.cnt);
    // A BREAK / CONTINUE that escaped the root had no target: malformed.
    return (r.k == SG_ERROR || r.k == SG_BREAK || r.k == SG_CONTINUE) ? -1 : s.len;
}
