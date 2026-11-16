// The structured program of a reducible branch graph (scf.h, step 3), after N. Ramsey, "Beyond Relooper: recursive
// translation of unstructured control flow to structured control flow" (ICFP 2022):
//
//   do_tree(x): x's code wrapped in one BLOCK per dominator-tree child of x that is a merge block (two or more forward
//     in-edges), innermost first by reverse postorder; each such child's own do_tree follows the BLOCK it ends. A loop
//     header's whole result is a LOOP.
//   do_branch(x -> t): back edge (t at or before x in reverse postorder): CONTINUE to t's LOOP. Merge block: BREAK out of
//     the BLOCK that t follows. Otherwise t has x as its only forward predecessor: do_tree(t) right here.
//
// No block's code is repeated. The label depth of a BREAK / CONTINUE counts the LOOP / BLOCK nodes between it and its
// target (an IF isn't a label).
#include <stdlib.h>
#include <string.h>

#include "scf.h"

typedef struct {
    const ScfCfg *g;
    const ScfDom *d;
    int *kids;    // dominator tree children, by parent: kids[kstart[p] .. kstart[p + 1])
    int *kstart;
    int *ctx;     // enclosing labels, innermost last: block index, +n for "BLOCK followed by", or -1 - block for "LOOP of"
    int nctx;
    ScfNode *pool;
    int npool, cap;
    bool bad;
} S;

static ScfNode *node(S *s, ScfKind k) {
    if (s->npool == s->cap) { s->bad = true; return NULL; }
    ScfNode *n = &s->pool[s->npool++];
    memset(n, 0, sizeof *n);
    n->kind = k;
    n->block = -1;
    return n;
}

static ScfNode *seq(S *s, ScfNode *a, ScfNode *b) {
    if (!a) return b;
    if (!b) return a;
    ScfNode *n = node(s, SCF_N_SEQ);
    if (n) { n->a = a; n->b = b; }
    return n;
}

static int label_depth(S *s, int want) {
    for (int i = s->nctx - 1; i >= 0; i--)
        if (s->ctx[i] == want) return s->nctx - 1 - i;
    s->bad = true;  // (can't happen for a reducible graph)
    return 0;
}

static ScfNode *do_tree(S *s, int x);

static ScfNode *do_branch(S *s, int x, int t) {
    if (t == SCF_EXIT) return node(s, SCF_N_EXIT);
    ScfNode *n;
    if (s->d->rpo[t] <= s->d->rpo[x]) {  // back edge (reducible: t is a loop header dominating x)
        n = node(s, SCF_N_CONTINUE);
        if (n) n->depth = label_depth(s, -1 - t);
        return n;
    }
    if (s->d->merge[t]) {
        n = node(s, SCF_N_BREAK);
        if (n) n->depth = label_depth(s, s->g->n + t);
        return n;
    }
    return do_tree(s, t);
}

// x's code, its terminator, inside BLOCKs for the merge children ys[0..ny) (sorted by rpo, lowest first: the last one
// is the outermost BLOCK and comes last in the code)
static ScfNode *node_within(S *s, int x, const int *ys, int ny) {
    if (s->bad) return NULL;
    if (ny == 0) {
        ScfNode *code = node(s, SCF_N_CODE);
        if (!code) return NULL;
        code->block = x;
        const ScfBlock *b = &s->g->b[x];
        if (b->term == SCF_GOTO) return seq(s, code, do_branch(s, x, b->succ[0]));
        ScfNode *i = node(s, SCF_N_IF);
        if (!i) return NULL;
        i->block = x;
        i->a = do_branch(s, x, b->succ[0]);
        i->b = do_branch(s, x, b->succ[1]);
        return seq(s, code, i);
    }
    int y = ys[ny - 1];
    s->ctx[s->nctx++] = s->g->n + y;
    ScfNode *inner = node_within(s, x, ys, ny - 1);
    s->nctx--;
    ScfNode *blk = node(s, SCF_N_BLOCK);
    if (!blk) return NULL;
    blk->a = inner;
    return seq(s, blk, do_tree(s, y));
}

static const ScfDom *by_rpo_dom;  // (qsort context)
static int by_rpo(const void *a, const void *b) {
    return by_rpo_dom->rpo[*(const int *)a] - by_rpo_dom->rpo[*(const int *)b];
}

static ScfNode *do_tree(S *s, int x) {
    if (s->bad) return NULL;
    int *ys = malloc(sizeof(int) * (size_t)(s->kstart[x + 1] - s->kstart[x] + 1));
    if (!ys) { s->bad = true; return NULL; }
    int ny = 0;
    for (int k = s->kstart[x]; k < s->kstart[x + 1]; k++)
        if (s->d->merge[s->kids[k]]) ys[ny++] = s->kids[k];
    by_rpo_dom = s->d;
    qsort(ys, (size_t)ny, sizeof *ys, by_rpo);
    ScfNode *r;
    if (s->d->loop_header[x]) {
        s->ctx[s->nctx++] = -1 - x;
        ScfNode *body = node_within(s, x, ys, ny);
        s->nctx--;
        r = node(s, SCF_N_LOOP);
        if (r) r->a = body;
    } else {
        r = node_within(s, x, ys, ny);
    }
    free(ys);
    return r;
}

ScfNode *scf_build(const ScfCfg *g, const ScfDom *d, ScfNode **pool) {
    *pool = NULL;
    if (!d->reducible || g->n == 0) return NULL;
    S s = {.g = g, .d = d};
    // every block becomes at most a CODE + an IF or one jump per successor, plus a BLOCK / LOOP / SEQ each: 8n is ample
    s.cap = 8 * g->n + 8;
    s.pool = calloc((size_t)s.cap, sizeof *s.pool);
    s.kids = calloc((size_t)g->n + 1, sizeof *s.kids);
    s.kstart = calloc((size_t)g->n + 2, sizeof *s.kstart);
    s.ctx = calloc((size_t)g->n * 2 + 2, sizeof *s.ctx);
    ScfNode *root = NULL;
    if (s.pool && s.kids && s.kstart && s.ctx) {
        for (int v = 0; v < g->n; v++) if (d->idom[v] >= 0) s.kstart[d->idom[v] + 1]++;
        for (int p = 0; p < g->n; p++) s.kstart[p + 1] += s.kstart[p];
        int *fill = calloc((size_t)g->n + 1, sizeof *fill);
        if (fill) {
            for (int v = 0; v < g->n; v++) {
                int p = d->idom[v];
                if (p >= 0) s.kids[s.kstart[p] + fill[p]++] = v;
            }
            free(fill);
            root = do_tree(&s, 0);
        } else {
            s.bad = true;
        }
    }
    free(s.kids);
    free(s.kstart);
    free(s.ctx);
    if (s.bad || !root) { free(s.pool); return NULL; }
    *pool = s.pool;
    return root;
}
