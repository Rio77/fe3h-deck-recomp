// Tests for scf_dom.c: hand-built graphs plus random graphs checked against naive references.
#include "scf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 64
#define X SCF_EXIT

static int ntests;

#define FAIL(...)                                      \
    do {                                               \
        fprintf(stderr, "FAIL (test %d): ", ntests);   \
        fprintf(stderr, __VA_ARGS__);                  \
        fprintf(stderr, "\n");                         \
        exit(1);                                       \
    } while (0)

typedef struct {
    ScfBlock b[MAXN];
    ScfCfg g;
} Gr;

static void gr_init(Gr *r, int n)
{
    memset(r, 0, sizeof *r);
    r->g.n = n;
    r->g.b = r->b;
    for (int i = 0; i < n; i++)
        r->b[i].succ[0] = r->b[i].succ[1] = X;
}
static void go(Gr *r, int i, int s)
{
    r->b[i].term = SCF_GOTO;
    r->b[i].succ[0] = s;
}
static void cond(Gr *r, int i, int s0, int s1)
{
    r->b[i].term = SCF_COND;
    r->b[i].succ[0] = s0;
    r->b[i].succ[1] = s1;
}

// Expected values for a hand-built case. flags strings: '1' = true, '0' = false, one char per block.
typedef struct {
    const char *name;
    const int *idom, *rpo;
    const char *loop, *merge;
    bool reducible;
    int nreach;
} Want;

static void check(Gr *r, const Want *w)
{
    ScfDom d;
    ntests++;
    if (!scf_dominators(&r->g, &d))
        FAIL("%s: scf_dominators failed", w->name);
    int n = r->g.n;
    if (d.n != n || d.nreach != w->nreach)
        FAIL("%s: n=%d nreach=%d, want %d", w->name, d.n, d.nreach, w->nreach);
    for (int i = 0; i < n; i++) {
        if (d.idom[i] != w->idom[i])
            FAIL("%s: idom[%d]=%d want %d", w->name, i, d.idom[i], w->idom[i]);
        if (d.rpo[i] != w->rpo[i])
            FAIL("%s: rpo[%d]=%d want %d", w->name, i, d.rpo[i], w->rpo[i]);
        if (d.loop_header[i] != (w->loop[i] == '1'))
            FAIL("%s: loop_header[%d]=%d", w->name, i, d.loop_header[i]);
        if (d.merge[i] != (w->merge[i] == '1'))
            FAIL("%s: merge[%d]=%d", w->name, i, d.merge[i]);
        if (d.rpo[i] >= 0 && d.order[d.rpo[i]] != i)
            FAIL("%s: order/rpo mismatch at %d", w->name, i);
    }
    if (d.reducible != w->reducible)
        FAIL("%s: reducible=%d", w->name, d.reducible);
    // scf_dominates sanity
    for (int a = 0; a < n; a++)
        for (int b2 = 0; b2 < n; b2++) {
            bool want = false;
            if (w->rpo[a] >= 0 && w->rpo[b2] >= 0)
                for (int x = b2; x >= 0; x = w->idom[x])
                    if (x == a)
                        want = true;
            if (scf_dominates(&d, a, b2) != want)
                FAIL("%s: dominates(%d,%d)", w->name, a, b2);
        }
    if (scf_dominates(&d, -1, 0) || scf_dominates(&d, 0, n) || scf_dominates(&d, 0, -1))
        FAIL("%s: dominates out of range", w->name);
    scf_dom_free(&d);
    if (d.idom || d.rpo || d.order || d.loop_header || d.merge || d.n || d.nreach)
        FAIL("%s: free did not zero", w->name);
    scf_dom_free(&d);  // safe on zeroed struct
}

static void hand_tests(void)
{
    Gr r;

    gr_init(&r, 3);  // straight line
    go(&r, 0, 1);
    go(&r, 1, 2);
    go(&r, 2, X);
    check(&r, &(Want){"line", (int[]){-1, 0, 1}, (int[]){0, 1, 2}, "000", "000", true, 3});

    gr_init(&r, 4);  // diamond
    cond(&r, 0, 1, 2);
    go(&r, 1, 3);
    go(&r, 2, 3);
    go(&r, 3, X);
    check(&r, &(Want){"diamond", (int[]){-1, 0, 0, 0}, (int[]){0, 2, 1, 3}, "0000", "0001", true, 4});

    gr_init(&r, 4);  // while: 0 -> 1(header) -> 2 (body) -> 1 ; 1 -> 3 exit
    go(&r, 0, 1);
    cond(&r, 1, 2, 3);
    go(&r, 2, 1);
    go(&r, 3, X);
    check(&r, &(Want){"while", (int[]){-1, 0, 1, 1}, (int[]){0, 1, 3, 2}, "0100", "0000", true, 4});

    gr_init(&r, 6);  // nested: 0->1(outer) ->2(inner) ->3->2 ; 2->4 ; 4->1 ; 4->5 (COND)
    go(&r, 0, 1);
    go(&r, 1, 2);
    cond(&r, 2, 3, 4);
    go(&r, 3, 2);
    cond(&r, 4, 1, 5);
    go(&r, 5, X);
    check(&r, &(Want){"nested", (int[]){-1, 0, 1, 2, 2, 4}, (int[]){0, 1, 2, 5, 3, 4}, "011000", "000000", true, 6});

    gr_init(&r, 5);  // loop with two exits: 1 header; 1 -> 2 | 4 ; 2 -> 3 | 1 ... 3 -> exit
    go(&r, 0, 1);
    cond(&r, 1, 2, 4);
    cond(&r, 2, 3, 1);
    go(&r, 3, X);
    go(&r, 4, X);
    check(&r, &(Want){"two exits", (int[]){-1, 0, 1, 2, 1}, (int[]){0, 1, 3, 4, 2}, "01000", "00000", true, 5});

    gr_init(&r, 3);  // irreducible two-entry loop
    cond(&r, 0, 1, 2);
    go(&r, 1, 2);
    go(&r, 2, 1);
    check(&r, &(Want){"irreducible", (int[]){-1, 0, 0}, (int[]){0, 1, 2}, "000", "011", false, 3});

    gr_init(&r, 5);  // unreachable blocks 2,3 (3 -> 1 edge from unreachable must not count)
    go(&r, 0, 1);
    go(&r, 1, X);
    go(&r, 2, 3);
    go(&r, 3, 1);
    go(&r, 4, 4);
    check(&r, &(Want){"unreachable", (int[]){-1, 0, -2, -2, -2}, (int[]){0, 1, -1, -1, -1}, "00000", "00000", true, 2});

    gr_init(&r, 3);  // EXIT successors, including a COND with an EXIT arm
    cond(&r, 0, X, 1);
    cond(&r, 1, 2, X);
    go(&r, 2, X);
    check(&r, &(Want){"exit", (int[]){-1, 0, 1}, (int[]){0, 1, 2}, "000", "000", true, 3});

    gr_init(&r, 2);  // COND with both successors the same: two in-edges
    cond(&r, 0, 1, 1);
    go(&r, 1, X);
    check(&r, &(Want){"cond same", (int[]){-1, 0}, (int[]){0, 1}, "00", "01", true, 2});

    gr_init(&r, 3);  // self loop
    go(&r, 0, 1);
    cond(&r, 1, 1, 2);
    go(&r, 2, X);
    check(&r, &(Want){"self loop", (int[]){-1, 0, 1}, (int[]){0, 1, 2}, "010", "000", true, 3});

    gr_init(&r, 1);  // entry self loop, single block
    go(&r, 0, 0);
    check(&r, &(Want){"entry self", (int[]){-1}, (int[]){0}, "1", "0", true, 1});

    gr_init(&r, 4);  // loop entered by two forward edges at the header: merge and header both
    cond(&r, 0, 1, 2);
    go(&r, 1, 3);
    go(&r, 2, 3);
    cond(&r, 3, 0, X);  // back edge to the entry
    check(&r, &(Want){"loop at entry", (int[]){-1, 0, 0, 0}, (int[]){0, 2, 1, 3}, "1000", "0001", true, 4});
}

// ---- random tests
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t m)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)((rng >> 11) % m);
}

static int succs(const ScfCfg *g, int u, int out[2])
{
    int k = 0;
    out[k++] = g->b[u].succ[0];
    if (g->b[u].term == SCF_COND)
        out[k++] = g->b[u].succ[1];
    return k;
}

// reference DFS (recursive; graphs are small)
static void ref_dfs(const ScfCfg *g, int u, bool *seen, int *post, int *np)
{
    seen[u] = true;
    int s[2], k = succs(g, u, s);
    for (int i = 0; i < k; i++)
        if (s[i] >= 0 && !seen[s[i]])
            ref_dfs(g, s[i], seen, post, np);
    post[(*np)++] = u;
}

// T1/T2 reduction on the reachable subgraph
static bool ref_reducible(const ScfCfg *g, const bool *reach)
{
    static bool adj[MAXN][MAXN];
    bool alive[MAXN];
    int n = g->n, left = 0;
    memset(adj, 0, sizeof adj);
    for (int u = 0; u < n; u++) {
        alive[u] = reach[u];
        left += reach[u];
        if (!reach[u])
            continue;
        int s[2], k = succs(g, u, s);
        for (int i = 0; i < k; i++)
            if (s[i] >= 0)
                adj[u][s[i]] = true;
    }
    for (bool ch = true; ch;) {
        ch = false;
        for (int v = 0; v < n; v++)
            if (alive[v] && adj[v][v]) {
                adj[v][v] = false;
                ch = true;
            }
        for (int v = 1; v < n; v++) {
            if (!alive[v])
                continue;
            int np = 0, p = -1;
            for (int u = 0; u < n; u++)
                if (alive[u] && adj[u][v]) {
                    np++;
                    p = u;
                }
            if (np != 1)
                continue;
            for (int w = 0; w < n; w++) {
                if (alive[w] && adj[v][w])
                    adj[p][w] = true;
                adj[w][v] = adj[v][w] = false;
            }
            alive[v] = false;
            left--;
            ch = true;
        }
    }
    return left == 1;
}

static void random_test(int iter)
{
    Gr r;
    int n = 5 + (int)rnd(56);
    gr_init(&r, n);
    for (int i = 0; i < n; i++) {
        int s0 = rnd(8) == 0 ? X : (int)rnd(n), s1 = rnd(8) == 0 ? X : (int)rnd(n);
        if (rnd(10) == 0 && i + 1 < n)
            s0 = i + 1;
        if (rnd(2))
            go(&r, i, s0);
        else
            cond(&r, i, s0, s1);
        if (r.b[i].term == SCF_GOTO)
            r.b[i].succ[1] = rnd(2) ? 0 : X;  // garbage that must be ignored
    }
    const ScfCfg *g = &r.g;
    ScfDom d;
    ntests++;
    if (!scf_dominators(g, &d))
        FAIL("random %d: failed", iter);

    bool reach[MAXN] = {0};
    int post[MAXN], np = 0;
    ref_dfs(g, 0, reach, post, &np);
    if (d.nreach != np)
        FAIL("random %d: nreach %d want %d", iter, d.nreach, np);
    int rpo[MAXN];
    for (int i = 0; i < n; i++)
        rpo[i] = -1;
    for (int i = 0; i < np; i++)
        rpo[post[np - 1 - i]] = i;

    // naive dominator sets
    uint64_t all = 0, dom[MAXN];
    for (int i = 0; i < n; i++)
        if (reach[i])
            all |= 1ull << i;
    for (int i = 0; i < n; i++)
        dom[i] = i == 0 ? 1ull : all;
    for (bool ch = true; ch;) {
        ch = false;
        for (int v = 1; v < n; v++) {
            if (!reach[v])
                continue;
            uint64_t m = all;
            for (int u = 0; u < n; u++) {
                if (!reach[u])
                    continue;
                int s[2], k = succs(g, u, s);
                for (int i = 0; i < k; i++)
                    if (s[i] == v)
                        m &= dom[u];
            }
            m |= 1ull << v;
            if (m != dom[v]) {
                dom[v] = m;
                ch = true;
            }
        }
    }

    bool lh[MAXN] = {0};
    int fwd[MAXN] = {0};
    bool red = true;
    for (int u = 0; u < n; u++) {
        if (!reach[u])
            continue;
        int s[2], k = succs(g, u, s);
        for (int i = 0; i < k; i++) {
            int v = s[i];
            if (v < 0)
                continue;
            if (dom[u] >> v & 1)
                lh[v] = true;
            else
                fwd[v]++;
            if (rpo[v] <= rpo[u] && !(dom[u] >> v & 1))
                red = false;
        }
    }

    for (int v = 0; v < n; v++) {
        int want = -2;
        if (reach[v] && v == 0)
            want = -1;
        else if (reach[v]) {
            uint64_t s = dom[v] & ~(1ull << v);
            for (int m = 0; m < n; m++)
                if ((s >> m & 1) && (s & ~(1ull << m) & ~dom[m]) == 0)
                    want = m;
        }
        if (d.idom[v] != want)
            FAIL("random %d: idom[%d]=%d want %d", iter, v, d.idom[v], want);
        if (d.rpo[v] != rpo[v])
            FAIL("random %d: rpo[%d]=%d want %d", iter, v, d.rpo[v], rpo[v]);
        if (d.loop_header[v] != lh[v])
            FAIL("random %d: loop_header[%d]", iter, v);
        if (d.merge[v] != (fwd[v] >= 2))
            FAIL("random %d: merge[%d]", iter, v);
        for (int a = 0; a < n; a++)
            if (scf_dominates(&d, a, v) != (reach[a] && reach[v] && (dom[v] >> a & 1)))
                FAIL("random %d: dominates(%d,%d)", iter, a, v);
    }
    for (int i = 0; i < np; i++)
        if (d.order[i] != post[np - 1 - i])
            FAIL("random %d: order[%d]", iter, i);
    if (d.reducible != red)
        FAIL("random %d: reducible=%d, retreating-edge reference says %d", iter, d.reducible, red);
    if (red != ref_reducible(g, reach))
        FAIL("random %d: T1/T2 says %d, retreating-edge reference says %d", iter, !red, red);
    scf_dom_free(&d);
}

int main(void)
{
    hand_tests();
    for (int i = 0; i < 2000; i++)
        random_test(i);

    // deep chain: no stack overflow, correct numbering
    enum { DEEP = 200000 };
    ScfCfg g = {DEEP, calloc(DEEP, sizeof(ScfBlock))};
    for (int i = 0; i < DEEP; i++)
        g.b[i].succ[0] = i + 1 < DEEP ? i + 1 : X, g.b[i].succ[1] = X;
    ScfDom d;
    ntests++;
    if (!scf_dominators(&g, &d) || d.nreach != DEEP || d.idom[DEEP - 1] != DEEP - 2 || d.rpo[DEEP - 1] != DEEP - 1)
        FAIL("deep chain");
    scf_dom_free(&d);
    free(g.b);

    printf("ok %d tests\n", ntests);
    return 0;
}
