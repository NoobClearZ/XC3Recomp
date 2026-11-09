// [XCDE] In-game settings overlay (runtime/ui/menu.h; Windows / Vulkan): F1 opens and closes it (Esc closes), the
// launcher's options over the running game. Dear ImGui drawn by its Vulkan backend into the window image after the
// game image (device.c calls menu_render inside that rendering). Events arrive on the main thread and are queued for
// the render thread, which owns the ImGui context; while the menu is open the game sees no keyboard / mouse / pad
// input. Options that the renderer reads every frame apply at once (resolution, upscaler and its quality, SMAA, depth
// of field, volume, FPS in the title, V-Sync); the others are saved to xcde.ini and apply at the next start
// (marked "al reiniciar"). "Guardar" writes xcde.ini (settings_save) for the launcher and the next start.
#include <SDL3/SDL.h>
#include <math.h>
#include <mutex>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

extern "C" {
#include "../nvn/vulkan/vk.h"
#include "../settings.h"
#include "menu.h"
bool vk_imgui_backend_init(void);
void input_reset(void);
void vk_request_swapchain_recreate(void);
bool dlss_available(void);
}

static std::mutex q_lock;
static SDL_Event q[256];
static int q_n;
static volatile bool open_, want_window;
static bool fs_applied = false, fs_init;
static volatile float cur_fps;
static volatile bool want_title;
static bool title_has_fps;

#define WIN_TITLE "Xenoblade Chronicles 3 (Recomp)"

static bool is_input(Uint32 t) {
    switch (t) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return true;
    default: return false;
    }
}

extern "C" bool menu_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
        (e->key.scancode == SDL_SCANCODE_F1 || (open_ && e->key.scancode == SDL_SCANCODE_ESCAPE))) {
        open_ = !open_ && e->key.scancode == SDL_SCANCODE_F1;
        input_reset();  // (keys held when the menu opened / closed are released for the game)
        return true;
    }
    if (!open_ || !is_input(e->type)) return false;
    std::lock_guard<std::mutex> g(q_lock);
    if (q_n < 256) q[q_n++] = *e;
    return true;
}

extern "C" bool menu_is_open(void) { return open_; }

extern "C" void menu_set_fps(float fps) {
    cur_fps = fps;
    want_title = true;
}

extern "C" void menu_apply_window_requests(void *sdl_window) {
    if (!fs_init) { fs_init = true; fs_applied = g_settings.fullscreen; }
    if (!sdl_window) return;
    if (want_window) {
        want_window = false;
        if (fs_applied != g_settings.fullscreen) {
            SDL_SetWindowFullscreen((SDL_Window *)sdl_window, g_settings.fullscreen);
            fs_applied = g_settings.fullscreen;
        }
    }
    {   // [XC3] the cursor is hidden while the game window has the focus and this menu is closed (the game has no mouse
        // pointer; F1 and Alt+Tab bring it back). The mouse camera's relative mode hides it on its own as well.
        SDL_Window *w = (SDL_Window *)sdl_window;
        SDL_WindowFlags f = SDL_GetWindowFlags(w);
        static int hidden = -1;
        int want = (f & SDL_WINDOW_INPUT_FOCUS) && !open_;
        if (want != hidden) { hidden = want; if (want) SDL_HideCursor(); else SDL_ShowCursor(); }
        static int logged;  // (the window as it came out: fullscreen or not, its size)
        if (!logged && (f & SDL_WINDOW_INPUT_FOCUS)) {
            logged = 1;
            int ww = 0, wh = 0;
            SDL_GetWindowSizeInPixels(w, &ww, &wh);
            fprintf(stderr, "[window] %s %dx%d (setting fullscreen = %d)\n", (f & SDL_WINDOW_FULLSCREEN) ? "fullscreen" : "windowed", ww, wh,
                    (int)g_settings.fullscreen);
        }
    }
    if (g_settings.show_fps && want_title && cur_fps > 0) {
        want_title = false;
        char title[128];
        snprintf(title, sizeof title, WIN_TITLE " - %.0f FPS", (double)cur_fps);
        SDL_SetWindowTitle((SDL_Window *)sdl_window, title);
        title_has_fps = true;
    } else if (!g_settings.show_fps && title_has_fps) {
        SDL_SetWindowTitle((SDL_Window *)sdl_window, WIN_TITLE);
        title_has_fps = false;
    }
}

extern "C" void menu_test_open(void) { open_ = true; }

static ImGuiKey key_of(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    default: return ImGuiKey_None;
    }
}
static ImGuiKey pad_key(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    default: return ImGuiKey_None;
    }
}

// Events queued by the main thread -> ImGui IO (render thread). Mouse positions: window points -> pixels.
static void feed(ImGuiIO &io, uint32_t w, uint32_t h) {
    SDL_Event ev[256];
    int n;
    {
        std::lock_guard<std::mutex> g(q_lock);
        n = q_n;
        memcpy(ev, q, sizeof(SDL_Event) * n);
        q_n = 0;
    }
    for (int i = 0; i < n; i++) {
        const SDL_Event &e = ev[i];
        float sx = 1, sy = 1;
        SDL_Window *win = SDL_GetWindowFromID(e.motion.windowID);
        if (win) {
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            if (ww > 0 && wh > 0) { sx = (float)w / ww; sy = (float)h / wh; }
        }
        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: io.AddMousePosEvent(e.motion.x * sx, e.motion.y * sy); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            io.AddMousePosEvent(e.button.x * sx, e.button.y * sy);
            io.AddMouseButtonEvent(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: io.AddMouseWheelEvent(e.wheel.x, e.wheel.y); break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            ImGuiKey k = key_of(e.key.scancode);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_KEY_DOWN);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: io.AddInputCharactersUTF8(e.text.text); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            ImGuiKey k = pad_key(e.gbutton.button);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) open_ = false;  // (B: back to the game)
            break;
        }
        default: break;
        }
    }
}

#define INI_FILE "xc3.ini"

static const char *const k_res_ui[] = {"Match window (recommended)", "1x - 1080p (like Switch on TV)", "2x - 2160p (4K)", "3x - 3240p",
                                       "75% of window", "50% of window", "Dynamic (holds 60 FPS)"};
static const int k_res_mode[] = {RES_MATCH, 1, 2, 3, RES_Q75, RES_Q50, RES_DYNAMIC};

static void ui(void) {
    static bool saved_note;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.5f, vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.y * 0.95f, vp->Size.y * 0.8f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("Options  (F1 / Esc to return to game)", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Graphics")) {
            ImGui::SeparatorText("Display");
            if (ImGui::Checkbox("Fullscreen", &g_settings.fullscreen)) want_window = true;
            ImGui::SeparatorText("Resolution and scaling");
            int ri = 0;
            for (int i = 0; i < 7; i++) if (k_res_mode[i] == g_settings.res_mode) ri = i;
            if (ImGui::Combo("Internal resolution", &ri, k_res_ui, 7)) g_settings.res_mode = k_res_mode[ri];
            static const char *ups[] = {"Bilinear (soft)", "AMD FSR 1.0 (sharp)", "AMD FSR 3.1 (temporal)", "NVIDIA DLSS (temporal, RTX)",
                                        "AMD FSR 4 (temporal, RDNA 4)"};
            static const int upv[] = {UPSCALE_BILINEAR, UPSCALE_FSR1, UPSCALE_FSR3, UPSCALE_DLSS, UPSCALE_FSR4};
            int ui_ = 0;
            for (int i = 0; i < 5; i++) if (upv[i] == g_settings.upscaler) ui_ = i;
            if (ImGui::Combo("Upscaling / anti-aliasing", &ui_, ups, 5)) g_settings.upscaler = upv[ui_];
            if (g_settings.upscaler == UPSCALE_DLSS && !dlss_available())
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "DLSS loads when the game starts: FSR 3.1 is used until a restart.");
            if (UPSCALE_TEMPORAL(g_settings.upscaler)) {  // [XC3] the scene's render size; no frame generation (launcher.cpp)
                static const char *uq[] = {"Native (anti-aliasing only)", "Quality (67 %)", "Balanced (59 %)", "Performance (50 %)",
                                           "Ultra performance (33 %)"};
                ImGui::Combo("Upscaling quality", &g_settings.upscale_quality, uq, 5);
            }
            if (false) {
                ImGui::BeginDisabled(g_settings.upscaler != UPSCALE_FSR3);
                ImGui::Checkbox("Frame generation (FSR 3; the first time it is enabled: after a restart)", &g_settings.frame_gen);
                ImGui::EndDisabled();
            }
            ImGui::SeparatorText("Quality");
            bool smaa = g_settings.antialias == AA_SMAA;
            ImGui::BeginDisabled(UPSCALE_TEMPORAL(g_settings.upscaler));  // [XC3] (exec.c post_process: not with them)
            if (ImGui::Checkbox("SMAA anti-aliasing", &smaa)) g_settings.antialias = smaa ? AA_SMAA : AA_OFF;
            ImGui::EndDisabled();
            if (UPSCALE_TEMPORAL(g_settings.upscaler)) ImGui::SameLine(), ImGui::TextDisabled("(not used with FSR 3.1 / FSR 4 / DLSS)");
            bool dof = !g_settings.dof_off;
            if (ImGui::Checkbox("Depth of field", &dof)) g_settings.dof_off = !dof;
            // [XC3] two switches (romfs_virtual.c xc3_libnx_patch: lib_nx.bin read once, at start)
            ImGui::Checkbox("Motion blur (after restart)", &g_settings.motion_blur);
            ImGui::BeginDisabled(UPSCALE_TEMPORAL(g_settings.upscaler));
            ImGui::Checkbox("Game's temporal anti-aliasing / TMAA (after restart)", &g_settings.game_taa);
            ImGui::EndDisabled();
            if (UPSCALE_TEMPORAL(g_settings.upscaler)) ImGui::SameLine(), ImGui::TextDisabled("(kept: FSR 3.1 / FSR 4 / DLSS use it)");
            ImGui::Checkbox("Show FPS in title bar", &g_settings.show_fps);
            ImGui::SeparatorText("Speed");
            static const int fpsv[] = {30, 60, 120, 144, 165, 0};
            static const char *fpsu[] = {"30 (like Switch)", "60", "120", "144", "165", "Unlimited"};
            int fi = 0;
            for (int i = 0; i < 6; i++) if (fpsv[i] == g_settings.fps) fi = i;
            if (fi > 1) fi = 1;  // [XC3] 30 / 60 only (framepace.c xc3_fps_frame)
            if (ImGui::Combo("FPS (after restart)", &fi, fpsu, 2)) {
                g_settings.fps = fpsv[fi];
                g_settings.frame_lag = (g_settings.fps == 60);  // [XC3] auto-enable frame lag with 60 fps
            }
            static const char *aniso_names[] = {"Default", "2x", "4x", "8x", "16x (Recommended)"};
            static const int aniso_vals[] = {0, 2, 4, 8, 16};
            int ai = 0;
            for (int i = 0; i < 5; i++) if (aniso_vals[i] == g_settings.anisotropy) ai = i;
            if (ImGui::Combo("Anisotropic filtering (AF)", &ai, aniso_names, 5)) {
                g_settings.anisotropy = aniso_vals[ai];
            }
            static const char *vs_names[] = {"V-Sync ON (FIFO)", "Fast Sync (Mailbox)", "V-Sync OFF / VRR (Immediate)"};
            int vsi = g_settings.present_mode >= 0 && g_settings.present_mode <= 2 ? g_settings.present_mode : (!g_settings.vsync ? 2 : 0);
            if (ImGui::Combo("Vertical sync (V-Sync / VRR)", &vsi, vs_names, 3)) {
                g_settings.present_mode = vsi;
                g_settings.vsync = (vsi != 2);
                vk_request_swapchain_recreate();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            int vol = (int)lroundf(g_settings.volume * 100);
            if (ImGui::SliderInt("Volume", &vol, 0, 200, "%d %%")) g_settings.volume = vol / 100.0f;
            ImGui::Checkbox("Video sound (after restart)", &g_settings.movie_audio);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            ImGui::TextWrapped("Key and button bindings can be changed in the launcher (Play XC3.bat), Controls tab.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Save")) { settings_save(); saved_note = true; }
    ImGui::SameLine();
    if (ImGui::Button("Back to the game")) { open_ = false; input_reset(); }
    if (saved_note) { ImGui::SameLine(); ImGui::TextDisabled("Saved to " INI_FILE); }
    ImGui::End();
}

static void fps_overlay(void) {
    ImGui::SetNextWindowPos(ImVec2(14.0f, 14.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##fps_overlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs)) {
        ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.0f), "%.0f FPS", (double)cur_fps);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

extern "C" void menu_render(void *cmd, uint32_t w, uint32_t h) {
    static ImGuiContext *ctx;
    static float font_h;
    static uint64_t t_last;
    bool draw_fps = g_settings.show_fps && cur_fps > 0;
    if (!open_ && !draw_fps) return;
    if (!ctx) {
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        font_h = fmaxf(16.0f, h / 1080.0f * 22.0f);
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", font_h, nullptr, io.Fonts->GetGlyphRangesDefault()))
            io.Fonts->AddFontDefault();
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(font_h / 16.0f);
        if (!vk_imgui_backend_init()) { ImGui::DestroyContext(ctx); ctx = nullptr; open_ = false; return; }
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    uint64_t now = SDL_GetTicksNS();
    io.DeltaTime = t_last ? (float)((now - t_last) / 1e9) : 1.0f / 60;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60;
    t_last = now;
    if (open_) feed(io, w, h);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (draw_fps) fps_overlay();
    if (open_) ui();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
}
