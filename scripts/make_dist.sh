#!/usr/bin/env bash
# [XCDE] A self-contained game folder to copy to another PC (no msys64, no sources, no build objects).
#   scripts/make_dist.sh <out dir> [--no-saves]
# Contents: build/xc3.exe + the launcher (build/xenoblade3_launcher.exe) + every
# DLL they load from msys64 (copied next to them), the upscaler DLLs, targets/ (exefs + romfs), data/, mods/, the ini,
# "Play XC3.bat" (without the msys64 PATH), the
# portable caches (cache/spirv: SPIR-V, cache/variants.bin: the variants seen in play) and save/ (unless --no-saves).
# Not copied: cache/vk_pipelines.bin and cache/precompile_done.txt (driver / GPU specific: the launcher precompiles
# again on the other PC). Files already there with the same size and time are skipped (robocopy), so a second run
# only copies what changed.
set -e
cd "$(dirname "$0")/.."
out="$1"
[ -n "$out" ] || { echo "usage: scripts/make_dist.sh <out dir> [--no-saves]"; exit 1; }
saves=1; [ "$2" = "--no-saves" ] && saves=0
U=/c/msys64/ucrt64/bin
export PATH="$U:$PATH"
launcher=build/xenoblade3_launcher.exe; play="Play XC3.bat"  # [XC3] (tools/launcher/build.sh)
exe=build/xc3.exe
mkdir -p "$out/build" "$out/cache" "$out/data"
cp -p "$exe" "$launcher" "$out/build/"
for d in build/*.dll; do cp -p "$d" "$out/build/"; done  # (DLSS / FSR / Reflex)
# the msys64 DLLs both programs load (ldd resolves them through PATH)
for e in "$exe" "$launcher"; do ldd "$e"; done | awk '{print $3}' | grep -i "^$U/" | sort -u |
    while read -r d; do cp -p "$d" "$out/build/"; done
w() { cygpath -w "$1"; }
rc() { MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" robocopy "$(w "$1")" "$(w "$2")" /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 > /dev/null || [ $? -lt 8 ]; }  # (robocopy: <8 = ok)
rc targets "$out/targets"
rc data "$out/data"
[ -d mods ] && rc mods "$out/mods"
[ -d cache/spirv ] && rc cache/spirv "$out/cache/spirv"
[ -f cache/variants.bin ] && cp -p cache/variants.bin "$out/cache/"
[ "$saves" = 1 ] && [ -d save ] && rc save "$out/save"
[ -f xc3.ini ] && cp -p xc3.ini "$out/"
sed 's|^set "PATH=C:\\msys64\\ucrt64\\bin;%PATH%"\r\?$|rem (the DLLs are in build\\)|' "$play" > "$out/$play"
echo "dist: $out ($(ls "$out/build" | wc -l) files in build/)"
