#!/usr/bin/env bash
# Link the windowed executable (M4): headless runtime + NVN/Vulkan backend + SDL3.
# --stack 256 MB: the default thread stack *reservation* (committed on use); guest threads rely on it (kernel.c thread_start).
cd "$(dirname "$0")/.."
. scripts/env.sh
Z="$XC3_CXX"
RT="build/rt/loader.o build/rt/dispatch.o build/rt/kernel.o build/rt/ipc.o build/rt/fnprof.o build/rt/platform_${XC3_PLAT}.o \
 build/rt/services_fs.o build/rt/services_romfs_virtual.o build/rt/services_am.o build/rt/services_acc.o build/rt/services_time.o \
 build/rt/services_hid_set.o build/rt/services_kbicons.o build/rt/services_misc.o build/rt/hle_heaps.o build/rt/hle_pl.o build/rt/hle_vi.o build/rt/hle_nvn_boot.o build/rt/hle_fibers.o \
 build/rt/host_stubs.o build/rt/host_settings_ini.o build/rt/host_stubs_gfx.o build/rt/services_audio.o build/rt/services_hwopus.o build/rt/hle_input.o build/rt/hle_party.o build/rt/hle_bindings.o build/rt/hle_movie.o build/rt/hle_framepace.o build/rt/fma_soft.o build/rt/hle_ultrawide.o build/rt/host_gamelib_static.o build/rt/host_main_boot_gfx.o build/rt/diag_diag.o build/rt/diag_sampler.o build/rt/diag_watchdog.o build/rt/diag_crashdump.o"
NVN="$(ls build/nvn/*.o | tr '\n' ' ')"
OUT="${XC3_EXE:-${XCDE_EXE:-build/xc3${XC3_EXE_EXT}}}"
# [XC3] Linked with the base CPU level (scripts/env.sh XC3_ARCH_BASE): zig builds its C runtime startup and
# compiler-rt for the -march it links with, and those run before platform/cpu_level.h's check can say that a v3 build
# is on a CPU without AVX2. The objects keep the level they were compiled for. The level is written next to the exe
# (<exe>.cpu_level) for scripts/make_dist.sh.
$Z -o "$OUT" $RT $NVN build/obj/main/*.o build/obj/sdk/*.o ${XC3_ARCH_BASE:-$XC3_ARCH} $XC3_SYS_LIBS 2> build/link_gfx.err
rc=$?; [ $rc = 0 ] && echo "${XC3_CPU_LEVEL:-v2}" > "${OUT%$XC3_EXE_EXT}.cpu_level"
grep "undefined symbol\|undefined reference" build/link_gfx.err | sed 's/.*undefined [^:]*: //' | sort -u | head -60; exit $rc
