// Frame pacing: the game's speed doesn't depend on the frame rate.
// The game advances its world each frame by its frame period (16.67 ms) times the swap interval its frame timer
// (f_700000c910) reads from the system object - normally 1. When a frame takes longer than a vsync (GPU-bound at
// 32:9, a hitch), the next frame starts a vsync later but still advanced only 16.67 ms: slow motion (30 fps = half
// speed). tools/patches.py FRAME_STEPS_SITE routes that read here: we return how many vsyncs really passed since
// the last frame, so the world advances by the time that passed. The game's own waiting is unaffected (it reads
// the interval nowhere else). Shared by the macOS and Windows builds.
// FINDING (2026-10-09): forcing 2 steps did NOT change Link's walking speed - gameplay is a true fixed step per
// frame; this timer feeds something else. Kept for research, off by default.
//   LARECOMP_FRAMEPACE=1     hand the timer the vsyncs elapsed (default: the game's own value)
//   LARECOMP_FRAME_STEPS_MAX maximum steps per frame (default 4: after a long stall the game doesn't jump ahead)
//   LARECOMP_FORCE_STEPS=<n> (test) always n
#include <stdlib.h>

#include "../hle.h"

extern volatile uint64_t g_vsync_count;  // runtime/hle/vi.c
uint64_t g_frame_steps_late;             // (stats) frames that advanced more than one step

uint32_t rt_frame_steps(uint32_t interval) {
    static int on = -1, max_steps, forced;
    if (on < 0) {
        const char *e = ENV("LARECOMP_FRAMEPACE");
        on = e ? atoi(e) != 0 : 0;  // off by default: the world (Link's movement) doesn't use this step (tested)
        e = ENV("LARECOMP_FRAME_STEPS_MAX");
        max_steps = e ? atoi(e) : 4;
        if (max_steps < 1) max_steps = 1;
        e = ENV("LARECOMP_FORCE_STEPS");
        forced = e ? atoi(e) : 0;
    }
    static int ncall;
    if (ENV("LARECOMP_LOG_FRAMEPACE") && ncall++ % 120 == 0) LOG("[framepace] call %d interval %u vsync %llu\n", ncall, interval, (unsigned long long)g_vsync_count);
    if (forced > 0) return (uint32_t)forced;
    static uint64_t last;
    uint64_t now = __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED);
    uint64_t steps = last ? now - last : 1;
    last = now;
    if (!on) return interval;
    if (steps < interval) steps = interval;  // (the game's own 30 fps mode: it waits that many vsyncs anyway)
    if (steps < 1) steps = 1;
    if (steps > (uint64_t)max_steps) steps = (uint64_t)max_steps;
    if (steps > 1) g_frame_steps_late++;
    return (uint32_t)steps;
}

// ---- the game's frame limiter: sleep instead of spinning
// The main loop's limiter (f_70005be430) waits for its frame time by calling nn::os::YieldThread and re-reading
// GetSystemTick until (now - last) * 10^7 / freq reaches its target (x23, in 100 ns units: 166666 for 60 fps).
// Here a yield is a sched_yield syscall: the game thread spun millions of times a second, a core always at 100%,
// and the pacer (vulkan/exec.c vk_pace_frame) counted the spin as busy time. From that one call site, the yield
// sleeps until just before the deadline instead (counted as blocked); the loop's own last checks stay as they were.
// Every other YieldThread caller gets the guest's own. LARECOMP_LIMITER_SPIN=1: the game's spin, as before.
#include "../platform.h"

#define LIMITER_YIELD_LR 0x70005be4c4ull  // return address of the limiter loop's YieldThread call
#define SDK_YIELD_THREAD 0x700431bb30ull  // the guest's nn::os::YieldThread
// Linux's powersave scheduler needs more headroom than the upstream 250 us.
// Kept adjustable so the Deck's measured frame pacing decides the shipped default.
#define LIMITER_MARGIN_US 3000ull
extern __thread uint64_t g_blocked_ns;    // kernel.c
uint64_t g_limiter_sleep_ns;              // (stats) time the limiter slept

static bool guest_ptr_ok(uint64_t p) { return p >= 0x7000000000ull && p < 0x7800000000ull && !(p & 7); }

HLE_FUNC("_ZN2nn2os11YieldThreadEv", os_yield_thread) {
    static int spin = -1;
    if (spin < 0) spin = ENV("LARECOMP_LIMITER_SPIN") && atoi(ENV("LARECOMP_LIMITER_SPIN"));
    static uint64_t margin_ns;
    if (!margin_ns) {
        const char *m = ENV("FE3H_LIMITER_MARGIN_US");
        uint64_t us = m ? strtoull(m, NULL, 10) : LIMITER_MARGIN_US;
        if (us < 250) us = 250;
        if (us > 3000) us = 3000;
        margin_ns = us * 1000;
    }
    if (c->x[30] == LIMITER_YIELD_LR && !spin && guest_ptr_ok(c->x[19]) && guest_ptr_ok(c->x[22])) {
        uint64_t last = *(volatile uint64_t *)(uintptr_t)c->x[19], freq = *(volatile uint64_t *)(uintptr_t)c->x[22];
        uint64_t target = c->x[23];  // 100 ns units
        if (freq && target && target < 10000000ull) {  // (under a second: a sane frame time)
            uint64_t deadline = last + (uint64_t)(((unsigned __int128)target * freq + 9999999u) / 10000000u);
            uint64_t now = rt_ticks();
            if (deadline > now) {
                uint64_t wait_ns = (uint64_t)((unsigned __int128)(deadline - now) * 1000000000u / freq);
                if (wait_ns > margin_ns) {
                    uint64_t t0 = plat_now_ns();
                    plat_sleep_until_ns(t0 + wait_ns - margin_ns);
                    uint64_t slept = plat_now_ns() - t0;
                    g_blocked_ns += slept;
                    g_limiter_sleep_ns += slept;
                    return;
                }
            }
        }
    }
    c->pc = SDK_YIELD_THREAD;
    rt_dispatch(c);
}
