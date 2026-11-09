// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include "../diag/diag.h"
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    diag_unimplemented("ipc", "[svc] %s cmd %u", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: diag_unimplemented("ipc", "[pctl] IParentalControlService cmd %u", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ aoc:u
// [XC3] Add-on content from <dump>/dlcs (g_dlc_dir): one folder holding the files of all four add-ons, whose names
// carry their number (bf3_dlc01..04.ard/.arh, sound/*_dlc2..4.pck). The game mounts each as "aoc<index>:" through
// nn::fs::MountAddOnContent; every index is served the same folder (services/fs.c), and opens only its own files.
// The data id of index i is base + i (the application id + 0x1000, as on the console; both ends are answered here).
// Only the add-ons whose archive pair (bf3_dlc<NN>.arh + .ard) is in the folder are listed.
#define AOC_BASE_ID 0x010074F013263000ull
#define AOC_MAX 4
static int g_aoc_list[AOC_MAX], g_aoc_n = -1;
static int aoc_list(void) {
    if (g_aoc_n >= 0) return g_aoc_n;
    g_aoc_n = 0;
    for (int i = 1; g_dlc_dir && i <= AOC_MAX; i++) {
        char h[1024], d[1024];
        snprintf(h, sizeof h, "%s/bf3_dlc%02d.arh", g_dlc_dir, i);
        snprintf(d, sizeof d, "%s/bf3_dlc%02d.ard", g_dlc_dir, i);
        FILE *a = fopen(h, "rb"), *b = fopen(d, "rb");
        if (a && b) g_aoc_list[g_aoc_n++] = i;
        else LOG("[aoc] add-on %d: bf3_dlc%02d.arh / .ard not in %s - not listed\n", i, i, g_dlc_dir);
        if (a) fclose(a);
        if (b) fclose(b);
    }
    if (g_dlc_dir) LOG("[aoc] %d add-ons listed from %s\n", g_aoc_n, g_dlc_dir);
    return g_aoc_n;
}
int aoc_index_of(uint64_t data_id) {  // a listed add-on's index, or 0: not one of ours
    for (int i = 0; i < aoc_list(); i++) if (data_id == AOC_BASE_ID + (uint64_t)g_aoc_list[i]) return g_aoc_list[i];
    return 0;
}
SERVICE("aoc:u", aoc_u) {
    (void)s;
    int n = aoc_list();
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = (uint32_t)n; resp_size(resp, 4); break;  // CountAddOnContent(pid)
    case 3: {  // ListAddOnContent(offset, count, pid) -> count, B buffer of s32 indices (layout read from XC3's request)
        uint32_t off = IN(uint32_t, 0), want = IN(uint32_t, 4), got = 0;
        IpcBuf *b = out_buf(req, 0);
        int32_t *out = b ? (int32_t *)(uintptr_t)b->addr : NULL;
        for (uint32_t i = off; out && i < (uint32_t)n && got < want && (got + 1) * 4 <= b->size; i++) out[got++] = g_aoc_list[i];
        OUT(uint32_t, 0) = got;
        resp_size(resp, 4);
        LOG("[aoc] ListAddOnContent(offset %u, count %u) -> %u\n", off, want, got);
        break;
    }
    case 5: OUT(uint64_t, 0) = AOC_BASE_ID; resp_size(resp, 8); break;  // GetAddOnContentBaseId(pid)
    case 7: LOG("[aoc] PrepareAddOnContent(%d)\n", IN(int32_t, 0)); break;  // PrepareAddOnContent(index, pid)
    case 8: case 10: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // GetAddOnContentListChangedEvent(WithProcessId)
    default: ok_logger(s, req, resp);
    }
}
