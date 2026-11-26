#!/usr/bin/env bash
# Build the launcher: xenoblade3_launcher(.exe) in the repo root (SDL3 + Dear ImGui, SDL renderer).
cd "$(dirname "$0")/../.."
. scripts/env.sh
Z="$XC3_CXX"
I=third_party/imgui
OUT=xenoblade3_launcher${XC3_EXE_EXT}
mkdir -p build/launcher
if [ "$XC3_OS" = linux ]; then
  INC="$(pkg-config --cflags sdl3 2>/dev/null || true)"
  SDL="$(pkg-config --libs sdl3 2>/dev/null || echo "-lSDL3") -lpthread -ldl"
  RES=""
  LINK_EXTRA=""
else
  M=${XC3_UCRT64:-/c/msys64/ucrt64}
  INC="-idirafter $M/include"
  RES=build/launcher/launcher.res
  $XC3_PY -m ziglang rc -- tools/launcher/launcher.rc $RES || exit 1
  SDL="$M/lib/libSDL3.a $M/lib/libiconv.a -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8"
  LINK_EXTRA="-Wl,--subsystem,windows -static-libstdc++"
fi
for f in $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_tables.cpp $I/imgui_widgets.cpp $I/backends/imgui_impl_sdl3.cpp \
         $I/backends/imgui_impl_sdlrenderer3.cpp tools/launcher/launcher.cpp; do
  o=build/launcher/$(basename ${f%.cpp}).o
  [ "$f" -nt "$o" ] || [ ! -f "$o" ] || continue  # [XC3] base CPU level (env.sh XC3_ARCH_BASE): the launcher starts on any PC
  $Z -c $f -o $o -O2 ${XC3_ARCH_BASE:-$XC3_ARCH} -std=c++17 -I$I -I$I/backends $INC -w || exit 1
done
NEW=build/launcher/xenoblade3_launcher.new${XC3_EXE_EXT}
OLD=build/launcher/xenoblade3_launcher.old${XC3_EXE_EXT}
$Z -o $NEW build/launcher/*.o $RES $SDL ${XC3_ARCH_BASE:-$XC3_ARCH} $LINK_EXTRA 2> build/launcher/link.err
rc=$?; [ $rc = 0 ] || { cat build/launcher/link.err | head -20; exit $rc; }
rm -f $OLD ${OLD%.exe}.pdb 2>/dev/null
[ -f $OUT ] && mv -f $OUT $OLD 2>/dev/null
mv -f $NEW $OUT || exit 1
[ -f ${NEW%.exe}.pdb ] && mv -f ${NEW%.exe}.pdb build/launcher/xenoblade3_launcher.pdb
rm -f build/xenoblade3_launcher${XC3_EXE_EXT} 2>/dev/null || true
exit 0
