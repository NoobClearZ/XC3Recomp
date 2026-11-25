#!/usr/bin/env bash
# One-time setup of a Linux machine for building XC3Recomp (equivalent to tools/setup_windows.ps1).
# Checks/installs system dependencies (SDL3, FFmpeg, Vulkan, shaderc, VMA, Opus, zstd, zlib, clang, pkg-config),
# initializes .venv with lz4, capstone, unicorn, ziglang, and builds the Linux FSR 3.1 / FSR 4 bridge (ffx-vk-linux).
# Safe to re-run ("=== k/5" lines track progress).
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
step() { echo; echo "=== $* ==="; }

step "1/5 Checking build tools"
missing_tools=()
for cmd in python3 pkg-config git cmake; do
  command -v "$cmd" >/dev/null 2>&1 || missing_tools+=("$cmd")
done
command -v clang >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || missing_tools+=(clang)

step "2/5 Checking libraries (SDL3, FFmpeg, Vulkan, shaderc, VMA, Opus, zstd, FSR deps)"
missing_libs=()
for pc in sdl3 vulkan shaderc opus libavcodec libavformat libavutil libswscale libswresample zlib libzstd openssl libdrm libdrm_amdgpu SPIRV-Tools; do
  pkg-config --exists "$pc" 2>/dev/null || missing_libs+=("$pc")
done
have_vma=0
for p in /usr/include/vk_mem_alloc.h /usr/include/vma/vk_mem_alloc.h \
         /usr/local/include/vk_mem_alloc.h /usr/local/include/vma/vk_mem_alloc.h \
         "$HOME/.local/include/vk_mem_alloc.h" "$HOME/.local/include/vma/vk_mem_alloc.h"; do
  [ -f "$p" ] && have_vma=1 && break
done
[ -n "${XC3_VMA_INC:-}" ] && [ -f "$XC3_VMA_INC/vk_mem_alloc.h" -o -f "$XC3_VMA_INC/vma/vk_mem_alloc.h" ] && have_vma=1

# On Arch/CachyOS, vulkan-memory-allocator is AUR-only (not in official pacman repos); fetch vk_mem_alloc.h below if missing.
if [ "$have_vma" != 1 ] && ! command -v pacman >/dev/null 2>&1; then
  missing_libs+=(vulkan-memory-allocator)
fi

if [ ${#missing_tools[@]} -gt 0 ] || [ ${#missing_libs[@]} -gt 0 ]; then
  echo "Missing tools/libraries: ${missing_tools[*]} ${missing_libs[*]}"
  install_cmd=""
  if command -v pacman >/dev/null 2>&1; then
    install_cmd="pacman -S --needed sdl3 ffmpeg vulkan-headers vulkan-icd-loader shaderc opus zstd zlib clang pkgconf python git cmake openssl libdrm spirv-tools spirv-headers"
  elif command -v apt-get >/dev/null 2>&1; then
    install_cmd="apt-get update && apt-get install -y libsdl3-dev libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev libopus-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev zlib1g-dev libzstd-dev clang pkg-config python3-venv git cmake libssl-dev libdrm-dev spirv-tools spirv-headers"
  elif command -v dnf >/dev/null 2>&1; then
    install_cmd="dnf install -y SDL3-devel vulkan-loader-devel vulkan-headers libshaderc-devel vulkan-memory-allocator-devel opus-devel ffmpeg-free-devel zlib-ng-compat-devel libzstd-devel clang pkgconf-pkg-config python3 git cmake openssl-devel libdrm-devel spirv-tools-devel spirv-headers-devel"
  fi
  if [ -n "$install_cmd" ]; then
    if [ "$(id -u)" = 0 ]; then
      sh -c "$install_cmd"
    elif [ -t 0 ] && command -v sudo >/dev/null 2>&1; then
      sudo sh -c "$install_cmd"
    elif command -v pkexec >/dev/null 2>&1 && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
      pkexec sh -c "$install_cmd"
    else
      echo "Please install the missing packages manually, then re-run:"
      echo "  sudo $install_cmd"
      exit 1
    fi
  else
    echo "Unsupported package manager. Install SDL3, Vulkan, shaderc, VMA, Opus, FFmpeg, zlib, zstd, clang, pkg-config, python3-venv."
    exit 1
  fi
fi

if [ "$have_vma" != 1 ]; then
  for p in /usr/include/vk_mem_alloc.h /usr/include/vma/vk_mem_alloc.h \
           /usr/local/include/vk_mem_alloc.h /usr/local/include/vma/vk_mem_alloc.h \
           "$HOME/.local/include/vk_mem_alloc.h" "$HOME/.local/include/vma/vk_mem_alloc.h"; do
    [ -f "$p" ] && have_vma=1 && break
  done
fi
if [ "$have_vma" != 1 ]; then
  vma_dst="$HOME/.local/include/vk_mem_alloc.h"
  vma_url="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.2.1/include/vk_mem_alloc.h"
  echo "Downloading vk_mem_alloc.h -> $vma_dst"
  mkdir -p "$HOME/.local/include"
  python3 -c "import urllib.request, sys; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])" "$vma_url" "${vma_dst}.tmp"
  grep -q "VMA_IMPLEMENTATION" "${vma_dst}.tmp" || { rm -f "${vma_dst}.tmp"; echo "Downloaded vk_mem_alloc.h looks invalid." >&2; exit 1; }
  mv -f "${vma_dst}.tmp" "$vma_dst"
  have_vma=1
fi

if [ ${#missing_tools[@]} -eq 0 ] && [ ${#missing_libs[@]} -eq 0 ] && [ "$have_vma" = 1 ]; then
  echo "System tools and libraries: OK"
fi

step "3/5 Python virtual environment (.venv)"
if [ ! -x "$root/.venv/bin/python" ]; then
  python3 -m venv "$root/.venv"
fi
"$root/.venv/bin/python" --version

step "4/5 Recompiler packages (.venv: capstone, unicorn, lz4, ziglang)"
"$root/.venv/bin/python" -m pip install -q --disable-pip-version-check lz4 capstone unicorn ziglang

step "5/5 AMD FSR 3.1 / FSR 4 Linux bridge (ffx-vk-linux)"
ffx_dir="$root/sdk/ffx-vk-linux"
ffx_repo="https://github.com/DuIslingr/ffx-vk-linux.git"
mkdir -p "$root/sdk"
if [ -d "$ffx_dir/.git" ]; then
  echo "Updating ffx-vk-linux in $ffx_dir..."
  git -C "$ffx_dir" pull --ff-only 2>&1 || echo "Warning: could not update ffx-vk-linux (using existing checkout)"
else
  echo "Downloading ffx-vk-linux from $ffx_repo..."
  rm -rf "$ffx_dir"
  git clone --depth 1 "$ffx_repo" "$ffx_dir"
fi
ffx_inc_dir=""
if [ ! -f /usr/include/spirv/unified1/spirv.hpp11 ] && [ ! -f /usr/local/include/spirv/unified1/spirv.hpp11 ]; then
  shim_dir="$ffx_dir/build/spirv-inc"
  mkdir -p "$shim_dir/spirv/unified1"
  if [ -f /usr/include/glslang/SPIRV/spirv.hpp11 ]; then
    ln -sf /usr/include/glslang/SPIRV/spirv.hpp11 "$shim_dir/spirv/unified1/spirv.hpp11"
  else
    spirv_url="https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/main/include/spirv/unified1/spirv.hpp11"
    echo "Downloading spirv.hpp11 -> $shim_dir/spirv/unified1/spirv.hpp11"
    python3 -c "import urllib.request, sys; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])" "$spirv_url" "$shim_dir/spirv/unified1/spirv.hpp11"
  fi
  ffx_inc_dir="$shim_dir"
fi
rm -f "$ffx_dir/build/CMakeCache.txt"
CPLUS_INCLUDE_PATH="${ffx_inc_dir}${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}" \
  cmake -S "$ffx_dir" -B "$ffx_dir/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
CPLUS_INCLUDE_PATH="${ffx_inc_dir}${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}" \
  cmake --build "$ffx_dir/build" --target ffx_vk_linux -j"$(nproc 2>/dev/null || echo 4)"
mkdir -p "$root/build"
scripts/copy_upscaler_dlls.sh
if [ -f "$root/build/libffx_vk_linux.so" ]; then
  echo "FSR 3.1 / FSR 4 Linux bridge ready: build/libffx_vk_linux.so"
fi

echo
echo "Done. Next: choose your game files (tools/setup_game_files.sh) and run scripts/build_all.sh."
