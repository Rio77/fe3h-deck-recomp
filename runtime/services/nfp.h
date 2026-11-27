// HLE nn::nfp (nfp:user / nfp:IUser) and virtual Amiibo directory scanner.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NFP_MAX_ENTRIES 256

typedef struct {
    char name[256];        // Relative path under Amiibo/ without .bin (e.g. "Marth" or "Fire Emblem/Chrom")
    char rel_path[512];    // Relative path under Amiibo/ (e.g. "Marth.bin")
    uint8_t uid[7];        // NTAG215 7-byte UID (bytes 0..2, 4..7; BCC0 at byte 3 skipped)
    uint8_t char_id[3];    // Amiibo character ID bytes (raw[0x54..0x56])
    uint8_t nfp_type;      // Type byte (raw[0x57])
    uint16_t numbering_id; // Big-endian raw[0x58..0x59] decoded as uint16_t: (raw[0x58] << 8) | raw[0x59]
    uint8_t series_id;     // Series byte (raw[0x5A])
} NfpAmiiboInfo;

typedef struct {
    int selected_ui;     // 0 = "None", 1..count = index into scanned list
    int count;           // Total valid .bin entries detected
    int nfp_state;       // 0 = NonInitialized, 1 = Initialized
    int dev_state;       // 0..4 (NfpDeviceState)
    bool has_active_tag;
    bool tag_read_completed;
    NfpAmiiboInfo active_tag;
} NfpUiState;

// Parse a raw NTAG215 .bin dump (532..1024 bytes). Exposed for unit tests.
bool nfp_parse_bin(const uint8_t *raw, size_t size, NfpAmiiboInfo *out);

// Rescan <rt_data_root()>/Amiibo/ (max depth 2, ignoring symlinks and dotfiles).
void nfp_rescan_amiibos(void);

// Thread-safe snapshot of scanned amiibos & selection for the F1 UI.
// If io_gen != NULL and *io_gen matches the internal scan generation, `out_entries` is not rewritten.
void nfp_ui_snapshot(NfpAmiiboInfo *out_entries, int max_entries, uint64_t *io_gen, NfpUiState *out_state);

// Select an entry from UI index (0 = "None", 1..count = detected amiibo).
// Outside the Garreg Mach Monastery gazebo (device_state != SearchingForTag): arms the selection without clearing or firing events.
// While the gazebo is waiting (device_state == SearchingForTag): triggers TagFound + activate event immediately.
void nfp_ui_select(int ui_idx);

// Test helper: reset all NFP state (including scanned entries).
void nfp_test_reset_all(void);

#ifdef __cplusplus
}
#endif
