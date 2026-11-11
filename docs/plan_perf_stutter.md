# Plan: fewer hitches, more frame rate (branch `perf-stutter`)

Status: **plan, not started** (2026-11-11). Branched from `fps-realtime`, so measurements above 60 fps use the
real-time frame step; rebase onto `main` once that branch is merged.

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
