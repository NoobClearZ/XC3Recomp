# Plan: fewer hitches, more frame rate (branch `perf-stutter`)

Status: **Phase 0 measured** (2026-11-11, see "Baseline"); phases 1 and 2 not started. `main` (with the real-time
frame step and 2.2.1) is merged in.

Sources: xc3-recomp (github.com/jmars125/xc3-recomp @ 2b88f29, MIT) `docs/architecture.md` and commits b26bb3a,
2707855; their numbers are their hardware (Ryzen 7 9800X3D, RX 9070) and are not ours until measured here.

## What we already have (no work)

- Variant seed: `data/variants_seed_xc3.bin`, 13,689 pipeline variants from play and the map-jump tour
  (`tools/merge_variants.py`), prewarmed when the game registers a program (exec.c T163/T211). xc3-recomp ships the
  same idea (3,830 pipelines, 7,445 variants) and builds it at boot.
- Asynchronous render worker (exec.c, at most 2 presents queued): xc3-recomp's "replay thread".
- Graphics pipeline libraries (exec.c T188), the SPIR-V pack and the Vulkan pipeline cache, write-watch uploads.

## Phase 0: baseline (before any change)

Every later step is compared against this, on the same PC, build and settings.

1. Route: one recorded Colony 9 session (`play/<stamp>`, input replay) plus one `tools/xc3_tour.py` chunk over
   several maps; once with the shader caches kept (steady play), once with `cache/` emptied (first visit).
2. Settings: the same resolution, upscaler and FPS limit for every run (`fps = unlimited`, V-Sync off).
3. `tools/perf_compare.py` (new): reads the `[vk] ... fps max ...ms ... build waits N (T ms)` lines of two
   `game.log` files and prints average fps, 1% low, frames over 25 ms and over 100 ms, the worst frame and the build
   waits, side by side.
4. One profile of each thread kind (`vk_prof_frame` for the render worker, `fnprof` for guest functions) to tell
   whether the render worker or the game's CPU work bounds the frame. That decides what comes after this branch.

### Baseline (2026-11-11, `main` at `40b220b` + `XC3_FRAME_TIMES`)

PC: i5-13400F, RX 9060 XT, 16 GB. Hidden runs, FSR 4 native at 1920x1080, V-Sync off, scratch copy of the base save.
Tools: `XC3_FRAME_TIMES=<file>` (device.c: every present interval), `tools/xc3_tour.py --frame-times`,
`tools/perf_compare.py <run> <run>`.

What changed against the plan above:
- The route is the tour alone (8 jump points of map 18: `--maps 18,19 --every 20 --limit 8 --wait 1200 --start 4500`),
  at the **60 option with `XC3_REALTIME=0`**. At `fps = unlimited` the boot logos run at 1,400-2,200 fps: the tour's
  start frame and the replay that loads the save (both frame numbers) pass before the title screen is up, and the
  tour then "jumps" on the title screen at 30 fps. The loading replay is an A press every 100 frames from 600 to 4000.
- "First visit" is a cold AMD driver cache with `cache/` kept (the SPIR-V pack is 2.9 GB and the driver's cache is
  per exe name): the same tour from a copy of `xc3.exe` under a new name. A rebuilt exe is partly cold too (run A).
- The frame rate is measured apart, standing in Colony 9 at the 165 option (real-time step), 65 s after loading.

Tour, 9,581 frames each (`build/tour_runs/p0_*`):

| | rebuilt exe (A) | warm (B) | warm (C) | cold driver cache |
|---|---|---|---|---|
| average fps | 58.3 | 59.8 | 60.0 | 53.8 |
| 1% low fps | 12.6 | 31.5 | 34.9 | 4.4 |
| frames over 25 ms | 35 | 27 | 26 | 64 |
| frames over 100 ms | 17 | 2 | 2 | 42 |
| worst frame, ms | 837 | 463 | 131 | 3,344 |
| pipeline waits (ms) | 46 (1,928) | 1 (445) | 0 | 148 (9,931) |
| program waits (ms) | 4 (74) | 3 (45) | 3 (48) | 9 (116) |
| loading holds, ms | 3,002 | 318 | 361 | 8,344 |

B and C are the warm baseline; their difference is the run-to-run noise (one 445 ms pipeline wait in B). Cold, the
time goes to pipeline waits and to loading holds waiting for the same builds: 18.3 s of a 178 s tour.

Standing in Colony 9 (Assembly Square, ~3,350 draws a frame), 165 option: **100.0 fps** average, 1% low 46.6, 11
frames over 25 ms, none over 100 ms, worst 55 ms. With `LARECOMP_VK_PROF=1` (88 fps; its own cost) the frames it logs
show the render worker's command execution at 10.1-10.4 ms a frame with GPU waits of 0.1 ms: as long as the frame time
there, so this scene is bound by the **render worker's CPU time** (~3 us a draw), not by the GPU. Read from the
logged slow frames only (over 50 ms, 36 of 6,010); a per-frame average of every frame was not taken, and the guest
threads were not profiled.

What it says for the phases:
- Steady play has few hitches left (2 frames over 100 ms in 160 s); the first visit is where they are, and they are
  the driver compiling pipelines. Precompiling does not warm that cache today (A/B of 2026-11-11: 300 waits / 22.1 s
  without, 324 / 23.9 s with), so find out why before 1.2-1.4; 1.1 does not depend on it.
- The frame rate above 60 is bound by the render worker: 2.2 (`-O2` for the runtime, built with `-O1` today) aims
  at exactly that and is the cheapest thing to measure.

## Phase 1: hitches

1.1 **Skip or wait for a pipeline that is not ready** (exec.c T174). Today a draw always waits for its pipeline
    (dropping it made objects flash black, ~18,000 draws in 6 minutes), so every first-time build is a hitch.
    xc3-recomp skips the draw only when every target of its pass was rendered in the previous frame (the old image
    stays, so nothing flashes), waits otherwise, and builds the stage libraries a waiting draw needs on idle
    workers in parallel. Same rule here: track the frame each render target was last written; `XC3_PIPE_SKIP=0`
    waits as today. Risk: an object one frame late on its first appearance.
1.2 **Precompile the add-ons' shaders** (ROADMAP "Open": precompile.c reads `bf3.ard` only). Read `bf3_dlcNN.ard`
    from `targets/dlcs` when present, the same way; Future Redeemed has its own shaders.
1.3 **Extend the seed** to every map and Future Redeemed: run `tools/xc3_tour.py` over the whole jump list (and FR
    from its save), merge with `tools/merge_variants.py`. Run on Windows by the maintainer; the seed holds hashes
    and render state only.
1.4 Only if 1.1-1.3 leave first-visit hitches: build the seed's pipelines at boot as xc3-recomp does (theirs takes
    about 5.4 s) instead of at program registration.

## Phase 2: frame rate (compiler flags)

2.1 **`-march` by the building CPU.** The game is built on the player's PC, so `scripts/build_boot.sh` and
    `scripts/build_rt_gfx.sh` can pick `-march=x86-64-v3` (AVX2, BMI2, FMA) when that CPU has it and keep
    `-march=x86_64_v2` otherwise; `XC3_MARCH` overrides. The README's minimum (SSE4.2) stays. `-ffp-contract=off`
    stays on the generated code, so FMA does not change float results. The build writes the level it used next to
    `xc3.exe`, and `xc3.exe` checks CPUID at boot with a clear message (a build copied to an older PC would
    otherwise crash on the first AVX2 instruction).
2.2 **`-O2` for the runtime** (not for the generated code: its compile time). Measured alone, kept only if it helps.

## Out of this branch (decided after Phase 0's profile)

- Render worker bound: xc3-recomp's recorder thread (`vk_rec.c`, MIT) and draw-state reuse with descriptor update
  templates.
- Game CPU bound: registers and NZCV in C locals in the lifter (`recomp/regs.py`, `flags.py`), after a whole-function
  differential test (their `tests/diff/func.py`). Frame generation for the areas no CPU change fixes.

## Done when

- `scripts/ci.sh`: no new failures.
- Colony 9 route, steady: 1% low and frames over 25 ms no worse than the baseline; each change kept only with its
  own measurement.
- First visit (empty `cache/`): frames over 100 ms and the worst frame lower than the baseline, Future Redeemed
  included.
- No black flashes on first appearances (the T174 report) with 1.1 on.
