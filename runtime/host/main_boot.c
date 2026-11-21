// Headless boot host (M3a): loads the modules, sets up the kernel and runs nn::init::Start, with strict logging of
// every service, SVC and import the game touches. No window, no renderer. Derived from LASRecomp runtime/main.c (MIT). [XCDE]
#include "../diag/diag.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../gamelib.h"
#include "../hle.h"
#include "../kernel.h"
#include "../rt.h"
#include "../settings.h"

const char *g_romfs_dir;
const char *g_dlc_dir;
// [XC3] The version the title screen shows (am GetDisplayVersion). 2.2.1 has the code and romfs of 2.2.0; only
// main.npdm differs (its ACID signature and key, bytes 0x80-0x27f), so its SHA-256 is what tells them apart.
const char *g_display_version = "2.2.0";
#define NPDM_SHA256_2_2_1 "45a8ecf07093d0b6a60c9ef3c2199bf4398c15bb453017a449a2d7690d433b7f"

#if defined(XCDE_GFX) && defined(_WIN32)
// [XCDE] T194: laptops with two GPUs (NVIDIA Optimus / AMD switchable graphics) run a program on the integrated one
// unless the exe exports these; the driver reads them at start. (device.c then picks the discrete GPU among those
// Vulkan lists; LARECOMP_GPU=<n> overrides.)
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

// [XC3] SHA-256 of a small file as hex, for the boot log: main.npdm is the one exefs file that differs
// between 2.2.0 and 2.2.1, so a log tells the two apart. hex stays "" when the file is not available.
static void sha256_block(uint32_t s[8], const uint8_t b[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64], a = s[0], e0 = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) | ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >> 3);
        uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
        uint32_t ch = (e & f) ^ ((~e) & g), t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
        uint32_t maj = (a & e0) ^ (a & c) ^ (e0 & c), t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = e0; e0 = a; a = t1 + t2;
    }
    s[0] += a; s[1] += e0; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}
static void file_sha256(const char *path, char hex[65], long *size) {
    hex[0] = 0;
    *size = -1;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t st[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t blk[64], buf[4096];
    size_t bl = 0, n;
    uint64_t total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        total += n;
        for (size_t i = 0; i < n; i++) {
            blk[bl++] = buf[i];
            if (bl == 64) { sha256_block(st, blk); bl = 0; }
        }
    }
    fclose(f);
    blk[bl++] = 0x80;
    if (bl > 56) { while (bl < 64) blk[bl++] = 0; sha256_block(st, blk); bl = 0; }
    while (bl < 56) blk[bl++] = 0;
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; i++) blk[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_block(st, blk);
    for (int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08x", st[i]);
    *size = (long)total;
}

static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, (unsigned long long)m->init,
        (unsigned long long)(m->init_array_sz / 8));
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}
static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }
// [XC3] The newer SDK's nn::init::Start takes three rtld callbacks (XC3 rtld +0x138c, +0x820, +0x870): x2 sets an rtld
// flag (after nnosInitialize), x3 runs every module's init in reverse load order (sdk, then main; after
// nninitStartup), x4 runs every module's fini after nnMain returns.
static void rtld_flag(Cpu *c) { (void)c; }
static void rtld_init_all(Cpu *c) { module_init(c, &g_modules[1]); module_init(c, &g_modules[0]); }
static void rtld_fini_all(Cpu *c) { (void)c; LOG("[boot] nnMain returned (module finalizers not run)\n"); }

__attribute__((constructor(101))) static void reserve_guest_space(void) {
    uint64_t lo = GUEST_LO;
    // [XC3] (ALIAS_BASE == GUEST_LO) its own reservation, the rest of the window after it as before
    if (!getenv("XC3_NO_PMEM") && plat_pmem_reserve(ALIAS_BASE, ALIAS_BASE + ALIAS_SIZE)) {
        g_pmem_reserved = true;
        lo = ALIAS_BASE + ALIAS_SIZE;
    }
    if (!plat_reserve_guest(lo, GUEST_HI)) {
        // [XCDE] T135: something the loader placed at random (ASLR) sits in the guest range in ~1 of 5 starts. A new
        // process gets a new layout: start this one again (same command line, same console), up to 8 times.
        const char *env_tries = getenv("XCDE_RELAUNCH");
        int tries = env_tries ? atoi(env_tries) : 0;
        fprintf(stderr, "[boot] guest address range %llx-%llx is not free (try %d)%s\n", (unsigned long long)GUEST_LO,
                (unsigned long long)GUEST_HI, tries + 1, tries < 8 ? ": starting again" : "");
        if (tries < 8) {
            char n[16];
            snprintf(n, sizeof n, "%d", tries + 1);
            setenv("XCDE_RELAUNCH", n, 1);
#ifdef _WIN32
            STARTUPINFOA si = {.cb = sizeof si};
            PROCESS_INFORMATION pi;
            char exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, sizeof exe);
            // the new process dies with this one (a test's timeout or a closed console must not leave it running)
            HANDLE job = CreateJobObjectA(NULL, NULL);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {0};
            jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof jl);
            if (CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
                if (job) AssignProcessToJobObject(job, pi.hProcess);
                ResumeThread(pi.hThread);
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD code = 1;
                GetExitCodeProcess(pi.hProcess, &code);
                ExitProcess(code);
            }
#else
            plat_exec(NULL);
#endif
        }
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", (unsigned long long)GUEST_LO,
                (unsigned long long)GUEST_HI);
        exit(1);
    }
}

// [XCDE] Watchdog (T027, first version): every few seconds the state of every guest thread; XCDE_WATCHDOG=<seconds>.
static void *watchdog_main(void *p) {
    unsigned secs = (unsigned)(uintptr_t)p;
    for (;;) {
        plat_sleep_until_ns(plat_now_ns() + (uint64_t)secs * 1000000000ull);
        LOG("[wd] ---- %u s ----"" %c", secs, 10);
        kernel_dump_threads(stderr);
    }
    return NULL;
}


int main(int argc, char **argv) {
    if (argc >= 6 && !strcmp(argv[1], "--minidump")) return diag_minidump_helper(argc, argv);  // [XCDE] T028: crash helper process
#ifdef XCDE_GFX
    if (argc >= 3 && !strcmp(argv[1], "--precompile")) {  // [XCDE] T187: fill the shader cache from the RomFS, then exit
        extern int vk_precompile_main(const char *root);
        return vk_precompile_main(argv[2]);
    }
    if (argc >= 2 && (!strcmp(argv[1], "--repack-spirv") || !strcmp(argv[1], "--compress-cache"))) {
        extern int vk_pack_recompress(void);
        return vk_pack_recompress();
    }
#endif
    plat_init();
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: xcde_boot <dump dir with exefs/ and romfs/> [seconds]\n"); return 2; }
    const char *root = argv[1];
    int seconds = argc > 2 ? atoi(argv[2]) : 20;
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    g_romfs_dir = strdup(romfs);
    {   // [XC3] add-on content: <dump>/dlcs (services/fs.c, services/misc.c aoc:u), unless xc3.ini says dlc = 0
        char dlcs[1024];
        struct stat sb;
        snprintf(dlcs, sizeof dlcs, "%s/dlcs", root);
        settings_load();
        if (!g_settings.dlc_off && stat(dlcs, &sb) == 0 && S_ISDIR(sb.st_mode)) g_dlc_dir = strdup(dlcs);
        fprintf(stderr, "[aoc] add-on content: %s\n", g_dlc_dir ? g_dlc_dir : g_settings.dlc_off ? "off (xc3.ini dlc = 0)" : "none");
    }

    diag_watchdog_start();
    if (getenv("XCDE_WATCHDOG")) {
        pthread_t wt;
        if (pthread_create(&wt, NULL, watchdog_main, (void *)(uintptr_t)atoi(getenv("XCDE_WATCHDOG"))) == 0) pthread_detach(wt);
    }
    loader_load(exefs);
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000, sys_resource = 0;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            sys_resource = *(uint32_t *)(meta + 0x14);  // [XC3] system resource size: 32 MB in XC3's npdm
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    kernel_set_system_resource_size(sys_resource);
    LOG("main thread: prio %d, stack 0x%x, system resource 0x%x\n", prio, stack_size, sys_resource);
    {   // [XC3] which update the dump is (2.2.0 and 2.2.1 differ in main.npdm only)
        char hex[65];
        long size;
        file_sha256(npdm_path, hex, &size);
        if (!strcmp(hex, NPDM_SHA256_2_2_1)) g_display_version = "2.2.1";
        LOG("main.npdm: sha256 %s, %ld bytes: version %s\n", hex[0] ? hex : "(not available)", size, g_display_version);
    }

    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    uint64_t start3 = start ? 0 : loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");  // [XC3] three callbacks
    if (!start && !start3) FATAL("nn::init::Start not found\n");
#if defined(XCDE_GFX) && defined(__linux__)
    // [XC3] Initialize Vulkan before guest threads start so Linux pmem can be backed by exported DMA-BUFs
    // (RADV rejects VK_EXT_external_memory_host on memfd MAP_SHARED mappings).
    extern void ui_window_init(void);
    ui_window_init();
#endif
    KThread *t = kernel_create_main_thread(start ? start : start3, stack_size, prio);
    if (start3) {
        LOG("[boot] nn::init::Start with three rtld callbacks\n");
        t->cpu.x[4] = hle_register("<rtld_fini_all>", rtld_fini_all);
        kernel_run_main(t, t->handle, 0, hle_register("<rtld_flag>", rtld_flag), hle_register("<rtld_init_all>", rtld_init_all));
    } else {
        uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
        uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
        kernel_run_main(t, t->handle, 0, init_fn, fini_fn);
    }
#ifdef XCDE_GFX
    extern int ui_run(void);  // window + Vulkan presenter on the main thread (runtime/nvn/vulkan/device.c)
    LOG("[boot] running with the window\n");
    (void)seconds;
    return ui_run();
#else
    LOG("[boot] running; stopping after %d s\n", seconds);
    plat_sleep_until_ns(plat_now_ns() + (uint64_t)seconds * 1000000000ull);
    LOG("[boot] timeout reached, exiting\n");
    return 0;
#endif
}
