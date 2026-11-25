#!/usr/bin/env bash
# Everything from a checked dump in targets/ to a playable build: recompile the game code (generated/), compile it
# and the runtime (build/xc3.exe), build the launcher. "Build XC3.bat" runs it; it also works on its own:
#   scripts/build_all.sh            (XC3_EXE=build/other.exe: link the game under another name, e.g. for tests)
# Needs: MSYS2 UCRT64 at C:\msys64 with the libraries, and .venv with capstone unicorn lz4 ziglang (see README).
set -e
cd "$(dirname "$0")/.."
. scripts/env.sh
step() { echo; echo "=== $* ==="; }
[ -x .venv/bin/python ] || [ -x .venv/Scripts/python ] || [ -f .venv/Scripts/python.exe ] || { echo "missing .venv (run tools/setup_linux.sh on Linux or \"Build XC3.bat\" on Windows)"; exit 1; }
step "1/5 Checking the game files (targets/)"
export XC3_GAME=${XC3_GAME:-xc3}
echo "game: games/$XC3_GAME"
$XC3_PY -X utf8 tools/extract/check_dump.py targets
step "2/5 Recompiling the game code into C (generated/, ~1 min)"
(cd tools && ../$XC3_PY -X utf8 gen_all.py --out ../generated)
step "3/5 Compiling the game code (build/obj/, several minutes the first time)"
XCDE_SKIP_BOOT_LINK=1 scripts/build_boot.sh
exe=${XC3_EXE:-${XCDE_EXE:-build/xc3${XC3_EXE_EXT}}}
step "4/5 Compiling the runtime and linking $exe"
scripts/build_rt_gfx.sh
[ -f "$exe" ] || { echo "the link failed (see build/link_gfx.err)"; exit 1; }
scripts/copy_upscaler_dlls.sh  # (FSR DLLs from third_party/fidelityfx; DLSS only with sdk/)
launcher=xenoblade3_launcher${XC3_EXE_EXT}
play="Play XC3.bat"; [ "$XC3_OS" = linux ] && play="scripts/play.sh"
step "5/5 Building the launcher ($launcher)"
tools/launcher/build.sh
echo
echo "Done: $exe and $launcher. Start the game with \"$play\" (the first time: Precompile shaders)."
