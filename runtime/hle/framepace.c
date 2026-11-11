// Frame pacing for XC3's FPS option. Two paths:
// - 30: the game's own code (vk_pace_frame in exec.c picks its 30 fps mode when frames are slow).
// - above 30 (60 to 360, or unlimited): the real-time frame step (xc3_frame_present below). Adapted from xc3-recomp
//   (github.com/jmars125/xc3-recomp @ 2b88f29, MIT, Copyright (c) 2026 Valiantsin Dzerakh; NOTICE.md) runtime/frame_rate.c,
//   itself adapted from MacNativeGames games/totk/runtime/game_patches.c (MIT): the game's frame-step update takes the
//   step from here (games/xc3/patches.py, set fps-realtime), its limiter returns at once, and this file holds each
//   present to the limit and steps the world by the time the frames are actually shown.
// - XC3_REALTIME=0 at 60: the community 60 FPS cheat's writes each presented frame (xc3_fps_frame), as before.
// XCDE's frame limiter hooks, its frame-step timer and the T224 memory research tools were tied to XCDE's patches and
// were removed with the XCDE profile.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../diag/diag.h"
#include "../hle.h"
#include "../platform.h"
#include "../settings.h"
#include "../cpu.h"

// The option (launcher "Cutscenes") holds a cutscene at 30 fps: the game's poses there change at 30, and with the
// camera at 60 over them the motion looks uneven (the user: cutscenes feel like 30, Dickson's ponytail shakes).
// cutscenes = 30fg (launcher "Frame generation in cutscenes only"): the same, plus frame generation only there -
// gameplay keeps its FPS without it (the user plays locked at 60; generated frames there would look laggy).
static bool g_cut30;
bool xcde_cutscene_30(void) { return g_cut30; }
// the frame rate the window is presented at (device.c repeat presents): 30 while a cutscene is held at 30
int xcde_present_fps(void) { return g_cut30 ? 30 : g_settings.fps; }

// ---- the real-time frame step (fps above 30)
// The game's frame-step update (f_700000f3c0) makes 60 / its vsync count the frame rate and steps the world by the
// frame's measured time clamped to [1 / rate, 2 / rate]: with the vsync count at 1 at least 1/60 s a frame, so at 120
// fps the world ran twice as fast. With the patch set on it reads the rate and the step from xc3_rt_fps / xc3_rt_step:
// the average of the last 5 intervals between presents, at most 1/15 s (slower frames slow the game down, as on the
// console). Cutscenes, story events and the title menus run at 30 (their animation advances per frame; at 120 a
// cutscene ran ahead of its voices): step 1/30, vsync count 2, presents held to 30.
// XC3_REALTIME=0: off (60 then uses the cheat path); off at the 30 option and in deterministic runs (replays).
// XC3_MENU_TIME=0: the interface keeps the game's per-frame step instead of the scaled one.
// XC3_CUTSCENE_30=0: cutscenes at the limit too.
volatile int xc3_rt_on, xc3_rt_menus;
volatile int32_t xc3_rt_fps = 60;
volatile float xc3_rt_step = 1.0f / 60.0f, xc3_rt_ratio = 0.5f;
volatile uint64_t xc3_rt_obj;

#define RT_SAMPLES 5
#define RT_STEP_MAX (1.0 / 15.0)
#define RT_RESET_NS 250000000ull   // a gap this long (loading, a debugger) restarts the average
#define RT_VSYNCS   ((volatile uint32_t *)(uintptr_t)0x7001B42DB0ull)   // the game's vsync count per frame (two u32)
#define RT_CUT_FLAG ((volatile uint8_t *)(uintptr_t)0x7001BA1A98ull)    // 1 in a cutscene (xc3_fps_frame)
#define RT_REPORT   0x7001b4d688ull   // *(*this) + 0x1e0: the story event playing (xc3-recomp frame_rate.c cutscene)

static bool guest_ptr(uint64_t a) { return a >= GUEST_LO && a < HEAP_BASE + HEAP_MAX; }

static int rt_init(void) {
    static int mode = -1;
    if (mode >= 0) return mode;
    const char *e = getenv("XC3_REALTIME");
    mode = g_settings.fps != 30 && !diag_deterministic() && !(e && !strcmp(e, "0"));
    const char *m = getenv("XC3_MENU_TIME");
    xc3_rt_menus = mode && !(m && !strcmp(m, "0"));
    xc3_rt_on = mode;
    if (mode) LOG("[fps] real-time frame step on: limit %d fps (0: unlimited), interface step %s\n", g_settings.fps,
                  xc3_rt_menus ? "scaled" : "the game's");
    return mode;
}

static bool rt_cutscene(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("XC3_CUTSCENE_30"); on = !(e && !strcmp(e, "0")); }
    if (!on) return false;
    if (*RT_CUT_FLAG) return true;
    uint64_t obj = xc3_rt_obj;   // bit 2 of +0x1fc: the game takes a fixed step (title menus, every cutscene)
    if (guest_ptr(obj) && (*(volatile uint32_t *)(uintptr_t)(obj + 0x1fc) & 4)) return true;
    uint64_t rep = *(volatile uint64_t *)(uintptr_t)RT_REPORT;
    if (!guest_ptr(rep)) return false;
    rep = *(volatile uint64_t *)(uintptr_t)rep;
    return guest_ptr(rep) && *(volatile uint32_t *)(uintptr_t)(rep + 0x1e0) != 0;
}

static void rt_store(double step) {
    xc3_rt_step = (float)step;
    xc3_rt_ratio = (float)(step * 30.0);
    xc3_rt_fps = (int32_t)(1.0 / step + 0.5);
}

// nvnQueuePresentTexture (the game's thread), before the image is queued: holds the frame to the limit and takes the
// interval since the last present as the next step. Returns false when the real-time step is off.
bool xc3_frame_present(void) {
    if (!rt_init()) return false;
    static uint64_t next, last;
    static double samples[RT_SAMPLES];
    static unsigned count, at;
    bool cut = rt_cutscene();
    if (cut != g_cut30) {
        g_cut30 = cut;
        count = at = 0;   // the average starts again at the new rate
        LOG("[fps] %s\n", cut ? "cutscene, story event or title menu: 30 fps" : "play: the limit again");
    }
    uint32_t want = cut ? 2 : 1;
    if (RT_VSYNCS[0] != want) RT_VSYNCS[0] = RT_VSYNCS[1] = want;
    int limit = cut ? 30 : g_settings.fps;
    uint64_t now = plat_now_ns();
    if (limit > 0) {
        uint64_t period = 1000000000ull / (uint64_t)limit;
        next = next && now < next + period ? next + period : now;   // (behind by a frame or more: no catching up)
        if (next > now) plat_sleep_until_ns(next);
        now = plat_now_ns();
    }
    if (cut) {
        rt_store(1.0 / 30.0);
    } else if (last) {
        uint64_t dt = now - last;
        if (dt > RT_RESET_NS || dt < 1000000ull) {
            count = at = 0;
        } else {
            double d = dt * 1e-9;
            if (d > RT_STEP_MAX) d = RT_STEP_MAX;
            samples[at++ % RT_SAMPLES] = d;
            if (count < RT_SAMPLES) count++;
            double sum = 0;
            for (unsigned i = 0; i < count; i++) sum += samples[i];
            rt_store(sum / count);
        }
    }
    last = now;
    static int lg = -1;   // XC3_FPS_LOG=1: the step, every 300 presents
    static uint64_t n;
    if (lg < 0) lg = getenv("XC3_FPS_LOG") != NULL;
    if (lg && ++n % 300 == 0) LOG("[fps] step %.2f ms (%d fps)%s\n", xc3_rt_step * 1e3, (int)xc3_rt_fps, cut ? ", cutscene" : "");
    return true;
}

// [XC3] 60 fps (FPS option 60): what the community cheat "60 FPS Gameplay + 30 FPS Cutscene + UI Speed Fix"
// (github.com/ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats, build 0AE74B263D8AC3CF) writes. Its addresses hold the same
// things in 2.2.0 (82D187FE9EF9BE92), read in a running game on 2026-10-29: main+0x1B42DB0 the frame interval (two
// u32, 2 = 30 fps), *(main+0x1BA1A90) + 0x80 the frame time the game steps its world by (1/30), main+0x1BA1A98 a
// cutscene flag (1 in a cutscene). Outside cutscenes interval 1 and 1/60; in cutscenes the game's 2 and 1/30 (held at
// 30 fps, presented at 30 - xcde_present_fps). Written each presented frame (device.c), as the cheat engine does.
// Above 60: not supported by these (the option is 30 / 60 for XC3).
static bool xc3_60;  // (this frame: 60 fps gameplay - xc3_ui_step)
void xc3_fps_frame(void) {
    static int on = -1;
    if (on < 0) on = g_settings.fps != 30 && !ENV("XC3_FPS_OFF");
    if (!on || xc3_rt_on) return;   // (the real-time step does this: xc3_frame_present)
    volatile uint32_t *iv = (volatile uint32_t *)(uintptr_t)0x7001B42DB0ull;
    volatile uint8_t *cut = (volatile uint8_t *)(uintptr_t)0x7001BA1A98ull;
    uint64_t ft = *(volatile uint64_t *)(uintptr_t)0x7001BA1A90ull;
    if (ft < GUEST_LO || ft >= HEAP_BASE + HEAP_MAX) return;  // (not made yet: boot)
    volatile float *step = (volatile float *)(uintptr_t)(ft + 0x80);
    bool in_cut = *cut != 0;
    xc3_60 = !in_cut;
    g_cut30 = in_cut;
    iv[0] = iv[1] = in_cut ? 2 : 1;
    // the world steps by this each frame: the cheat writes 1/60, which is slow motion when a frame takes longer (40 fps:
    // 0.67x). The measured frame time instead (average of the last 8 presents, 1/60 .. 1/30): the world keeps its
    // speed between 30 and 60 fps. XC3_FPS_FIXED_STEP=1: the cheat's fixed 1/60.
    static uint64_t last, hist[8];
    static int nh, fixed = -1;
    if (fixed < 0) fixed = ENV("XC3_FPS_FIXED_STEP") != NULL || diag_deterministic();
    uint64_t now = plat_now_ns();
    if (last && now - last < 200000000ull) hist[nh++ % 8] = now - last;  // (a stall: not a frame time)
    last = now;
    float dt = 1.0f / 60.0f;
    if (!fixed && nh >= 8) {
        uint64_t s = 0;
        for (int i = 0; i < 8; i++) s += hist[i];
        dt = (float)(s / 8) * 1e-9f;
        dt = dt < 1.0f / 60.0f ? 1.0f / 60.0f : dt > 1.0f / 30.0f ? 1.0f / 30.0f : dt;
    }
    *step = in_cut ? 1.0f / 30.0f : dt;
    {   // XC3_FPS_LOG=1: the step written, every 300 presents
        static int lg = -1;
        static uint64_t n;
        if (lg < 0) lg = ENV("XC3_FPS_LOG") != NULL;
        if (lg && ++n % 300 == 0) LOG("[fps] XC3 step %.2f ms (%s), interval %u\n", *step * 1e3, in_cut ? "cutscene" : "gameplay", iv[0]);
    }
    static int logged;
    if (!logged) { logged = 1; LOG("[fps] XC3 60 fps: interval and frame time written (cutscenes at 30)\n"); }
}
// [XC3] the cheat's "UI Speed Fix" (games/xc3/patches.py, main+0x67370): at 60 fps the UI layout's frame step
// ([layout + 0x214]) stays 1/30 - its 2D animations otherwise ran twice as fast
void xc3_ui_step(uint64_t layout) {
    if (xc3_60 && layout && !xc3_rt_on) *(volatile float *)(uintptr_t)(layout + 0x214) = 1.0f / 30.0f;
}
