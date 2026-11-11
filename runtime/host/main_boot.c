// Headless boot host (M3a): loads the modules, sets up the kernel and runs nn::init::Start, with strict logging of
// every service, SVC and import the game touches. No window, no renderer. Derived from LASRecomp runtime/main.c (MIT). [XCDE]
#include "../diag/diag.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <windows.h>

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

#ifdef XCDE_GFX
// [XCDE] T194: laptops with two GPUs (NVIDIA Optimus / AMD switchable graphics) run a program on the integrated one
// unless the exe exports these; the driver reads them at start. (device.c then picks the discrete GPU among those
// Vulkan lists; LARECOMP_GPU=<n> overrides.)
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

// [XC3] SHA-256 of a small file as hex (advapi32), for the boot log: main.npdm is the one exefs file that differs
// between 2.2.0 and 2.2.1, so a log tells the two apart. hex stays "" when the file or the hash is not available.
#include <wincrypt.h>
static void file_sha256(const char *path, char hex[65], long *size) {
    hex[0] = 0;
    *size = -1;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    HCRYPTPROV prov = 0;
    HCRYPTHASH h = 0;
    uint8_t buf[4096], dig[32];
    DWORD dn = sizeof dig;
    size_t n;
    long total = 0;
    if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(prov, CALG_SHA_256, 0, 0, &h)) {
        bool ok = true;
        while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) ok = CryptHashData(h, buf, (DWORD)n, 0), total += (long)n;
        if (ok && CryptGetHashParam(h, HP_HASHVAL, dig, &dn, 0) && dn == sizeof dig) {
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", dig[i]);
            *size = total;
        }
    }
    if (h) CryptDestroyHash(h);
    if (prov) CryptReleaseContext(prov, 0);
    fclose(f);
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
        char n[16] = "0";
        GetEnvironmentVariableA("XCDE_RELAUNCH", n, sizeof n);
        int tries = atoi(n);
        fprintf(stderr, "[boot] guest address range %llx-%llx is not free (try %d)%s\n", (unsigned long long)GUEST_LO,
                (unsigned long long)GUEST_HI, tries + 1, tries < 8 ? ": starting again" : "");
        if (tries < 8) {
            snprintf(n, sizeof n, "%d", tries + 1);
            SetEnvironmentVariableA("XCDE_RELAUNCH", n);
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
        }
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", (unsigned long long)GUEST_LO,
                (unsigned long long)GUEST_HI);
        exit(1);
    }
}

// [XCDE] Watchdog (T027, first version): every few seconds the state of every guest thread; XCDE_WATCHDOG=<seconds>.
static DWORD WINAPI watchdog_main(LPVOID p) {
    unsigned secs = (unsigned)(uintptr_t)p;
    for (;;) { Sleep(secs * 1000); LOG("[wd] ---- %u s ----"" %c", secs, 10); kernel_dump_threads(stderr); }
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
    if (getenv("XCDE_WATCHDOG")) CreateThread(NULL, 0, watchdog_main, (LPVOID)(uintptr_t)atoi(getenv("XCDE_WATCHDOG")), 0, NULL);
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
