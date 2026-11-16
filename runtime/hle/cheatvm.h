// Cheats from cheats.txt (Atmosphere's format) run by an Atmosphere cheat VM: runtime/hle/cheatvm.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    bool on, master;
    bool unsupported;  // an opcode the VM doesn't run
    bool needs_gen;    // an instruction it patches isn't compiled in yet (make gen-main, then make)
    const char *desc;  // its "# ..." lines ("" if none)
    const char *excludes;  // the cheats patching the same code, switched off when this one goes on ("" if none)
} CheatInfo;

void cheatvm_frame(void);  // once a frame: the enabled cheats
int cheatvm_count(void);
bool cheatvm_info(int i, CheatInfo *out);
void cheatvm_set(int i, bool on);
uint32_t cheat_word(uint64_t addr);  // the generated code's question: the active instruction there (0: the original)
bool cheatvm_settings_load(const char *key, const char *val);  // cheat.<name> = 1
void cheatvm_settings_save(FILE *f);

#ifdef __cplusplus
}
#endif
