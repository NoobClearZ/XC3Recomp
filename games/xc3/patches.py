"""XC3 instruction patches (contract: docs/contracts/patch-format.md).

Every XCDE patch targets XCDE addresses; XC3's sites are found again one by one.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "lifter"))
from patchfmt import Patch  # noqa: E402,F401


def _hook_x21_then_original(fn):
    """Call a runtime hook with x21, then the original instruction (which must lift to plain C)."""
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op':
            raise ValueError(f'{addr:#x}: hook patch needs a plain instruction, got {ins.kind}')
        return f'{{ extern void {fn}(uint64_t); {fn}(c->x[21]); }} {ins.code}'
    return emit


def _hook_cpu_then_original(fn):  # (fn(Cpu *) runs, then the original instruction)
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op':
            raise ValueError(f'{addr:#x}: hook patch needs a plain instruction, got {ins.kind}')
        return f'{{ extern void {fn}(void *); {fn}(c); }} {ins.code}'
    return emit


# ---- [XC3] frame rate above 60 (runtime/hle/framepace.c). Sites and what they do from xc3-recomp
# (github.com/jmars125/xc3-recomp, MIT) recomp/patches-upd.txt, sets `realtime` and `menus`; its 2.2.1 main is our
# main byte for byte (README "Game files"). There each site loads a patch page; here the replacement is written in C
# and reads the runtime's variables. Every site keeps the original instruction behind the switch, so with
# xc3_rt_on = 0 (the 30 option, deterministic runs, XC3_REALTIME=0) the game runs its own code.
# (the variables are declared in runtime/cpu.h)


def _sget(n):
    return f'({{ float _f; memcpy(&_f, &c->v[{n}], 4); _f; }})'


def _sset(n, expr):
    return f'{{ float _f = (float)({expr}); uint32_t _u; memcpy(&_u, &_f, 4); c->v[{n}] = _u; }}'


def _rt_site(switch, replacement, original):
    """if (<switch>) { replacement } else { original }: both plain C (no branches)."""
    def emit(gen, addr):
        return f'{{ if ({switch}) {{ {replacement} }} else {{ {original} }} }}'
    return emit


# The frame-step update (f_700000f3c0, x19 its object): fps = 60 / vsync count (+0x210), step = 1 / fps (+0x214), the
# frame's measured time clamped to [step, 2 step] stepped (+0x218). With the switch the fps and the step are the host's
# (framepace.c: the average of the last display intervals) and the measured time is the step itself.
_SDIV_W8 = ('{ int32_t _a = (int32_t)c->x[8], _b = (int32_t)c->x[0]; '
            'c->x[8] = (uint32_t)(_b == 0 ? 0 : (_a == INT32_MIN && _b == -1) ? _a : _a / _b); }')
_REALTIME = {
    # sdiv w8, w8, w0 -> w8 = the host's fps; the object is remembered (its +0x1fc bit 2: fixed step, cutscenes)
    0x700000f3dc: Patch(0x1ac00d08, _rt_site('xc3_rt_on', 'xc3_rt_obj = c->x[19]; c->x[8] = (uint32_t)xc3_rt_fps;',
                                             _SDIV_W8), 'fps-realtime', 'sdiv w8, w8, w0 - frame rate'),
    # fdiv s1, s1, s0 -> s1 = the host's step
    0x700000f3e8: Patch(0x1e201821, _rt_site('xc3_rt_on', _sset(1, 'xc3_rt_step'), _sset(1, f'{_sget(1)} / {_sget(0)}')),
                        'fps-realtime', 'fdiv s1, s1, s0 - frame step'),
    # bl f_70012800c4 (the measured frame time, in s0) -> s0 = the host's step
    0x700000f400: Patch(0x9449c331, _rt_site('xc3_rt_on', _sset(0, 'xc3_rt_step'),
                                             'void f_70012800c4(Cpu *); c->x[30] = 0x700000f404ull; f_70012800c4(c);'),
                        'fps-realtime', 'bl - measured frame time'),
    # The main loop's limiter (f_700124fa34) returns at once when [x0+0xa0] bit 0 or [x0+0x194] is set:
    # orr w8, w8, w9 -> orr w8, w8, #1, so the host pacer alone holds frames to the limit (framepace.c).
    0x700124fa70: Patch(0x2a090108, _rt_site('xc3_rt_on', 'c->x[8] = (uint32_t)(c->x[8] | 1);',
                                             'c->x[8] = (uint32_t)(c->x[8] | c->x[9]);'),
                        'fps-realtime', 'orr w8, w8, w9 - frame limiter'),
}

# Interface animation: each layout advances per frame by its speed (+0x290) times the layout manager's rate ([x8+0x80]),
# so above 30 fps menus and the HUD ran fast. Each site loads the rate, multiplies the speed (s0) by it and loads the
# frame's step in 30 fps frames (xc3_rt_ratio) in its place, so the game's own multiply that follows applies both.
_MENU_SITES = [0x1015c84, 0x1015ff0, 0x10162dc, 0x1016444, 0x1021f48, 0x102202c, 0x1023ef8, 0x1024218,
               0x1029fbc, 0x102a098, 0x102d73c, 0x102d8fc]


def _menu_site(reg):
    load = f'LD32(c->x[8] + 0x80)'
    orig = f'{{ uint32_t _u = {load}; c->v[{reg}] = _u; }}'
    repl = (f'{{ float _r; uint32_t _u = {load}; memcpy(&_r, &_u, 4); '
            + _sset(0, f'{_sget(0)} * _r') + ' ' + _sset(reg, 'xc3_rt_ratio') + ' }')
    return _rt_site('xc3_rt_menus', repl, orig)


_MENUS = {0x7000000000 + a: Patch(0xbd408101, _menu_site(1), 'fps-realtime', 'ldr s1, [x8, #0x80] - layout rate')
          for a in _MENU_SITES}
_MENUS[0x7001031e88] = Patch(0xbd408102, _menu_site(2), 'fps-realtime', 'ldr s2, [x8, #0x80] - layout rate')

# ---- [XC3] ultrawide (runtime/hle/ultrawide.c). Sites and what they do from xc3-recomp
# (github.com/jmars125/xc3-recomp, MIT) recomp/patches-upd.txt, sets `canvas` and `uifit`.
# - `canvas`: the layout manager's setup (f_7001037790) caps its canvas to the resolution table's second size (shipped
#   1280x720). When ultrawide writes the widened size into both slots of that table, keep pointing the layout manager at
#   xc3_ui_canvas ({1280, 720}) so world-anchored layouts (speech bubbles, icons) stay on screen.
# - `uifit`: when the runtime narrows the 16:9 UI back to its natural width on an ultrawide frame (cmdbuf.c / ultrawide.c),
#   spread world-projected UI X coordinates by 1 / k so talk bubbles, quest markers, and enemy labels stay on their 3D targets.
_UIFIT_S9_REPL = ('{ float _x = ' + _sget(0) + ' * xc3_uifit_inv; '
                  + _sset(0, '_x') + ' ' + _sset(9, f'{_sget(8)} - _x') + ' }')
_UIFIT_S9_ORIG = _sset(9, f'{_sget(8)} - {_sget(0)}')
_ULTRAWIDE = {
    0x70010377b4: Patch(0xf9449d08, _rt_site('xc3_uw_on', 'c->x[8] = (uint64_t)(uintptr_t)&xc3_ui_canvas[0];',
                                             'c->x[8] = LD64(c->x[8] + 0x938ull);'),
                        'ultrawide', 'ldr x8, [x8, #0x938] - UI canvas width'),
    0x70010377e4: Patch(0xf944a108, _rt_site('xc3_uw_on', 'c->x[8] = (uint64_t)(uintptr_t)&xc3_ui_canvas[1];',
                                             'c->x[8] = LD64(c->x[8] + 0x940ull);'),
                        'ultrawide', 'ldr x8, [x8, #0x940] - UI canvas height'),
    0x7001270144: Patch(0x1e213801, _rt_site('xc3_uifit_on', _sset(1, f'{_sget(0)} - ({_sget(1)} * xc3_uifit_inv)'),
                                             _sset(1, f'{_sget(0)} - {_sget(1)}')),
                        'ultrawide', 'fsub s1, s0, s1 - world-to-UI X projection'),
    0x7000587f48: Patch(0x1e203909, _rt_site('xc3_uifit_on', _UIFIT_S9_REPL, _UIFIT_S9_ORIG),
                        'ultrawide', 'fsub s9, s8, s0 - quest marker X projection 1'),
    0x7000799904: Patch(0x1e203909, _rt_site('xc3_uifit_on', _UIFIT_S9_REPL, _UIFIT_S9_ORIG),
                        'ultrawide', 'fsub s9, s8, s0 - quest marker X projection 2'),
    0x700083f5f4: Patch(0x1e203909, _rt_site('xc3_uifit_on', _UIFIT_S9_REPL, _UIFIT_S9_ORIG),
                        'ultrawide', 'fsub s9, s8, s0 - quest marker X projection 3'),
}


# ---- [XC3] LOD and draw-distance scaling (runtime/host/settings_ini.c).
# f_7000f17c00 computes each scene object's projected screen-space size (which scales as 1/distance) into obj[+0x110],
# obj[+0x10c], obj[+0x114] and projected bounding-sphere screen area (scales as 1/distance^2) into obj[+0x11c].
# Scaling those stored metrics pushes back every screen-size LOD transition (/monolib/shader/lod.bin in f_7000f9c678,
# per-object screen-size LODs, sub-mesh alpha fade in f_7000f9f110, detail/shadow thresholds in f_7000f15870, and
# sphere-area culling in f_7000f1fb14) without mutating s0 (which the >= 0.5f branch reuses for obj[+0x10c]).
# f_7000f9c378 performs 3D Euclidean distance culling against [x8 + 0x10]; scaling the loaded max distance pushes
# back distance-culled props, rocks, and foliage chunks proportionally.
def _lod_store(off, scale_var):
    repl = f'{{ float _v = {_sget(0)} * {scale_var}; uint32_t _u; memcpy(&_u, &_v, 4); ST32(c->x[19] + {off}ull, _u); }}'
    orig = f'ST32(c->x[19] + {off}ull, VS32(0));'
    return _rt_site('xc3_lod_on', repl, orig)


def _lod_load(base_reg, off_expr, dst_reg, scale_var='xc3_lod_scale'):
    ea = f'c->x[{base_reg}] {off_expr}'
    repl = (f'{{ uint32_t _u = LD32({ea}); float _f; memcpy(&_f, &_u, 4); '
            f'_f *= {scale_var}; memcpy(&_u, &_f, 4); c->v[{dst_reg}] = _u; }}')
    orig = f'{{ uint64_t ea = {ea}; c->v[{dst_reg}] = LD32(ea); }}'
    return _rt_site('xc3_lod_on', repl, orig)


# Grass incremental grid update (main_321.c 0x7000f06d14): scale layer radius s8 by xc3_grass_scale, and when the user
# changes lod_scale live in the F1 menu (xc3_lod_grass_dirty > 0), invalidate this layer's cached prev_cam.x so the
# grass grid regenerates immediately even if the player stands still (works in both directions, including back to 1.0x).
_LOD_GRASS_INCR = (lambda _g, _a:
    '{ uint32_t _u = LD32(c->x[8] + 0x8ull); '
    'if (xc3_lod_on) { float _f; memcpy(&_f, &_u, 4); _f *= xc3_grass_scale; memcpy(&_u, &_f, 4); } '
    'c->v[8] = _u; '
    'if (xc3_lod_grass_dirty > 0) { xc3_lod_grass_dirty--; '
    'uint64_t _p = LD64(c->x[21] + 0xc0ull); if (_p) ST32(_p + (c->x[26] << 4), 0x461c4000u); } }'
)

# Instanced foliage/rock/prop BVH (main_321.c), procedural grass grid spawner (main_321.c), static map object BVH &
# 12-byte-stride LOD tables (main_338.c), and distance-density falloff (main_326.c):
# - main_338.c f_7000fb4768 / f_7000fb48a0: coarse static-map BVH node culling against pre-squared node.max_dist_sq
#   ([x1 + 0x10]) and leaf LOD0 max_dist ([x15 + 0x4]).
# - main_338.c f_7000fb4a50 / f_7000fb5b40 / f_7000fb6dbc: per-object static map / rock / tree / building / prop LOD
#   selector loop ([reg - 4]); scaling s9 in f_7000fb4a50 also pushes back the last-LOD alpha fade-out and sub-groups.
# - main_321.c f_7000f06aa0 (0x7000f06c0c / 0x7000f06d14) + f_7000f0a620 (0x7000f0a6f0): procedural grass grid
#   spawn/unload radius s8, which propagates into grass_mgr[+0xac] -> cell[+0x10] so fade-out stays synced to the grid.
# - main_321.c f_7000f0bf84 / f_7000f0c158 / f_7000f0c268 / f_7000f0c660: instanced clutter/foliage BVH & leaf cull.
# - main_326.c f_7000f3bcd4: distance-based detail/density falloff ([x15 + 0], squared at 0x7000f3bd38).
_LOD_BVH_ADD = _rt_site('xc3_lod_on', _sset(0, f'{_sget(0)} + ({_sget(8)} * xc3_lod_scale)'),
                        _sset(0, f'{_sget(0)} + {_sget(8)}'))
_LOD_FMOV_S8_GRASS = _rt_site(
    'xc3_lod_on',
    '{ uint32_t _u = (uint32_t)c->x[12]; float _f; memcpy(&_f, &_u, 4); '
    '_f *= xc3_grass_scale; memcpy(&_u, &_f, 4); c->v[8] = _u; }',
    'c->v[8] = (uint32_t)c->x[12];',
)
_LOD = {
    # Dynamic object screen-space size & sphere area (f_7000f17c00, main_323.c)
    0x7000f17c90: Patch(0xbd011260, _lod_store('0x110', 'xc3_lod_scale'),
                        'lod', 'str s0, [x19, #0x110] - primary screen-space LOD size'),
    0x7000f17ccc: Patch(0xbd010e60, _lod_store('0x10c', 'xc3_lod_scale'),
                        'lod', 'str s0, [x19, #0x10c] - secondary screen-space LOD size'),
    0x7000f17d10: Patch(0xbd011660, _lod_store('0x114', 'xc3_lod_scale'),
                        'lod', 'str s0, [x19, #0x114] - focus screen-space LOD size'),
    0x7000f17e54: Patch(0xbd011e60, _lod_store('0x11c', 'xc3_lod_scale_sq'),
                        'lod', 'str s0, [x19, #0x11c] - projected sphere screen area'),
    # Per-object 3D Euclidean distance culling (f_7000f9c378, main_334.c)
    0x7000f9c488: Patch(0xbd401101, _lod_load(8, '+ 0x10ull', 1),
                        'lod', 'ldr s1, [x8, #0x10] - 3D distance cull threshold'),
    0x7000f9c4d0: Patch(0xbd401101, _lod_load(8, '+ 0x10ull', 1),
                        'lod', 'ldr s1, [x8, #0x10] - 3D distance cull threshold (sqrt fallback)'),
    # Static map objects / rocks / trees / buildings / props BVH & per-object LOD tables (main_338.c)
    0x7000fb479c: Patch(0xbd401021, _lod_load(1, '+ 0x10ull', 1, 'xc3_lod_scale_sq'),
                        'lod', 'ldr s1, [x1, #0x10] - static map BVH node max_dist^2 (draw)'),
    0x7000fb48d0: Patch(0xbd401021, _lod_load(1, '+ 0x10ull', 1, 'xc3_lod_scale_sq'),
                        'lod', 'ldr s1, [x1, #0x10] - static map BVH node max_dist^2 (stream)'),
    0x7000fb49c0: Patch(0xbd4005e1, _lod_load(15, '+ 0x4ull', 1),
                        'lod', 'ldr s1, [x15, #0x4] - static map BVH leaf lod0 max_dist'),
    0x7000fb4b24: Patch(0xbd400500, _lod_load(8, '+ 0x4ull', 0),
                        'lod', 'ldr s0, [x8, #0x4] - static map last-LOD max_dist clamp'),
    0x7000fb4bdc: Patch(0xbc5fc309, _lod_load(24, '- 4', 9),
                        'lod', 'ldur s9, [x24, #-4] - static map per-object LOD max_dist & fade'),
    0x7000fb5d90: Patch(0xbc5fc380, _lod_load(28, '- 4', 0),
                        'lod', 'ldur s0, [x28, #-4] - static map secondary pass LOD max_dist'),
    0x7000fb6ebc: Patch(0xbc5fc122, _lod_load(9, '- 4', 2),
                        'lod', 'ldur s2, [x9, #-4] - static map tertiary pass LOD max_dist'),
    # Procedural grass grid spawn/unload radius (main_321.c)
    0x7000f06c0c: Patch(0xbd400908, _lod_load(8, '+ 0x8ull', 8, 'xc3_grass_scale'),
                        'lod', 'ldr s8, [x8, #0x8] - grass grid spawn/unload radius (camera cut)'),
    0x7000f06d14: Patch(0xbd400908, _LOD_GRASS_INCR,
                        'lod', 'ldr s8, [x8, #0x8] - grass grid spawn/unload radius (per-frame + live refresh)'),
    0x7000f0a6f0: Patch(0x1e270188, _LOD_FMOV_S8_GRASS,
                        'lod', 'fmov s8, w12 - grass grid 5m-cell default 50m radius'),
    # Instanced foliage/rock/prop BVH traversal & leaf culling (main_321.c)
    0x7000f0bfb4: Patch(0x1e282800, _LOD_BVH_ADD,
                        'lod', 'fadd s0, s0, s8 - BVH node radius + max_dist (1)'),
    0x7000f0c188: Patch(0x1e282800, _LOD_BVH_ADD,
                        'lod', 'fadd s0, s0, s8 - BVH node radius + max_dist (2)'),
    0x7000f0c2ac: Patch(0xbd40074a, _lod_load(26, '+ 0x4ull', 10),
                        'lod', 'ldr s10, [x26, #0x4] - BVH leaf instance max_dist (1)'),
    0x7000f0c6a0: Patch(0xbd40074a, _lod_load(26, '+ 0x4ull', 10),
                        'lod', 'ldr s10, [x26, #0x4] - BVH leaf instance max_dist (2)'),
    # Distance-based detail/density falloff (f_7000f3bcd4, main_326.c)
    0x7000f3bd38: Patch(0x1e220840, _rt_site('xc3_lod_on', _sset(0, f'({_sget(2)} * {_sget(2)}) * xc3_lod_scale_sq'),
                                             _sset(0, f'{_sget(2)} * {_sget(2)}')),
                        'lod', 'fmul s0, s2, s2 - detail density cull max_dist^2'),
    0x7000f3bd74: Patch(0xbd4001e2, _lod_load(15, '+ 0x0ull', 2),
                        'lod', 'ldr s2, [x15, #0x0] - detail density falloff max_dist'),
}


PATCHES = {
    # XC3 tour (runtime/hle/party.c xc3_tour): f_7000d31ec0(manager, task) registers a task; the map change task's
    # manager (it lives in an object, +0x60 - no global) is remembered from here. Before `mov x29, sp`
    0x7000d31ec8: Patch(0x910003fd, _hook_cpu_then_original('xc3_jump_mgr_seen'), 'tour', 'XC3 tour: map change manager'),
    # 60 fps: the community cheat's "UI Speed Fix" (github.com/ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats, build
    # 0AE74B263D8AC3CF; the same code in 2.2.0). In the UI layout update, x21 = the layout: its frame step
    # [x21 + 0x214] stays 1/30 at 60 fps (runtime/hle/framepace.c xc3_ui_step). The cheat branched to a code cave over
    # an import stub instead; here the hook runs before the original `mov x0, x21`.
    0x7000067370: Patch(0xaa1503e0, _hook_x21_then_original('xc3_ui_step'), '60fps',
                        'mov x0, x21 - UI layout update (x21 = layout)'),
}
PATCHES.update(_REALTIME)
PATCHES.update(_MENUS)
PATCHES.update(_ULTRAWIDE)
PATCHES.update(_LOD)
