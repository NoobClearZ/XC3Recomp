// [XC3] Native Hor+ ultrawide support: widens XC3's internal resolution table (main+0x14dfd8c) to base_w x 1080
// and narrows 16:9 UI elements back to their natural aspect ratio while shifting edge HUD layouts to the screen sides.
// Adapted from xc3-recomp (github.com/jmars125/xc3-recomp, MIT) runtime/resolution.c and runtime/nvn_api_cmd.c.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "../nvn/output_scale.h"
#include "../platform.h"
#include "../rt.h"
#include "../settings.h"
#include "ultrawide.h"

#define BASE_W 1920u
#define BASE_H 1080u
#define RES_TABLE_OFF 0x14dfd8cull  // XC3 2.2.0 / 2.2.1 main rodata: {1920, 1080, 1280, 720}

// Globals read by the patched sites in games/xc3/patches.py (declared in runtime/cpu.h).
volatile int xc3_uw_on, xc3_uifit_on;
volatile float xc3_uifit_inv = 1.0f;
uint32_t xc3_ui_canvas[2] = {1280, 720};

static uint32_t g_uw_w = BASE_W, g_uw_h = BASE_H;
static float g_ui_fit_k = 1.0f;

static double parse_aspect_env(const char *s, int disp_w, int disp_h) {
    if (!s || !*s) return -1.0;
    if (!strcmp(s, "16:9") || !strcmp(s, "off") || !strcmp(s, "0")) return 16.0 / 9.0;
    if (!strcmp(s, "auto")) {
        int tw = (g_settings.fullscreen && disp_w > 0) ? disp_w : g_window_w;
        int th = (g_settings.fullscreen && disp_h > 0) ? disp_h : g_window_h;
        return (tw > 0 && th > 0) ? (double)tw / (double)th : (16.0 / 9.0);
    }
    if (!strcmp(s, "21:9")) return 2560.0 / 1080.0;
    if (!strcmp(s, "32:9")) return 3840.0 / 1080.0;
    unsigned a = 0, b = 0;
    if ((sscanf(s, "%ux%u", &a, &b) == 2 || sscanf(s, "%u:%u", &a, &b) == 2) && a > 0 && b > 0)
        return (double)a / (double)b;
    double f = atof(s);
    if (f >= 1.7 && f <= 4.0) return f;
    return -1.0;
}

static double pick_target_aspect(int disp_w, int disp_h) {
    const char *env = getenv("XC3_ASPECT");
    if (env && *env) {
        double a = parse_aspect_env(env, disp_w, disp_h);
        if (a > 0) return a;
    }
    // Keep deterministic / hidden test runs at 16:9 unless XC3_ASPECT explicitly asks for ultrawide.
    if (getenv("LARECOMP_HIDDEN") || getenv("XC3_DETERMINISTIC") || getenv("XCDE_DETERMINISTIC") ||
        getenv("XC3_INPUT_REPLAY") || getenv("XCDE_INPUT_REPLAY"))
        return 16.0 / 9.0;
    int tw = (g_settings.fullscreen && disp_w > 0) ? disp_w : g_window_w;
    int th = (g_settings.fullscreen && disp_h > 0) ? disp_h : g_window_h;
    double target_a = (tw > 0 && th > 0) ? (double)tw / (double)th : (16.0 / 9.0);
    switch (g_settings.aspect) {
    case ASPECT_AUTO:
        return target_a;
    case ASPECT_21_9:
        // If the active display/window is already a 21:9 variant (2560x1080, 3440x1440, 3840x1600), match it exactly
        // so there are no 2-pixel pillarboxes.
        if (target_a >= 2.30 && target_a <= 2.45) return target_a;
        return 2560.0 / 1080.0;
    case ASPECT_32_9:
        if (target_a >= 3.45 && target_a <= 3.65) return target_a;
        return 3840.0 / 1080.0;
    default:
        return 16.0 / 9.0;
    }
}

void xc3_ultrawide_init(int disp_w, int disp_h) {
    xc3_uw_on = 0;
    xc3_uifit_on = 0;
    xc3_uifit_inv = 1.0f;
    g_uw_w = BASE_W;
    g_uw_h = BASE_H;
    g_ui_fit_k = 1.0f;

    double aspect = pick_target_aspect(disp_w, disp_h);
    if (aspect <= (16.0 / 9.0) + 0.01) return;
    if (aspect > 3.65) aspect = 3.65;

    uint32_t bw = ((uint32_t)llround((double)BASE_H * aspect) + 1u) & ~1u;
    if (bw <= BASE_W) return;
    if (bw > 4096u) bw = 4096u;

    uint64_t table_at = MAIN_BASE + RES_TABLE_OFF;
    uint32_t cur[4];
    memcpy(cur, (const void *)(uintptr_t)table_at, sizeof cur);
    if (cur[0] != BASE_W || cur[1] != BASE_H || cur[2] != 1280u || cur[3] != 720u) {
        LOG("[ultrawide] unexpected resolution table at main+%#llx (%u,%u,%u,%u); keeping 16:9\n",
            (unsigned long long)RES_TABLE_OFF, cur[0], cur[1], cur[2], cur[3]);
        return;
    }

    uint64_t page = table_at & ~0xFFFull;
    if (plat_protect(page, 0x1000, PLAT_PROT_RW) != 0) {
        LOG("[ultrawide] failed to make main+%#llx writable; keeping 16:9\n", (unsigned long long)RES_TABLE_OFF);
        return;
    }
    uint32_t want[4] = {bw, BASE_H, bw, BASE_H};
    memcpy((void *)(uintptr_t)table_at, want, sizeof want);
    plat_protect(page, 0x1000, PLAT_PROT_READ);

    g_uw_w = bw;
    g_uw_h = BASE_H;
    xc3_uw_on = 1;
    output_scale_set_base((int)bw, (int)BASE_H);

    bool fit_on = !g_settings.ui_fit_off;
    const char *fenv = getenv("XC3_UI_FIT");
    if (fenv && *fenv) fit_on = atoi(fenv) != 0;
    float k = (16.0f / 9.0f) * (float)g_uw_h / (float)g_uw_w;
    if (fit_on && k < 0.995f) {
        g_ui_fit_k = k;
        xc3_uifit_inv = 1.0f / k;
        xc3_uifit_on = 1;
    }
    LOG("[ultrawide] active: game base %ux%u (aspect %.3f), ui_fit=%.3f (%s)\n",
        g_uw_w, g_uw_h, (double)g_uw_w / (double)g_uw_h, g_ui_fit_k, xc3_uifit_on ? "on" : "stretched");
}

void xc3_ultrawide_base(uint32_t *w, uint32_t *h) {
    if (w) *w = g_uw_w;
    if (h) *h = g_uw_h;
}

float xc3_ui_fit_scale(void) { return g_ui_fit_k; }

// ================================================================== UI fit, edge shift & clipping
// Ported from xc3-recomp (github.com/jmars125/xc3-recomp, MIT) runtime/nvn_api_cmd.c.
// The game lays its interface out on a 16:9 canvas regardless of frame aspect. On a wider frame:
// - Each flat UI element's first vertex UBO holds a 2x4 clip-space transform; xc3_ui_fit narrows row 0 by k = 16:9 / aspect
//   about the frame centre and shifts single-side HUD layouts (minimap, area name, prompts) toward that edge.
// - Perspective UI layouts (the tilted main menu on X) already use the widened frame aspect in UBO 1 and stay untouched.
// - Full-width fades / dim layers keep full screen coverage.
// - Clipping panes (minimap) narrow their viewport & scissor in xc3_ui_rect; elements drawn inside stay unnarrowed.
#define UI_BIND_RET (MAIN_BASE + 0xea5e90ull)   // x30 in the NVN bind call
#define UI_BIND_LR  (MAIN_BASE + 0x1038b8cull)  // the bind wrapper's return address, saved in its frame
#define UI_CLIP_LR  (MAIN_BASE + 0x10386f0ull)  // the clip rectangle setter's return address, saved in its frame
#define UI_ROOT_LR  (MAIN_BASE + 0x10348a0ull)  // layout draw loop return address
#define UI_FULL_WIDTH 1.9f
#define UI_EDGE_SIDE  0.33f
#define UI_EDGE_REACH 0.85f
#define UI_EDGE_STAY  0.25f
#define UI_EDGE_CROSS 0.15f
#define UI_EDGE_BAND  0.2f
#define UI_NODE_STALE   30
#define UI_NODE_SETTLED 60
#define UI_SIDE_HOLD    60
#define UI_NODE_CENTRES 64
#define UI_NODE_MOVE    0.004f
#define UI_MOVE_HOLD    120
#define UI_LAYOUT_POS   0xe0ull  // root transform x, y from the layout list entry (layout = entry - 0x20)

static _Thread_local int ui_on_output, ui_in_rect;
static _Thread_local uint64_t ui_cam;
static volatile uint32_t ui_frames;

void xc3_ui_frame(void) { __atomic_add_fetch(&ui_frames, 1, __ATOMIC_RELAXED); }

void xc3_ui_note_target(uint32_t w, uint32_t h) {
    if (g_ui_fit_k >= 1.0f) return;
    ui_on_output = (w == g_uw_w && h == g_uw_h);
    ui_in_rect = 0;
    ui_cam = 0;
}

static uint64_t ui_frame_lr(uint64_t fp) {
    static _Thread_local uint64_t ok_page;
    if (!fp || (fp & 7)) return 0;
    if ((fp >> 12) != ok_page || ((fp + 15) >> 12) != ok_page) {
        if (!plat_mem_readable(fp, 16)) return 0;
        if ((fp >> 12) == ((fp + 15) >> 12)) ok_page = fp >> 12;
    }
    return *(const uint64_t *)(uintptr_t)(fp + 8);
}

static uint64_t ui_node(const Cpu *c) {
    uint64_t fp = c->x[29];
    for (int d = 0; d < 24 && fp; d++) {
        uint64_t lr = ui_frame_lr(fp);
        if (!lr) return 0;
        if (lr == UI_ROOT_LR) {
            if (!plat_mem_readable(fp + 0x58, 8)) return 0;
            return *(const uint64_t *)(uintptr_t)(fp + 0x58);
        }
        uint64_t next = *(const uint64_t *)(uintptr_t)fp;
        if (next <= fp) return 0;
        fp = next;
    }
    return 0;
}

static int ui_spans(float lw, float rw) { return lw >= UI_EDGE_BAND && rw >= UI_EDGE_BAND; }

static float ui_median(const float *v, int n) {
    float s[UI_NODE_CENTRES];
    for (int i = 0; i < n; i++) {
        int j = i;
        for (; j > 0 && s[j - 1] > v[i]; j--) s[j] = s[j - 1];
        s[j] = v[i];
    }
    return (n & 1) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) * 0.5f;
}

static int ui_side_for(int side, float mid, float lo, float hi, int moving) {
    int want = side && mid * side > UI_EDGE_STAY ? side : mid < -UI_EDGE_SIDE ? -1 : mid > UI_EDGE_SIDE ? 1 : 0;
    if ((want < 0 && lo > -UI_EDGE_REACH) || (want > 0 && hi < UI_EDGE_REACH)) return 0;
    // A layout anchored to one screen edge must not span past the centre into the opposite half
    // (e.g. the title screen / PRESS ANY BUTTON layout spans lo=-0.61..hi=+0.98 and must stay centred).
    if ((want < 0 && hi > UI_EDGE_CROSS) || (want > 0 && lo < -UI_EDGE_CROSS)) return 0;
    if (want && want != side && moving) return 0;
    return want;
}

// wide: an element as wide as the canvas (xc3_ui_fit), passed as its origin (lo == hi).
static float ui_shift(uint64_t node, float lo, float hi, int wide, float k) {
    static _Thread_local struct {
        uint64_t node;
        uint32_t frame, centred, moved;
        float c[UI_NODE_CENTRES], lw, rw, lo, hi;
        float plo, phi, pmid;
        int n, pn, have, side, hold, posok;
    } seen[32];
    static _Thread_local unsigned next;
    if (!node) return 0.0f;
    uint32_t frame = __atomic_load_n(&ui_frames, __ATOMIC_RELAXED);
    int i = 0;
    while (i < 32 && seen[i].node != node) i++;
    if (i == 32) {
        i = (int)(next++ % 32);
        memset(&seen[i], 0, sizeof seen[i]);
        seen[i].node = node;
        seen[i].frame = seen[i].centred = frame;
        seen[i].lo = seen[i].plo = 1.0f;
        seen[i].hi = seen[i].phi = -1.0f;
        seen[i].moved = frame - UI_MOVE_HOLD;
        seen[i].posok = plat_mem_readable(node + UI_LAYOUT_POS, 8) ? 1 : 0;
    }
    if (seen[i].posok) {
        const float *pos = (const float *)(uintptr_t)(node + UI_LAYOUT_POS);
        if (pos[0] != 0.0f || pos[1] != 0.0f) return 0.0f;
    }
    if (seen[i].frame != frame) {
        int stale = frame - seen[i].frame > UI_NODE_STALE;
        seen[i].have = !stale && seen[i].n > 0;
        if (stale) { seen[i].side = seen[i].hold = 0; seen[i].centred = frame; seen[i].pn = 0; }
        if (seen[i].have) {
            float mid = ui_median(seen[i].c, seen[i].n);
            if (seen[i].pn == seen[i].n && fabsf(mid - seen[i].pmid) > UI_NODE_MOVE) seen[i].moved = frame;
            float rlo = seen[i].pn ? fmaxf(seen[i].lo, seen[i].plo) : seen[i].lo;
            float rhi = seen[i].pn ? fminf(seen[i].hi, seen[i].phi) : seen[i].hi;
            int want = ui_spans(seen[i].lw, seen[i].rw) ? 0
                     : ui_side_for(seen[i].side, mid, rlo, rhi, frame - seen[i].moved < UI_MOVE_HOLD);
            if (want == seen[i].side) seen[i].hold = 0;
            else if (seen[i].side || frame - seen[i].centred < UI_NODE_SETTLED || ++seen[i].hold >= UI_SIDE_HOLD)
                seen[i].side = want, seen[i].hold = 0;
            if (seen[i].side) seen[i].centred = frame;
            seen[i].pmid = mid; seen[i].pn = seen[i].n; seen[i].plo = seen[i].lo; seen[i].phi = seen[i].hi;
        }
        seen[i].frame = frame; seen[i].n = 0; seen[i].lw = seen[i].rw = 0; seen[i].lo = 1.0f; seen[i].hi = -1.0f;
    }
    float centre = (lo + hi) * 0.5f, width = wide ? 2.0f : hi - lo;
    if (seen[i].n < UI_NODE_CENTRES) seen[i].c[seen[i].n++] = centre;
    if (centre < -UI_EDGE_SIDE && width > seen[i].lw) seen[i].lw = width;
    if (centre > UI_EDGE_SIDE && width > seen[i].rw) seen[i].rw = width;
    if (lo < seen[i].lo) seen[i].lo = lo;
    if (hi > seen[i].hi) seen[i].hi = hi;
    if (!seen[i].have)
        seen[i].side = ui_spans(seen[i].lw, seen[i].rw) ? 0
                     : ui_side_for(seen[i].side, ui_median(seen[i].c, seen[i].n), seen[i].lo, seen[i].hi, 0);
    if (lo < 0.0f && hi > 0.0f && fabsf(centre) < UI_EDGE_STAY) return 0.0f;
    return seen[i].side < 0 ? k - 1.0f : seen[i].side > 0 ? 1.0f - k : 0.0f;
}

void xc3_ui_note_cam(const Cpu *c, int stage, uint64_t addr, uint64_t size) {
    if (g_ui_fit_k >= 1.0f || !ui_on_output || stage != 0 || c->x[30] != UI_BIND_RET) return;
    ui_cam = size >= 0x40 ? addr : 0;
}

void xc3_ui_fit(const Cpu *c, int stage, uint64_t addr, uint64_t size) {
    static _Thread_local struct { uint64_t addr; float x; } done[64];
    float k = g_ui_fit_k;
    if (k >= 1.0f || !ui_on_output || ui_in_rect || stage != 0 || c->x[30] != UI_BIND_RET || size < 0x80 || size > 0x400 ||
        ui_frame_lr(c->x[29]) != UI_BIND_LR)
        return;
    if (ui_cam) {
        const float *p = (const float *)(uintptr_t)ui_cam;
        if (p[12] != 0.0f || p[13] != 0.0f || p[14] != 0.0f) return;
    }
    float *f = (float *)(uintptr_t)addr;
    if (f[2] != 0.0f || f[6] != 0.0f || f[8] != 0.0f || f[9] != 0.0f || f[10] != 0.0f || f[11] != 0.0f) return;
    unsigned slot = (unsigned)(addr >> 4) % 64;
    if (done[slot].addr == addr && done[slot].x == f[0]) return;
    float a = f[3], b = f[3] + f[0], lo = a < b ? a : b, hi = a < b ? b : a;
    float rx = hypotf(f[0], f[1]), ry = hypotf(f[4], f[5]);
    int is_canvas_unit = rx >= 1.0f && fabsf(ry / rx - 16.0f / 9.0f) <= 0.02f;
    int wide = 0;
    if (rx >= UI_FULL_WIDTH || (hi - lo >= 1.0f && (lo <= -0.99f || hi >= 0.99f)) || is_canvas_unit) {
        if (!is_canvas_unit) return;
        if (f[1] == 0.0f && f[4] == 0.0f && f[0] > 0.0f && f[5] < 0.0f && fabsf(f[3] + 1.0f) < 0.001f && fabsf(f[7] - 1.0f) < 0.001f)
            return;
        lo = hi = f[3];
        wide = rx >= UI_FULL_WIDTH;
    }
    float shift = ui_shift(ui_node(c), lo, hi, wide, k);
    f[0] *= k;
    f[1] *= k;
    f[3] = f[3] * k + shift;
    done[slot].addr = addr;
    done[slot].x = f[0];
}

void xc3_ui_rect(const Cpu *c, float r[4], int viewport) {
    float k = g_ui_fit_k;
    if (k >= 1.0f || !ui_on_output) return;
    if (r[0] <= 0.0f && r[2] >= (float)g_uw_w) {
        if (viewport) ui_in_rect = 0;
        return;
    }
    if (ui_frame_lr(c->x[29]) != UI_CLIP_LR) return;
    float mid = (float)g_uw_w * 0.5f;
    float lo = r[0] / mid - 1.0f, hi = (r[0] + r[2]) / mid - 1.0f;
    float x = mid + (r[0] - mid) * k + ui_shift(ui_node(c), lo, hi, 0, k) * mid, w = r[2] * k;
    r[0] = floorf(x + 0.5f);
    r[2] = floorf(x + w + 0.5f) - r[0];
    if (viewport) ui_in_rect = 1;
}

// ================================================================== Battle Arts palette corner anchoring (LAGP)
// mnu417_cont01.wilay holds both the right Class Arts diamond and the left Master Arts diamond in one layout node, so
// ui_shift() keeps it centred. Shifting its right-side children by +dx and left-side children by -dx in stage units
// (dx = (1 - k) / (2 * k)) places both palettes cleanly in the bottom-right and bottom-left corners after ui_fit()
// multiplies X by k. LAGP table offsets follow xc3-recomp runtime/palette.c (MIT).
typedef struct { uint8_t *p; size_t n; uint32_t syms, nsyms, tracks, rows, ntracks; } UwLagp;

static int uw_u32(const UwLagp *f, size_t o, uint32_t *v) {
    if (o > f->n || f->n - o < 4) return 0;
    memcpy(v, f->p + o, 4);
    return 1;
}
static const char *uw_cstr(const UwLagp *f, size_t o) {
    if (o >= f->n || !memchr(f->p + o, 0, f->n - o)) return NULL;
    return (const char *)f->p + o;
}
static int uw_lagp_open(UwLagp *f, uint8_t *p, size_t n) {
    *f = (UwLagp){p, n, 0, 0, 0, 0, 0};
    uint32_t off;
    if (n < 0x70 || memcmp(p, "LAGP", 4) || !uw_u32(f, 0x18, &f->tracks) || !uw_u32(f, 0x3c, &f->syms) ||
        !uw_u32(f, f->syms, &off) || !uw_u32(f, f->syms + 4, &f->nsyms) || !uw_u32(f, f->tracks + 4, &f->rows) ||
        !uw_u32(f, f->tracks + 8, &f->ntracks))
        return 0;
    f->rows += f->tracks;
    return (uint64_t)f->syms + off + 28ull * f->nsyms <= n && (uint64_t)f->rows + 0x40ull * f->ntracks <= n;
}
static int uw_sym_field(const UwLagp *f, uint32_t i, int k, uint32_t *v) {
    uint32_t off;
    return uw_u32(f, f->syms, &off) && uw_u32(f, (size_t)f->syms + off + 28u * i + 4u * k, v);
}
static const char *uw_sym_name(const UwLagp *f, uint32_t i) {
    uint32_t o;
    return uw_sym_field(f, i, 6, &o) ? uw_cstr(f, (size_t)f->syms + o) : NULL;
}
static int uw_sym_find(const UwLagp *f, const char *name, uint32_t *out) {
    for (uint32_t i = 0; i < f->nsyms; i++) {
        const char *s = uw_sym_name(f, i);
        if (s && !strcmp(s, name)) { *out = i; return 1; }
    }
    return 0;
}
static int uw_child_track(const UwLagp *f, uint32_t parent, const char *name, uint32_t *track) {
    uint32_t place, kids, n, off, poff;
    if (!uw_sym_field(f, parent, 1, &place) || !uw_sym_field(f, parent, 3, &kids)) return 0;
    size_t k0 = (size_t)f->syms + kids, p0 = (size_t)f->syms + place;
    if (!uw_u32(f, k0, &n) || !uw_u32(f, k0 + 4, &off) || !uw_u32(f, p0, &poff)) return 0;
    for (uint32_t k = 0; k < n && k < 4096; k++) {
        size_t e = k0 + off + 12u * k;
        uint32_t kind, b;
        uint16_t slot, sym;
        if (!uw_u32(f, e, &kind) || e + 8 > f->n) return 0;
        memcpy(&slot, f->p + e + 4, 2);
        memcpy(&sym, f->p + e + 6, 2);
        const char *s = (kind == 0 && sym < f->nsyms) ? uw_sym_name(f, sym) : NULL;
        if (!s || strcmp(s, name)) continue;
        if (!uw_u32(f, p0 + poff + 8u * slot + 4, &b)) return 0;
        *track = b >> 16;
        return *track < f->ntracks;
    }
    return 0;
}
static int uw_shift_track(UwLagp *f, uint32_t t, float dx) {
    uint32_t r[16];
    size_t row = f->rows + 0x40ull * t;
    for (int i = 0; i < 16; i++)
        if (!uw_u32(f, row + 4u * i, &r[i])) return 0;
    uint32_t d1 = r[2], d2 = r[3], d5 = r[6], kind = r[12], ch = kind >> 16, how = kind & 0xffff;
    if (!d1 || (how != 1 && how != 2)) return 0;
    size_t b1 = (size_t)f->tracks + d1;
    if (ch & 8) {
        if (how == 1 || (d2 == d1 + 8 && d5 <= d2 + 24)) {
            if (b1 + 16 > f->n) return 0;
            float v[4];
            memcpy(v, f->p + b1, 16);
            if (v[0] != 0.0f || v[1] != 0.0f) return 0;
            v[2] += dx;
            memcpy(f->p + b1 + 8, &v[2], 4);
            return 1;
        }
        if (!d2 || d5 <= d2 + 20 || (size_t)f->tracks + d5 > f->n) return 0;
        size_t b = (size_t)f->tracks + d2 + 20, end = (size_t)f->tracks + d5;
        float first[2];
        memcpy(first, f->p + b, 8);
        int n = 0;
        for (; b + 12 <= end && n < 64; b += 12) {
            float v[2];
            memcpy(v, f->p + b, 8);
            if (fabsf(v[0] - first[0]) > 0.05f || fabsf(v[1] - first[1]) > 0.05f) break;
            v[0] += dx;
            memcpy(f->p + b, &v[0], 4);
            n++;
        }
        return n > 0;
    }
    uint32_t e = d2 ? d2 : d5;
    if (e <= d1 || (e - d1) % 8 || (e - d1) / 8 > 64 || (size_t)f->tracks + e > f->n) return 0;
    int n = (int)((e - d1) / 8);
    for (int i = 0; i < n; i++) {
        size_t at = b1 + 8u * (size_t)i;
        float x;
        memcpy(&x, f->p + at, 4);
        x += dx;
        memcpy(f->p + at, &x, 4);
    }
    return n > 0;
}

int xc3_uw_patch_battle_hud(uint8_t *buf, size_t len) {
    float k = g_ui_fit_k;
    if (k >= 1.0f) return 0;
    UwLagp f;
    if (!uw_lagp_open(&f, buf, len)) return 0;
    float dx = (1.0f - k) / (2.0f * k);
    static const struct { const char *parent, *child; float sign; } items[] = {
        {"mc_Btn", "mc_BaseR", +1.0f},  {"mc_Btn", "mc_BtnR01", +1.0f}, {"mc_Btn", "mc_BtnR02", +1.0f},
        {"mc_Btn", "mc_BtnR03", +1.0f}, {"mc_Btn", "mc_BtnR04", +1.0f},
        {"mc_Btn", "mc_BaseL", -1.0f},  {"mc_Btn", "mc_BtnL01", -1.0f}, {"mc_Btn", "mc_BtnL02", -1.0f},
        {"mc_Btn", "mc_BtnL03", -1.0f}, {"mc_Btn", "mc_BtnL04", -1.0f},
        {"mc_Info", "mc_InfoR01", +1.0f}, {"mc_Info", "mc_InfoR02", +1.0f},
        {"mc_Info", "mc_InfoR03", +1.0f}, {"mc_Info", "mc_InfoR04", +1.0f},
        {"mc_Info", "mc_InfoL01", -1.0f}, {"mc_Info", "mc_InfoL02", -1.0f},
        {"mc_Info", "mc_InfoL03", -1.0f}, {"mc_Info", "mc_InfoL04", -1.0f},
        {"mc_InterLinkAnim", "mc_InterLinkR01", +1.0f}, {"mc_InterLinkAnim", "mc_InterLinkR02", +1.0f},
        {"mc_InterLinkAnim", "mc_InterLinkR03", +1.0f}, {"mc_InterLinkAnim", "mc_SubAi", +1.0f},
        {"mc_InterLinkAnim", "mc_InterLinkL01", -1.0f}, {"mc_InterLinkAnim", "mc_InterLinkL02", -1.0f},
        {"mc_InterLinkAnim", "mc_InterLinkL03", -1.0f}, {"mc_InterLinkAnim", "mc_SubReturn", -1.0f},
    };
    int moved = 0;
    for (size_t i = 0; i < sizeof items / sizeof *items; i++) {
        uint32_t pi, tr;
        if (uw_sym_find(&f, items[i].parent, &pi) && uw_child_track(&f, pi, items[i].child, &tr))
            moved += uw_shift_track(&f, tr, dx * items[i].sign);
    }
    if (moved) LOG("[ultrawide] battle HUD palette: shifted %d groups by +/-%.4f\n", moved, dx);
    return moved;
}

int xc3_uw_ard_patch(const char *ard, uint64_t *off, uint8_t **data, uint64_t *len) {
    if (g_ui_fit_k >= 1.0f) return 0;
    static const uint64_t k_off = 0x23aef9d40ull;  // mnu417_cont01.wilay in 2.2.0 / 2.2.1 bf3.ard
    FILE *f = fopen(ard, "rb");
    if (!f) return 0;
    uint8_t h[0x30];
    if (plat_fseek64(f, k_off, SEEK_SET) || fread(h, 1, 0x30, f) != 0x30) { fclose(f); return 0; }
    uint32_t type, ds, cs;
    memcpy(&type, h + 4, 4); memcpy(&ds, h + 8, 4); memcpy(&cs, h + 12, 4);
    if (memcmp(h, "xbc1", 4) || strncmp((const char *)h + 0x14, "mnu417_cont01.wilay", 28) || type != 3 ||
        cs > (8u << 20) || ds > (32u << 20)) {
        fclose(f);
        return 0;
    }
    uint8_t *c = malloc(cs), *d = malloc(ds);
    size_t got = (c && d && fread(c, 1, cs, f) == cs) ? ZSTD_decompress(d, ds, c, cs) : 0;
    free(c);
    fclose(f);
    if (!got || ZSTD_isError(got) || !xc3_uw_patch_battle_hud(d, got)) { free(d); return 0; }
    size_t cap = ZSTD_compressBound(got);
    uint8_t *blk = calloc(1, 0x30 + (cap > cs ? cap : cs));
    size_t ncs = blk ? ZSTD_compress(blk + 0x30, cap, d, got, 12) : ZSTD_CONTENTSIZE_ERROR;
    if (ZSTD_isError(ncs) || ncs > cs) ncs = blk ? ZSTD_compress(blk + 0x30, cap, d, got, 19) : ZSTD_CONTENTSIZE_ERROR;
    free(d);
    if (!blk || ZSTD_isError(ncs) || ncs > cs) {
        LOG("[ultrawide] bf3.ard mnu417_cont01.wilay: recompressed %zu > %u bytes - skipped\n",
            ZSTD_isError(ncs) ? 0 : ncs, cs);
        free(blk);
        return 0;
    }
    memcpy(blk, h, 0x30);
    uint32_t nds = (uint32_t)got, ncs32 = (uint32_t)ncs;
    memcpy(blk + 8, &nds, 4); memcpy(blk + 12, &ncs32, 4);
    *off = k_off;
    *data = blk;
    *len = 0x30 + (uint64_t)cs;
    return 1;
}
