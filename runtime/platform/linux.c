// Linux host layer (runtime/platform.h). x86-64 and aarch64.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

// ---- guest address space
// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the window doesn't count against overcommit).
static uint64_t g_lo, g_hi, g_pmem_lo, g_pmem_hi;
static int g_pmem_fd_lo = -1, g_pmem_fd_hi = -1;
static uint64_t g_pmem_lo_bytes;
static PlatPmemDmabufFn g_pmem_dmabuf_alloc;

void plat_pmem_set_dmabuf_alloc(PlatPmemDmabufFn fn) { g_pmem_dmabuf_alloc = fn; }

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    g_lo = lo;
    g_hi = hi;
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// [XC3] Physical memory pool (platform.h): reserve the alias region as PROT_NONE and back pool pages with memfd_create.
bool plat_pmem_reserve(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }
    g_pmem_lo = lo;
    g_pmem_hi = hi;
    return true;
}

void *plat_pmem_create(uint64_t size) {
    if (g_pmem_dmabuf_alloc) {
        int fd_lo = -1, fd_hi = -1;
        uint64_t lo_bytes = 0;
        void *view = g_pmem_dmabuf_alloc(size, &fd_lo, &fd_hi, &lo_bytes);
        if (view) {
            g_pmem_fd_lo = fd_lo;
            g_pmem_fd_hi = fd_hi;
            g_pmem_lo_bytes = lo_bytes;
            return view;
        }
    }
    int fd = memfd_create("xc3_pmem", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return NULL; }
    void *view = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) { close(fd); return NULL; }
    g_pmem_fd_lo = fd;
    g_pmem_fd_hi = -1;
    g_pmem_lo_bytes = size;
    return view;
}

bool plat_pmem_map(uint64_t addr, uint64_t off) {
    int fd = off < g_pmem_lo_bytes ? g_pmem_fd_lo : g_pmem_fd_hi;
    uint64_t foff = off < g_pmem_lo_bytes ? off : off - g_pmem_lo_bytes;
    if (fd < 0) return false;
    return mmap((void *)(uintptr_t)addr, PLAT_PMEM_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                fd, (off_t)foff) == (void *)(uintptr_t)addr;
}

bool plat_pmem_unmap(uint64_t addr) {
    return mmap((void *)(uintptr_t)addr, PLAT_PMEM_PAGE, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == (void *)(uintptr_t)addr;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

bool plat_mem_readable(uint64_t addr, size_t size) {
    if (!addr) return false;
    uint64_t end = addr + (size ? size : 1);
    pid_t pid = getpid();
    for (uint64_t a = addr; a < end; a = (a | 4095ull) + 1) {
        uint8_t byte = 0;
        struct iovec loc = {&byte, 1}, rem = {(void *)(uintptr_t)a, 1};
        if (process_vm_readv(pid, &loc, 1, &rem, 1, 0) != 1) return false;
    }
    return true;
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}

// ---- process
// Started from a desktop there is no terminal: stdout / stderr go to xc3.log in the data folder.
static char g_log_path[1024];

static void log_to_file_without_terminal(void) {
    if (isatty(STDERR_FILENO)) return;
    char dir[900];
    const char *env = getenv("XC3_DATA_ROOT");
    if (!env || !*env) env = getenv("LARECOMP_DATA_ROOT");
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    struct stat st;
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
    else if ((stat("targets", &st) == 0 && S_ISDIR(st.st_mode)) ||
             (stat("extracted", &st) == 0 && S_ISDIR(st.st_mode))) return;  // dev mode: wherever stderr goes already
    else if (xdg && *xdg == '/') snprintf(dir, sizeof dir, "%s/xc3recomp", xdg);
    else snprintf(dir, sizeof dir, "%s/.local/share/xc3recomp", home && *home ? home : "/tmp");
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    snprintf(g_log_path, sizeof g_log_path, "%s/xc3.log", dir);
    bool append = getenv("XC3_LOG_APPEND") != NULL || getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("XC3_LOG_APPEND", "1", 1);
}

#if defined(__x86_64__)
// [XC3] The CPU level the build was made for (platform/cpu_level.h), checked before main and again from plat_init.
#include "cpu_level.h"
__attribute__((constructor)) CPU_BASE static void require_cpu(void) {
    if (CPU_LEVEL_OK()) return;
    fprintf(stderr, "FATAL: this XC3Recomp build needs an " CPU_LEVEL_NAME " CPU. " CPU_LEVEL_HINT "\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

void plat_init(void) {
    log_to_file_without_terminal();
    require_cpu();
}

// (inside an AppImage: the binary in its mount, which stays mounted while this process lives - re-exec included)
bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// Our own command line, from /proc/self/cmdline (NUL-separated).
static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

// ---- diagnostics
// Exported symbols only (the Makefile links with -rdynamic); frames in the generated code are left to the caller,
// which maps them to guest functions (runtime/dispatch.c). Static runtime functions come out as the nearest guest
// function, so backtrace lines also give the offset in the executable (addr2line -fe lasrecomp <offset>).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: say so in a message box rather than vanishing (the details are in the log).
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1200];
    snprintf(text, sizeof text, "XC3Recomp stopped because of an error.\n\nDetails: %s", g_log_path);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "XC3Recomp", text, NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (xc3+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// End the process at once, skipping the library teardown: as on Windows, the GPU driver's threads crashed in exit()
// while the game's threads still submitted work.
void plat_terminate(void) { _exit(0); }

// [XCDE] T169: memory report helpers used by runtime/nvn/vulkan/exec.c
void plat_mem_info(uint64_t *priv, uint64_t *ws, uint64_t *guest_commit) {
    *priv = *ws = *guest_commit = 0;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long size = 0, res = 0, shared = 0;
        if (fscanf(f, "%lu %lu %lu", &size, &res, &shared) >= 2) {
            *ws = (uint64_t)res * (uint64_t)pg;
            *priv = res > shared ? (uint64_t)(res - shared) * (uint64_t)pg : *ws;
        }
        fclose(f);
    }
    uint64_t wlo = g_pmem_lo && g_pmem_lo < g_lo ? g_pmem_lo : g_lo;
    uint64_t whi = g_hi;
    FILE *smaps = fopen("/proc/self/smaps", "r");
    if (smaps && whi > wlo) {
        char line[512];
        bool in_guest = false;
        while (fgets(line, sizeof line, smaps)) {
            unsigned long long a = 0, b = 0;
            if (sscanf(line, "%llx-%llx ", &a, &b) == 2) {
                in_guest = (a >= wlo && b <= whi);
            } else if (in_guest && strncmp(line, "Rss:", 4) == 0) {
                unsigned long long kb = 0;
                if (sscanf(line + 4, "%llu", &kb) == 1) *guest_commit += kb * 1024ull;
            }
        }
        fclose(smaps);
    } else {
        *guest_commit = *priv;
    }
}

void plat_mem_map(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    typedef struct { uint64_t base, bytes; char name[64]; } MapEntry;
    static MapEntry entries[4096];
    int n = 0;
    uint64_t total = 0, small = 0;
    uint64_t wlo = g_pmem_lo && g_pmem_lo < g_lo ? g_pmem_lo : g_lo;
    uint64_t whi = g_hi;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long a = 0, b = 0;
        char perms[8] = "";
        unsigned long long off = 0, inode = 0;
        char dev[32] = "", path[256] = "";
        int fields = sscanf(line, "%llx-%llx %7s %llx %31s %llu %255[^\n]", &a, &b, perms, &off, dev, &inode, path);
        if (fields < 2) continue;
        if (wlo && a >= wlo && b <= whi) continue;
        if (perms[0] != 'r' && perms[1] != 'w') continue;
        if (inode != 0 && strchr(perms, 'p') == NULL) continue;
        uint64_t sz = b - a;
        total += sz;
        if (sz < (1u << 20)) small += sz;
        if (n < 4096) {
            entries[n].base = a;
            entries[n].bytes = sz;
            const char *p = path;
            while (*p == ' ') p++;
            snprintf(entries[n].name, sizeof entries[n].name, "%s", *p ? p : "[anon]");
            n++;
        }
    }
    fclose(f);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (entries[j].bytes > entries[i].bytes) { MapEntry t = entries[i]; entries[i] = entries[j]; entries[j] = t; }
    fprintf(stderr, "[memmap] mapped outside the guest window: %.0f MB in %d regions (%.0f MB under 1 MB)\n",
            total / 1048576.0, n, small / 1048576.0);
    for (int i = 0; i < n && i < 24; i++)
        fprintf(stderr, "[memmap]   %#llx  %8.1f MB  %s\n", (unsigned long long)entries[i].base,
                entries[i].bytes / 1048576.0, entries[i].name);
}

// [XCDE] T122: not sampled on this platform
uint64_t plat_thread_pc(void *pthread_handle) { (void)pthread_handle; return 0; }
void plat_thread_name(const char *name) {
    char buf[16];
    snprintf(buf, sizeof buf, "%.15s", name ? name : "");
    pthread_setname_np(pthread_self(), buf);
}
void plat_thread_background(void) {
    const char *e = getenv("LARECOMP_WORKER_PRIO");
    if (e && atoi(e) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
}
void plat_thread_audio(void) {
    const char *e = getenv("LARECOMP_AUDIO_PRIO");
    if (e && atoi(e) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);
}
int plat_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 4; }

// [XCDE] T122: not available on this platform
int plat_thread_backtrace(void *pthread_handle, uint64_t *pcs, int max) { (void)pthread_handle; (void)pcs; (void)max; return 0; }
int plat_guest_find(const uint8_t *pat, size_t n, uint64_t *out, int max) { (void)pat; (void)n; (void)out; (void)max; return 0; }  // [XCDE] (not wired)
uint64_t plat_guest_dump(const char *path) { (void)path; return 0; }  // [XCDE] (not wired)
