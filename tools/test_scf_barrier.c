#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "scf.h"
int main(void) {
    const uint64_t code[] = {0, 0x50b0000000070f00ull, 0xf0a8000000070000ull, 0xe30000000007000full};
    ScfCfg g = {0}; char why[80];
    unsetenv("FE3H_COMPUTE_SCF");
    assert(!scf_build_cfg(code, 4, &g, why, sizeof why));
    setenv("FE3H_COMPUTE_SCF", "1", 1);
    assert(!scf_insn_is_flow(code[2]));
    assert(scf_build_cfg(code, 4, &g, why, sizeof why));
    assert(g.n == 1 && g.b[0].first <= 2 && g.b[0].last >= 2);
    assert(g.b[0].succ[0] == SCF_EXIT);
    free(g.b);
    puts("ok: BAR preserved inside its original block; default fallback retained");
    return 0;
}
