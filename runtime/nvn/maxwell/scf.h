// Structured control flow for translated shaders (LARECOMP_SCF): instead of one `switch (pc)` loop over every branch
// target, the shader's branch graph is turned into nested if / loop / block constructs (after N. Ramsey, "Beyond
// Relooper", ICFP 2022). Shaders the method can't handle keep the switch loop.
//
//   1. scf_cfg.c   the branch graph (ScfCfg) of a Maxwell program: blocks of instructions and their successors, with the
//                  SSY / PBK / PCNT / CAL stacks resolved statically
//   2. scf_dom.c   dominator tree, reverse postorder, loop headers, merge blocks, reducibility (ScfDom)
//   3. scf_build.c the structured program (ScfNode tree) from ScfCfg + ScfDom; translate.c prints it as GLSL
//   4. scf_sim.c   runs a ScfCfg and a ScfNode tree with the same branch decisions (tests only: tools/test_scf_*.c check
//                  that both go through the same blocks in the same order)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- branch graph
#define SCF_EXIT (-1)  // successor "leave the shader"

typedef enum { SCF_GOTO, SCF_COND } ScfTerm;

typedef struct {
    uint32_t first, last;  // the block's instructions: indices (pc / 8), first..last inclusive (scheduling words skipped)
    ScfTerm term;
    // GOTO: succ[0] (the next block, a jump target or SCF_EXIT). COND: succ[0] when the condition holds (the branch is
    // taken), succ[1] when it doesn't (falls through). Each is a block index or SCF_EXIT.
    int succ[2];
    int pred;       // COND: predicate register 0..6 (7 is PT and never makes a COND)
    bool pred_neg;  // COND: the condition is !P
} ScfBlock;

typedef struct {
    int n;        // blocks; block 0 is the entry
    ScfBlock *b;  // n blocks
} ScfCfg;

// Builds g (allocating g->b) for the program's instructions code[0..end) (index = pc / 8; every fourth word is a
// scheduling word). False with a reason in why when the program isn't handled: indirect or absolute jumps, barriers,
// flow stacks that differ between paths to one instruction, overflow or popping an empty stack.
bool scf_build_cfg(const uint64_t *code, size_t end, ScfCfg *g, char *why, size_t whylen);
// Whether instruction w is a flow instruction (jumps and SSY / PBK / PCNT / PRET pushes): translated as structure,
// not as an instruction. (False for one that never runs, @!PT.)
bool scf_insn_is_flow(uint64_t w);

// ---- dominators (scf_dom.c)
typedef struct {
    int n;               // = cfg n
    int *idom;           // immediate dominator; the entry: -1; blocks unreachable from the entry: -2
    int *rpo;            // reverse postorder number of a reachable block (entry 0); unreachable: -1
    int *order;          // order[i]: the block with rpo number i, i < nreach
    int nreach;          // reachable blocks
    bool *loop_header;   // target of a back edge: an edge u -> h where h dominates u (u reachable)
    bool *merge;         // two or more forward in-edges from reachable blocks (back edges don't count)
    bool reducible;      // every retreating edge (rpo[target] <= rpo[source], reachable source) is a back edge
} ScfDom;

// Fills d (allocating its arrays) for g. False only if an allocation failed (d is then freed). SCF_EXIT successors are
// ignored (no virtual exit node). Successor order: succ[0] is visited before succ[1] in the depth-first search.
bool scf_dominators(const ScfCfg *g, ScfDom *d);
void scf_dom_free(ScfDom *d);
// Whether a dominates b (every block dominates itself). Both reachable; otherwise false.
bool scf_dominates(const ScfDom *d, int a, int b);

// ---- structured program (scf_build.c)
// Like WebAssembly's control instructions: LOOP and BLOCK are labels. BREAK n leaves the n-th enclosing LOOP or BLOCK
// (0 = innermost) and continues after it; CONTINUE n jumps back to the start of the n-th enclosing construct, which must
// be a LOOP. Falling off the end of a LOOP's body leaves the loop (a loop repeats only by CONTINUE).
typedef enum {
    SCF_N_SEQ,       // a, then b (b may be NULL)
    SCF_N_CODE,      // the instructions of CFG block `block` (its terminator is not part of it)
    SCF_N_IF,        // the condition of CFG block `block`'s COND terminator: a when it holds, b when not (either NULL)
    SCF_N_LOOP,      // body a
    SCF_N_BLOCK,     // body a
    SCF_N_BREAK,     // depth
    SCF_N_CONTINUE,  // depth
    SCF_N_EXIT,      // leave the shader
} ScfKind;

typedef struct ScfNode ScfNode;
struct ScfNode {
    ScfKind kind;
    int block;
    int depth;
    ScfNode *a, *b;
};

// The structured program for g (d = its dominators), or NULL when g is irreducible (or out of memory). The nodes live
// in one allocation returned in *pool: free(*pool) frees the tree. Unreachable blocks don't appear.
ScfNode *scf_build(const ScfCfg *g, const ScfDom *d, ScfNode **pool);

// ---- simulation (scf_sim.c, for the tests in tools/test_scf_*.c): runs a CFG and a structured program with the same branch
// decisions and records the CFG blocks whose CODE ran, in order. Decision for a COND block = scf_decide(seed, block,
// k) where k counts how often that block's condition was evaluated before (0, 1, 2, ...).
bool scf_decide(uint64_t seed, int block, int k);
// Each returns the trace length (at most max; stops after max CODE steps or at the exit). trace[i]: block index.
int scf_sim_cfg(const ScfCfg *g, uint64_t seed, int *trace, int max);
int scf_sim_tree(const ScfNode *root, const ScfCfg *g, uint64_t seed, int *trace, int max);

#ifdef __cplusplus
}
#endif
