#!/usr/bin/env bash
# Native (C) runtime tests: fibers (T032b) and condvar timeout semantics (T031). No game data needed.
set -e
cd "$(dirname "$0")/.."
. scripts/env.sh
Z="$XC3_CC"
EXT="$XC3_EXE_EXT"
PLAT="$XC3_PLAT"
if [ "$XC3_OS" = linux ]; then
  PLAT_CF="$(pkg-config --cflags sdl3 2>/dev/null || true)"
  PLAT_LD="$(pkg-config --libs sdl3 2>/dev/null || echo "-lSDL3") -lpthread -ldl -lm -rdynamic"
  FIB_LD="-lpthread"
else
  PLAT_CF=""
  PLAT_LD="-lwinmm -ldbghelp"
  FIB_LD=""
fi
mkdir -p build/test build/rt
$Z -c runtime/platform/${PLAT}.c -o build/rt/platform_${PLAT}.o -O1 -w -Iruntime $PLAT_CF
$Z -O1 -w -Iruntime -o build/test/test_fibers${EXT} tests/native/test_fibers.c runtime/hle/fibers.c $FIB_LD
$Z -O1 -w -Iruntime -o build/test/test_condvar${EXT} tests/native/test_condvar.c build/rt/platform_${PLAT}.o runtime/diag/diag.c runtime/diag/sampler.c $PLAT_LD
build/test/test_fibers${EXT}
build/test/test_condvar${EXT}
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_tex${EXT} tests/native/test_translate_tex.c runtime/nvn/maxwell/translate.c
build/test/test_translate_tex${EXT}
$Z -O1 -w -Iruntime -o build/test/test_movie_buf${EXT} tests/native/test_movie_buf.c
build/test/test_movie_buf${EXT}
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_ldc${EXT} tests/native/test_translate_ldc.c runtime/nvn/maxwell/translate.c
build/test/test_translate_ldc${EXT}
$Z -O1 -w -Iruntime/nvn/maxwell -o build/test/test_translate_xfb${EXT} tests/native/test_translate_xfb.c runtime/nvn/maxwell/translate.c
build/test/test_translate_xfb${EXT}
$Z -O1 -w -Iruntime/nvn -o build/test/test_tegra_block_height${EXT} tests/native/test_tegra_block_height.c runtime/nvn/tegra.c runtime/nvn/formats.c
build/test/test_tegra_block_height${EXT}
$Z -O1 -w -Iruntime/nvn -o build/test/test_tegra_copy${EXT} tests/native/test_tegra_copy.c runtime/nvn/tegra.c runtime/nvn/formats.c
build/test/test_tegra_copy${EXT}
$Z -O1 -w -o build/test/test_time_math${EXT} tests/native/test_time_math.c
build/test/test_time_math${EXT}
