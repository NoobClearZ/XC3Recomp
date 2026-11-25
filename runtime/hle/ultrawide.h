// [XC3] Ultrawide support (Hor+ resolution table patching + 16:9 UI fit / edge shift / clipping).
// Adapted from xc3-recomp (github.com/jmars125/xc3-recomp, MIT) runtime/resolution.c and runtime/nvn_api_cmd.c.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../cpu.h"

// Patch main's shipped {1920, 1080, 1280, 720} resolution table once modules are loaded (before nn::init::Start).
// disp_w x disp_h: the primary display's size in pixels (0 if unknown), used for Auto / 21:9 matching in fullscreen.
void xc3_ultrawide_init(int disp_w, int disp_h);

// Active game base dimensions (1920x1080 when 16:9, widened base_w x 1080 when ultrawide is active).
void xc3_ultrawide_base(uint32_t *w, uint32_t *h);

// Horizontal UI scale factor (16:9 / frame aspect when ultrawide + UI fit are active, else 1.0f).
float xc3_ui_fit_scale(void);

// Per-frame and NVN command buffer hooks (runtime/nvn/objects.c, runtime/nvn/cmdbuf.c).
void xc3_ui_frame(void);
void xc3_ui_note_target(uint32_t w, uint32_t h);
void xc3_ui_note_cam(const Cpu *c, int stage, uint64_t addr, uint64_t size);
void xc3_ui_fit(const Cpu *c, int stage, uint64_t addr, uint64_t size);
void xc3_ui_rect(const Cpu *c, float r[4], int viewport);

// Shift the battle Arts palette inside mnu417_cont01.wilay (LAGP) outward to the screen corners on ultrawide.
int xc3_uw_patch_battle_hud(uint8_t *buf, size_t len);
int xc3_uw_ard_patch(const char *ard, uint64_t *off, uint8_t **data, uint64_t *len);
