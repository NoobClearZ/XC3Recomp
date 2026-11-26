// [XC3] The CPU level a build was made for (scripts/env.sh XC3_CPU_LEVEL: x86-64-v2 or -v3) against the CPU it runs
// on. CPUID by inline asm: zig's compiler-rt __builtin_cpu_supports() reports 0 on Linux, and the <cpuid.h> helpers
// would be compiled for the build's level. The checks are compiled for plain x86-64 (CPU_BASE) and the platform files
// run them as a constructor, before main, so a v3 build copied to a PC without AVX2 (scripts/make_dist.sh) says why
// instead of stopping on an illegal instruction. Best effort: the C runtime's own startup runs earlier.
#pragma once
#include <stdbool.h>

#define CPU_BASE __attribute__((target("arch=x86-64")))
#define CPUID(leaf, sub, r) __asm__ volatile("cpuid" : "=a"((r)[0]), "=b"((r)[1]), "=c"((r)[2]), "=d"((r)[3]) : "a"(leaf), "c"(sub))

CPU_BASE static inline bool cpu_has_v2(void) {
    unsigned r[4];
    CPUID(0u, 0u, r);
    if (r[0] < 1) return false;
    CPUID(1u, 0u, r);
    return (r[2] & (1u << 20)) /* SSE4.2 */ && (r[2] & (1u << 23)) /* POPCNT */;
}

CPU_BASE static inline bool cpu_has_v3(void) {
    unsigned r[4], max;
    if (!cpu_has_v2()) return false;
    CPUID(0u, 0u, r);
    max = r[0];
    CPUID(1u, 0u, r);
    const unsigned need1 = (1u << 12) /* FMA */ | (1u << 22) /* MOVBE */ | (1u << 27) /* OSXSAVE */ |
                           (1u << 28) /* AVX */ | (1u << 29) /* F16C */;
    if ((r[2] & need1) != need1 || max < 7) return false;
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) return false;  // the OS saves the XMM and YMM registers
    CPUID(7u, 0u, r);
    const unsigned need7 = (1u << 3) /* BMI1 */ | (1u << 5) /* AVX2 */ | (1u << 8) /* BMI2 */;
    if ((r[1] & need7) != need7) return false;
    CPUID(0x80000000u, 0u, r);
    if (r[0] < 0x80000001u) return false;
    CPUID(0x80000001u, 0u, r);
    return r[2] & (1u << 5);  // LZCNT
}

#if defined(__AVX2__)
#define CPU_LEVEL_NAME "x86-64-v3 (AVX2, FMA, BMI2)"
#define CPU_LEVEL_OK() cpu_has_v3()
#else
#define CPU_LEVEL_NAME "x86-64-v2 (SSE4.2, POPCNT)"
#define CPU_LEVEL_OK() cpu_has_v2()
#endif
#define CPU_LEVEL_HINT "Build it again on this PC (\"Build XC3.bat\" / scripts/build_all.sh), or with XC3_MARCH=v2 for older CPUs."
