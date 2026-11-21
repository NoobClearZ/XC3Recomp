// [XC3] Game-thread hooks of the pad read (runtime/hle/input.c fill_state): the map jump tour (XCDE_TOUR, below).
// XCDE's party mod (Xenoblade 3-style leader switching, arts hotkeys, the direct mouse camera; docs/plan_party_switch.md)
// was built on XCDE's addresses and was removed with the XCDE profile: the calls input.c still makes are no-ops here.
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../rt.h"
#include "../cpu.h"
#include "../diag/diag.h"
#include "../platform.h"
#include "../settings.h"

// From the keys / pad (runtime/hle/input.c): XCDE's party switch and arts hotkeys - not in XC3 (requests dropped)
void party_switch_request(int step) { (void)step; }
void party_art_request(int slot) { (void)slot; }
uint64_t party_macro_buttons(void) { return 0; }  // (no presses of our own: added to the pad's buttons)
bool party_camera_direct(uint64_t frame) { (void)frame; return false; }  // (the mouse goes through the stick conversion)

// [XC3] map jumps (tour): a map change request built the way the game's own callers do (f_7000d65624 for the map
// menu's fast travel): task 0x160 from heap 4 (f_700003d1ac / f_700003d250), f_7000d24a6c(task, request),
// f_7000d31ec0(manager, task), queued on manager +0x50[+0x70] (f_7000e93638, count +0x10), then task vtable +0x18.
// Request (0xA0 bytes): +0 u32 jump type (1 fast travel: +0x30 a SYS_MapJumpList jump point hash, the game resolves the
// position; 3 the save load's jump: +0x10/14/18 x y z), +4 u32 map id (SYS_MapList row: 18 ma01a ...), +0x58 1.0f,
// +0x74 flags (0x204 fast travel, 0x200 with type 3), +0x90 0.5f 0.5f. Found 2026-11-06 (tour stages 1-2).
static uint64_t call_guest5(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    Cpu saved = *c;
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4;
    c->sp = (c->sp - 0x400) & ~15ull;
    guest_call(c, fn);
    uint64_t r = c->x[0];
    *c = saved;
    return r;
}
// the manager: games/xc3/patches.py hooks f_7000d31ec0 (x0 manager, x1 task); other task managers pass there too, so
// only a map change task's (vtable [0x7001b63f98] + 0x10, set by f_7000d24a6c) counts. The save load's own jump
// registers one, so it is known once a save is loaded.
static volatile uint64_t g_xc3_jump_mgr;
void xc3_jump_mgr_seen(void *cpu) {
    Cpu *c = cpu;
    uint64_t task = c->x[1], vt = *(volatile uint64_t *)(uintptr_t)0x7001b63f98ull + 0x10;
    if (!task || *(volatile uint64_t *)(uintptr_t)task != vt) return;
    if (c->x[0] != g_xc3_jump_mgr) LOG("[tour] map change manager %llx\n", (unsigned long long)c->x[0]);
    g_xc3_jump_mgr = c->x[0];
}
static bool xc3_jump(Cpu *c, uint64_t frame, unsigned type, unsigned map, unsigned flags, unsigned hash, const float *pos) {
    uint64_t mgr = g_xc3_jump_mgr;
    if (!mgr) { LOG("[tour] frame %llu: no map change manager seen yet\n", (unsigned long long)frame); return false; }
    uint8_t *req = (uint8_t *)(uintptr_t)((c->sp - 0x200) & ~15ull);  // (below the caller, above call_guest5's frames)
    memset(req, 0, 0xa0);
    *(uint32_t *)(req + 0) = type;
    *(uint32_t *)(req + 4) = map;
    float one = 1.0f;
    memcpy(req + 0x58, &one, 4);
    *(uint64_t *)(req + 0x90) = 0x3f0000003f000000ull;
    *(uint32_t *)(req + 0x74) = flags;
    if (pos) memcpy(req + 0x10, pos, 12);
    if (type == 1) *(uint32_t *)(req + 0x30) = hash;
    uint64_t heap = call_guest5(c, 0x700003d1acull, 4, 0, 0, 0, 0);
    uint64_t task = call_guest5(c, 0x700003d250ull, 0x160, heap, 0x10, 0x70014c6ad6ull, 0);
    if (!task) { LOG("[tour] frame %llu: no task memory\n", (unsigned long long)frame); return false; }
    call_guest5(c, 0x7000d24a6cull, task, (uint64_t)(uintptr_t)req, 0, 0, 0);
    call_guest5(c, 0x7000d31ec0ull, mgr, task, 0, 0, 0);
    uint64_t slot = *(volatile uint64_t *)(uintptr_t)(mgr + 0x50) + (uint64_t)((int64_t) * (volatile int32_t *)(uintptr_t)(mgr + 0x70) * 32);
    call_guest5(c, 0x7000e93638ull, task + 8, slot, 0, 0, 0);
    *(volatile uint32_t *)(uintptr_t)(slot + 0x10) += 1;
    uint64_t fn = *(volatile uint64_t *)(uintptr_t)(*(volatile uint64_t *)(uintptr_t)task + 0x18);
    call_guest5(c, fn, task, 0, 0, 0, 0);
    return true;
}

// [XC3] XCDE_TOUR=<file> (XC3_TOUR too): steps the game follows from XCDE_TOUR_START (default 3200, after a save load
// replay), one per line:
//   jump <map id> <jump point hash hex>   fast travel to a SYS_MapJumpList point (build/tours/xc3_jumps.txt)
//   jumppos <map id> <x> <y> <z>          the save load's jump (type 3) to a position
//   wait <frames>
//   shot <name>                           the window image -> <XCDE_TOUR_SHOTS dir, default build/tour>/<name>.png
//   # comment / label                     logged
// After a jump: XCDE_TOUR_WARP_WAIT frames (default 600: fade, load, fade-in); XCDE_TOUR_SHOT_AUTO=1 then takes
// "step<NNNN>_m<map>_<hash>.png". No jump starts while the game's cutscene flag (main+0x1BA1A98) is set; after 60 s
// in one it goes anyway. A step that can't start (no manager yet) is retried every 30 frames.
// XCDE_TOUR_FROM=<n>: start at line n; XCDE_TOUR_EXIT=1: end the process when the tour is done. Log lines: "[tour]".
extern char g_tour_shot[260];  // (runtime/diag/diag.c diag_shot_path: the next presented frame is saved there, once)
static void tour_shot(const char *name) {
    static char dir[200];
    if (!dir[0]) {
        const char *d = getenv("XCDE_TOUR_SHOTS");
        snprintf(dir, sizeof dir, "%s", d ? d : "build/tour");
        mkdir(dir, 0755);
    }
    snprintf(g_tour_shot, sizeof g_tour_shot, "%s/%s.png", dir, name);
}
static void xc3_tour(Cpu *c, uint64_t frame) {
    static char **line;
    static int nline = -1, cur;
    static uint64_t until, scene_from;
    static int ww = 600, auto_shot;
    static char pending_shot[96];
    if (nline < 0) {
        nline = 0;
        const char *path = getenv("XCDE_TOUR");
        FILE *fp = path ? fopen(path, "r") : NULL;
        char buf[256];
        while (fp && fgets(buf, sizeof buf, fp)) {
            buf[strcspn(buf, "\r\n")] = 0;
            if (!buf[0]) continue;
            line = realloc(line, sizeof *line * (nline + 1));
            line[nline++] = strdup(buf);
        }
        if (fp) fclose(fp);
        if (path && !nline) LOG("[tour] %s: no steps\n", path);
        if (nline) LOG("[tour] %d steps from %s\n", nline, path);
        if (getenv("XCDE_TOUR_FROM")) cur = atoi(getenv("XCDE_TOUR_FROM"));
        until = getenv("XCDE_TOUR_START") ? strtoull(getenv("XCDE_TOUR_START"), NULL, 10) : 3200;
        if (getenv("XCDE_TOUR_WARP_WAIT")) ww = atoi(getenv("XCDE_TOUR_WARP_WAIT"));
        auto_shot = getenv("XCDE_TOUR_SHOT_AUTO") != NULL;
    }
    if (!nline || frame < until) return;
    if (pending_shot[0]) { tour_shot(pending_shot); pending_shot[0] = 0; until = frame + 2; return; }
    if (cur >= nline) {
        static int told;
        if (!told++) {
            LOG("[tour] done at frame %llu\n", (unsigned long long)frame);
            until = frame + 30;  // (the last capture: a few frames for the writer)
        } else if (getenv("XCDE_TOUR_EXIT")) {
            extern void vk_shot_writer_flush(void);
            vk_shot_writer_flush();
            fflush(stderr);
            plat_terminate();
        }
        return;
    }
    const char *l = line[cur];
    if (l[0] == '#') { LOG("[tour] frame %llu: %s\n", (unsigned long long)frame, l); cur++; return; }
    bool in_cut = *(volatile uint8_t *)(uintptr_t)0x7001BA1A98ull != 0;
    if (!strncmp(l, "jump ", 5) || !strncmp(l, "jumppos ", 8)) {
        if (in_cut) {
            if (!scene_from) scene_from = frame;
            if (frame - scene_from < 3600) { until = frame + 30; return; }
            LOG("[tour] frame %llu: still in a cutscene after 60 s, jumping anyway\n", (unsigned long long)frame);
        }
        unsigned map = 0, hash = 0;
        float pos[3] = {0};
        bool ok, by_pos = l[4] == 'p';
        if (by_pos) {
            sscanf(l + 8, "%u %f %f %f", &map, &pos[0], &pos[1], &pos[2]);
            ok = xc3_jump(c, frame, 3, map, 0x200, 0, pos);
        } else {
            sscanf(l + 5, "%u %x", &map, &hash);
            ok = xc3_jump(c, frame, 1, map, 0x204, hash, NULL);
        }
        if (!ok) { until = frame + 30; return; }
        scene_from = 0;
        LOG("[tour] frame %llu: step %d %s\n", (unsigned long long)frame, cur, l);
        if (auto_shot) snprintf(pending_shot, sizeof pending_shot, "step%04d_m%u_%08x", cur, map, by_pos ? 0u : hash);
        until = frame + (uint64_t)(ww > 0 ? ww : 1), cur++;
    } else if (!strncmp(l, "wait ", 5)) {
        LOG("[tour] frame %llu: step %d %s\n", (unsigned long long)frame, cur, l);
        until = frame + strtoull(l + 5, NULL, 10), cur++;
    } else if (!strncmp(l, "shot ", 5)) {
        char name[96] = {0};
        sscanf(l + 5, "%95s", name);
        tour_shot(name);
        LOG("[tour] frame %llu: step %d %s\n", (unsigned long long)frame, cur, l);
        until = frame + 2, cur++;
    } else {
        LOG("[tour] frame %llu: step %d unknown: %s\n", (unsigned long long)frame, cur, l);
        cur++;
    }
}

void party_game_tick(Cpu *c, uint64_t frame) {
    xc3_tour(c, frame);  // (XCDE_TOUR: map jump tours)
}
