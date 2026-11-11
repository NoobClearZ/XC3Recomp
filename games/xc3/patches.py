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
