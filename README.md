# XC3Recomp (Xenoblade Chronicles 3)

> [!IMPORTANT]
> **This project was made almost entirely with AI.** Nearly all of the code, tooling, reverse engineering and
> documentation was written by an AI (Anthropic's Claude, through Claude Code), directed, tested and reviewed by a
> human. It is shared as is, in the hope that someone takes it further. You run it at your own discretion.

**XC3Recomp** is a static recompilation of *Xenoblade Chronicles 3* (Nintendo Switch, 2022) into a native Windows
x86-64 application with a Vulkan renderer. It is **not an emulator**: the game's ARM64 code is translated ahead of
time into C and compiled natively for your PC, and the console's operating system and graphics API (NVN) are
reimplemented on top of Windows and Vulkan.

It evolves from **Monado** (the static recompilation of *Xenoblade Chronicles: Definitive Edition*) and
[LASRecomp](https://github.com/peiikai/LASRecomp) (MIT), adapting their generator and dedicated runtime. User-facing
tooling and configuration are specialized for XC3 (`Build XC3.bat`, `Play XC3.bat`, `build/xenoblade3_launcher.exe`,
`build/xc3.exe`, `xc3.ini`). XC3 is the only game built: the XCDE profile and its code paths were removed (2026-11-06);
the `XCDE_*` environment switches and `[XCDE]` comment markers keep their names (where the code came from).

> [!NOTE]
> **No game files are included.** You need your own legally obtained copy of the game, **version 2.2.0 or 2.2.1** (the
> base game with that update applied), extracted to an `exefs` folder and a `romfs` folder. Instructions on how to
> obtain or extract these files will **not** be provided. Everything derived from the game (the recompiled code,
> translated shaders, caches) is generated on your own machine and never belongs in this repository.

## Status

The project is currently in active development (**Phases 1 & 2** of the [Roadmap](docs/ROADMAP.md)):
- **Playable areas:** The opening sequence, title screen, early cutscenes, and Colony 9 are running with full audio, video playback, and controls on Windows (tested on NVIDIA RTX and AMD GPUs). An automated map-jump tour (`tools/xc3_tour.py`) visits the game's fast-travel points: the first five field maps have been reached without a crash so far, the rest is being toured.
- **Performance & graphics features:** In-place alias memory pool reads, multi-threaded Zstd-compressed SPIR-V shader cache reads, automatic ASTC GPU compute shader decoding, transform feedback draw support (`nvnCommandBufferDrawTransformFeedback`), GPU queue sync (`CMD_WAIT_SYNC`), shadow-pass async builds, forced 16x anisotropic filtering, and a 60 FPS mode with a frame-lag reduction option.
- **Under active work:** Visual fixes (e.g. Colony 9 waterfall draw artifact) and testing gameplay stability across later regions and extended battles. See [Known issues](#known-issues) and [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Features

**Graphics**
- Internal resolution: match the window (default), 1x–3x the Switch's resolution (up to 4K), 75 % / 50 % of the window, or dynamic resolution following performance. Ultrawide displays maintain correct aspect ratios.
- Upscaling and anti-aliasing:
  - Spatial: AMD FSR 1.0;
  - Temporal: AMD FSR 3.1 / FSR 4 or NVIDIA DLSS (with native-AA / DLAA modes);
  - Extra: SMAA (automatically bypassed when temporal upscalers like FSR/DLSS are active).
- The game's own motion blur and temporal anti-aliasing (TMAA) can each be turned off (launcher: Graphics; `motion_blur` / `game_taa`). TMAA stays on with FSR 3.1 / FSR 4 / DLSS, which take their inputs from it.
- Shader precompilation: Ahead-of-time pipeline building to eliminate in-game shader compilation stutters. The SPIR-V cache is packed into `cache/spirv.pack`.

**Frame rate**
- 30 / 60 FPS options (the 60 FPS option is adapted from the community 60 FPS patch; cutscenes remain locked at a steady 30 FPS).
- Automatic frame-lag reduction at 60 FPS: allows one frame in flight for lower latency, smoother frametimes, and holding 60 FPS in heavy areas.
- V-Sync toggle.

**DLC (add-on content)**
- The Expansion Pass add-ons and Future Redeemed are served from your own extracted DLC files (`<game folder>/dlcs`, see below): the game finds them as installed and mounts them. Future Redeemed can be started from the title screen and plays.

**Controls & UI**
- Full Keyboard and Mouse support with camera mouse-look, plus configurable sensitivity and keybindings.
- Controller support with full button remapping and customizable stick dead zones.
- Dynamic button prompts: keyboard layout prompts and Xbox / PlayStation controller icon packs (supported via `mods/`).
- Cursor auto-hide while the game window is focused.

**Robustness & Diagnostics**
- **Safe saves:** Save commits are verified with an automated backup system kept per session (`save/backup/`; only the newest 5 are kept, so copy one elsewhere to keep it longer).
- **Session recordings & replay:** Built-in session recorder (`play/<timestamp>`) for diagnostics, regression testing, and performance profiling.
- **Map-jump tour (developers):** `tools/xc3_tour.py` replays a save load, then fast-travels through a list of jump points in hidden batches (a scratch copy of the save, never the real one) and writes a per-point summary (reached, worst frame time, shader build waits) plus optional captures. The jump list is generated from your own game files and is never committed.
- **Hardware-aware scaling:** Detects CPU core counts and GPU capabilities, tuning thread pools and memory budgets accordingly.

**Launcher & Setup**
- **Xenoblade Chronicles 3 Launcher** (`build/xenoblade3_launcher.exe`): modern dark theme, dedicated tabs for graphics, controls, and an "Install & build" tab with game file picker, version validation, and single-click builds.
- Romfs mod support via `mods/<name>/romfs/`.

## Requirements

- **OS:** Windows 10 / 11, x86-64 (x86-64-v2: SSE4.2 minimum).
- **GPU:** Vulkan 1.3 compatible GPU.
  - **Tested:** NVIDIA GeForce RTX series and modern AMD Radeon GPUs.
  - **Intel / iGPUs:** Basic support logic present, but community testing and reports are welcome.
- **RAM:** 8 GB minimum, 16 GB recommended. The game heap is ~3.25 GB (`0xD0000000`).
- **Disk:** ~16 GB for the game files, ~5 GB for build outputs and caches.
- **Your game files:** see the next section.

## Game files: what and where

You need the game files extracted from your own copy of **Xenoblade Chronicles 3**. Instructions on how to dump or extract them are **not** provided.

**Format:** Plain extracted folders, not packages. Archives such as `.nsp`, `.xci`, `.nca`, `.nsz`, or `.xcz` are rejected.

**Version:** **2.2.0** or **2.2.1** (title ID `010074f013262000`): the base game with either update applied. 2.2.1
(June 2025, adjustments for Nintendo Switch 2) has the same `main`, `sdk`, `subsdk0` and `rtld` as 2.2.0 and romfs files
of the same names and sizes; `main.npdm` differs. Both pass the same check and build the same game.
- `exefs/` must have these build-ids:
  - `main` (build-id `82d187fe9ef9be92`)
  - `main.npdm`
  - `rtld` (build-id `e79ab9af2ab05174`)
  - `sdk` (build-id `e8cdceb1e77a8465`)
  - `subsdk0` (build-id `be1ee79a9968a35a`)
- `romfs/` must contain the update's files layered over the base game (250 files across root, `movie/`, `sound/`, etc., ~15.2 GB total).

**Layout:**
```
<your game folder>/
├── exefs/
│   ├── main          (build-id: 82d187fe9ef9be92)
│   ├── main.npdm
│   ├── rtld          (build-id: e79ab9af2ab05174)
│   ├── sdk           (build-id: e8cdceb1e77a8465)
│   └── subsdk0       (build-id: be1ee79a9968a35a)
├── romfs/
│   ├── movie/
│   ├── sound/
│   └── ...
└── dlcs/             (optional: the DLC's files, all add-ons in one folder)
    ├── bf3_dlc01.arh / bf3_dlc01.ard   ... bf3_dlc04.arh / bf3_dlc04.ard
    ├── movie/
    └── sound/
```

**DLC (optional):** the extracted romfs of the add-ons, merged into one folder. An add-on counts when both of its archives (`bf3_dlcNN.arh` and `.ard`) are there; 04 is Future Redeemed. The game files step links it to `targets/dlcs` when the chosen folder has one; the launcher's **Install & build** tab can also choose it on its own (**DLC (optional)**), and `xc3.ini` `dlc = 0` hides it from the game. Check a DLC folder with `python tools/extract/check_dump.py --dlcs <folder>`.

**Setup:**
- In `Build XC3.bat` or the launcher's **Install & build** tab, choose that folder to link it to `targets/exefs` and `targets/romfs` automatically.
- Alternatively, create directory links manually: `targets/exefs/` and `targets/romfs/`.
- Verify the dump using the verification tool:
  ```bash
  python tools/extract/check_dump.py targets
  ```

## Building

The build runs the recompiler on your machine to translate the game's code, then compiles it with the dedicated runtime. It takes approximately 10–20 minutes on the first build.

### The easiest way: the release zip

Download `XC3Recomp-<version>.zip` from the repository's **Releases** page and extract it. Start
`build\xenoblade3_launcher.exe` and go through its **Install & build** tab (described below): build tools, game files,
build, shaders. The zip has the source and a prebuilt launcher only - no game files; your own 2.2.0 or 2.2.1 dump is still
needed.

### The easy way: `Build XC3.bat`

The only prerequisite is **winget** (App Installer, part of Windows 10 / 11). Git for Windows is not needed: MSYS2's
bash runs the build scripts.

Run `Build XC3.bat` from the repository root:
1. **Tools:** If tools are missing, it runs `tools\setup_windows.ps1` to install MSYS2 with SDL3, FFmpeg, Vulkan, shaderc, VMA, and Opus, installs Python 3.12 (via winget), and initializes `.venv` with the necessary packages.
2. **Game files:** Select the folder containing your extracted `exefs` and `romfs` (version 2.2.0 or 2.2.1). It verifies file hashes and build-IDs, then links `targets/`.
3. **Build:** Runs `scripts/build_all.sh` to recompile the ARM64 binaries to C, compile the native game objects, build the runtime executable (`build/xc3.exe`), and build the launcher (`build/xenoblade3_launcher.exe`).

The launcher's **Install & build** tab does the same four steps with progress bars and logs: **Build tools**
(`tools\setup_windows.ps1`, log `build/setup_windows.log`), **Game files**, **Build** (`build/build_all.log`) and
**Shaders** (the precompilation). The launcher links SDL3 statically, so it runs on a PC with none of the tools yet.

When finished, launch the game with `Play XC3.bat`. Precompile the shaders once (the **Shaders** step, or **PLAY** does it the first time), then click **PLAY**. Shaders are automatically compressed into `cache/spirv.pack` using Zstandard (Zstd), keeping the cache footprint around ~2.5–3.5 GB (down from ~15 GB uncompressed). Existing uncompressed caches can be compressed with `build/xc3.exe --repack-spirv` or `python tools/repack_spirv.py`.

### By hand

1. **Install required tools** (or run `powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1`):
   - [MSYS2](https://www.msys2.org/) with UCRT64 libraries:
     ```bash
     pacman -S mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-shaderc \
               mingw-w64-ucrt-x86_64-vulkan-memory-allocator mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-opus
     ```
   - Python 3.12+ (the python.org build). The build scripts run in MSYS2's bash (`C:\msys64\usr\bin\bash.exe`, with
     `C:\msys64\ucrt64\bin` and `C:\msys64\usr\bin` on the PATH) or in Git Bash.
   - Python virtual environment with Zig (used as the C compiler) and binary analysis tools:
     ```bash
     python -m venv .venv
     .venv/Scripts/pip install capstone unicorn lz4 ziglang
     ```
2. **Place and verify game files:**
   Link your files to `targets/exefs/` and `targets/romfs/`, then run:
   ```bash
   python tools/extract/check_dump.py targets
   ```
3. **Recompile the game code to C** (`generated/`, ~240 MB):
   ```bash
   cd tools && ../.venv/Scripts/python -X utf8 gen_all.py --out ../generated && cd ..
   ```
4. **Compile game objects and runtime** (in MSYS2's bash or Git Bash):
   ```bash
   XCDE_SKIP_BOOT_LINK=1 scripts/build_boot.sh   # Recompiled game -> build/obj/
   scripts/build_rt_gfx.sh                       # Runtime + Vulkan renderer -> build/xc3.exe
   tools/launcher/build.sh                       # Launcher -> build/xenoblade3_launcher.exe
   ```
5. **Upscaler DLLs:** FSR 3.1 / FSR 4 use AMD's signed FidelityFX SDK 2.4.0 DLLs, included in `third_party/fidelityfx/` (MIT) and copied next to the exe by `scripts/copy_upscaler_dlls.sh` (`build_all.sh` runs it). DLSS needs NVIDIA Streamline 2.14.16, not redistributed here: put it in `sdk/` and run the same script.
6. **Launch:** Run `Play XC3.bat` or start `build/xenoblade3_launcher.exe`.

## Settings

Settings are managed via the launcher or directly in `xc3.ini` in the repository root:

| Key | Values | Description |
|---|---|---|
| `resolution` | `match`, `1`–`3`, `75%`, `50%`, `dynamic` | Rendering resolution scale |
| `upscaler` | `bilinear`, `fsr`, `fsr3`, `fsr4`, `dlss` | Upscaling method (`fsr4` falls back to FSR 3.1 when its DLLs are missing) |
| `upscale_quality` | `native`, `quality`, `balanced`, `performance`, `ultra` | Quality preset for temporal upscalers |
| `fps` | `30`, `60` | Target framerate |
| `frame_lag` | `0`, `1` | One frame in flight at 60 FPS to reduce latency |
| `vsync` | `0`, `1` | Vertical sync |
| `cutscenes` | `same`, `30`, `30fg` | Framerate behaviour during cutscenes |
| `antialias` | `smaa`, `off` | Post-process anti-aliasing |
| `motion_blur` | `0`, `1` | The game's motion blur and fur blur (applied at start) |
| `game_taa` | `0`, `1` | The game's temporal anti-aliasing, TMAA (applied at start; kept with FSR 3.1 / FSR 4 / DLSS) |
| `dlc` | `0`, `1` | Serve the DLC in `targets/dlcs` to the game |
| `language` | `auto`, `en-US`, `ja`, `fr`, `de`, `it`, `es`, `ko`, `zh-Hans`, `zh-Hant` | In-game text language (only the languages your copy has) |
| `stick_deadzone` | `0`–`30` | Analog stick deadzone percentage |
| `bind_key_*`, `bind_pad_*` | Various | Key and controller bindings |
| `mod_<folder>` | `0`, `1` | Enable or disable specific romfs mods |

In game, **F1** opens the settings overlay.

## Known issues

- **Early development status:** Active testing is currently focused on the opening and Colony 9 areas. Extended gameplay in later chapters, long battle encounters, and deep story progression are undergoing validation. The map-jump tour checks that areas load and render; it does not play them.
- **First visit to an area:** Shader and pipeline builds can still cause hitches of up to a few seconds the first time an area is entered on a fresh cache (measured by the tour); with the shaders cached, later visits hitch much less.
- **Graphical quirks:**
  - Red / black box artifact near the Colony 9 waterfall (one draw call undergoing investigation).
- **DLC shaders:** the shader precompilation reads only the base game's archive, so the DLC's shaders build the first time they are seen.
- **Memory scaling in long sessions:** Pipeline libraries and driver memory grow over multi-hour play sessions; restarting periodically resolves any accumulated stutter.
- **Platform support:** Currently focused on Windows x86-64 with Vulkan 1.3.

## Where the knowledge is

For developers looking to inspect or contribute to the project:

| Document | Description |
|---|---|
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Current project phases, completed features, open tasks, and active goals for XC3. |
| [`docs/RECOMP_KNOWLEDGE.md`](docs/RECOMP_KNOWLEDGE.md) | Knowledge base and bug solutions collected from ~20 public Switch recompilation projects. |
| [`docs/LESSONS_FOR_XC2.md`](docs/LESSONS_FOR_XC2.md) | Engine comparison and shared architecture notes across the Xenoblade series. |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Project constitution (clean room principles, zero copyright assets, attribution, test discipline). |
| [`docs/dev/HANDOFF.md`](docs/dev/HANDOFF.md) | Session-by-session technical log, measured performance numbers, and debugging findings. |
| [`docs/hle_audit.md`](docs/hle_audit.md) | Audit of Horizon OS SVCs, IPC services, and HLE implementation fidelity. |

## Repository layout

```
runtime/        Dedicated runtime: loader, kernel (SVCs, threads, sync), IPC services, HLE,
                NVN -> Vulkan translation, Maxwell shader translator, audio/video, input, diagnostics
tools/          Recompiler lifter (AArch64 -> C), dump extraction validator, launcher, test tooling
games/xc3/      Active game profile: Xenoblade Chronicles 3 (v2.2.0 / v2.2.1 build-IDs, modules, patches, HLE symbols)
scripts/        Automated build, test, and run scripts (Git Bash)
tests/          Unit tests, native C test suites, input replay tests
data/           variants_seed_xc3.bin: pipeline variants recorded in play (shader hashes + render state, no game content)
docs/           Architecture notes, roadmap, research documents, and handoffs
third_party/    Dear ImGui, SMAA, AMD FidelityFX headers, NVIDIA Streamline headers, bcdec
```

## Credits and license

- [LASRecomp](https://github.com/peiikai/LASRecomp) by Peiikai (MIT): The base recompiler lifter and runtime architecture this project builds upon.
- Monado (XCDE): The Definitive Edition recompilation milestone that laid the foundation for Monolith Soft engine support.
- Architectural reference and community findings from SSBU-Recomp, B2recomp, and other projects listed in [`docs/RECOMP_KNOWLEDGE.md`](docs/RECOMP_KNOWLEDGE.md).
- Third-party components and license notices: see [`NOTICE.md`](NOTICE.md).

XC3Recomp is released under the **MIT License** (see [`LICENSE`](LICENSE)). It is an independent open-source project, not affiliated with or endorsed by Nintendo or Monolith Soft. *Xenoblade Chronicles* is a trademark of Nintendo.
