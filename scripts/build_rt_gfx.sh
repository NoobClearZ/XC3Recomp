#!/usr/bin/env bash
# XC3_EXE=build/other.exe builds/links a separate exe without killing running games.
# Compile every runtime object build/xc3.exe links (build/rt/*.o, build/nvn/*.o), then link it (scripts/link_gfx.sh).
#   scripts/build_rt_gfx.sh            # all objects
#   scripts/build_rt_gfx.sh kernel hle/movie nvn/vulkan/exec   # only these (paths under runtime/, no .c)
# The game objects (build/obj) come from scripts/build_boot.sh. The window build of main_boot gets -DXCDE_GFX.
cd "$(dirname "$0")/.."
. scripts/env.sh
Z="$XC3_CC"
ZXX="$XC3_CXX"
# [XC3] On Windows, MSYS2 headers are searched after zig's own libc (-idirafter). On Linux, pkg-config supplies system cflags.
SYS_CXX="$XC3_SYS_CFLAGS"
CF="-O1 $XC3_ARCH -fno-strict-aliasing -fvisibility=hidden -Iruntime -Iruntime/nvn -Ithird_party $SYS_CXX -w"
# [XC3] 64-bit off_t / stat (as runtime/platform/win_compat.h asks): with mingw's 32-bit ones stat() fails on files over
# 2 GB and the virtual romfs left out XC3's 9.7 GB bf3.ard.
CF="$CF -D_FILE_OFFSET_BITS=64"
# [XC3] the language served to the game when the player picked none: the profile's default_language (XC3: en-GB)
lang=$($XC3_PY -c "import json,sys; print(json.load(open(sys.argv[1])).get('default_language', ''))" \
       "games/${XC3_GAME:-xc3}/profile.json" 2>/dev/null | tr -d '\r')
if [[ "$lang" =~ ^[A-Za-z-]+$ ]]; then CF="$CF -DGAME_DEFAULT_LANGUAGE=\"$lang\""; fi
RT="loader dispatch kernel ipc fnprof platform/${XC3_PLAT} services/fs services/romfs_virtual services/am services/acc services/time
    services/hid_set services/kbicons services/misc services/audio services/hwopus hle/heaps hle/pl hle/vi hle/nvn_boot hle/fibers hle/input hle/party
    hle/bindings hle/movie hle/framepace hle/ultrawide host/stubs host/settings_ini host/stubs_gfx host/gamelib_static host/main_boot diag/diag diag/sampler diag/watchdog
    diag/crashdump"
NVN="astc cmdbuf formats maxwell/translate maxwell/spirv_emit objects output_scale smaa_data tegra texpatch vulkan/device vulkan/exec vulkan/precompile vulkan/shaders vulkan/textures vulkan/temporal"
mkdir -p build/rt build/nvn
want="$*"
pick() { [ -z "$want" ] || [[ " $want " == *" $1 "* ]]; }
fail=0
for f in $RT; do
  pick "$f" || continue
  o=build/rt/$(echo $f | tr / _).o; extra=""
  [ "$f" = host/main_boot ] && { o=build/rt/host_main_boot_gfx.o; extra="-DXCDE_GFX"; }
  $Z -c runtime/$f.c -o $o.$$.tmp $CF -DGAMELIB_ID='"dev"' $extra && mv -f $o.$$.tmp $o || { echo "FAIL $f"; fail=1; }  # (tmp + rename: a parallel link never reads half an object)
done
for f in $NVN; do
  pick "nvn/$f" || continue
  extra=""
  if [ "$f" = smaa_data ]; then  # [XCDE] the SMAA reference shader (third_party/smaa, MIT) as a C string
    mkdir -p build/inc
    $XC3_PY tools/gen_smaa_inc.py third_party/smaa/SMAA.hlsl build/inc/smaa_hlsl.inc || fail=1
    extra="-Ibuild/inc -Ithird_party/smaa/Textures"
  fi
  o=build/nvn/nvn_$(echo $f | tr / _).o
  $Z -c runtime/nvn/$f.c -o $o.$$.tmp $CF -DGAMELIB_ID='"dev"' $extra && mv -f $o.$$.tmp $o || { echo "FAIL nvn/$f"; fail=1; }
done
# [XCDE] In-game settings overlay (F1): Dear ImGui + its Vulkan backend (third_party/imgui, MIT) and runtime/ui/menu_vk.cpp
# (replaces the menu stub object)
if pick ui/menu; then
  I=third_party/imgui
  rm -f build/nvn/host_menu_stub.o
  for f in $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_tables.cpp $I/imgui_widgets.cpp $I/backends/imgui_impl_vulkan.cpp \
           runtime/nvn/vulkan/imgui_glue.cpp runtime/ui/menu_vk.cpp; do
    $ZXX -c $f -o build/nvn/ui_$(basename ${f%.cpp}).o -std=c++17 -O1 $XC3_ARCH \
      -fvisibility=hidden -Iruntime -Iruntime/nvn -Iruntime/nvn/vulkan -I$I -I$I/backends $SYS_CXX -w \
      || { echo "FAIL $f"; fail=1; }
  done
fi
# [XCDE] NVIDIA DLSS through Streamline (C++ headers, third_party/streamline, MIT)
if pick nvn/vulkan/dlss; then
  $ZXX -c runtime/nvn/vulkan/dlss.cpp -o build/nvn/nvn_vulkan_dlss.o -std=c++17 -O1 $XC3_ARCH -fvisibility=hidden -Iruntime -Iruntime/nvn -Ithird_party/streamline/include $SYS_CXX -w || { echo "FAIL nvn/vulkan/dlss"; fail=1; }
fi
# [XCDE] Vulkan Memory Allocator (header-only) and the path helpers: built here with the
# same baseline CPU as everything else.
if pick nvn/vulkan/vma; then
  $ZXX -c runtime/nvn/vulkan/vma.cpp -o build/nvn/vma.o -std=c++17 -O1 $XC3_ARCH -fvisibility=hidden $SYS_CXX -w || { echo "FAIL nvn/vulkan/vma"; fail=1; }
fi
if pick paths; then
  $Z -c runtime/paths.c -o build/nvn/paths.o $CF -DGAMELIB_ID='"dev"' || { echo "FAIL paths"; fail=1; }
fi
[ $fail = 0 ] || exit 1
[ "$XC3_OS" = windows ] && [ -z "$XCDE_EXE" ] && taskkill //F //IM xcde.exe >/dev/null 2>&1 || true
scripts/link_gfx.sh && scripts/copy_upscaler_dlls.sh
