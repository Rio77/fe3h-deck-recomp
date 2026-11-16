// Structured programs (scf_build.c) against their branch graphs: random graphs, and for each reducible one the tree
// built from it must run through the same blocks in the same order as the graph for many branch decisions (scf_sim.c).
// Irreducible graphs must give NULL.
//   gcc -std=c11 -O1 -Iruntime/nvn/maxwell -o /tmp/test_scf_build tools/test_scf_build.c runtime/nvn/maxwell/scf_*.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scf.h"

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(uint32_t n) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs % n); }

// mostly forward edges with some back edges, so most graphs are reducible loops and ifs
static void gen(ScfCfg *g, int n) {
    g->n = n;
    for (int i = 0; i < n; i++) {
        ScfBlock *b = &g->b[i];
        memset(b, 0, sizeof *b);
        int fwd = i + 1 < n ? i + 1 + (int)rnd((uint32_t)(n - i - 1 < 4 ? n - i - 1 : 4)) : SCF_EXIT;
        b->term = rnd(100) < 55 ? SCF_COND : SCF_GOTO;
        b->succ[0] = fwd;
        int r = (int)rnd(100);
        b->succ[1] = r < 20 && i > 0 ? (int)rnd((uint32_t)i + 1)                 // back edge
                   : r < 30 ? SCF_EXIT
                   : i + 1 < n ? i + 1 + (int)rnd((uint32_t)(n - i - 1)) : SCF_EXIT;  // forward, anywhere
        if (b->term == SCF_GOTO && rnd(100) < 15 && i > 0) b->succ[0] = (int)rnd((uint32_t)i + 1);
        if (rnd(2)) { int t = b->succ[0]; b->succ[0] = b->succ[1]; b->succ[1] = t; }
        if (b->term == SCF_GOTO && b->succ[0] == SCF_EXIT && i + 1 < n && rnd(2)) b->succ[0] = i + 1;
    }
}

int main(int argc, char **argv) {
    int graphs = argc > 1 ? atoi(argv[1]) : 20000;
    static ScfBlock blocks[200];
    static int t1[4000], t2[4000];
    int red = 0, irr = 0, fails = 0;
    for (int k = 0; k < graphs && fails < 5; k++) {
        ScfCfg g = {.b = blocks};
        gen(&g, 2 + (int)rnd(k < graphs / 2 ? 20 : 120));
        ScfDom d;
        if (!scf_dominators(&g, &d)) { printf("dominators: out of memory\n"); return 1; }
        ScfNode *pool, *root = scf_build(&g, &d, &pool);
        if (!d.reducible) {
            irr++;
            if (root) { printf("graph %d: irreducible but built\n", k); fails++; }
            free(pool);
            scf_dom_free(&d);
            continue;
        }
        red++;
        if (!root) { printf("graph %d (%d blocks): reducible but not built\n", k, g.n); fails++; scf_dom_free(&d); continue; }
        for (uint64_t seed = 1; seed <= 200; seed++) {
            int n1 = scf_sim_cfg(&g, seed, t1, 4000), n2 = scf_sim_tree(root, &g, seed, t2, 4000);
            if (n1 != n2 || memcmp(t1, t2, sizeof(int) * (size_t)(n1 > 0 ? n1 : 0))) {
                printf("graph %d (%d blocks) seed %llu: traces differ (%d vs %d steps)\n", k, g.n,
                       (unsigned long long)seed, n1, n2);
                for (int i = 0; i < g.n; i++)
                    printf("  b%d %s %d %d\n", i, g.b[i].term == SCF_COND ? "COND" : "GOTO", g.b[i].succ[0], g.b[i].succ[1]);
                fails++;
                break;
            }
        }
        free(pool);
        scf_dom_free(&d);
    }
    if (fails) return 1;
    printf("ok %d graphs (%d reducible, %d irreducible)\n", red + irr, red, irr);
    return 0;
}
