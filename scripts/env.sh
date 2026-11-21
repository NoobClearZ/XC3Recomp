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
