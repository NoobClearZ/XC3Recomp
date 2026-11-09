# XC3Recomp roadmap

The plan for the native Windows port of *Xenoblade Chronicles 3* (version 2.2.0, build ID `82D187FE9EF9BE92`).
Each phase lists what is done and what is open; update it when an item lands (commit hash) or a new one is found.
Status as of 2026-11-10.

## Phase 1: Native Windows development

Status: **in progress**.

Done:
- Static recompilation of `main` / `sdk` into C (`generated/`), linked into `build/xc3.exe` (`scripts/build_all.sh`).
- Kernel / HLE services, the alias region backed by a physical memory pool imported into Vulkan.
- Vulkan renderer: Maxwell shaders translated to SPIR-V, precompiled shader cache, pipeline cache.
- FSR 3.1 / FSR 4 in place of the game's own temporal upscale (DLSS path present, untested: AMD GPU).
- Input (pad, keyboard, mouse), audio, movies, saves (with a backup per launch).
- `nvnCommandBufferDrawTransformFeedback` implemented (translates captured vertices into synthesized array draws).
- Vulkan executor: `CMD_WAIT_SYNC` (command 50) implemented with in-pass barrier flush.
- ASTC textures decoded by a compute shader on the GPU, with the CPU decoder as fallback (`xc3.ini astc_decoder = cpu`
  or `XC3_ASTC_CPU=1` to force it); Zstd-compressed SPIR-V cache; forced 16x anisotropic filtering (`7baedf6`).
- XC3 only: the XCDE game profile (`games/xcde/`), its `GAME_XCDE` code paths (61 blocks in the runtime and launcher) and
  the build switch `XCDE_GAME` removed; the preprocessed XC3 code is unchanged (token for token, line numbers aside).
- The physical memory pool is imported into Vulkan as two allocations of at most 2 GB (`a568719`). As one 3.2 GB
  allocation the AMD driver (maxMemoryAllocationSize 2 GB) accepted it, but shader reads past 2 GB faulted: GPU page
  fault, device lost (a driver timeout) on every jump into map 22. Shaders now pick the import by pool page.

- DLC (`e12d217`): `aoc:u` lists the add-ons whose `bf3_dlcNN.arh` / `.ard` are in `<dump>/dlcs` (all four in one
  merged folder), `fsp-srv` 202 (`OpenDataStorageByDataId`, what `nn::fs::MountAddOnContent` uses) serves that folder as
  a second virtual RomFS image, movies `aocN:/...` are read from it; `xc3.ini dlc = 0` hides it. `GetDisplayVersion`
  answers 2.2.0 (it was 1.0.0, and the title screen showed no Future Redeemed entry with it).
- The game's motion blur and TMAA as two switches (`motion_blur`, `game_taa`): `monolib/shader/lib_nx.bin` in
  `bf3.ard` is an "INIB" table (murmur3 key hashes) with XCDE's `lib_nx.ini` keys; `blur2` / `furBlur` / `tmaa` /
  `AntiAliasing` are set to 0 in a re-compressed copy of its block. TMAA is kept with FSR 3.1 / 4 / DLSS (their inputs).

Open:
- Future Redeemed: its 3D scene does not render (grey / empty; HUD, battles, movies work), also without FSR 3 / 4.
  `XC3_NO_PMEM=1` turns it into a blocky but visible scene, so it is in the pool / shader global memory path.
  Repro: the replay `play/20261109_233911` on a copy of `save/backup/save_0_20261109_233913`, frame 2400.
- The DLC's shaders are not precompiled (precompile.c reads `bf3.ard` only); `bf3_dlc04.ard` has its own.
- Black / red box by the Colony 9 waterfall: one draw (vs `61e0c6ed00`, fs `61e0c6f400`, G-buffer albedo pure red).
  Not caused by the alias-region pool reads (checked with `XC3_PMM_MIRROR=1`, deterministic replay). Cause open.
- (Rechecked 2026-11-06: the "about 17 fragment shaders failed to translate" note is resolved - those failures, VMNMX
  `0x3a..` / `0x3b..` and a TLD4S form, were in logs of 2026-10-28/29 before `fbf85d7`; precompilation now translates
  all 15,170 programs, 0 failed, and no tour or replay log since then has a translation failure.)

## Phase 2: Gameplay compatibility and optimization

Status: **partially underway**.

Done:
- FPS option 30 / 60 (60 = the community 60 FPS patch, ported; cutscenes stay at 30). 120 was tried and dropped:
  movement ran too fast.
- Alias-region streams read from the pool in place (`59be42a`): Colony 9 at the 30 option 18 -> 29 fps (capped),
  GPU 37 -> 12 ms per frame.
- Session recording without capture freezes (`3e8e0ca`).
- Shader stutter at first use: the SPIR-V cache (`cache/spirv.pack`) is read through a handle per build thread, not
  one shared handle (Windows ran those reads one at a time): program builds over 3 ms 1095 -> 84 per Colony 9 replay,
  pack reads 5.7 -> 0.2 s. Shadow-pass programs are not waited for either (a model's shadow comes a few frames late).
  Loading-frame holds (frames of 0-45 draws held for the build work) checked on a recording: the long ones are the
  loading screens (by design: the next area starts with its shaders built), menus hold 1-13 ms - left as they are.
  A 4.5 min session with menus and travel after these fixes: 8 gameplay hitches, 34-51 ms.
  Still open: program builds waiting 8-14 ms on a translation lock ("lock" in [build-wait]).
- Pipeline waits measured again (map 20 tour twice, 2026-11-06): with warm caches only 3 waits over 4 ms (24 ms in
  all), so AMD's driver cache does serve the pipeline libraries. The big ones (280-731 ms on one frame) are programs new
  to the PC (`[variant-miss]`: SPIR-V compiled at first use, then libraries compiled from scratch by the driver).
  Frame wait budget for those pipelines only: at most `XC3_FRAME_WAIT_MS` (default 25, 0 = off) per frame, then the
  draw is skipped until its pipeline is built. Warm tour: 132 draws skipped vs 127 before, budget never spent. Not
  measured on a cold driver cache (the case it is for) - live play shows `[vk] frame wait budget spent` when it acts.
  Tried and not kept: the budget on every wait (529-947 draws skipped per tour, hitches no shorter), `XCDE_PRELINK=1`
  at 16 GB (no change), `LARECOMP_ASYNC_SKIP=1` (8,560 draws skipped). `XCDE_LIB_BUDGET=12000` at 16 GB: no library
  eviction, draw waits in hitch frames 249 -> 158 ms - memory cost not measured yet.
  The remaining 70-106 ms hitches after an area change wait for no build (30-165 MB uploads in those frames): cause
  not examined.
- Frame lag at the 60 option (launcher / F1 "Frame lag (60 FPS)", `xc3.ini frame_lag`, default on): one frame in
  flight instead of CPU and GPU taking turns; Colony 9 replay 52 -> 60 fps, ~10 min of live play without issues.
- SPIR-V cache index that outlived its pack: entries pointing past the pack are stored again instead of compiled
  every session (`15a5241`); the precompile stamp names the exe that ran it (`ea6a677`).
- Pipeline variant seed `data/variants_seed_xc3.bin` (`58c8f71`): 6,806 variants recorded in play (opening, Colony 9);
  precompilation builds 6,403 pipelines on an empty cache instead of 2,544. Extended with the map-jump tour's variants
  (maps 18-30): 13,689 variants, 13,116 pipelines built on an empty cache (0 failed, 24 s of the ~4 min).
- Map-jump tour (`40a0d85`): `XCDE_TOUR` steps `jump <map> <point hash>` (the map menu's fast travel, request type 1)
  and `jumppos`, run in hidden batches by `tools/xc3_tour.py` (scratch save, per-point summary, captures). The jump
  list (`build/tours/xc3_jumps.txt`, from the game's own jump point table) is generated locally and never committed.
  First full run: maps 18-21 and the start of 22 reached without a crash (850 points); it found the map 22 GPU fault
  above. Resumed after the fix (527 points, maps 22-30, 14 runs): every point reached, no crash, no device lost.
  Frames over 1 s at 16 points (11 in map 22, worst 2.5 s; 3 in map 25; 1 each in maps 27 and 29), median 41-85 ms
  per map (map 29: 408 ms).

Open:
- Test beyond the title screen, opening and Colony 9: Everblight canyon, battles, long cutscenes, other regions.
  The tour covers loading and rendering of each jump point, not battles, cutscenes or story progress.
- Tour follow-ups: the worst first-visit hitches (up to 6.5 s in map 20 on a fresh cache, 2.5 s in map 22; to be
  measured again with the extended seed); jump points of maps 0, 1
  and 17 land in a dark scene (not field maps, probably).
- Render-thread CPU cost (~2.7 us per draw), ~800 pass splits per frame (writer draws + game barriers), ~75 hitches
  per replay (mostly loading).
- Validate save loading and story progression; stability across areas.
- Long sessions: pipeline libraries and driver memory grow over hours of play (README, Known issues).

How it is tested: the user records a session from the launcher ("Record the session" -> `play/<timestamp>`, a save
backup in `save/backup/`); it is replayed hidden on a scratch copy of the save (`LARECOMP_SAVE_DIR`). Replays are not
deterministic: A/B comparisons of images use `XCDE_DETERMINISTIC=1` (+ `XCDE_TMP_DETERMINISTIC=1` to keep FSR on);
performance is measured without it (it changes the timing).

## Phase 3: Launcher and installation

Status: **planned, early work done** (lower priority than phases 1 and 2).

Done:
- `tools/launcher`: graphics, upscaler, FPS, controls, button prompts, language, session recording, reports.
- Launcher named "Xenoblade Chronicles 3" (`build/xenoblade3_launcher.exe`, `bf9de01`), dark theme, our own icon,
  optional background `mods/launcher/background.png` (`c15a3c7`).
- Launcher options for presentation (V-Sync FIFO / Fast Sync mailbox / V-Sync off for VRR - FreeSync / G-Sync) and
  anisotropic filtering, English launcher UI (`7baedf6`).
- In game: cursor hidden while the window has the focus (`658faa1`); SMAA skipped while FSR / DLSS runs (`f0ad94e`).
- `scripts/build_all.sh`: dump check, recompilation, build; shader precompilation.

- Launcher tab "Install & build": the game files (folder picker, version check, `targets\` junctions) and the build
  (`build_all.sh` with a step bar, Stop, log), the launcher swapping its own exe (renamed, not overwritten).
- The tab has the whole first-time setup (2026-11-07): **Build tools** (`setup_windows.ps1`, now with step lines and
  exit-code checks), Game files, Build, **Shaders** (precompilation). The launcher links SDL3 statically (no MSYS2 DLL
  needed to start it), and MSYS2's bash runs the build scripts (launcher and `Build XC3.bat`): Git for Windows is no
  longer needed.
- Release zip built by CI (`.github/workflows/release.yml`, on a pushed tag `v*`): the repository at that tag plus a
  prebuilt `build/xenoblade3_launcher.exe` (package_guard checked), so a new user extracts it and starts the launcher:
  no bat at all.

Open:
- Choosing an install folder other than the repository.
- Extraction from the user's own game packages (today: already extracted `exefs` / `romfs` folders only).
- DLC folder in the launcher's Install & build tab (status, folder picker, "Use the DLC"), linked by
  `setup_game_files.ps1` (`-Dlc`) as `targets\dlcs` and checked by `check_dump.py --dlcs` (`e12d217`).
- Update 2.2.1 (June 2025, adjustments for Nintendo Switch 2): everything (patches, hooks, addresses, the
  `lib_nx.bin` and button atlas block offsets) is tied to 2.2.0; another version means finding every address again.
  New dumps are likely 2.2.1, which `check_dump.py` refuses today.
- Installation management.

## Phase 4: Public source and testing

Status: **planned**.

Before publishing:
- Audit the repository **and its git history** for anything derived from the game: `dump/`, `targets/`,
  `generated/`, caches, saves, `play/` recordings, screenshots (`.gitignore` covers them today; history must be checked
  too - a file once committed stays in it).
- Check the licences of third-party code and SDKs for redistribution (`LICENSE`, `NOTICE.md`): LASRecomp / Monado,
  FidelityFX SDK, Streamline / DLSS, SDL.
- No Nintendo / Monolith Soft assets or branding in the repository or test builds.
- README for XC3: done (`7baedf6`), updated 2026-11-06 (`build/xc3.exe`, `fsr4`, the tour; the layout lists
  `data/variants_seed_xc3.bin`). The old `data/variants_seed.bin` (27 MB, removed in `7baedf6`) stays in the git history: include it in
  the audit above.

Then:
- Build and installation documentation; test builds; issue reporting and contribution guidelines
  (`CONTRIBUTING.md` is titled for XC3, but its test steps still describe XCDE; `[XCDE]` markers and `XCDE_*` switches
  keep their names).
