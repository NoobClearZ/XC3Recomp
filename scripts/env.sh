#!/usr/bin/env bash
# Shared OS and toolchain detection for XC3Recomp build and test scripts (sourced after cd to repo root).
case "$(uname -s)" in
  Linux*)
    XC3_OS=linux
    XC3_EXE_EXT=""
    XC3_PLAT=linux
    if [ -x .venv/bin/python ]; then XC3_PY=.venv/bin/python; else XC3_PY=python3; fi
    if "$XC3_PY" -c "import ziglang" 2>/dev/null; then
      XC3_CC="${XC3_CC:-$XC3_PY -m ziglang cc}"
      XC3_CXX="${XC3_CXX:-$XC3_PY -m ziglang c++}"
      XC3_ARCH="-march=x86_64_v2"
    else
      XC3_CC="${XC3_CC:-clang}"
      XC3_CXX="${XC3_CXX:-clang++}"
      XC3_ARCH="-march=x86-64-v2"
    fi
    if [ -z "${XC3_VMA_INC:-}" ]; then
      if [ ! -f /usr/include/vk_mem_alloc.h ] && [ ! -f /usr/include/vma/vk_mem_alloc.h ] &&
         [ ! -f /usr/local/include/vk_mem_alloc.h ] && [ ! -f /usr/local/include/vma/vk_mem_alloc.h ] &&
         { [ -f "$HOME/.local/include/vk_mem_alloc.h" ] || [ -f "$HOME/.local/include/vma/vk_mem_alloc.h" ]; }; then
        XC3_VMA_INC="$HOME/.local/include"
      fi
    fi
    XC3_SYS_CFLAGS="$(pkg-config --cflags sdl3 vulkan shaderc opus libavcodec libavformat libavutil libswscale libswresample zlib libzstd 2>/dev/null || true)${XC3_VMA_INC:+ -I$XC3_VMA_INC}"
    XC3_SYS_LIBS="$(pkg-config --libs sdl3 vulkan shaderc opus libavcodec libavformat libavutil libswscale libswresample zlib libzstd 2>/dev/null || echo "-lSDL3 -lvulkan -lshaderc_shared -lopus -lavcodec -lavformat -lavutil -lswscale -lswresample -lz -lzstd") -rdynamic -lpthread -ldl -lm"
    ;;
  *)
    XC3_OS=windows
    XC3_EXE_EXT=".exe"
    XC3_PLAT=windows
    export PATH="/c/msys64/ucrt64/bin:$PATH"
    XC3_PY=".venv/Scripts/python"
    XC3_CC="${XC3_CC:-$XC3_PY -m ziglang cc}"
    XC3_CXX="${XC3_CXX:-$XC3_PY -m ziglang c++}"
    XC3_ARCH="-march=x86_64_v2"
    XC3_M=${XC3_UCRT64:-/c/msys64/ucrt64}
    XC3_SYS_CFLAGS="-idirafter $XC3_M/include"
    XC3_SYS_LIBS="$XC3_M/lib/libSDL3.dll.a $XC3_M/lib/libshaderc_shared.dll.a $XC3_M/lib/libvulkan-1.dll.a $XC3_M/lib/libopus.dll.a $XC3_M/lib/libavcodec.dll.a $XC3_M/lib/libavformat.dll.a $XC3_M/lib/libavutil.dll.a $XC3_M/lib/libswscale.dll.a $XC3_M/lib/libswresample.dll.a $XC3_M/lib/libz.a $XC3_M/lib/libzstd.a -lwinmm -ldbghelp -Wl,--stack,268435456"
    ;;
esac
# [XC3] CPU level of the game code and the runtime: x86-64-v3 (AVX2, FMA, BMI1/2, LZCNT, MOVBE, F16C) when the building
# CPU has all of it, else x86-64-v2 (SSE4.2). The game's FMADD/FMLA family must round once (runtime/simd.h): with FMA
# that is one instruction, without it a call to the C library's software fma/fmaf per lane, which xc3-recomp measured
# at 20-40 ns each and battles at a few fps (their issue #36). The compiler itself is asked what -march=native has, so
# Windows (MSYS2) and Linux detect the same way. XC3_MARCH=v2|v3 forces a level (a build for another PC:
# scripts/make_dist.sh). XC3_ARCH_BASE stays v2 for what must start anywhere (the launcher).
XC3_ARCH_BASE="$XC3_ARCH"
if [ -z "${XC3_CPU_LEVEL:-}" ]; then
  case "${XC3_MARCH:-auto}" in
    v2|v3) XC3_CPU_LEVEL=$XC3_MARCH ;;
    auto)
      XC3_CPU_LEVEL=v3
      xc3_defs=$($XC3_CC -march=native -dM -E -x c - </dev/null 2>/dev/null || true)
      for xc3_m in __AVX2__ __FMA__ __BMI__ __BMI2__ __LZCNT__ __MOVBE__ __F16C__; do
        case "$xc3_defs" in *"#define $xc3_m 1"*) ;; *) XC3_CPU_LEVEL=v2 ;; esac
      done
      unset xc3_defs xc3_m ;;
    *) echo "XC3_MARCH=$XC3_MARCH: use v2 or v3" >&2; XC3_CPU_LEVEL=v2 ;;
  esac
  export XC3_CPU_LEVEL
fi
# (an if, not `[ ] &&`: a v2 level would leave a false status that ends the scripts that source this under set -e)
if [ "$XC3_CPU_LEVEL" = v3 ]; then XC3_ARCH="${XC3_ARCH%v2}v3"; fi
