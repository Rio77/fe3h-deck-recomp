// Checks that scf_sim_cfg and scf_sim_tree agree on hand-built (CFG, structured tree) pairs, and that the checker
// notices wrong trees.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scf.h"

#define MAXT 2000
#define SEEDS 500

static ScfNode pool[256];
static int npool;

static ScfNode *nd(ScfKind k, int block, int depth, ScfNode *a, ScfNode *b) {
    ScfNode *p = &pool[npool++];
    *p = (ScfNode){k, block, depth, a, b};
    return p;
}
#define SEQ(a, b) nd(SCF_N_SEQ, 0, 0, a, b)
#define CODE(i) nd(SCF_N_CODE, i, 0, NULL, NULL)
#define IF(i, a, b) nd(SCF_N_IF, i, 0, a, b)
#define LOOP(a) nd(SCF_N_LOOP, 0, 0, a, NULL)
#define BLOCK(a) nd(SCF_N_BLOCK, 0, 0, a, NULL)
#define BRK(d) nd(SCF_N_BREAK, 0, d, NULL, NULL)
#define CONT(d) nd(SCF_N_CONTINUE, 0, d, NULL, NULL)
#define EXITN() nd(SCF_N_EXIT, 0, 0, NULL, NULL)

static ScfBlock G(int s) { return (ScfBlock){0, 0, SCF_GOTO, {s, 0}, 0, false}; }
static ScfBlock C(int t, int f) { return (ScfBlock){0, 0, SCF_COND, {t, f}, 0, false}; }

static int ntests;

// Returns the number of seeds whose traces differ (a tree returning -1 counts as differing).
static int compare(const ScfBlock *b, int n, const ScfNode *tree) {
    ScfCfg g = {n, (ScfBlock *)b};
    static int t1[MAXT], t2[MAXT];
    int bad = 0;
    for (uint64_t s = 0; s < SEEDS; s++) {
        int l1 = scf_sim_cfg(&g, s, t1, MAXT), l2 = scf_sim_tree(tree, &g, s, t2, MAXT);
        if (l1 < 0 || l1 != l2 || memcmp(t1, t2, (size_t)l1 * sizeof(int))) bad++;
    }
    return bad;
}

static void expect_same(const char *name, const ScfBlock *b, int n, const ScfNode *tree) {
    ntests++;
    int bad = compare(b, n, tree);
    if (bad) {
        printf("FAIL %s: %d of %d seeds differ\n", name, bad, SEEDS);
        exit(1);
    }
}

static void expect_differ(const char *name, const ScfBlock *b, int n, const ScfNode *tree) {
    ntests++;
    if (!compare(b, n, tree)) {
        printf("FAIL %s: wrong tree was not detected\n", name);
        exit(1);
    }
}

int main(void) {
    // sanity: decide is deterministic and not constant
    int ones = 0;
    for (int k = 0; k < 1000; k++) {
        if (scf_decide(7, 3, k) != scf_decide(7, 3, k)) { puts("FAIL decide not deterministic"); return 1; }
        ones += scf_decide(7, 3, k);
    }
    if (ones < 400 || ones > 600) { printf("FAIL decide bias: %d/1000\n", ones); return 1; }
    ntests++;

    // straight line
    ScfBlock line[] = {G(1), G(2), G(SCF_EXIT)};
    expect_same("line", line, 3, SEQ(CODE(0), SEQ(CODE(1), CODE(2))));

    // diamond
    ScfBlock dia[] = {C(1, 2), G(3), G(3), G(SCF_EXIT)};
    expect_same("diamond", dia, 4, SEQ(CODE(0), SEQ(IF(0, CODE(1), CODE(2)), CODE(3))));
    expect_differ("diamond swapped", dia, 4, SEQ(CODE(0), SEQ(IF(0, CODE(2), CODE(1)), CODE(3))));

    // while loop
    ScfBlock wh[] = {G(1), C(3, 2), G(1), G(SCF_EXIT)};
    expect_same("while", wh, 4,
                SEQ(CODE(0), SEQ(LOOP(SEQ(CODE(1), SEQ(IF(1, BRK(0), NULL), SEQ(CODE(2), CONT(0))))), CODE(3))));
    expect_differ("while continue for break", wh, 4,
                  SEQ(CODE(0), SEQ(LOOP(SEQ(CODE(1), SEQ(IF(1, CONT(0), NULL), SEQ(CODE(2), CONT(0))))), CODE(3))));
    expect_differ("while missing back edge", wh, 4,
                  SEQ(CODE(0), SEQ(LOOP(SEQ(CODE(1), SEQ(IF(1, BRK(0), NULL), CODE(2)))), CODE(3))));

    // do-while: 0 -> 1 -> 2; 2: true -> 1, false -> 3
    ScfBlock dw[] = {G(1), G(2), C(1, 3), G(SCF_EXIT)};
    expect_same("do-while", dw, 4, SEQ(CODE(0), SEQ(LOOP(SEQ(CODE(1), SEQ(CODE(2), IF(2, CONT(0), NULL)))), CODE(3))));
    expect_same("do-while via BLOCK", dw, 4,
                SEQ(CODE(0), SEQ(BLOCK(LOOP(SEQ(CODE(1), SEQ(CODE(2), IF(2, CONT(0), BRK(1)))))), CODE(3))));

    // nested loops, break out of both: outer header 1, inner header 2
    ScfBlock nest[] = {G(1), C(5, 2), C(4, 3), C(5, 2), G(1), G(SCF_EXIT)};
    ScfNode *inner = LOOP(SEQ(CODE(2), IF(2, BRK(0), SEQ(CODE(3), IF(3, BRK(1), CONT(0))))));
    ScfNode *outer = LOOP(SEQ(CODE(1), SEQ(IF(1, BRK(0), NULL), SEQ(inner, SEQ(CODE(4), CONT(0))))));
    expect_same("nested break 1", nest, 6, SEQ(CODE(0), SEQ(outer, CODE(5))));
    ScfNode *inner2 = LOOP(SEQ(CODE(2), IF(2, BRK(0), SEQ(CODE(3), IF(3, BRK(0), CONT(0))))));
    ScfNode *outer2 = LOOP(SEQ(CODE(1), SEQ(IF(1, BRK(0), NULL), SEQ(inner2, SEQ(CODE(4), CONT(0))))));
    expect_differ("nested break 0 for 1", nest, 6, SEQ(CODE(0), SEQ(outer2, CODE(5))));

    // early exit inside an if
    ScfBlock ee[] = {C(1, 2), G(SCF_EXIT), G(3), G(SCF_EXIT)};
    expect_same("early exit", ee, 4, SEQ(CODE(0), SEQ(IF(0, SEQ(CODE(1), EXITN()), CODE(2)), CODE(3))));
    expect_differ("early exit missing", ee, 4, SEQ(CODE(0), SEQ(IF(0, CODE(1), CODE(2)), CODE(3))));

    // malformed: CONTINUE to a BLOCK, and a BREAK / CONTINUE without a target
    {
        ScfCfg g = {1, (ScfBlock[]){G(SCF_EXIT)}};
        int t[MAXT];
        ntests++;
        if (scf_sim_tree(BLOCK(CONT(0)), &g, 1, t, MAXT) != -1) { puts("FAIL continue to block"); return 1; }
        ntests++;
        if (scf_sim_tree(LOOP(BLOCK(SEQ(CODE(0), CONT(1)))), &g, 1, t, MAXT) != MAXT ||
            scf_sim_tree(LOOP(CONT(1)), &g, 1, t, MAXT) != -1) {
            puts("FAIL continue depth");
            return 1;
        }
        ntests++;
        if (scf_sim_tree(SEQ(CODE(0), BRK(0)), &g, 1, t, MAXT) != -1) { puts("FAIL break without target"); return 1; }
    }

    printf("ok %d tests\n", ntests);
    return 0;
}
