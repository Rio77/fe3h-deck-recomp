// The game's RomFS image, laid out from a folder of game files (runtime/romfs_build.c). The game reads its data through
// one raw RomFS image; runtime/services/fs.c serves that image from this layout - the header and tables in memory, each
// file's data straight from its file in the player's romfs/ folder - so no second copy of the game data is kept.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROMFS_DATA_START 0x200u
typedef struct { uint64_t start, size; char *path; } RomfsLayoutFile;  // start: offset in the image
typedef struct {
    uint8_t header[ROMFS_DATA_START];  // the image's first bytes (the 0x50-byte header, then zeros)
    uint64_t tables_off, tables_size;  // dir hash, dir entries, file hash and file entries, one blob
    uint8_t *tables;
    RomfsLayoutFile *files;  // sorted by start; the bytes between files read as zeros
    int nfiles;
    uint64_t size;  // the whole image
} RomfsLayout;

// Lays out the RomFS image of the folder `dir`. false = failed (why: a short message).
bool romfs_layout(const char *dir, RomfsLayout *out, char *why, size_t why_size);

// Lays out one RomFS image from a stack of folders (mods: runtime/mods.c): dirs[0] first (the game's romfs), each
// later folder on top of the ones before - a file at the same path replaces the earlier one, new files and folders
// are added. note (optional) is told of each file a later folder brings: its folder's index, its path in the image
// ("a/b/c") and the index of the folder whose file it replaces (-1: a new file). false = failed (why).
typedef void (*RomfsStackNote)(void *ctx, int dir, const char *rel, int replaced);
bool romfs_layout_stack(const char *const *dirs, int ndirs, RomfsLayout *out, RomfsStackNote note, void *ctx,
                        char *why, size_t why_size);
void romfs_layout_free(RomfsLayout *l);

// Bytes of file data under dir (the image is about this plus its tables).
uint64_t romfs_dir_bytes(const char *dir);

// Whether dir looks like a clean copy of FE3H 1.2.0's romfs (the base game with the update applied, no mods or edited
// files): its file count and the sizes of DATA0.bin, DATA1.bin and patch4/INFO0.bin, as in a clean dump. Mods
// installed into the game files change them (a mod's own patch4/INFO0.bin, its files under mods/); a mismatch can
// crash the game. false = not clean (why: what differs, for the log and the start screen). A warning only: the game
// still starts.
#define ROMFS_CLEAN_FILES 3696
bool romfs_check_clean(const char *dir, char *why, size_t why_size);

// Copies the folder tree src to dst (hard links where the file system allows: no second copy of 6 GB). progress as
// above. false = failed.
bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));

#ifdef __cplusplus
}
#endif

// Existing importer structural checks; content mismatches are warnings separately.
bool romfs_check_structure(const char *dir, char *why, size_t why_size);
