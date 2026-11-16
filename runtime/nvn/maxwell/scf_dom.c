// Dominators, reverse postorder, loop headers, merge blocks and reducibility of an ScfCfg.
// Dominators: K. Cooper, T. Harvey, K. Kennedy, "A Simple, Fast Dominance Algorithm".
#include "scf.h"

#include <stdlib.h>
#include <string.h>

// i-th successor of block u (i < 2), or -1 when there is no such edge.
static int succ_of(const ScfCfg *g, int u, int i)
{
    if (i == 1 && g->b[u].term != SCF_COND)
        return -1;
    int s = g->b[u].succ[i];
    return (s >= 0 && s < g->n) ? s : -1;
}

void scf_dom_free(ScfDom *d)
{
    free(d->idom);
    free(d->rpo);
    free(d->order);
    free(d->loop_header);
    free(d->merge);
    memset(d, 0, sizeof *d);
}

bool scf_dominates(const ScfDom *d, int a, int b)
{
    if (a < 0 || b < 0 || a >= d->n || b >= d->n || d->rpo[a] < 0 || d->rpo[b] < 0)
        return false;
    for (int x = b; x >= 0; x = d->idom[x])
        if (x == a)
            return true;
    return false;
}

static int intersect(const ScfDom *d, int a, int b)
{
    while (a != b) {
        while (d->rpo[a] > d->rpo[b])
            a = d->idom[a];
        while (d->rpo[b] > d->rpo[a])
            b = d->idom[b];
    }
    return a;
}

bool scf_dominators(const ScfCfg *g, ScfDom *d)
{
    int n = g->n;
    size_t cnt = n > 0 ? (size_t)n : 1;
    memset(d, 0, sizeof *d);
    d->n = n;
    d->idom = malloc(cnt * sizeof(int));
    d->rpo = malloc(cnt * sizeof(int));
    d->order = malloc(cnt * sizeof(int));
    d->loop_header = calloc(cnt, sizeof(bool));
    d->merge = calloc(cnt, sizeof(bool));
    int *stack = malloc(cnt * sizeof(int));  // DFS stack of blocks
    int *next = calloc(cnt, sizeof(int));    // next successor slot to try, per block
    bool *seen = calloc(cnt, sizeof(bool));
    int *post = malloc(cnt * sizeof(int));
    int *pstart = calloc(cnt + 1, sizeof(int));  // predecessor lists (CSR), reachable sources only
    int *pfill = malloc(cnt * sizeof(int));
    int nedges = 0;
    int *preds = NULL;
    if (!d->idom || !d->rpo || !d->order || !d->loop_header || !d->merge || !stack || !next || !seen || !post ||
        !pstart || !pfill)
        goto fail;

    for (int i = 0; i < n; i++) {
        d->idom[i] = -2;
        d->rpo[i] = -1;
    }

    // iterative DFS from the entry
    int npost = 0, sp = 0;
    if (n > 0) {
        stack[sp++] = 0;
        seen[0] = true;
    }
    while (sp > 0) {
        int u = stack[sp - 1];
        if (next[u] < 2) {
            int s = succ_of(g, u, next[u]++);
            if (s >= 0 && !seen[s]) {
                seen[s] = true;
                stack[sp++] = s;
            }
        } else {
            post[npost++] = u;
            sp--;
        }
    }
    d->nreach = npost;
    for (int i = 0; i < npost; i++) {
        int v = post[npost - 1 - i];
        d->order[i] = v;
        d->rpo[v] = i;
    }

    // predecessor lists
    for (int i = 0; i < npost; i++)
        for (int k = 0; k < 2; k++) {
            int s = succ_of(g, d->order[i], k);
            if (s >= 0) {
                pstart[s + 1]++;
                nedges++;
            }
        }
    for (int i = 0; i < n; i++)
        pstart[i + 1] += pstart[i];
    preds = malloc((nedges > 0 ? (size_t)nedges : 1) * sizeof(int));
    if (!preds)
        goto fail;
    memcpy(pfill, pstart, cnt * sizeof(int));
    for (int i = 0; i < npost; i++)
        for (int k = 0; k < 2; k++) {
            int u = d->order[i], s = succ_of(g, u, k);
            if (s >= 0)
                preds[pfill[s]++] = u;
        }

    // iterate to a fixpoint
    if (npost > 0)
        d->idom[0] = 0;
    for (bool changed = npost > 1; changed;) {
        changed = false;
        for (int i = 1; i < npost; i++) {
            int v = d->order[i], nd = -2;
            for (int p = pstart[v]; p < pstart[v + 1]; p++) {
                int u = preds[p];
                if (d->idom[u] == -2)
                    continue;
                nd = nd == -2 ? u : intersect(d, u, nd);
            }
            if (nd != d->idom[v]) {
                d->idom[v] = nd;
                changed = true;
            }
        }
    }
    if (npost > 0)
        d->idom[0] = -1;

    // loop headers, merges, reducibility
    d->reducible = true;
    for (int v = 0; v < n; v++) {
        int fwd = 0;
        for (int p = pstart[v]; p < pstart[v + 1]; p++) {
            int u = preds[p];
            if (scf_dominates(d, v, u))
                d->loop_header[v] = true;
            else {
                fwd++;
                if (d->rpo[v] <= d->rpo[u])
                    d->reducible = false;
            }
        }
        d->merge[v] = fwd >= 2;
    }

    free(stack);
    free(next);
    free(seen);
    free(post);
    free(pstart);
    free(pfill);
    free(preds);
    return true;

fail:
    free(stack);
    free(next);
    free(seen);
    free(post);
    free(pstart);
    free(pfill);
    free(preds);
    scf_dom_free(d);
    return false;
}
