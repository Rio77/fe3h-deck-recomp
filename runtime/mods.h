// Mods: folders of game data laid over the game's own (runtime/mods.c).
//
// A mod is a folder in the mods folder next to the game files, holding a romfs/ like the game's (the Yuzu / Ryujinx
// layout): <game files>/mods/<name>/romfs/..., and for a DLC pack <game files>/mods/<name>/dlc/<content id>/romfs/...
// (the pack's own files, e.g. its DATA0.bin / DATA1.bin). Mods are on when listed in <game files>/mods/load_order.txt,
// one folder name per line (later lines win where two mods change the same thing; lines starting with # are skipped).
// The game's own folders are never changed: runtime/services/fs.c and aoc.c lay out the game's data images from the
// stacks of folders.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The folders to lay out the game's data from, for the game files' romfs folder romfs_dir: romfs_dir first, then each
// enabled mod's romfs/ in load order, then the merged update indexes (when mods change one). Builds those merged
// indexes and logs the load order. Returns the count; *dirs stays valid until the next call. 1: no mods.
int mods_romfs_stack(const char *romfs_dir, const char *const **dirs);
const char *mods_last_error(void);

// The same for a DLC pack's romfs folder (content id: id): the pack's folder, then the enabled mods' dlc/<id>/romfs.
// The load order is the one read for the game's data (mods_romfs_stack, which runs first). 1: no mods for this pack.
int mods_dlc_stack(const char *dlc_romfs, uint64_t id, const char *const **dirs);

// For romfs_layout_stack: a mod's file going into the image (counted, conflicts logged).
void mods_note_file(void *ctx, int dir, const char *rel, int replaced);

// After the layout: what each mod brought, for the log.
void mods_report(void);

// ---- the start screen's Mods tab (runtime/ui/launcher.mm)
enum { MOD_TEXT_NONE = 0, MOD_TEXT_US = 1, MOD_TEXT_EU = 2 };  // (bits) the English text a mod changes: US / European
typedef struct {
    char name[256];               // its folder in <game files>/mods
    int place;                    // 0: off; else its place in the load order (1 first; later ones win)
    bool has_romfs, has_exefs;    // (exefs: code changes, which can't apply)
    int dlc_packs;                // DLC packs it has files for
    int new_files, replaced_files;    // romfs files: new / at the game's own paths
    int index_changed, index_added;   // update index entries changed / added against the game's
    int text;                     // MOD_TEXT_* bits
} ModInfo;

// The folders in <game_root>/mods, with their places from load_order.txt and what each changes. Sorted: the ones on
// in load order, then the ones off by name. Returns the count (at most max).
int mods_scan(const char *game_root, ModInfo *out, int max);
// Writes load_order.txt from the places in mods (0: off). false: couldn't write it.
bool mods_save_order(const char *game_root, const ModInfo *mods, int n);
// The conflicts between the mods that are on (places in mods), one line each to line(); nothing written. Returns
// the count.
int mods_check_conflicts(const char *game_root, const ModInfo *mods, int n, void (*line)(void *ctx, const char *text),
                         void *ctx);

#ifdef __cplusplus
}
#endif
