#!/usr/bin/env bash
# Build the recompiled game code objects (build/obj/) with the project venv's zig cc.
#   1. python tools/gen_all.py            -> generated/{main,sdk}
#   2. this script                        -> build/obj/*.o
set -e
cd "$(dirname "$0")/.."
. scripts/env.sh
Z="$XC3_CC"; export Z
CF="-O1 $XC3_ARCH -ffp-contract=off -fno-strict-aliasing -fvisibility=hidden -Iruntime -w"; export CF
# [XC3] 64-bit off_t / stat for the runtime objects too (build_rt_gfx.sh)
CF="$CF -D_FILE_OFFSET_BITS=64"
mkdir -p build/obj/main build/obj/sdk build/rt
# drop objects whose chunk no longer exists (the chunk layout shifts when the generator output changes)
for o in build/obj/*/*.o; do m=$(basename "$(dirname "$o")"); b=$(basename "$o" .o); [ -f "generated/$m/$b.c" ] || rm -f "$o"; done
cc1() { f=$1; m=$(basename "$(dirname "$f")"); b=$(basename "$f" .c); o="build/obj/$m/$b.o"; [ "$o" -nt "$f" ] || $Z -c "$f" -o "$o" $CF || echo "FAIL $f"; }
export -f cc1
ls generated/main/*.c generated/sdk/*.c | xargs -P "${JOBS:-16}" -I{} bash -c 'cc1 {}'

