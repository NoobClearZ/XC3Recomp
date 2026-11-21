// XC3 launcher: edits xc3.ini (graphics, scaling, controls, audio, language) and starts the game.
// Standalone (SDL3 + Dear ImGui with the SDL renderer); the game reads the same file when its window opens
// (runtime/host/settings_ini.c). Build: tools/launcher/build.sh. Run: "Play XC3.bat" at the repository root.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <sys/stat.h>
#include <algorithm>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

// ------------------------------------------------------------------ settings (same keys as settings_ini.c)
// the Switch inputs, then the keyboard / mouse actions (runtime/input_bind.h BIND_ART1..: the arts palette, the party)
enum { NACT = 35, NPAD = 16, NSTICK = 24 };
static const char *const k_ini[NACT] = {"a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus",
                                        "left", "up", "right", "down", "ls_up", "ls_down", "ls_left", "ls_right",
                                        "rs_up", "rs_down", "rs_left", "rs_right",
                                        "art1", "art2", "art3", "art4", "art5", "art6", "art7", "art8", "talent",
                                        "party_up", "party_down"};
static const char *const k_names[NACT] = {"A (acción, usar arte, aceptar)", "B (saltar, atrás)", "X (menú principal)", "Y (mapa)",
                                          "Pulsar stick izq.", "Pulsar stick der.", "L (objetivo anterior)", "R (objetivo siguiente)",
                                          "ZL (cámara)", "ZR (comandos de combate)", "+ (menú del sistema)", "- (Minus)",
                                          "Cruceta izquierda (arte)", "Cruceta arriba", "Cruceta derecha (arte)", "Cruceta abajo",
                                          "Avanzar", "Retroceder", "Izquierda", "Derecha",
                                          "Cámara arriba", "Cámara abajo", "Cámara izquierda", "Cámara derecha",
                                          "Arte 1 (izquierda)", "Arte 2", "Arte 3", "Arte 4 (junto al centro)",
                                          "Arte 5 (junto al centro)", "Arte 6", "Arte 7", "Arte 8 (derecha)",
                                          "Arte de talento (centro)", "Personaje de arriba", "Personaje de abajo"};
// mouse buttons / wheel bind like keys (runtime/input_bind.h BIND_MOUSE_*; their xcde.ini names)
enum { MOUSE_BASE = 0x400, MOUSE_END = MOUSE_BASE + 8, M_LEFT = MOUSE_BASE + 1, M_MIDDLE, M_RIGHT, M_X1, M_X2, M_WUP, M_WDOWN };
static const char *const k_mouse_ini[8] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2", "Wheel Up", "Wheel Down"};
static const char *const k_mouse_ui[8] = {"", "Ratón izq.", "Ratón central", "Ratón der.", "Ratón lateral 1", "Ratón lateral 2",
                                          "Rueda arriba", "Rueda abajo"};
// controller buttons by position (runtime/hle/bindings.c k_pad_ini; 26/27 = the analog triggers)
enum { PADB_LT = 26, PADB_RT, PADB_COUNT };
static const char *const k_pad_ini[PADB_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2",
    "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "ltrigger", "rtrigger"};
static const char *const k_pad_label[PADB_COUNT] = {
    "Abajo (Xbox A / PS Cruz / Switch B)", "Derecha (Xbox B / PS Círculo / Switch A)", "Izquierda (Xbox X / PS Cuadrado / Switch Y)",
    "Arriba (Xbox Y / PS Triángulo / Switch X)", "Back / View / Share", "Guide / Home", "Start / Menu / Options",
    "Pulsar stick izq.", "Pulsar stick der.", "LB / L1 / L", "RB / R1 / R", "Cruceta arriba", "Cruceta abajo",
    "Cruceta izquierda", "Cruceta derecha", "Misc 1", "Paleta der. 1", "Paleta izq. 1", "Paleta der. 2", "Paleta izq. 2",
    "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT / L2 / ZL", "RT / R2 / ZR"};
static const int k_def_pad[NPAD] = {1, 0, 3, 2, 7, 8, 9, 10, PADB_LT, PADB_RT, 6, 4, 13, 11, 14, 12};
// (runtime/hle/bindings.c DEF_KEY / DEF_KEY2: keyboard + mouse as an MMO)
static const int k_def_key[NACT] = {
    SDL_SCANCODE_F, SDL_SCANCODE_SPACE, SDL_SCANCODE_I, SDL_SCANCODE_M, SDL_SCANCODE_LSHIFT, M_MIDDLE,
    SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_J,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_5,
    SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6,
    SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
    SDL_SCANCODE_8, SDL_SCANCODE_T, SDL_SCANCODE_Z, SDL_SCANCODE_X};
static const int k_def_key2[NACT] = {
    SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, 0, 0, 0, SDL_SCANCODE_KP_0, 0, SDL_SCANCODE_TAB, 0, 0, 0, 0,
    0, SDL_SCANCODE_R, 0, SDL_SCANCODE_G};
static const char *T(const char *es);  // (launcher language, below)
static const char *key_ini_name(int k) { return k > MOUSE_BASE && k < MOUSE_END ? k_mouse_ini[k - MOUSE_BASE] : k ? SDL_GetScancodeName((SDL_Scancode)k) : "none"; }
static const char *key_ui_name(int k) { return k > MOUSE_BASE && k < MOUSE_END ? T(k_mouse_ui[k - MOUSE_BASE]) : k ? SDL_GetScancodeName((SDL_Scancode)k) : T("(ninguna)"); }
static int key_from_ini(const char *v) {
    for (int i = 1; i < 8; i++) if (!strcmp(v, k_mouse_ini[i])) return MOUSE_BASE + i;
    return strcmp(v, "none") ? SDL_GetScancodeFromName(v) : SDL_SCANCODE_UNKNOWN;
}

#define APP_NAME "Xenoblade Chronicles 3"  // [XC3] (window title, ini header, the big title)
#define INI_FILE "xc3.ini"
#ifdef _WIN32
#define GAME_EXE "build/xc3.exe"
#define GAME_EXE_ALT "build/xcde.exe"
#define LAUNCHER_EXE "build/xenoblade3_launcher.exe"
#define VENV_PY ".venv/Scripts/python.exe"
#else
#define GAME_EXE "build/xc3"
#define GAME_EXE_ALT "build/xcde"
#define LAUNCHER_EXE "build/xenoblade3_launcher"
#define VENV_PY ".venv/bin/python"
#endif
#define DEF_WIN 2             // [XC3] 1920x1080: the size XC3 draws its gameplay frame at (its window crop)

struct Cfg {
    bool fullscreen = false;
    int win = DEF_WIN;      // index into k_wins
    int res = 0;            // index into k_res
    int upscaler = 1;       // 0 bilinear, 1 FSR 1, 2 FSR 3.1, 3 DLSS, 4 FSR 4 ([XC3])
    int uquality = 3;       // FSR 3 / DLSS: 0 native AA, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    bool frame_gen = false;
    bool smaa = false;
    bool dof = true;        // the game's depth of field (T183)
    bool motion_blur = true;  // [XC3] the game's motion blur and fur blur (runtime/services/romfs_virtual.c, lib_nx.bin)
    bool game_taa = true;     // [XC3] the game's temporal AA (TMAA; kept with FSR 3.1 / FSR 4 / DLSS)
    bool vsync = true;      // FIFO presentation at 30 / 60 fps
    int present_mode = 0;   // [XC3] 0 = FIFO, 1 = Mailbox, 2 = Immediate (VRR)
    int anisotropy = 16;    // [XC3] 0 = Default, 2..16 Forced Anisotropic Filtering (default: 16)
    int astc_mode = 0;     // [XC3] 0 = GPU Compute Shader (Fast), 1 = CPU Software Fallback
    bool frame_lag = true;  // [XC3] one frame in flight above 30 fps (runtime/nvn/objects.c)
    int cutscenes = 0;      // T224: 0 at the FPS setting, 1 at 30 (xcde.ini cutscenes = same | 30)
    bool cut_fg = false;    // T224: cutscenes at 30 + frame generation, gameplay without it (xcde.ini cutscenes = 30fg)
    bool party_switch = false;  // party mod: switch the controlled character (F2 / L2 + L1 / R1), runtime/hle/party.c
    bool show_fps = false;
    int fps = 0;  // index into k_fps_ini
    int volume = 100;
    bool movie_audio = true;
    bool dlc = true;        // [XC3] serve the add-on content in targets/dlcs (xc3.ini dlc = 0/1; runtime/services/misc.c)
    int lang = 0;           // index into k_langs
    int pad[NPAD];
    int key[NACT], key2[NACT];  // SDL_Scancode or mouse (MOUSE_BASE + ...)
    int mouse_look = 1;         // the mouse moves the camera: 0 no, 1 with the right button held, 2 always
    int mouse_sens = 50;
    int stick_deadzone = 0;     // % of the stick's travel read as centred (0: raw)
    bool mouse_invert = false;
    int ui_lang = 1;            // the launcher's own language: permanently 1 (English)
    int icons = 0;              // button prompts: 0 auto (the connected controller, else the keyboard), 1 Switch, 2 Xbox, 3 PlayStation, 4 keyboard
    bool record = false;        // [XC3] record the session (as scripts/play.sh): not saved, off at every start
};
// [XCDE] button prompt packs: romfs mods the player installs (GameBanana, CompC: "Xbox control layout" 694794,
// "PS5 DualSense control layout" 541180 - CC BY-NC-ND, so not shipped) in these folders; the launcher turns the one
// for the chosen / connected controller on (xcde.ini mod_<folder>) and keeps them out of the Mods tab.
// (keyboard: drawn by the game from the key bindings, runtime/services/kbicons.c - xcde.ini mod_icons_keyboard)
static const char *const k_icon_ini[] = {"auto", "switch", "xbox", "playstation", "keyboard"};
static const char *const k_icon_mod[] = {"icons_xbox", "icons_playstation"};
static bool is_icon_mod(const std::string &n) { return n == k_icon_mod[0] || n == k_icon_mod[1] || n == "icons_keyboard"; }
// 2 Xbox, 3 PlayStation, 4 keyboard (auto without a controller), else 1 (Switch)
static int icons_effective(int sel) {
    if (sel) return sel;
    int n = 0, t = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) t = SDL_GetGamepadTypeForID(ids[0]);
    SDL_free(ids);
    if (n <= 0) return 4;
    return t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE ? 2 :
           t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? 3 : 1;
}
static const char *const k_ups_ini[] = {"bilinear", "fsr", "fsr3", "dlss", "fsr4"};
static const char *const k_uq_ini[] = {"native", "quality", "balanced", "performance", "ultra"};
static const char *const k_wins[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"};
static const char *const k_res_ini[] = {"match", "1", "2", "3", "75%", "50%", "dynamic"};
// [XC3] N x the frame the game draws: 1920x1080 in gameplay (Switch docked), 1280x720 on the title screen and in movies
static const char *const k_res_ui[] = {"Match window (recommended)", "1x - 1080p (like Switch on TV)", "2x - 2160p (4K)",
                                       "3x - 3240p", "75% of window", "50% of window", "Dynamic (based on performance)"};
#define RES_HINT "XC3 renders the game at 1080p (Switch docked). Higher resolution = sharper image and higher GPU load."
static const char *const k_fps_ini[] = {"30", "60", "120", "144", "165", "unlimited"};
static const char *const k_lang_ini[] = {"auto", "en-US", "es", "fr", "de", "it", "ja", "ko", "zh-Hans", "zh-Hant"};
static const char *const k_lang_ui[] = {"English (default)", "English", "Spanish", "French", "German", "Italian", "Japanese", "Korean", "Chinese (Simplified)", "Chinese (Traditional)"};

static Cfg g;

// [XCDE] launcher language (xcde.ini launcher_lang = es | en): the Spanish text is the key, T() gives the English one.
static const char *const k_en[][2] = {
    {"A (acción, usar arte, aceptar)", "A (action, use art, confirm)"}, {"B (saltar, atrás)", "B (jump, back)"},
    {"X (menú principal)", "X (main menu)"}, {"Y (mapa)", "Y (map)"}, {"Pulsar stick izq.", "Left stick press"},
    {"Pulsar stick der.", "Right stick press"}, {"L (objetivo anterior)", "L (previous target)"},
    {"R (objetivo siguiente)", "R (next target)"}, {"ZL (cámara)", "ZL (camera)"}, {"ZR (comandos de combate)", "ZR (battle commands)"},
    {"+ (menú del sistema)", "+ (system menu)"}, {"Cruceta izquierda (arte)", "D-pad left (art)"}, {"Cruceta arriba", "D-pad up"},
    {"Cruceta derecha (arte)", "D-pad right (art)"}, {"Cruceta abajo", "D-pad down"}, {"Avanzar", "Forward"},
    {"Retroceder", "Back"}, {"Izquierda", "Left"}, {"Derecha", "Right"}, {"Cámara arriba", "Camera up"},
    {"Cámara abajo", "Camera down"}, {"Cámara izquierda", "Camera left"}, {"Cámara derecha", "Camera right"},
    {"Arte 1 (izquierda)", "Art 1 (left)"}, {"Arte 2", "Art 2"}, {"Arte 3", "Art 3"}, {"Arte 4 (junto al centro)", "Art 4 (next to centre)"},
    {"Arte 5 (junto al centro)", "Art 5 (next to centre)"}, {"Arte 6", "Art 6"}, {"Arte 7", "Art 7"}, {"Arte 8 (derecha)", "Art 8 (right)"},
    {"Arte de talento (centro)", "Talent art (centre)"}, {"Personaje de arriba", "Character above"}, {"Personaje de abajo", "Character below"},
    {"Ratón izq.", "Mouse left"}, {"Ratón central", "Mouse middle"}, {"Ratón der.", "Mouse right"}, {"Ratón lateral 1", "Mouse side 1"},
    {"Ratón lateral 2", "Mouse side 2"}, {"Rueda arriba", "Wheel up"}, {"Rueda abajo", "Wheel down"}, {"(ninguna)", "(none)"},
    {"Abajo (Xbox A / PS Cruz / Switch B)", "Bottom (Xbox A / PS Cross / Switch B)"},
    {"Derecha (Xbox B / PS Círculo / Switch A)", "Right (Xbox B / PS Circle / Switch A)"},
    {"Izquierda (Xbox X / PS Cuadrado / Switch Y)", "Left (Xbox X / PS Square / Switch Y)"},
    {"Arriba (Xbox Y / PS Triángulo / Switch X)", "Top (Xbox Y / PS Triangle / Switch X)"},
    {"Cruceta izquierda", "D-pad left"}, {"Cruceta derecha", "D-pad right"}, {"Paleta der. 1", "Right paddle 1"},
    {"Paleta izq. 1", "Left paddle 1"}, {"Paleta der. 2", "Right paddle 2"}, {"Paleta izq. 2", "Left paddle 2"},
    {"Igual que la ventana (recomendado)", "Match the window (recommended)"}, {"1x - 720p (como Switch)", "1x - 720p (like Switch)"},
    {"1x - 1080p (como Switch en TV)", "1x - 1080p (like Switch docked)"},
    {"XC3 renderiza el juego a 1080p (Switch en TV). Más resolución = imagen más nítida y más carga para la GPU.",
     "XC3 draws its gameplay at 1080p (Switch docked). More resolution = a sharper image and more GPU load."},
    {"Depuración", "Debugging"}, {"Grabar la sesión (capturas, entradas y registro en play/)", "Record the session (screenshots, inputs and log in play/)"},
    {"Como scripts/play.sh: una captura cada 5 s, las entradas (input.txt, para repetirlas) y el registro (game.log)\nen play/<fecha_hora>/. Envía esa carpeta si algo falla.",
     "As scripts/play.sh: a screenshot every 5 s, your inputs (input.txt, to replay them) and the log (game.log)\nin play/<date_time>/. Send that folder when something goes wrong."},
    {"Juego iniciado. Grabando en ", "Game started. Recording to "},
    {"75% de la ventana", "75% of the window"}, {"50% de la ventana", "50% of the window"},
    {"Dinámica (según rendimiento)", "Dynamic (by performance)"},
    {"Inglés (por defecto)", "English (default)"}, {"Inglés", "English"}, {"Español", "Spanish"}, {"Francés", "French"},
    {"Alemán", "German"}, {"Italiano", "Italian"}, {"Japonés", "Japanese"}, {"Coreano", "Korean"},
	{"Chino simplificado", "Chinese (Simplified)"}, {"Chino tradicional", "Chinese (Traditional)"},
    {"Pantalla", "Display"}, {"Pantalla completa", "Fullscreen"}, {"Tamaño de la ventana", "Window size"},
    {"Resolución y escalado", "Resolution and scaling"}, {"Resolución interna", "Internal resolution"},
    {"El juego original renderiza a 720p. Más resolución = imagen más nítida y más carga para la GPU.",
     "The original game renders at 720p. More resolution = a sharper image and more GPU load."},
    {"Bilineal (suave)", "Bilinear (soft)"}, {"AMD FSR 1.0 (nítido)", "AMD FSR 1.0 (sharp)"},
    {"AMD FSR 3.1 (temporal, cualquier GPU)", "AMD FSR 3.1 (temporal, any GPU)"},
    {"NVIDIA DLSS (temporal, GPU RTX)", "NVIDIA DLSS (temporal, RTX GPU)"}, {"Escalado / antialiasing", "Upscaling / anti-aliasing"},
    {"AMD FSR 4 (temporal, GPU RDNA 4)", "AMD FSR 4 (temporal, RDNA 4 GPU)"},
    {"FSR 1.0: escalado espacial, solo cuando la resolución interna es menor que la ventana.\n"
     "FSR 3.1, FSR 4 y DLSS: reconstrucción temporal - sustituyen al escalado del propio juego (que\n"
     "renderiza a la mitad del tamaño de la ventana) y reconstruyen la imagen con varios frames.\n"
     "FSR 4 necesita una GPU AMD RDNA 4 (Radeon RX 9000); en otras se usa FSR 3.1.\n"
     "DLSS necesita una GPU NVIDIA RTX; si no está disponible se usa FSR 3.1.",
     "FSR 1.0: spatial upscaling, only when the internal resolution is below the window.\n"
     "FSR 3.1, FSR 4 and DLSS: temporal reconstruction - they replace the game's own upscaling (it\n"
     "renders at half the window's size) and rebuild the image from several frames.\n"
     "FSR 4 needs an AMD RDNA 4 GPU (Radeon RX 9000); on others FSR 3.1 is used.\n"
     "DLSS needs an NVIDIA RTX GPU; without one, FSR 3.1 is used."},
    {"FSR 1.0: escalado espacial, solo cuando la resolución interna es menor que la ventana.\n"
     "FSR 3.1 y DLSS: reconstrucción temporal - renderizan a menos resolución (según la calidad)\n"
     "y reconstruyen la imagen de la ventana con varios frames: más FPS y bordes sin dientes.\n"
     "Sustituyen al antialiasing temporal del juego. DLSS necesita una GPU NVIDIA RTX; si no\n"
     "está disponible se usa FSR 3.1.",
     "FSR 1.0: spatial upscaling, only when the internal resolution is below the window.\n"
     "FSR 3.1 and DLSS: temporal reconstruction - they render at a lower resolution (by quality)\n"
     "and rebuild the window's image from several frames: more FPS and smooth edges.\n"
     "They replace the game's temporal anti-aliasing. DLSS needs an NVIDIA RTX GPU; without\n"
     "one, FSR 3.1 is used."},
    {"Nativo (solo antialiasing: DLAA / FSR Native AA)", "Native (anti-aliasing only: DLAA / FSR Native AA)"},
    {"Calidad (67 %)", "Quality (67 %)"}, {"Equilibrado (59 %)", "Balanced (59 %)"}, {"Rendimiento (50 %)", "Performance (50 %)"},
    {"Ultra rendimiento (33 %)", "Ultra performance (33 %)"}, {"Calidad del escalado", "Upscaling quality"},
    {"Con 'Resolución interna' en 'Igual que la ventana': la resolución a la que se renderiza\n"
     "antes de reconstruir (porcentaje del lado de la ventana).",
     "With 'Internal resolution' on 'Match the window': the resolution rendered\n"
     "before reconstruction (percentage of the window's side)."},
    {"La resolución a la que se renderiza la escena antes de reconstruirla (porcentaje del lado\n"
     "de la ventana). Rendimiento = la del juego original (la mitad); Nativo = la de la ventana.",
     "The resolution the scene is rendered at before reconstruction (percentage of the window's\n"
     "side). Performance = the original game's (half); Native = the window's."},
    {"60 o más: el juego a esa velocidad fuera de las cinemáticas (que siguen a 30), con el tiempo del juego\n"
     "medido en cada imagen: correr y el mundo van a la velocidad de la consola a cualquier FPS.\n"
     "Necesita una GPU que lo mantenga: en zonas pesadas ayuda FSR en Rendimiento.",
     "60 or more: the game at that rate outside cutscenes (they stay at 30), with game time measured\n"
     "each frame: running and the world move at the console's speed at any FPS.\n"
     "Needs a GPU that holds it: in heavy areas FSR on Performance helps."},
    {"Los iconos de los botones del juego: Xbox (A B X Y, LB RB LT RT), PlayStation (símbolos,\n"
     "L1 R1 L2 R2) o las teclas asignadas, según los controles. Se aplica al iniciar el juego.",
     "The game's button icons: Xbox (A B X Y, LB RB LT RT), PlayStation (symbols,\n"
     "L1 R1 L2 R2) or the keys you bound, following your controls. Applied when the game starts."},
    {"Generación de frames (FSR 3)", "Frame generation (FSR 3)"},
    {"Inserta un frame interpolado entre cada dos del juego en todo el juego: el doble de imágenes\n"
     "por segundo (p. ej. FPS 30 -> 60 en pantalla). La de AMD: funciona con FSR 3.1 y con DLSS.\n"
     "Añade algo de latencia. Se aplica al iniciar el juego.",
     "Inserts an interpolated frame between every two of the game's, everywhere: twice the images\n"
     "per second (e.g. 30 FPS -> 60 on screen). AMD's: works with FSR 3.1 and with DLSS.\n"
     "Adds some latency. Applied when the game starts."},
    {"Generación de frames solo en cinemáticas", "Frame generation in cutscenes only"},
    {"El juego va a tus FPS sin generación de frames; en las cinemáticas (donde el juego anima a\n"
     "los personajes a 30) baja a 30 parejos y la generación las lleva a 60 en pantalla.\n"
     "Pensado para jugar a 60 en un monitor de 60 Hz. Se aplica al iniciar el juego.",
     "The game runs at your FPS without frame generation; in cutscenes (where the game animates\n"
     "characters at 30) it drops to an even 30 and generation brings them to 60 on screen.\n"
     "Meant for playing at 60 on a 60 Hz monitor. Applied when the game starts."},
    {"Se aplica cuando la resolución interna es menor que la ventana.", "Applies when the internal resolution is below the window."},
    {"Calidad", "Quality"}, {"Antialiasing SMAA", "SMAA anti-aliasing"},
    {"(no se usa con FSR 3.1 / FSR 4 / DLSS)", "(not used with FSR 3.1 / FSR 4 / DLSS)"},
    {"Profundidad de campo (desenfoque del juego)", "Depth of field (the game's blur)"},
    {"El desenfoque de lo cercano y lo lejano que el juego aplica también en Switch.\n"
     "Desactivado: toda la escena nítida (y algo menos de trabajo para la GPU).",
     "The near / far blur the game also applies on Switch.\n"
     "Off: the whole scene sharp (and a bit less GPU work)."},
    {"Antialiasing y desenfoque de movimiento del juego", "The game's anti-aliasing and motion blur"},
    {"El antialiasing temporal (TMAA), el desenfoque de movimiento y el del pelo que el juego\n"
     "aplica para los 720p de la Switch: a 1080p o más suavizan toda la imagen.\n"
     "Desactivado: imagen nítida (combínalo con SMAA para los bordes).",
     "The temporal anti-aliasing (TMAA), motion blur and hair blur the game applies for the\n"
     "Switch's 720p: at 1080p or more they soften the whole image.\n"
     "Off: a sharp image (combine it with SMAA for the edges)."},
    {"Mostrar FPS en la barra de título", "Show FPS"}, {"Velocidad", "Speed"},
    {"30 (como en Switch)", "30 (like Switch)"}, {"Sin límite", "Unlimited"},
    {"Más de 30: la velocidad del mundo, el reloj y las escenas no cambian (probado a 60 y 120);\n"
     "la cámara queda un poco más lejos. El V-Sync forzado en el panel de NVIDIA / AMD o el límite\n"
     "de RivaTuner también limitan los FPS: déjalos en 'configuración de la aplicación' / 0.",
     "Above 30: world speed, the clock and scenes don't change (tested at 60 and 120);\n"
     "the camera sits a bit farther. V-Sync forced in the NVIDIA / AMD panel or a RivaTuner\n"
     "limit also cap the FPS: leave them on 'application setting' / 0."},
    {"Filtrado anisotrópico (AF)", "Anisotropic filtering (AF)"},
    {"16x (Recomendado)", "16x (Recommended)"},
    {"Mejora notablemente la nitidez de texturas en superficies oblicuas o a distancia (suelos, rocas, caminos)\n"
     "sin costo apreciable en GPUs modernas.",
     "Noticeably improves texture clarity on angled surfaces or in the distance (terrain, rocks, roads)\n"
     "with negligible cost on modern GPUs."},
    {"Sincronización vertical (V-Sync / VRR)", "Vertical sync (V-Sync / VRR)"},
    {"V-Sync ON (FIFO)", "V-Sync ON (FIFO)"},
    {"Fast Sync (Mailbox)", "Fast Sync (Mailbox)"},
    {"V-Sync OFF / VRR (Immediate)", "V-Sync OFF / VRR (Immediate)"},
    {"FIFO: V-Sync clásico sin tearing, sincronizado al refresco de la pantalla.\n"
     "Mailbox: Fast Sync / latencia mínima sin tearing.\n"
     "Immediate: V-Sync OFF, recomendado para monitores con G-Sync / FreeSync (VRR).",
     "FIFO: Classic tear-free V-Sync, locked to display refresh rate.\n"
     "Mailbox: Fast Sync / minimal latency without screen tearing.\n"
     "Immediate: V-Sync OFF, recommended for G-Sync / FreeSync (VRR) monitors."},
    {"V-Sync (con 30 / 60 FPS)", "V-Sync (at 30 / 60 FPS)"},
    {"Cada imagen se muestra exactamente una vez, sincronizada con la pantalla: movimiento\n"
     "fluido (sin imágenes repetidas o saltadas). Con más de 60 FPS o sin límite no se aplica.",
     "Each image is shown exactly once, in sync with the display: smooth motion\n"
     "(no repeated or skipped images). Not applied above 60 FPS or unlimited."},
    {"Frame lag (60 FPS)", "Frame lag (60 FPS)"},
    {"Con 60 FPS, la GPU termina un frame mientras el juego prepara el siguiente: más FPS\n"
     "(p. ej. 49 -> 60 en Colony 9); la imagen va un frame (17 ms) por detrás de los controles.",
     "At 60 FPS the GPU finishes a frame while the game prepares the next one: more FPS\n"
     "(e.g. 49 -> 60 in Colony 9); the image is one frame (17 ms) behind the controls."},
    {"Como el resto del juego", "Like the rest of the game"}, {"30 estables (como en Switch)", "Steady 30 (like Switch)"},
    {"Cinemáticas", "Cutscenes"},
    {"En las cinemáticas el juego anima a los personajes a 30 aunque juegues a más FPS: con la\n"
     "cámara a 60 encima se notan saltos (p. ej. el pelo de Dickson).\n"
     "30 estables: las cinemáticas van a 30 parejos, cámara y personajes juntos (con V-Sync en un\n"
     "monitor de 60 Hz cada imagen dura exactamente dos refrescos).\n"
     "Con 'Generación de frames solo en cinemáticas' también van a 30, con generación encima.",
     "In cutscenes the game animates characters at 30 even at higher FPS: with the camera\n"
     "at 60 on top, jumps show (e.g. Dickson's hair).\n"
     "Steady 30: cutscenes run at an even 30, camera and characters together (with V-Sync on a\n"
     "60 Hz monitor each image lasts exactly two refreshes).\n"
     "With 'Frame generation in cutscenes only' they also run at 30, with generation on top."},
    {"Pulsa %s para \"%s\"... (%d s, Supr = ninguna)", "Press %s for \"%s\"... (%d s, Del = none)"},
    {"un botón del mando", "a controller button"}, {"una tecla o un botón del ratón", "a key or a mouse button"},
    {"Haz clic en una asignación y pulsa la tecla, botón del ratón o del mando nuevo. Si ya estaba en uso, se intercambian.",
     "Click a binding and press the new key, mouse button or controller button. If it was in use, they swap."},
    {"Restaurar teclado", "Reset keyboard"}, {"Restaurar mando", "Reset controller"},
    {"   Mando conectado: %s", "   Controller connected: %s"}, {"   (ningún mando detectado)", "   (no controller detected)"},
    {"Cambiar de personaje como en Xenoblade 3 (mantén L2 y pulsa L1 / R1: el de arriba / abajo; F2)",
     "Switch characters like in Xenoblade 3 (hold L2 and press L1 / R1: the one above / below; F2)"},
    {"Pasa el control al siguiente miembro del grupo, en el campo y en combate. Cada uno se queda\n"
     "donde está y conserva sus recargas de artes; el anterior pasa a la IA. No funciona en menús ni\n"
     "escenas, y en combate salta a los miembros caídos.",
     "Hands control to the next party member, in the field and in battle. Each stays where it is\n"
     "and keeps its art cooldowns; the previous one goes to the AI. Not in menus or scenes,\n"
     "and in battle it skips downed members."},
    {"Automático (el mando conectado; sin mando, teclado)", "Automatic (the connected controller; without one, keyboard)"},
    {"Teclado (las teclas asignadas)", "Keyboard (the bound keys)"}, {"Iconos de botones", "Button icons"},
    {"Falta el pack: descarga \"%s\" de GameBanana y pon su carpeta romfs en mods\\%s",
     "Pack missing: download \"%s\" from GameBanana and put its romfs folder in mods\\%s"},
    {"Xbox / PlayStation: packs de iconos de CompC (GameBanana). Teclado: el juego dibuja en cada\n"
     "icono la tecla asignada a ese botón (también ratón). Se aplica al iniciar el juego.",
     "Xbox / PlayStation: CompC's icon packs (GameBanana). Keyboard: the game draws on each icon\n"
     "the key bound to that button (mouse too). Applied when the game starts."},
    {"Manteniendo el botón derecho", "Holding the right button"}, {"Siempre", "Always"},
    {"Cámara con el ratón", "Mouse camera"},
    {"Siempre: el ratón mueve la cámara directamente y el cursor queda oculto dentro del juego.\n"
     "F1 (menú) o Alt+Tab lo liberan; al mover el ratón sobre el juego vuelve a capturarse.",
     "Always: the mouse moves the camera directly and the cursor is hidden in the game.\n"
     "F1 (menu) or Alt+Tab release it; moving the mouse over the game captures it again."},
    {"Sensibilidad", "Sensitivity"}, {"Invertir eje Y", "Invert Y axis"}, {"Zona muerta de los sticks (%)", "Stick dead zone (%)"},
    {"Para un mando con drift (el personaje o la cámara se mueven solos): esa parte del recorrido\n"
     "del stick cuenta como centrada. 0 = sin cambios (el juego ya tiene la suya).",
     "For a controller with drift (the character or camera move by themselves): that part of the\n"
     "stick's travel counts as centred. 0 = unchanged (the game has its own)."},
    {"Artes 1-8 y talento: en combate seleccionan el arte de ese hueco de la paleta y lo usan.",
     "Arts 1-8 and talent: in battle they select the art in that palette slot and use it."},
    {"Acción (botón de Switch)", "Action (Switch button)"}, {"Tecla", "Key"}, {"Tecla 2", "Key 2"}, {"Mando", "Controller"},
    {"(stick del mando)", "(controller stick)"}, {"(cruceta + A)", "(d-pad + A)"},
    {"Pon cada mod en su carpeta: mods\\<nombre>\\romfs\\... (la misma estructura que en Ryujinx / yuzu). "
     "Sus archivos reemplazan o añaden archivos del juego. Se aplican en orden alfabético: si dos mods "
     "cambian el mismo archivo, gana el último. Los mods de código (exefs, parches .pchtxt / IPS) no "
     "funcionan en esta versión recompilada.",
     "Put each mod in its own folder: mods\\<name>\\romfs\\... (the same layout as Ryujinx / yuzu). "
     "Its files replace or add game files. They apply in alphabetical order: if two mods change the "
     "same file, the last one wins. Code mods (exefs, .pchtxt / IPS patches) don't work in this "
     "recompiled version."},
    {"Abrir carpeta de mods", "Open mods folder"}, {"Actualizar lista", "Refresh list"}, {"No hay mods instalados.", "No mods installed."},
    {"Volumen", "Volume"}, {"Sonido de los vídeos", "Video sound"}, {"Idioma del juego", "Game language"}, {"Idioma", "Language"},
    {"Textos del juego. Solo los idiomas que incluya tu copia; si uno falla, vuelve a Inglés.",
     "The game's text. Only the languages your copy includes; if one fails, it goes back to English."},
    {"Recompilación nativa para PC · configuración", "Native PC recompilation · settings"},
    {"Gráficos", "Graphics"}, {"Controles", "Controls"}, {"Audio e idioma", "Audio and language"},
    {"Faltan los archivos del juego en targets/ (exefs y romfs de tu copia).", "The game files are missing in targets/ (exefs and romfs of your copy)."},
    {"Falta build/xc3.exe (compila el juego primero).", "build/xc3.exe is missing (build the game first)."},
    {"Listo. Guardado: save/   Ajustes: " INI_FILE, "Ready. Saves: save/   Settings: " INI_FILE},
    {"Listo. La primera vez, JUGAR prepara los shaders antes (unos minutos)", "Ready. The first time, PLAY prepares the shaders first (a few minutes)"},
    {"Omitir", "Skip"}, {"Detener", "Stop"},
    {"Precompilación detenida (se puede seguir otro día: lo ya hecho se conserva)", "Precompilation stopped (it can go on another day: what's done is kept)"},
    {"JUGAR", "PLAY"}, {"Precompilar shaders", "Precompile shaders"},
    {"Traduce y compila todos los shaders del juego por adelantado, para que la primera visita\n"
     "a una zona no se detenga a compilarlos. Solo hace falta una vez (o tras actualizar el juego).",
     "Translates and compiles all of the game's shaders ahead of time, so the first visit to an\n"
     "area doesn't stop to compile them. Only needed once (or after updating the game)."},
    {"Guardar ajustes", "Save settings"}, {"Ajustes guardados en " INI_FILE, "Settings saved to " INI_FILE},
    {"No se pudo escribir " INI_FILE, "Could not write " INI_FILE}, {"Crear reporte", "Create report"},
    {"Junta en un .zip los logs de las dos últimas partidas, " INI_FILE " y el último informe de\n"
     "cierre inesperado (sin partidas guardadas ni archivos del juego), para adjuntarlo a un reporte.",
     "Puts in a .zip the logs of the last two sessions, " INI_FILE " and the latest crash report\n"
     "(no saves or game files), to attach to a bug report."},
    {"Salir", "Quit"}, {"No se pudo iniciar build/xc3.exe (error ", "Could not start build/xc3.exe (error "},
    {"Juego iniciado. Registro: build/last_game.log", "Game started. Log: build/last_game.log"},
    {"No se pudo iniciar la precompilación (error ", "Could not start the precompilation (error "},
    {"Buscando shaders en los archivos del juego...", "Looking for shaders in the game files..."},
    {"Preparando shaders antes de la primera partida (solo esta vez)", "Preparing shaders before the first game (only this time)"},
    {"Precompilando shaders", "Precompiling shaders"}, {"Variantes conocidas (semilla)…", "Known variants (seed)…"},
    {"Archivos %llu de %llu · programas de shaders listos %llu de %llu", "Files %llu of %llu · shader programs ready %llu of %llu"},
    {"El controlador gráfico falló en un shader: se salta y se sigue (intento ", "The graphics driver failed on a shader: it is skipped (attempt "},
    {"Shaders precompilados", "Shaders precompiled"},
    {"La precompilación falló (registro: build/last_precompile.log)", "Precompilation failed (log: build/last_precompile.log)"},
    {"No hay nada que reportar todavía (ni logs ni " INI_FILE ")", "Nothing to report yet (no logs or " INI_FILE ")"},
    {"No se pudo crear el reporte (falta tar.exe de Windows)", "Could not create the report (Windows' tar.exe is missing)"},
    {"No se pudo crear el reporte", "Could not create the report"}, {"Reporte creado: ", "Report created: "},
    {" (adjúntalo al reportar el problema)", " (attach it to the bug report)"},
    {"Idioma del launcher", "Launcher language"},
    // [XC3] tab "Instalación y build" (tab_build)
    {"Instalación y build", "Install & build"}, {"Archivos del juego", "Game files"}, {"Compilación", "Build"},
    {"Comprobando los archivos del juego...", "Checking the game files..."},
    {"Listos: Xenoblade Chronicles 3, versión 2.2.0 o 2.2.1 (targets/)", "Ready: Xenoblade Chronicles 3, version 2.2.0 or 2.2.1 (targets/)"},
    {"Hace falta la versión 2.2.0 o 2.2.1 (el juego base con esa actualización)", "Version 2.2.0 or 2.2.1 is needed (the base game with that update)"},
    {"Faltan archivos del romfs o son distintos (copia incompleta)", "Some romfs files are missing or different (an incomplete copy)"},
    {"Son paquetes (.xci / .nsp / .nca): hacen falta las carpetas exefs y romfs", "Those are packages (.xci / .nsp / .nca): the exefs and romfs folders are needed"},
    {"No hay archivos del juego en targets/", "No game files in targets/"},
    {"Elegir archivos del juego...", "Choose game files..."},
    {"Elige la carpeta que contiene las carpetas exefs y romfs de tu copia del juego.",
     "Choose the folder that holds the exefs and romfs folders of your copy of the game."},
    {"Archivos del juego listos", "Game files ready"},
    {"Esa carpeta no sirve (registro: build/setup_game_files.log)", "That folder does not work (log: build/setup_game_files.log)"},
    {"No se pudo abrir el selector (error ", "Could not open the folder picker (error "},
    {"Faltan las herramientas de compilación: ejecuta Build XC3.bat una vez (las instala).",
     "The build tools are missing: run Build XC3.bat once (it installs them)."},
    {"Recompila el código del juego y compila el runtime y el launcher (unos minutos; la primera vez, 10-20).",
     "Recompiles the game code and builds the runtime and the launcher (a few minutes; the first time 10-20)."},
    {"Paso %d de 5", "Step %d of 5"}, {"Cerrar el launcher detiene la compilación.", "Closing the launcher stops the build."},
    {"Compilar / actualizar", "Build / update"}, {"Abrir registro", "Open log"},
    {"Vuelve a abrir el launcher para usar la versión nueva.", "Reopen the launcher to use the new version."},
    {"No se pudo iniciar la compilación (error ", "Could not start the build (error "},
    {"Compilando...", "Building..."}, {"Compilación detenida", "Build stopped"}, {"Compilación terminada", "Build finished"},
    {"La compilación falló (registro: build/build_all.log)", "The build failed (log: build/build_all.log)"},
};
static const char *T(const char *es) {
    for (const auto &e : k_en) if (!strcmp(e[0], es)) return e[1];
    return es;
}
// (combo lists: each element through T)
struct TList { const char *p[48]; };
static TList TL(const char *const *a, int n) { TList t{}; for (int i = 0; i < n && i < 48; i++) t.p[i] = T(a[i]); return t; }
static std::string g_root;  // repository root (where targets/, build/, xcde.ini live)

static std::string path(const char *rel) { return g_root + "/" + rel; }
struct Mod { std::string name; bool on = true; bool present = true; };
static std::vector<Mod> g_mods;  // (mods tab; xcde.ini mod_<name>)

static int find(const char *const *list, int n, const char *v) {
    for (int i = 0; i < n; i++) if (!strcmp(list[i], v)) return i;
    return -1;
}

static void load() {
    for (int i = 0; i < NPAD; i++) g.pad[i] = k_def_pad[i];
    for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
    FILE *f = fopen(path(INI_FILE).c_str(), "r");
    if (!f) return;
    int layout = 1;  // (bind_layout: an older file's keyboard lines are the old layout - the new defaults replace them)
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        if (!memcmp(k, "\xEF\xBB\xBF", 3)) k += 3;  // (a UTF-8 BOM: the file was saved from Notepad)
        while (*k == ' ' || *k == '\t') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
        int i;
        if (!strcmp(k, "fullscreen")) g.fullscreen = atoi(v) != 0;
        else if (!strcmp(k, "window") && (i = find(k_wins, 5, v)) >= 0) g.win = i;
        else if (!strcmp(k, "resolution") && (i = find(k_res_ini, 7, v)) >= 0) g.res = i;
        else if (!strcmp(k, "upscaler")) g.upscaler = !strcmp(v, "fsr") ? 1 : !strcmp(v, "fsr3") ? 2 : !strcmp(v, "dlss") ? 3 : !strcmp(v, "fsr4") ? 4 : 0;
        else if (!strcmp(k, "upscale_quality") && (i = find(k_uq_ini, 5, v)) >= 0) g.uquality = i;
        else if (!strcmp(k, "frame_gen")) g.frame_gen = atoi(v) != 0;
        else if (!strcmp(k, "antialias")) g.smaa = !strcmp(v, "smaa");
        else if (!strcmp(k, "depth_of_field")) g.dof = atoi(v) != 0;
        else if (!strcmp(k, "game_blur")) { if (atoi(v) == 0) g.motion_blur = g.game_taa = false; }  // (the older single switch)
        else if (!strcmp(k, "motion_blur")) g.motion_blur = atoi(v) != 0;
        else if (!strcmp(k, "game_taa")) g.game_taa = atoi(v) != 0;
        else if (!strcmp(k, "vsync")) {
            if (!strcmp(v, "immediate") || !strcmp(v, "vrr") || !strcmp(v, "off") || !strcmp(v, "0")) {
                g.vsync = false;
                g.present_mode = 2;
            } else if (!strcmp(v, "mailbox") || !strcmp(v, "fast")) {
                g.vsync = true;
                g.present_mode = 1;
            } else {
                g.vsync = true;
                g.present_mode = 0;
            }
        } else if (!strcmp(k, "present_mode")) {
            if (!strcmp(v, "immediate") || !strcmp(v, "vrr")) g.present_mode = 2;
            else if (!strcmp(v, "mailbox") || !strcmp(v, "fast")) g.present_mode = 1;
            else g.present_mode = 0;
        } else if (!strcmp(k, "anisotropy")) {
            int a = atoi(v);
            g.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 0;
        } else if (!strcmp(k, "astc_decoder")) {
            g.astc_mode = (!strcmp(v, "cpu") || !strcmp(v, "software") || !strcmp(v, "1")) ? 1 : 0;
        }
        else if (!strcmp(k, "frame_lag")) g.frame_lag = atoi(v) != 0;
        else if (!strcmp(k, "cutscenes")) { g.cutscenes = !strcmp(v, "30") || !strcmp(v, "30fg") ? 1 : 0; g.cut_fg = !strcmp(v, "30fg"); }
        else if (!strcmp(k, "party_switch")) g.party_switch = atoi(v) != 0;
        else if (!strcmp(k, "show_fps")) g.show_fps = atoi(v) != 0;
        else if (!strcmp(k, "fps")) { g.fps = 0; for (int j = 0; j < 6; j++) if (!strcmp(v, k_fps_ini[j])) g.fps = j; }
        else if (!strcmp(k, "volume")) g.volume = atoi(v);
        else if (!strcmp(k, "movie_audio")) g.movie_audio = atoi(v) != 0;
        else if (!strcmp(k, "dlc")) g.dlc = atoi(v) != 0;
        else if (!strcmp(k, "language") && (i = find(k_lang_ini, 10, v)) >= 0) g.lang = i;
        else if (!strncmp(k, "mod_", 4)) g_mods.push_back(Mod{k + 4, atoi(v) != 0, false});
        else if (!strncmp(k, "bind_pad_", 9) && (i = find(k_ini, NPAD, k + 9)) >= 0) {
            int h = find(k_pad_ini, PADB_COUNT, v);
            if (h >= 0) g.pad[i] = h;
        } else if (!strncmp(k, "bind_key_", 9) && (i = find(k_ini, NACT, k + 9)) >= 0) {
            g.key[i] = key_from_ini(v);
        } else if (!strncmp(k, "bind_key2_", 10) && (i = find(k_ini, NACT, k + 10)) >= 0) {
            g.key2[i] = key_from_ini(v);
        } else if (!strcmp(k, "bind_layout")) layout = atoi(v);
        else if (!strcmp(k, "mouse_look")) g.mouse_look = std::clamp(atoi(v), 0, 2);
        else if (!strcmp(k, "mouse_sens")) g.mouse_sens = std::clamp(atoi(v), 1, 100);
        else if (!strcmp(k, "stick_deadzone")) g.stick_deadzone = std::clamp(atoi(v), 0, 30);
        else if (!strcmp(k, "launcher_lang")) g.ui_lang = 1;
        else if (!strcmp(k, "mouse_invert")) g.mouse_invert = atoi(v) != 0;
        else if (!strcmp(k, "button_icons") && (i = find(k_icon_ini, 5, v)) >= 0) g.icons = i;
    }
    fclose(f);
    g.frame_lag = (g.fps != 0);  // [XC3] frame lag above 30 fps
    if (layout < 2) for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
}

static bool save() {
    FILE *f = fopen(path(INI_FILE).c_str(), "w");
    if (!f) return false;
    fprintf(f, "# " APP_NAME " settings (written by the launcher)\n");
    fprintf(f, "fullscreen = %d\nwindow = %s\nresolution = %s\nupscaler = %s\nantialias = %s\nshow_fps = %d\n", g.fullscreen,
            k_wins[g.win], k_res_ini[g.res], k_ups_ini[g.upscaler], g.smaa ? "smaa" : "off", g.show_fps);
    g.frame_gen = g.cut_fg = false;  // [XC3] not offered (see the upscaler options)
    g.frame_lag = (g.fps != 0);      // [XC3] frame lag above 30 fps
    fprintf(f, "upscale_quality = %s\nframe_gen = %d\n", k_uq_ini[g.uquality], g.frame_gen);
    fprintf(f, "depth_of_field = %d\n", g.dof);
    fprintf(f, "motion_blur = %d\ngame_taa = %d\n", g.motion_blur, g.game_taa);
    fprintf(f, "dlc = %d\n", g.dlc);
    if (g.anisotropy) fprintf(f, "anisotropy = %d\n", g.anisotropy);
    if (g.astc_mode) fprintf(f, "astc_decoder = cpu\n");
    if (g.present_mode == 2) fprintf(f, "vsync = immediate\n");
    else if (g.present_mode == 1) fprintf(f, "vsync = mailbox\n");
    else fprintf(f, "vsync = %d\n", g.vsync);
    fprintf(f, "frame_lag = %d\n", g.frame_lag);
    fprintf(f, "cutscenes = %s\n", g.cut_fg ? "30fg" : g.cutscenes ? "30" : "same");
    fprintf(f, "party_switch = %d\n", g.party_switch);
    fprintf(f, "volume = %d\nmovie_audio = %d\nlanguage = %s\nfps = %s\n", g.volume, g.movie_audio, k_lang_ini[g.lang], k_fps_ini[g.fps]);
    for (const Mod &m : g_mods) if (!is_icon_mod(m.name)) fprintf(f, "mod_%s = %d", m.name.c_str(), m.on), fputc(10, f);
    int ic = icons_effective(g.icons);
    fprintf(f, "button_icons = %s\nmod_%s = %d\nmod_%s = %d\nmod_icons_keyboard = %d\n", k_icon_ini[g.icons], k_icon_mod[0], ic == 2,
            k_icon_mod[1], ic == 3, ic == 4);
    for (int i = 0; i < NPAD; i++) fprintf(f, "bind_pad_%s = %s\n", k_ini[i], k_pad_ini[g.pad[i]]);
    fprintf(f, "bind_layout = 2\n");
    for (int i = 0; i < NACT; i++) {
        fprintf(f, "bind_key_%s = %s\n", k_ini[i], key_ini_name(g.key[i]));
        fprintf(f, "bind_key2_%s = %s\n", k_ini[i], key_ini_name(g.key2[i]));
    }
    fprintf(f, "mouse_look = %d\nmouse_sens = %d\nmouse_invert = %d\n", g.mouse_look, g.mouse_sens, g.mouse_invert);
    fprintf(f, "stick_deadzone = %d\n", g.stick_deadzone);
    fprintf(f, "launcher_lang = en\n");
    fclose(f);
    return true;
}

static bool exists(const char *rel) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path(rel).c_str(), &pi);
}

static std::string get_game_exe_path() {
    if (exists(GAME_EXE)) return path(GAME_EXE);
    if (exists(GAME_EXE_ALT)) return path(GAME_EXE_ALT);
    return path(GAME_EXE);
}

static bool have_game_exe() {
    return exists(GAME_EXE) || exists(GAME_EXE_ALT);
}

static bool l_is_dir(const std::string &abs) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(abs.c_str(), &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
}
static bool l_file_ok(const std::string &abs) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(abs.c_str(), &pi);
}
static long long l_mtime(const std::string &abs) {
    struct stat st;
    return stat(abs.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
}
static void l_mkdir(const std::string &abs) {
#ifdef _WIN32
    CreateDirectoryA(abs.c_str(), NULL);
#else
    mkdir(abs.c_str(), 0755);
#endif
}
static void l_set_env(const char *k, const char *v) {
#ifdef _WIN32
    SetEnvironmentVariableA(k, v);
#else
    setenv(k, v, 1);
#endif
}
static bool l_self_path(char *buf, size_t n) {
#ifdef _WIN32
    DWORD r = GetModuleFileNameA(NULL, buf, (DWORD)n);
    return r > 0 && r < n;
#else
    ssize_t r = readlink("/proc/self/exe", buf, n - 1);
    if (r <= 0) return false;
    buf[r] = 0;
    return true;
#endif
}
static void l_open_path(const std::string &abs) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", abs.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp("xdg-open", "xdg-open", abs.c_str(), (char *)NULL);
        _exit(127);
    }
#endif
}

#ifdef _WIN32
typedef HANDLE LProc;
#define LPROC_NULL NULL
static std::string l_win_quote(const std::vector<std::string> &argv) {
    std::string cmd;
    for (size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += ' ';
        cmd += '"';
        cmd += argv[i];
        cmd += '"';
    }
    return cmd;
}
static LProc l_spawn(const std::vector<std::string> &argv, const char *log_rel, HANDLE job, bool low_prio, int *err_out = nullptr) {
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    HANDLE h = INVALID_HANDLE_VALUE;
    if (log_rel) {
        SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
        h = CreateFileA(path(log_rel).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = h;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
    }
    std::string cmd = l_win_quote(argv);
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    DWORD flags = CREATE_NO_WINDOW | (job ? CREATE_SUSPENDED : 0) | (low_prio ? BELOW_NORMAL_PRIORITY_CLASS : 0);
    BOOL ok = CreateProcessA(argv[0].c_str(), buf.data(), NULL, NULL, TRUE, flags, NULL, g_root.c_str(), &si, &pi);
    if (err_out) *err_out = ok ? 0 : (int)GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) return NULL;
    if (job) { AssignProcessToJobObject(job, pi.hProcess); ResumeThread(pi.hThread); }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
static bool l_poll(LProc &p, int *code) {
    if (!p) return false;
    if (WaitForSingleObject(p, 0) != WAIT_OBJECT_0) return false;
    DWORD c = 1;
    GetExitCodeProcess(p, &c);
    CloseHandle(p);
    p = NULL;
    if (code) *code = (int)c;
    return true;
}
static void l_kill(LProc &p, HANDLE job) {
    if (!p) return;
    if (job) TerminateJobObject(job, 1);
    else TerminateProcess(p, 1);
    WaitForSingleObject(p, 2000);
    CloseHandle(p);
    p = NULL;
}
#else
typedef pid_t LProc;
#define LPROC_NULL 0
static LProc l_spawn(const std::vector<std::string> &argv, const char *log_rel, bool new_group, bool low_prio, int *err_out = nullptr) {
    pid_t pid = fork();
    if (pid < 0) { if (err_out) *err_out = errno; return 0; }
    if (pid == 0) {
        if (chdir(g_root.c_str()) != 0) _exit(127);
        if (new_group) setpgid(0, 0);
        if (low_prio) (void)nice(10);
        int fd = open(log_rel ? path(log_rel).c_str() : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); if (fd > 2) close(fd); }
        std::vector<char *> cargv;
        for (const auto &s : argv) cargv.push_back(const_cast<char *>(s.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    if (err_out) *err_out = 0;
    return pid;
}
static bool l_poll(LProc &p, int *code) {
    if (p <= 0) return false;
    int st = 0;
    pid_t r = waitpid(p, &st, WNOHANG);
    if (r == 0) return false;
    p = 0;
    if (code) *code = (r > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
    return true;
}
static void l_kill(LProc &p, bool group) {
    if (p <= 0) return;
    kill(group ? -p : p, SIGKILL);
    waitpid(p, NULL, 0);
    p = 0;
}
#endif

static std::string g_status;
static bool g_launched_ok;  // (launch() started the game: the launcher closes)
static float g_scale = 1;

// ================================================================== look ([XC3]: dark blue, ether-cyan accent)
// Every colour of the launcher is here. The background is drawn (theme_background): a gradient with soft cyan glows
// and arcs, or mods/launcher/background.png when it exists (any image of the user's: mods/ is never committed),
// darkened so the settings stay readable.
namespace pal {
static const ImVec4 bg_top(0.035f, 0.055f, 0.105f, 1), bg_bottom(0.02f, 0.03f, 0.06f, 1);
static const ImVec4 card(0.07f, 0.11f, 0.19f, 0.86f), card_border(0.20f, 0.32f, 0.50f, 0.55f);
static const ImVec4 frame(0.12f, 0.18f, 0.29f, 1), frame_hover(0.16f, 0.24f, 0.38f, 1), frame_active(0.19f, 0.29f, 0.46f, 1);
static const ImVec4 frame_border(0.27f, 0.40f, 0.60f, 0.70f), tab_selected(0.13f, 0.23f, 0.38f, 1);
static const ImVec4 accent(0.25f, 0.83f, 0.96f, 1), accent_hover(0.45f, 0.90f, 1.0f, 1), accent_dim(0.16f, 0.52f, 0.70f, 1);
static const ImVec4 text(0.90f, 0.94f, 0.98f, 1), muted(0.56f, 0.64f, 0.75f, 1), on_accent(0.02f, 0.06f, 0.12f, 1);
static const ImVec4 button(0.11f, 0.18f, 0.30f, 1), button_hover(0.16f, 0.27f, 0.44f, 1), button_active(0.20f, 0.34f, 0.55f, 1);
static const ImVec4 warn(1.0f, 0.45f, 0.42f, 1);
}
static ImFont *g_font_bold, *g_font_title;
static SDL_Texture *g_bg_tex;
static float g_bg_w, g_bg_h;

static void theme_apply(ImGuiStyle &st) {
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0; st.ChildRounding = 10; st.FrameRounding = 6; st.PopupRounding = 8; st.GrabRounding = 6;
    st.TabRounding = 6; st.ScrollbarRounding = 8;
    st.WindowPadding = ImVec2(18, 14); st.FramePadding = ImVec2(10, 6); st.ItemSpacing = ImVec2(10, 9);
    st.ItemInnerSpacing = ImVec2(8, 6); st.ScrollbarSize = 12; st.WindowBorderSize = 0; st.ChildBorderSize = 1;
    st.FrameBorderSize = 1; st.TabBorderSize = 0; st.TabBarBorderSize = 1; st.TabBarOverlineSize = 2;
    st.SeparatorTextBorderSize = 1; st.SeparatorTextPadding = ImVec2(0, 4); st.SeparatorTextAlign = ImVec2(0, 0.5f);
    ImVec4 *c = st.Colors;
    c[ImGuiCol_Text] = pal::text; c[ImGuiCol_TextDisabled] = pal::muted;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ChildBg] = pal::card; c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.08f, 0.15f, 0.98f);
    c[ImGuiCol_Border] = pal::frame_border; c[ImGuiCol_Separator] = pal::card_border;
    c[ImGuiCol_FrameBg] = pal::frame; c[ImGuiCol_FrameBgHovered] = pal::frame_hover; c[ImGuiCol_FrameBgActive] = pal::frame_active;
    c[ImGuiCol_Button] = pal::button; c[ImGuiCol_ButtonHovered] = pal::button_hover; c[ImGuiCol_ButtonActive] = pal::button_active;
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.26f, 0.42f, 1); c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.33f, 0.52f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.40f, 0.62f, 1);
    c[ImGuiCol_CheckMark] = pal::accent; c[ImGuiCol_SliderGrab] = pal::accent_dim; c[ImGuiCol_SliderGrabActive] = pal::accent;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TabHovered] = pal::button_hover; c[ImGuiCol_TabSelected] = pal::tab_selected;
    c[ImGuiCol_TabSelectedOverline] = pal::accent; c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = pal::tab_selected; c[ImGuiCol_TabDimmedSelectedOverline] = pal::accent;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = pal::button; c[ImGuiCol_ScrollbarGrabHovered] = pal::button_hover;
    c[ImGuiCol_ScrollbarGrabActive] = pal::button_active; c[ImGuiCol_PlotHistogram] = pal::accent;
    c[ImGuiCol_NavHighlight] = pal::accent;
    c[ImGuiCol_TableHeaderBg] = pal::tab_selected; c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TableBorderStrong] = pal::card_border; c[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.32f, 0.50f, 0.30f);
}

// a section title in the tabs (was ImGui::SeparatorText): bold, accent colour
static void section(const char *label) {
    if (g_font_bold) ImGui::PushFont(g_font_bold);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::accent);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
    if (g_font_bold) ImGui::PopFont();
}

static void theme_load_background(SDL_Renderer *ren, const std::string &path) {
    SDL_Surface *s = SDL_LoadPNG(path.c_str());
    if (!s) return;
    g_bg_tex = SDL_CreateTextureFromSurface(ren, s);
    g_bg_w = (float)s->w; g_bg_h = (float)s->h;
    SDL_DestroySurface(s);
}

static void theme_background(void) {
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    float w = vp->Size.x, h = vp->Size.y;
    ImU32 top = ImGui::GetColorU32(pal::bg_top), bot = ImGui::GetColorU32(pal::bg_bottom);
    dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
    if (g_bg_tex) {  // (cover: fills the window, cropped at the long side)
        float k = std::max(w / g_bg_w, h / g_bg_h), iw = g_bg_w * k, ih = g_bg_h * k;
        ImVec2 p(a.x + (w - iw) * 0.5f, a.y + (h - ih) * 0.5f);
        dl->AddImage((ImTextureID)(intptr_t)g_bg_tex, p, ImVec2(p.x + iw, p.y + ih));
        dl->AddRectFilledMultiColor(a, b, IM_COL32(4, 8, 18, 150), IM_COL32(4, 8, 18, 150), IM_COL32(3, 5, 12, 225), IM_COL32(3, 5, 12, 225));
        return;
    }
    // soft glows (stacked translucent discs) and thin arcs
    ImVec2 g1(a.x + w * 0.86f, a.y + h * 0.08f), g2(a.x + w * 0.06f, a.y + h * 0.98f);
    for (int i = 10; i > 0; i--) {
        dl->AddCircleFilled(g1, h * 0.09f * i, IM_COL32(64, 200, 245, 3), 64);
        dl->AddCircleFilled(g2, h * 0.07f * i, IM_COL32(40, 120, 255, 3), 64);
    }
    for (int i = 0; i < 4; i++) {
        float r = h * (0.55f + 0.16f * i);
        dl->PathArcTo(ImVec2(a.x + w * 1.02f, a.y - h * 0.10f), r, 1.62f, 2.95f, 96);
        dl->PathStroke(IM_COL32(90, 215, 250, 26 - i * 5), 0, (2.0f - 0.35f * i) * g_scale);
    }
}

// the big call to action (PLAY): accent fill, dark bold text
static bool play_button(const char *label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, pal::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal::accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, pal::accent_dim);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::on_accent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8 * g_scale);
    if (g_font_bold) ImGui::PushFont(g_font_bold);
    bool r = ImGui::Button(label, size);
    if (g_font_bold) ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

static void launch() {
    if (!save()) { g_status = T("No se pudo escribir " INI_FILE); return; }
    std::string exe = get_game_exe_path();
#ifdef _WIN32
    const char *old = getenv("PATH");
    std::string p = std::string("C:\\msys64\\ucrt64\\bin;") + (old ? old : "");
    SetEnvironmentVariableA("PATH", p.c_str());
#endif
    std::string log_rel = "build/last_game.log", rec_dir;
    if (g.record) {  // [XC3] as scripts/play.sh: play/<date_time>/ with screenshots every 5 s, input.txt and game.log
        char stamp[32];
        time_t now = time(NULL);
        struct tm tm_now;
#ifdef _WIN32
        localtime_s(&tm_now, &now);
#else
        localtime_r(&now, &tm_now);
#endif
        strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm_now);
        rec_dir = std::string("play/") + stamp;
        l_mkdir(path("play"));
        l_mkdir(path(rec_dir.c_str()));
        log_rel = rec_dir + "/game.log";
        l_set_env("XC3_SHOT_EVERY", ("150:" + rec_dir).c_str());
        l_set_env("XC3_INPUT_RECORD", (rec_dir + "/input.txt").c_str());
        l_set_env("XC3_STALL", "5");
        l_set_env("XCDE_SHOT_EVERY", ("150:" + rec_dir).c_str());
        l_set_env("XCDE_INPUT_RECORD", (rec_dir + "/input.txt").c_str());
        l_set_env("XCDE_STALL", "5");
    }
    int err = 0;
#ifdef _WIN32
    LProc proc = l_spawn({exe, "targets"}, log_rel.c_str(), NULL, false, &err);
    if (proc) CloseHandle(proc);
#else
    // Detached child for game launch: don't reap synchronously; parent exits when launched.
    LProc proc = l_spawn({exe, "targets"}, log_rel.c_str(), false, false, &err);
#endif
    if (!proc) { g_status = std::string(T("No se pudo iniciar build/xc3.exe (error ")) + std::to_string(err) + ")"; return; }
    g_status = rec_dir.empty() ? std::string(T("Juego iniciado. Registro: build/last_game.log")) : std::string(T("Juego iniciado. Grabando en ")) + rec_dir;
    g_launched_ok = true;
}

// ------------------------------------------------------------------ shader precompilation (T187)
// "Precompilar shaders" runs `<exe> --precompile targets` (runtime/nvn/vulkan/precompile.c): it translates every
// shader in the game's files into cache/spirv, so the first visit to an area doesn't stop to compile. It reports
// progress in cache/precompile_progress.txt and writes cache/precompile_done.txt at the end. JUGAR runs it first
// when it was never done (once, a few minutes; "Omitir" skips it).
static LProc g_pc_proc = LPROC_NULL;
static bool g_pc_then_play, g_pc_play_now;
static float g_pc_frac;
static std::string g_pc_text;

// [XCDE] T211: done for THIS build - precompile_done.txt names the game exe (size, time) it was made with; a new
// build translates shaders differently, so JUGAR precompiles again (unchanged shaders are cache hits: quick)
static bool pc_done() {
    FILE *f = fopen(path("cache/precompile_done.txt").c_str(), "r");
    if (!f) return false;
    char line[256];
    long long sz = -1, mt = -1;
    while (fgets(line, sizeof line, f)) sscanf(line, "exe %lld %lld", &sz, &mt);
    fclose(f);
    struct stat es;
    if (stat(get_game_exe_path().c_str(), &es) != 0) return true;
    return sz == (long long)es.st_size && mt == (long long)es.st_mtime;
}
static bool pc_running() { return g_pc_proc != LPROC_NULL; }

static void pc_start(bool then_play) {
    if (pc_running()) return;
    std::string exe = get_game_exe_path();
#ifdef _WIN32
    const char *old = getenv("PATH");
    std::string p = std::string("C:\\msys64\\ucrt64\\bin;") + (old ? old : "");
    SetEnvironmentVariableA("PATH", p.c_str());
#endif
    l_mkdir(path("cache"));
    remove(path("cache/precompile_progress.txt").c_str());
    int err = 0;
#ifdef _WIN32
    g_pc_proc = l_spawn({exe, "--precompile", "targets"}, "build/last_precompile.log", NULL, true, &err);
#else
    g_pc_proc = l_spawn({exe, "--precompile", "targets"}, "build/last_precompile.log", false, true, &err);
#endif
    if (!g_pc_proc) { g_status = std::string(T("No se pudo iniciar la precompilación (error ")) + std::to_string(err) + ")"; return; }
    g_pc_then_play = then_play;
    g_pc_frac = 0;
    g_pc_text = T("Buscando shaders en los archivos del juego...");
    g_status = then_play ? T("Preparando shaders antes de la primera partida (solo esta vez)") : T("Precompilando shaders");
}

static void pc_stop() {
    if (!pc_running()) return;
#ifdef _WIN32
    l_kill(g_pc_proc, NULL);
#else
    l_kill(g_pc_proc, false);
#endif
}

// (every frame while it runs) progress from the file; at the end, the result and JUGAR if it was waiting
static void pc_poll() {
    if (!pc_running()) return;
    static Uint64 last;
    Uint64 now = SDL_GetTicks();
    if (now - last < 200) return;
    last = now;
    FILE *f = fopen(path("cache/precompile_progress.txt").c_str(), "r");
    if (f) {
        char phase[16] = "";
        unsigned long long fd = 0, ft = 0, found = 0, done = 0;
        if (fscanf(f, "%15s %llu %llu %llu %llu", phase, &fd, &ft, &found, &done) == 5 && ft) {
            // reading the files is ~1/5 of the time, building the programs found the rest (it follows the reading)
            float fs = (float)fd / (float)ft, fc = found ? (float)done / (float)found : 0.0f;
            g_pc_frac = 0.15f * fs + 0.85f * fs * fc;
            char t[160];
            if (!strcmp(phase, "seed")) snprintf(t, sizeof t, "%s", T("Variantes conocidas (semilla)…"));  // [XCDE] T211: the seed pass
            else snprintf(t, sizeof t, T("Archivos %llu de %llu · programas de shaders listos %llu de %llu"), fd, ft, done, found);
            g_pc_text = t;
        }
        fclose(f);
    }
    int code = 1;
    if (!l_poll(g_pc_proc, &code)) return;
    // [XCDE] T240: a run that died while the driver was building (cache/precompile_inflight.bin left behind) starts
    // again by itself - the new run skips what was being built (precompile.c crash-safe) - up to 3 times
    static int retries;
    if (!(code == 0 && pc_done()) && l_file_ok(path("cache/precompile_inflight.bin")) && retries < 3) {
        retries++;
        bool then_play = g_pc_then_play;
        pc_start(then_play);
        g_status = std::string(T("El controlador gráfico falló en un shader: se salta y se sigue (intento ")) + std::to_string(retries + 1) + ")";
        return;
    }
    retries = 0;
    g_status = code == 0 && pc_done() ? T("Shaders precompilados") : T("La precompilación falló (registro: build/last_precompile.log)");
    if (g_pc_then_play) g_pc_play_now = true;
}

// ------------------------------------------------------------------ install & build ([XC3] tab "Instalación y build")
// What Build XC3.bat does, from the launcher, in four steps: the build tools (tools/setup_windows.ps1: MSYS2 and its
// libraries, Python, .venv; its "=== k/4" lines as the progress; build/setup_windows.log), the game files
// (tools/setup_game_files.ps1: the folder dialog, the check, the targets\ junctions; check_dump.py for the state shown),
// the build (scripts/build_all.sh in MSYS2's bash - Git Bash if MSYS2 has none -, its "=== k/5" lines as the progress;
// build/build_all.log) and the shader precompilation (pc_start). [XC3] The launcher links SDL3 statically
// (tools/launcher/build.sh), so it starts before any tool is installed. The tools install and the build run in a job
// object: Stop and closing the launcher end them with everything they started. The launcher's own exe is swapped by
// tools/launcher/build.sh (renamed, not overwritten): the new one runs from the next start.
static LProc g_bd_proc = LPROC_NULL, g_gf_proc = LPROC_NULL, g_gf_pick = LPROC_NULL, g_dlc_pick = LPROC_NULL, g_tl_proc = LPROC_NULL;
#ifdef _WIN32
static HANDLE g_bd_job;
#endif
static int g_bd_step, g_tl_step, g_gf_code = -2;  // (game files: -2 not checked, -1 checking, else check_dump.py's exit code)
static std::string g_bd_line, g_tl_line;
static bool g_bd_new_launcher;
static long long g_self_mtime;
static int g_linux_libs_ok = -1;

// [XC3] MSYS2's bash first on Windows; standard bash on Linux
static std::string bd_bash() {
#ifdef _WIN32
    std::string c[3] = {"C:\\msys64\\usr\\bin\\bash.exe"};
    if (const char *p = getenv("ProgramFiles")) c[1] = std::string(p) + "\\Git\\bin\\bash.exe";
    if (const char *p = getenv("LOCALAPPDATA")) c[2] = std::string(p) + "\\Programs\\Git\\bin\\bash.exe";
    for (auto &s : c) if (!s.empty() && l_file_ok(s)) return s;
    return "";
#else
    if (l_file_ok("/bin/bash")) return "/bin/bash";
    if (l_file_ok("/usr/bin/bash")) return "/usr/bin/bash";
    return "bash";
#endif
}
static bool tl_msys_ok() {
#ifdef _WIN32
    return l_file_ok("C:\\msys64\\ucrt64\\lib\\libSDL3.dll.a") && l_file_ok("C:\\msys64\\ucrt64\\lib\\libshaderc_shared.dll.a");
#else
    if (g_linux_libs_ok < 0) {
        g_linux_libs_ok = (system("pkg-config --exists sdl3 vulkan shaderc opus libavcodec libzstd >/dev/null 2>&1") == 0) ? 1 : 0;
    }
    return g_linux_libs_ok == 1;
#endif
}
static bool bd_tools_ok() { return !bd_bash().empty() && exists(VENV_PY) && tl_msys_ok(); }
static bool tl_running() { return g_tl_proc != LPROC_NULL; }
static bool bd_running() { return g_bd_proc != LPROC_NULL; }
static bool gf_busy() { return g_gf_proc != LPROC_NULL || g_gf_pick != LPROC_NULL || g_dlc_pick != LPROC_NULL; }

static LProc bd_spawn_argv(const std::vector<std::string> &argv, const char *log_rel, bool use_job, int *err_out = nullptr) {
#ifdef _WIN32
    char cur[32768];
    DWORD n = GetEnvironmentVariableA("PATH", cur, sizeof cur);
    std::string p = n && n < sizeof cur ? cur : "";
    for (const char *d : {"C:\\msys64\\ucrt64\\bin;", "C:\\msys64\\usr\\bin;"})
        for (size_t at; (at = p.find(d)) != std::string::npos;) p.erase(at, strlen(d));
    p = "C:\\msys64\\ucrt64\\bin;C:\\msys64\\usr\\bin;" + p;  // (ucrt64 first: the game's DLLs)
    SetEnvironmentVariableA("PATH", p.c_str());
    l_mkdir(path("build"));
    if (use_job && !g_bd_job) {
        g_bd_job = CreateJobObjectA(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (g_bd_job) SetInformationJobObject(g_bd_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    return l_spawn(argv, log_rel, use_job ? g_bd_job : NULL, false, err_out);
#else
    l_mkdir(path("build"));
    return l_spawn(argv, log_rel, use_job, false, err_out);
#endif
}

static void gf_check() {
    if (gf_busy() || !exists(VENV_PY)) return;
    g_gf_proc = bd_spawn_argv({path(VENV_PY), "-X", "utf8", "tools/extract/check_dump.py", "targets"}, "build/check_dump.log", false);
    g_gf_code = g_gf_proc ? -1 : -2;
}
static void gf_pick() {
    if (gf_busy() || bd_running()) return;
    int err = 0;
#ifdef _WIN32
    char sys[MAX_PATH] = "";
    GetSystemDirectoryA(sys, sizeof sys);
    std::string ps = std::string(sys) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
    g_gf_pick = bd_spawn_argv({ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\\setup_game_files.ps1"}, "build/setup_game_files.log", false, &err);
#else
    g_gf_pick = bd_spawn_argv({bd_bash(), "tools/setup_game_files.sh"}, "build/setup_game_files.log", false, &err);
#endif
    if (!g_gf_pick) g_status = std::string(T("No se pudo abrir el selector (error ")) + std::to_string(err) + ")";
}
// [XC3] the add-on content (optional)
static void dlc_pick() {
    if (gf_busy() || bd_running()) return;
    int err = 0;
#ifdef _WIN32
    char sys[MAX_PATH] = "";
    GetSystemDirectoryA(sys, sizeof sys);
    std::string ps = std::string(sys) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
    g_dlc_pick = bd_spawn_argv({ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\\setup_game_files.ps1", "-Dlc"}, "build/setup_dlc.log", false, &err);
#else
    g_dlc_pick = bd_spawn_argv({bd_bash(), "tools/setup_game_files.sh", "--dlc"}, "build/setup_dlc.log", false, &err);
#endif
    if (!g_dlc_pick) g_status = std::string(T("No se pudo abrir el selector (error ")) + std::to_string(err) + ")";
}
// the add-ons in targets/dlcs whose two archives are there ("1, 2, 3, 4"; empty: none), as runtime/services/misc.c lists them
static std::string dlc_found() {
    std::string s;
    for (int i = 1; i <= 4; i++) {
        char h[64], d[64];
        snprintf(h, sizeof h, "targets/dlcs/bf3_dlc%02d.arh", i);
        snprintf(d, sizeof d, "targets/dlcs/bf3_dlc%02d.ard", i);
        if (exists(h) && exists(d)) s += (s.empty() ? "" : ", ") + std::to_string(i);
    }
    return s;
}

// the last 16 KB of a log: the step of its last "=== k/<max> ..." line (1..max) and its last line
static void log_tail(const char *log, int max, int *step, std::string *line) {
    FILE *f = fopen(path(log).c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f), from = n > 16384 ? n - 16384 : 0;
    std::string t((size_t)(n - from), '\0');
    fseek(f, from, SEEK_SET);
    t.resize(fread(&t[0], 1, t.size(), f));
    fclose(f);
    size_t s = t.rfind("=== ");
    if (s != std::string::npos && s + 5 < t.size() && t[s + 4] >= '1' && t[s + 4] <= '0' + max) *step = t[s + 4] - '0';
    size_t e = t.find_last_not_of("\r\n ");
    if (e != std::string::npos) {
        size_t b = t.find_last_of('\n', e);
        *line = t.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1) + 1);
        if (line->size() > 140) *line = line->substr(0, 140) + "...";
    }
}

#ifdef _WIN32
#define TOOLS_LOG "build/setup_windows.log"
#define TOOLS_STEPS 4
#else
#define TOOLS_LOG "build/setup_linux.log"
#define TOOLS_STEPS 5
#endif

// [XC3] step 1: tools/setup_windows.ps1 or tools/setup_linux.sh
static void tl_start() {
    if (tl_running() || bd_running() || pc_running() || gf_busy()) return;
    int err = 0;
#ifdef _WIN32
    char sys[MAX_PATH] = "";
    GetSystemDirectoryA(sys, sizeof sys);
    std::string ps = std::string(sys) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
    g_tl_proc = bd_spawn_argv({ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\\setup_windows.ps1"}, TOOLS_LOG, true, &err);
#else
    g_tl_proc = bd_spawn_argv({bd_bash(), "tools/setup_linux.sh"}, TOOLS_LOG, true, &err);
#endif
    if (!g_tl_proc) { g_status = std::string(T("Could not start the tools install (error ")) + std::to_string(err) + ")"; return; }
    g_tl_step = 0;
    g_tl_line.clear();
    g_status = T("Installing the build tools...");
}
static void tl_stop() {
    if (!tl_running()) return;
#ifdef _WIN32
    l_kill(g_tl_proc, g_bd_job);
#else
    l_kill(g_tl_proc, true);
#endif
    g_status = T("Tools install stopped");
}

static void bd_start() {
    if (bd_running() || tl_running() || pc_running() || gf_busy()) return;
    std::string bash = bd_bash();
    if (bash.empty()) return;
    int err = 0;
    g_bd_proc = bd_spawn_argv({bash, "-c", "scripts/build_all.sh"}, "build/build_all.log", true, &err);
    if (!g_bd_proc) { g_status = std::string(T("No se pudo iniciar la compilación (error ")) + std::to_string(err) + ")"; return; }
    g_bd_step = 0;
    g_bd_line.clear();
    g_status = T("Compilando...");
}
static void bd_stop() {
    if (!bd_running()) return;
#ifdef _WIN32
    l_kill(g_bd_proc, g_bd_job);
#else
    l_kill(g_bd_proc, true);
#endif
    g_status = T("Compilación detenida");
}

static void bd_poll() {
    int code = 1;
    if (l_poll(g_gf_proc, &code)) g_gf_code = code;
    if (l_poll(g_gf_pick, &code)) {
        if (code == 0) g_status = T("Archivos del juego listos");
        else if (code != 2) g_status = T("Esa carpeta no sirve (registro: build/setup_game_files.log)");
        gf_check();
    }
    if (l_poll(g_dlc_pick, &code)) {
        if (code == 0) g_status = T("DLC ready");
        else if (code != 2) g_status = T("No DLC files in that folder (log: build/setup_dlc.log)");
    }
    if (!bd_running() && !tl_running()) return;
    static Uint64 last;
    Uint64 now = SDL_GetTicks();
    if (now - last < 300) return;
    last = now;
    if (tl_running()) {
        log_tail(TOOLS_LOG, TOOLS_STEPS, &g_tl_step, &g_tl_line);
        if (!l_poll(g_tl_proc, &code)) return;
        g_linux_libs_ok = -1;
        g_status = code == 0 && bd_tools_ok() ? T("Build tools installed") : T("The tools install failed (log: " TOOLS_LOG ")");
        g_gf_code = -2;  // (check_dump.py needs .venv: check the game files again)
        return;
    }
    log_tail("build/build_all.log", 5, &g_bd_step, &g_bd_line);
    if (!l_poll(g_bd_proc, &code)) return;
    g_status = code == 0 ? T("Compilación terminada") : T("La compilación falló (registro: build/build_all.log)");
    char me[1024];
    if (l_self_path(me, sizeof me)) g_bd_new_launcher = l_mtime(me) != g_self_mtime;
    long long cur_mt = l_mtime(path(LAUNCHER_EXE));
    if (cur_mt) g_bd_new_launcher |= cur_mt > g_self_mtime;
}

static void tab_build() {
    // [XC3] step 1: the build tools
    section(T("Build tools"));
    bool tools = bd_tools_ok();
    if (tl_running()) {
        char t[64];
        snprintf(t, sizeof t, T("Step %d of %d"), g_tl_step ? g_tl_step : 1, TOOLS_STEPS);
        ImGui::ProgressBar((g_tl_step ? g_tl_step - 1 : 0) / (float)TOOLS_STEPS + 0.1f, ImVec2(-150 * g_scale, 0), t);
        ImGui::SameLine();
        if (ImGui::Button(T("Stop##tools"), ImVec2(-1, 0))) tl_stop();
        ImGui::TextDisabled("%s", g_tl_line.c_str());
#ifdef _WIN32
        ImGui::TextDisabled("%s", T("Windows may ask for administrator rights to install MSYS2."));
#endif
    } else {
#ifdef _WIN32
        if (tools) ImGui::TextColored(pal::accent, "%s", T("Ready: system libraries and .venv"));
        else ImGui::TextColored(pal::warn, "%s", T("Not installed yet: build libraries (SDL3, FFmpeg, Vulkan, shaderc, Opus) and .venv packages."));
#else
        bool fsr_ok = exists("build/libffx_vk_linux.so") || exists("sdk/ffx-vk-linux/build/libffx_vk_linux.so");
        if (tools && fsr_ok) ImGui::TextColored(pal::accent, "%s", T("Ready: system libraries, .venv, and FSR 3.1 / FSR 4 bridge"));
        else if (tools) ImGui::TextColored(pal::warn, "%s", T("System libraries and .venv ready (click Update to download the FSR 3.1 / FSR 4 library)"));
        else ImGui::TextColored(pal::warn, "%s", T("Not installed yet: build libraries (SDL3, FFmpeg, Vulkan, shaderc, Opus), .venv, and FSR library."));
#endif
        ImGui::BeginDisabled(bd_running() || pc_running() || gf_busy());
        if (ImGui::Button(tools ? T("Update the build tools") : T("Install the build tools"))) tl_start();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Open log##tools"))) l_open_path(path(TOOLS_LOG));
    }
    if (!tools || tl_running()) return;
    section(T("Archivos del juego"));
    if (g_gf_code == -2) gf_check();
    if (g_gf_code == -1 || g_gf_proc) ImGui::TextDisabled("%s", T("Comprobando los archivos del juego..."));
    else if (g_gf_code == 0) ImGui::TextColored(pal::accent, "%s", T("Listos: Xenoblade Chronicles 3, versión 2.2.0 o 2.2.1 (targets/)"));
    else ImGui::TextColored(pal::warn, "%s", g_gf_code == 4 ? T("Hace falta la versión 2.2.0 o 2.2.1 (el juego base con esa actualización)")
                                           : g_gf_code == 5 ? T("Faltan archivos del romfs o son distintos (copia incompleta)")
                                           : g_gf_code == 2 ? T("Son paquetes (.xci / .nsp / .nca): hacen falta las carpetas exefs y romfs")
                                                            : T("No hay archivos del juego en targets/"));
    ImGui::BeginDisabled(gf_busy() || bd_running());
    if (ImGui::Button(T("Elegir archivos del juego..."))) gf_pick();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(T("Elige la carpeta que contiene las carpetas exefs y romfs de tu copia del juego."));
    // [XC3] the add-on content (optional; no rebuild needed: the game reads it at start)
    section(T("DLC (optional)"));
    std::string dl = dlc_found();
    if (!dl.empty()) ImGui::TextColored(pal::accent, T("Found: DLC %s (targets/dlcs)"), dl.c_str());
    else ImGui::TextDisabled("%s", T("No DLC files in targets/dlcs. The game runs without them."));
    ImGui::BeginDisabled(gf_busy() || bd_running());
    if (ImGui::Button(T("Choose DLC files..."))) dlc_pick();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(T("Choose the folder with your copy's DLC files (bf3_dlc01.arh, bf3_dlc01.ard, ... with movie and sound)."));
    ImGui::SameLine();
    ImGui::BeginDisabled(dl.empty());
    ImGui::Checkbox(T("Use the DLC"), &g.dlc);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(T("Off: the game starts as if no DLC were installed. Saved with Save settings / Play."));
    section(T("Compilación"));
    ImGui::TextDisabled("%s", T("Recompila el código del juego y compila el runtime y el launcher (unos minutos; la primera vez, 10-20)."));
    if (bd_running()) {
        char t[64];
        snprintf(t, sizeof t, T("Paso %d de 5"), g_bd_step ? g_bd_step : 1);
        ImGui::ProgressBar((g_bd_step ? g_bd_step - 1 : 0) / 5.0f + 0.1f, ImVec2(-150 * g_scale, 0), t);
        ImGui::SameLine();
        if (ImGui::Button(T("Detener"), ImVec2(-1, 0))) bd_stop();
        ImGui::TextDisabled("%s", g_bd_line.c_str());
        ImGui::TextDisabled("%s", T("Cerrar el launcher detiene la compilación."));
    } else {
        ImGui::BeginDisabled(pc_running() || gf_busy() || g_gf_code != 0);
        if (ImGui::Button(T("Compilar / actualizar"))) bd_start();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Abrir registro"))) l_open_path(path("build/build_all.log"));
        if (g_bd_new_launcher) ImGui::TextColored(pal::accent, "%s", T("Vuelve a abrir el launcher para usar la versión nueva."));
    }
    // [XC3] step 4: the shader precompilation (Play also runs it when it was not done for this build)
    section(T("Shaders"));
    if (!l_file_ok(get_game_exe_path())) { ImGui::TextDisabled("%s", T("After the build.")); return; }
    if (pc_running()) { ImGui::TextDisabled("%s", T("Precompiling the shaders (progress at the bottom)...")); return; }
    if (pc_done()) ImGui::TextColored(pal::accent, "%s", T("Precompiled for this build"));
    else ImGui::TextColored(pal::warn, "%s", T("Not precompiled for this build yet (a few minutes, once: fewer stutters the first time an area is seen)."));
    ImGui::BeginDisabled(bd_running() || tl_running());
    if (ImGui::Button(T("Precompilar shaders"))) pc_start(false);
    ImGui::EndDisabled();
}

// ------------------------------------------------------------------ remapping capture
static int cap_dev = -1, cap_slot = -1;  // dev 0 keyboard, 1 controller, 2 keyboard (second key)
static Uint64 cap_until, cap_since;

// a key / mouse input for the slot being captured: one already in use elsewhere (either column) takes the slot's old one
static void capture_key(int k) {
    int *t = cap_dev == 2 ? g.key2 : g.key, *o = cap_dev == 2 ? g.key : g.key2;
    for (int i = 0; i < NACT; i++) {
        if (t[i] == k && i != cap_slot) t[i] = t[cap_slot];
        if (o[i] == k) o[i] = t[cap_slot];
    }
    t[cap_slot] = k;
    cap_dev = -1;
}

static bool capture_event(const SDL_Event &e) {
    if (cap_dev < 0) return false;
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.scancode == SDL_SCANCODE_DELETE && cap_dev != 1) { capture_key(0); return true; }  // (Supr: none)
        if (e.key.scancode == SDL_SCANCODE_ESCAPE && SDL_GetTicks() - cap_since < 700) { cap_dev = -1; return true; }
        if (cap_dev != 1) capture_key(e.key.scancode);
        else if (e.key.scancode == SDL_SCANCODE_ESCAPE) cap_dev = -1;
        return true;
    }
    // mouse buttons / wheel (not the click that started the capture)
    if (cap_dev != 1 && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && SDL_GetTicks() - cap_since > 250) {
        capture_key(MOUSE_BASE + (e.button.button < 6 ? e.button.button : 5));
        return true;
    }
    if (cap_dev != 1 && e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y) {
        capture_key(e.wheel.y > 0 ? M_WUP : M_WDOWN);
        return true;
    }
    if (cap_dev == 1 && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        int h = e.gbutton.button;
        for (int i = 0; i < NPAD; i++) if (g.pad[i] == h && i != cap_slot) g.pad[i] = g.pad[cap_slot];
        g.pad[cap_slot] = h;
        cap_dev = -1;
        return true;
    }
    if (cap_dev == 1 && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 20000 &&
        (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        int h = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LT : PADB_RT;
        for (int i = 0; i < NPAD; i++) if (g.pad[i] == h && i != cap_slot) g.pad[i] = g.pad[cap_slot];
        g.pad[cap_slot] = h;
        cap_dev = -1;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ UI
static void tab_graphics() {
    section(T("Pantalla"));
    ImGui::Checkbox(T("Pantalla completa"), &g.fullscreen);
    ImGui::SetNextItemWidth(260 * g_scale);
    ImGui::Combo(T("Tamaño de la ventana"), &g.win, k_wins, 5);
    section(T("Resolución y escalado"));
    ImGui::SetNextItemWidth(360 * g_scale);
    ImGui::Combo(T("Resolución interna"), &g.res, TL(k_res_ui, 7).p, 7);
    ImGui::TextDisabled(T(RES_HINT));
    // [XC3] FSR 4 too; the temporal upscalers replace the game's own (half-size) upscale, so no quality mode (the render
    // size is the game's) and no frame generation (not ported to FidelityFX SDK 2.x)
    const char *ups[] = {"Bilineal (suave)", "AMD FSR 1.0 (nítido)", "AMD FSR 3.1 (temporal, cualquier GPU)",
                         "NVIDIA DLSS (temporal, GPU RTX)", "AMD FSR 4 (temporal, GPU RDNA 4)"};
    ImGui::SetNextItemWidth(320 * g_scale);
    ImGui::Combo(T("Escalado / antialiasing"), &g.upscaler, TL(ups, 5).p, 5);
    ImGui::SetItemTooltip(T("FSR 1.0: escalado espacial, solo cuando la resolución interna es menor que la ventana.\n"
                          "FSR 3.1, FSR 4 y DLSS: reconstrucción temporal - sustituyen al escalado del propio juego (que\n"
                          "renderiza a la mitad del tamaño de la ventana) y reconstruyen la imagen con varios frames.\n"
                          "FSR 4 necesita una GPU AMD RDNA 4 (Radeon RX 9000); en otras se usa FSR 3.1.\n"
                          "DLSS necesita una GPU NVIDIA RTX; si no está disponible se usa FSR 3.1."));
    if (g.upscaler >= 2) {  // [XC3] the scene's render size (output_scale_scene_factor); 'Performance' = the game's own
        const char *uq[] = {"Nativo (solo antialiasing: DLAA / FSR Native AA)", "Calidad (67 %)", "Equilibrado (59 %)",
                            "Rendimiento (50 %)", "Ultra rendimiento (33 %)"};
        ImGui::SetNextItemWidth(320 * g_scale);
        ImGui::Combo(T("Calidad del escalado"), &g.uquality, TL(uq, 5).p, 5);
        ImGui::SetItemTooltip(T("La resolución a la que se renderiza la escena antes de reconstruirla (porcentaje del lado\n"
                              "de la ventana). Rendimiento = la del juego original (la mitad); Nativo = la de la ventana."));
    }
    if (false) {
        const char *uq[] = {"Nativo (solo antialiasing: DLAA / FSR Native AA)", "Calidad (67 %)", "Equilibrado (59 %)",
                            "Rendimiento (50 %)", "Ultra rendimiento (33 %)"};
        ImGui::SetNextItemWidth(320 * g_scale);
        ImGui::Combo(T("Calidad del escalado"), &g.uquality, TL(uq, 5).p, 5);
        ImGui::SetItemTooltip(T("Con 'Resolución interna' en 'Igual que la ventana': la resolución a la que se renderiza\n"
                              "antes de reconstruir (porcentaje del lado de la ventana)."));
        ImGui::Checkbox(T("Generación de frames (FSR 3)"), &g.frame_gen);
        ImGui::SetItemTooltip(T("Inserta un frame interpolado entre cada dos del juego en todo el juego: el doble de imágenes\n"
                              "por segundo (p. ej. FPS 30 -> 60 en pantalla). La de AMD: funciona con FSR 3.1 y con DLSS.\n"
                              "Añade algo de latencia. Se aplica al iniciar el juego."));
        ImGui::BeginDisabled(g.frame_gen);
        ImGui::Checkbox(T("Generación de frames solo en cinemáticas"), &g.cut_fg);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(T("El juego va a tus FPS sin generación de frames; en las cinemáticas (donde el juego anima a\n"
                              "los personajes a 30) baja a 30 parejos y la generación las lleva a 60 en pantalla.\n"
                              "Pensado para jugar a 60 en un monitor de 60 Hz. Se aplica al iniciar el juego."));
    } else if (g.upscaler < 2) {  // ([XC3] the temporal ones: nothing more)
        ImGui::TextDisabled(T("Se aplica cuando la resolución interna es menor que la ventana."));
    }
    section(T("Calidad"));
    ImGui::BeginDisabled(g.upscaler >= 2);  // [XC3] FSR 3.1 / DLSS / FSR 4 (k_ups_ini): SMAA is not used with them
    ImGui::Checkbox(T("Antialiasing SMAA"), &g.smaa);
    ImGui::EndDisabled();
    if (g.upscaler >= 2) ImGui::SameLine(), ImGui::TextDisabled(T("(no se usa con FSR 3.1 / FSR 4 / DLSS)"));
    // [XC3] the game's own blur and TAA (runtime/services/romfs_virtual.c xc3_libnx_patch: lib_nx.bin)
    ImGui::Checkbox(T("Motion blur"), &g.motion_blur);
    ImGui::SetItemTooltip(T("The motion blur (and fur blur) the game applies when the camera or characters move.\n"
                          "Off: a sharper image in motion. Applied when the game starts."));
    ImGui::BeginDisabled(g.upscaler >= 2);
    ImGui::Checkbox(T("Game's temporal anti-aliasing (TMAA)"), &g.game_taa);
    ImGui::EndDisabled();
    if (g.upscaler >= 2) ImGui::SameLine(), ImGui::TextDisabled(T("(kept: FSR 3.1 / FSR 4 / DLSS use it)"));
    ImGui::SetItemTooltip(T("The temporal anti-aliasing the game uses for the Switch's resolution: smooth edges, but a\n"
                          "softer image. Off: a sharper image (SMAA can smooth the edges). Applied when the game starts."));
    ImGui::Checkbox(T("Mostrar FPS en la barra de título"), &g.show_fps);
    static const char *aniso_names[] = {"Default", "2x", "4x", "8x", "16x (Recomendado)"};
    static const int aniso_vals[] = {0, 2, 4, 8, 16};
    int ai = 0;
    for (int i = 0; i < 5; i++) if (aniso_vals[i] == g.anisotropy) ai = i;
    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::Combo(T("Filtrado anisotrópico (AF)"), &ai, TL(aniso_names, 5).p, 5)) {
        g.anisotropy = aniso_vals[ai];
    }
    ImGui::SetItemTooltip(T("Mejora notablemente la nitidez de texturas en superficies oblicuas o a distancia (suelos, rocas, caminos)\n"
                          "sin costo apreciable en GPUs modernas."));
    section(T("Velocidad"));
    static const char *fps_ui[] = {"30 (como en Switch)", "60", "120", "144", "165", "Sin límite"};
    // [XC3] above 30: the real-time frame step (runtime/hle/framepace.c xc3_frame_present)
    if (g.fps < 0 || g.fps > 5) g.fps = 1;
    if (ImGui::Combo("FPS", &g.fps, TL(fps_ui, 6).p, 6)) {
        g.frame_lag = (g.fps != 0);  // [XC3] frame lag (one frame in flight) above 30
    }
    ImGui::SetItemTooltip(T("60 o más: el juego a esa velocidad fuera de las cinemáticas (que siguen a 30), con el tiempo del juego\n"
                          "medido en cada imagen: correr y el mundo van a la velocidad de la consola a cualquier FPS.\n"
                          "Necesita una GPU que lo mantenga: en zonas pesadas ayuda FSR en Rendimiento."));
    static const char *vs_names[] = {"V-Sync ON (FIFO)", "Fast Sync (Mailbox)", "V-Sync OFF / VRR (Immediate)"};
    int vsi = g.present_mode >= 0 && g.present_mode <= 2 ? g.present_mode : (!g.vsync ? 2 : 0);
    ImGui::SetNextItemWidth(300 * g_scale);
    if (ImGui::Combo(T("Sincronización vertical (V-Sync / VRR)"), &vsi, TL(vs_names, 3).p, 3)) {
        g.present_mode = vsi;
        g.vsync = (vsi != 2);
    }
    ImGui::SetItemTooltip(T("FIFO: V-Sync clásico sin tearing, sincronizado al refresco de la pantalla.\n"
                          "Mailbox: Fast Sync / latencia mínima sin tearing.\n"
                          "Immediate: V-Sync OFF, recomendado para monitores con G-Sync / FreeSync (VRR)."));
    section(T("Depuración"));  // [XC3]
    ImGui::Checkbox(T("Grabar la sesión (capturas, entradas y registro en play/)"), &g.record);
    ImGui::SetItemTooltip(T("Como scripts/play.sh: una captura cada 5 s, las entradas (input.txt, para repetirlas) y el registro (game.log)\n"
                          "en play/<fecha_hora>/. Envía esa carpeta si algo falla."));
}

static void tab_controls() {
    if (cap_dev >= 0) {
        int left = (int)((cap_until - SDL_GetTicks()) / 1000) + 1;
        if (SDL_GetTicks() > cap_until) cap_dev = -1;
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), T("Pulsa %s para \"%s\"... (%d s, Supr = ninguna)"),
                           cap_dev == 1 ? T("un botón del mando") : T("una tecla o un botón del ratón"), T(k_names[cap_slot]), left);
    } else {
        ImGui::TextDisabled(T("Haz clic en una asignación y pulsa la tecla, botón del ratón o del mando nuevo. Si ya estaba en uso, se intercambian."));
    }
    if (ImGui::Button(T("Restaurar teclado"))) for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
    ImGui::SameLine();
    if (ImGui::Button(T("Restaurar mando"))) for (int i = 0; i < NPAD; i++) g.pad[i] = k_def_pad[i];
    ImGui::SameLine();
    int npads = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&npads);
    if (npads > 0) ImGui::Text(T("   Mando conectado: %s"), SDL_GetGamepadNameForID(ids[0]));
    else ImGui::TextDisabled(T("   (ningún mando detectado)"));
    SDL_free(ids);
    {
        static const char *ic_ui[] = {"Automático (el mando conectado; sin mando, teclado)", "Switch", "Xbox", "PlayStation",
                                      "Teclado (las teclas asignadas)"};
        ImGui::SetNextItemWidth(340 * g_scale);
        ImGui::Combo(T("Iconos de botones"), &g.icons, TL(ic_ui, 5).p, 5);
        // [XC3] all styles drawn by the game itself (runtime/services/kbicons.c, texpatch.c): no packs needed
        ImGui::SetItemTooltip(T("Los iconos de los botones del juego: Xbox (A B X Y, LB RB LT RT), PlayStation (símbolos,\n"
                              "L1 R1 L2 R2) o las teclas asignadas, según los controles. Se aplica al iniciar el juego."));
    }
    {
        static const char *ml_ui[] = {"No", "Manteniendo el botón derecho", "Siempre"};
        ImGui::SetNextItemWidth(260 * g_scale);
        ImGui::Combo(T("Cámara con el ratón"), &g.mouse_look, TL(ml_ui, 3).p, 3);
        ImGui::SetItemTooltip(T("Siempre: el ratón mueve la cámara directamente y el cursor queda oculto dentro del juego.\n"
                              "F1 (menú) o Alt+Tab lo liberan; al mover el ratón sobre el juego vuelve a capturarse."));
    }
    ImGui::BeginDisabled(!g.mouse_look);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140 * g_scale);
    ImGui::SliderInt(T("Sensibilidad"), &g.mouse_sens, 1, 100);
    ImGui::SameLine();
    ImGui::Checkbox(T("Invertir eje Y"), &g.mouse_invert);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(140 * g_scale);
    ImGui::SliderInt(T("Zona muerta de los sticks (%)"), &g.stick_deadzone, 0, 30);
    ImGui::SetItemTooltip(T("Para un mando con drift (el personaje o la cámara se mueven solos): esa parte del recorrido\n"
                          "del stick cuenta como centrada. 0 = sin cambios (el juego ya tiene la suya)."));
    ImGui::TextDisabled(T("Artes 1-8 y talento: en combate seleccionan el arte de ese hueco de la paleta y lo usan."));
    if (ImGui::BeginTable("binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn(T("Acción (botón de Switch)"), ImGuiTableColumnFlags_WidthFixed, 220 * g_scale);
        ImGui::TableSetupColumn(T("Tecla"), ImGuiTableColumnFlags_WidthFixed, 130 * g_scale);
        ImGui::TableSetupColumn(T("Tecla 2"), ImGuiTableColumnFlags_WidthFixed, 130 * g_scale);
        ImGui::TableSetupColumn(T("Mando"));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < NACT; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(T(k_names[i]));
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            char lbl[96];
            for (int c = 0; c < 2; c++) {
                int dev = c ? 2 : 0, k = c ? g.key2[i] : g.key[i];
                snprintf(lbl, sizeof lbl, "%s##k%d", cap_dev == dev && cap_slot == i ? "..." : key_ui_name(k), c);
                if (ImGui::Button(lbl, ImVec2(120 * g_scale, 0))) { cap_dev = dev; cap_slot = i; cap_since = SDL_GetTicks(); cap_until = cap_since + 5000; }
                ImGui::TableNextColumn();
            }
            if (i < NPAD) {
                snprintf(lbl, sizeof lbl, "%s##p", cap_dev == 1 && cap_slot == i ? "..." : T(k_pad_label[g.pad[i]]));
                if (ImGui::Button(lbl, ImVec2(-1, 0))) { cap_dev = 1; cap_slot = i; cap_since = SDL_GetTicks(); cap_until = cap_since + 5000; }
            } else if (i < NSTICK) {
                ImGui::TextDisabled(T("(stick del mando)"));
            } else if (i < NACT - 2) {
                ImGui::TextDisabled(T("(cruceta + A)"));
            } else {
                ImGui::TextDisabled(i == NACT - 2 ? "(L2 + L1)" : "(L2 + R1)");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Mods: every folder in mods/ with a romfs/ inside (runtime/services/romfs_virtual.c applies them alphabetically,
// a later one wins for the same file); xcde.ini mod_<folder> = 0/1, on by default.
static void scan_mods() {
    std::vector<Mod> found;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path("mods\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            if (!l_is_dir(path((std::string("mods/") + fd.cFileName + "/romfs").c_str()))) continue;
            Mod m{fd.cFileName, true};
            for (const Mod &o : g_mods) if (o.name == m.name) m.on = o.on;
            found.push_back(m);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(path("mods").c_str());
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            if (!l_is_dir(path((std::string("mods/") + de->d_name + "/romfs").c_str()))) continue;
            Mod m{de->d_name, true};
            for (const Mod &o : g_mods) if (o.name == m.name) m.on = o.on;
            found.push_back(m);
        }
        closedir(d);
    }
#endif
    std::sort(found.begin(), found.end(), [](const Mod &a, const Mod &b) { return a.name < b.name; });
    for (const Mod &o : g_mods) {  // (keep the settings of folders not present now)
        bool here = false;
        for (const Mod &m : found) here |= m.name == o.name;
        if (!here) { Mod k = o; k.present = false; found.push_back(k); }
    }
    g_mods = found;
}

// [XCDE] "Crear reporte": reports/xc3_report_<date>.zip with what a bug report needs - the last two game logs,
// xc3.ini and the newest crash report (crash/). No saves or game files.
static void make_report() {
    std::vector<std::string> files;
    for (const char *f : {"build/lasrecomp.log", "build/lasrecomp.prev.log", "build/last_game.log", INI_FILE})
        if (l_file_ok(path(f))) files.push_back(f);
    std::string newest;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path("crash/crash_*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do if (newest.empty() || strcmp(fd.cFileName, newest.c_str()) > 0) newest = fd.cFileName; while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *cd = opendir(path("crash").c_str());
    if (cd) {
        struct dirent *de;
        while ((de = readdir(cd))) {
            size_t len = strlen(de->d_name);
            if (!strncmp(de->d_name, "crash_", 6) && len > 4 && !strcmp(de->d_name + len - 4, ".txt"))
                if (newest.empty() || strcmp(de->d_name, newest.c_str()) > 0) newest = de->d_name;
        }
        closedir(cd);
    }
#endif
    if (!newest.empty()) {  // (crash_<stamp>.txt sorts by date)
        files.push_back("crash/" + newest);
        std::string dmp = "crash/" + newest.substr(0, newest.size() - 4) + ".dmp";
        if (l_file_ok(path(dmp.c_str()))) files.push_back(dmp);
    }
    if (files.empty()) { g_status = T("No hay nada que reportar todavía (ni logs ni " INI_FILE ")"); return; }
    l_mkdir(path("reports"));
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm_now);
    std::string zip = std::string("reports/xc3_report_") + stamp + ".zip";
    int code = 1;
#ifdef _WIN32
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof sys);
    std::string tar = std::string(sys) + "\\tar.exe";
    std::vector<std::string> argv = {tar, "-a", "-c", "-f", zip};
    for (const std::string &f : files) argv.push_back(f);
    LProc p = l_spawn(argv, NULL, NULL, false);
    if (!p) { g_status = T("No se pudo crear el reporte (falta tar.exe de Windows)"); return; }
    WaitForSingleObject(p, 30000);
    DWORD wcode = 1;
    GetExitCodeProcess(p, &wcode);
    CloseHandle(p);
    code = (int)wcode;
#else
    std::vector<std::string> argv = {"python3", "-m", "zipfile", "-c", zip};
    for (const std::string &f : files) argv.push_back(f);
    LProc p = l_spawn(argv, NULL, false, false);
    if (!p) { g_status = T("No se pudo crear el reporte"); return; }
    int st = 0;
    if (waitpid(p, &st, 0) > 0 && WIFEXITED(st)) code = WEXITSTATUS(st);
#endif
    if (code) { g_status = T("No se pudo crear el reporte"); return; }
    g_status = std::string(T("Reporte creado: ")) + zip + T(" (adjúntalo al reportar el problema)");
    l_open_path(path("reports"));
}

static void tab_mods() {
    static bool scanned;
    if (!scanned) { scanned = true; scan_mods(); }
    ImGui::TextWrapped(T("Pon cada mod en su carpeta: mods\\<nombre>\\romfs\\... (la misma estructura que en Ryujinx / yuzu). "
                       "Sus archivos reemplazan o añaden archivos del juego. Se aplican en orden alfabético: si dos mods "
                       "cambian el mismo archivo, gana el último. Los mods de código (exefs, parches .pchtxt / IPS) no "
                       "funcionan en esta versión recompilada."));
    if (ImGui::Button(T("Abrir carpeta de mods"))) {
        l_mkdir(path("mods"));
        l_open_path(path("mods"));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar lista"))) scan_mods();
    ImGui::Separator();
    int shown = 0;
    for (Mod &m : g_mods) {
        if (!m.present || is_icon_mod(m.name)) continue;
        ImGui::Checkbox(m.name.c_str(), &m.on);
        shown++;
    }
    if (!shown) ImGui::TextDisabled(T("No hay mods instalados."));
}

static void tab_audio() {
    section(T("Audio"));
    ImGui::SetNextItemWidth(300 * g_scale);
    ImGui::SliderInt(T("Volumen"), &g.volume, 0, 200, "%d%%");
    ImGui::Checkbox(T("Sonido de los vídeos"), &g.movie_audio);
    section(T("Idioma del juego"));
    ImGui::SetNextItemWidth(260 * g_scale);
    ImGui::Combo(T("Idioma"), &g.lang, TL(k_lang_ui, 10).p, 10);
    ImGui::TextDisabled(T("Textos del juego. Solo los idiomas que incluya tu copia; si uno falla, vuelve a Inglés."));
}

// [XCDE] start-up trace (build/launcher.log) and a visible error instead of a silent exit: a PC where the launcher "did
// nothing" (no window, no message) had no way to tell why.
static FILE *g_llog;
static void llog(const char *fmt, ...) {
    if (!g_llog) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_llog, fmt, ap);
    va_end(ap);
    fputc('\n', g_llog);
    fflush(g_llog);
}
static int fail(const char *what) {
    char msg[1024];
    snprintf(msg, sizeof msg, "%s\n\n%s\n\n(build/launcher.log)", what, SDL_GetError());
    llog("ERROR: %s: %s", what, SDL_GetError());
#ifdef _WIN32
    MessageBoxA(NULL, msg, APP_NAME " - Launcher", MB_OK | MB_ICONERROR);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME " - Launcher", msg, NULL);
#endif
    return 1;
}
#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    char msg[256];
    snprintf(msg, sizeof msg, "The launcher crashed (exception %#lx at %p).\n\n(build\\launcher.log)",
             e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    llog("CRASH: exception %#lx at %p", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    MessageBoxA(NULL, msg, APP_NAME " - Launcher", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static std::string find_linux_font(const char *pattern) {
    FILE *p = popen(pattern, "r");
    if (!p) return "";
    char buf[512] = "";
    if (fgets(buf, sizeof buf, p)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    }
    pclose(p);
    return buf[0] && l_file_ok(buf) ? std::string(buf) : "";
}
#endif

int main(int argc, char **argv) {
    // root: the folder above the launcher's build/ folder (or the working directory)
    const char *base = SDL_GetBasePath();
    g_root = base ? base : ".";
    while (!g_root.empty() && (g_root.back() == '/' || g_root.back() == '\\')) g_root.pop_back();
    if (g_root.size() > 6 && (g_root.compare(g_root.size() - 6, 6, "\\build") == 0 || g_root.compare(g_root.size() - 6, 6, "/build") == 0))
        g_root.resize(g_root.size() - 6);
    // --shot <file.bmp> [tab] [precompile]: render a few frames of that tab, save the window as a BMP and quit (tests)
    const char *shot = argc > 2 && !strcmp(argv[1], "--shot") ? argv[2] : NULL;
    int shot_tab = argc > 3 ? atoi(argv[3]) : 0, frame_no = 0;
    bool shot_pc = shot && argc > 4 && !strcmp(argv[4], "precompile");  // (... <tab> precompile: the bar after ~5 s of a run)
    // [XC3] ... <tab> build: the build's bar after ~5 s; ... <tab> build-wait: the tab once the build has ended
    bool shot_bd = shot && argc > 4 && !strcmp(argv[4], "build"), shot_bd_wait = shot && argc > 4 && !strcmp(argv[4], "build-wait");
    l_mkdir(path("build"));
    g_llog = fopen(path("build/launcher.log").c_str(), "w");
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#endif
    llog("launcher start, root %s", g_root.c_str());
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");  // [XC3] the window's icon: resource 1 (launcher.rc)
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        llog("SDL_Init with gamepads failed (%s): video only", SDL_GetError());
        if (!SDL_Init(SDL_INIT_VIDEO)) return fail("Could not start SDL (video).");
    }
    SDL_Window *win = SDL_CreateWindow(APP_NAME " - Launcher", 980, 680, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!win) return fail("Could not create the launcher window.");
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {  // (an old or missing graphics driver: the CPU renderer still shows the launcher)
        llog("GPU renderer failed (%s): software renderer", SDL_GetError());
        ren = SDL_CreateRenderer(win, "software");
    }
    if (!ren) return fail("Could not create a renderer for the launcher window (graphics driver?).");
    llog("renderer %s", SDL_GetRendererName(ren));
    SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    float scale = SDL_GetWindowDisplayScale(win);
    if (scale <= 0) scale = 1;
    g_scale = scale;
    SDL_SetWindowSize(win, (int)(980 * scale), (int)(680 * scale));
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (!shot) SDL_ShowWindow(win);
    {
#ifdef _WIN32
        // fonts: Segoe UI (text), Segoe UI Bold (section titles, PLAY), Bahnschrift (the title); each only if present
        auto font = [&](const char *file, float size) -> ImFont * {
            std::string p = std::string("C:\\Windows\\Fonts\\") + file;
            return l_file_ok(p) ? io.Fonts->AddFontFromFileTTF(p.c_str(), size * scale) : NULL;
        };
        if (!font("segoeui.ttf", 17)) io.Fonts->AddFontDefault();
        g_font_bold = font("segoeuib.ttf", 17);
        g_font_title = font("bahnschrift.ttf", 34);
        if (!g_font_title) g_font_title = font("segoeuib.ttf", 30);
#else
        std::string reg = find_linux_font("fc-match -f '%{file}' 'sans:regular' 2>/dev/null");
        std::string bold = find_linux_font("fc-match -f '%{file}' 'sans:bold' 2>/dev/null");
        if (reg.empty() || !io.Fonts->AddFontFromFileTTF(reg.c_str(), 17 * scale)) io.Fonts->AddFontDefault();
        if (!bold.empty()) {
            g_font_bold = io.Fonts->AddFontFromFileTTF(bold.c_str(), 17 * scale);
            g_font_title = io.Fonts->AddFontFromFileTTF(bold.c_str(), 32 * scale);
        }
#endif
    }
    ImGuiStyle &st = ImGui::GetStyle();
    theme_apply(st);
    st.ScaleAllSizes(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    if (exists("mods/launcher/background.png")) theme_load_background(ren, path("mods/launcher/background.png"));
    load();
    {   // (the launcher's own exe as it started: a build that replaced it says so - bd_poll)
        char me[1024];
        if (l_self_path(me, sizeof me)) g_self_mtime = l_mtime(me);
    }
    bool have_game = exists("targets/exefs/main") || exists("targets/exefs");
    bool have_exe = have_game_exe();
    bool run = true, launched = false;
    if (shot_pc) pc_start(false);
    bool shot_bd_started = false;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (capture_event(e)) continue;
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) run = false;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        theme_background();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (g_font_title) ImGui::PushFont(g_font_title);
        ImGui::TextColored(pal::accent, APP_NAME);  // [XC3]
        if (g_font_title) ImGui::PopFont();
        ImGui::TextDisabled("%s", T("Recompilación nativa para PC · configuración"));
        ImGui::Dummy(ImVec2(0, 4 * scale));
        float play_h = ImGui::GetFrameHeight() * 1.45f;
        float bottom = ImGui::GetFrameHeightWithSpacing() * (pc_running() ? 2.4f : 1.3f) + play_h + st.ItemSpacing.y;
        ImGui::BeginChild("tabs", ImVec2(0, -bottom), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        if (ImGui::BeginTabBar("t")) {
            ImGuiTabItemFlags f0 = shot && shot_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0, f1 = shot && shot_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0,
                              f2 = shot && shot_tab == 2 ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(T("Gráficos"), NULL, f0)) { tab_graphics(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(T("Controles"), NULL, f1)) { tab_controls(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(T("Audio e idioma"), NULL, f2)) { tab_audio(); ImGui::EndTabItem(); }
            ImGuiTabItemFlags f3 = shot && shot_tab == 3 ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(T("Instalación y build"), NULL, f3)) { tab_build(); ImGui::EndTabItem(); }  // [XC3]
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        bd_poll();
        have_game = exists("targets/exefs/main") || exists("targets/exefs");  // (the tab may have set them up / built it)
        have_exe = have_game_exe();
        if ((shot_bd || shot_bd_wait) && !shot_bd_started && g_gf_code == 0) { shot_bd_started = true; bd_start(); }
        if (!have_game) ImGui::TextColored(pal::warn, "%s", T("Faltan los archivos del juego en targets/ (exefs y romfs de tu copia)."));
        else if (!have_exe) ImGui::TextColored(pal::warn, "%s", T("Falta build/xc3.exe (compila el juego primero)."));
        else ImGui::TextDisabled(T("%s"), g_status.empty() ? (pc_done() ? T("Listo. Guardado: save/   Ajustes: " INI_FILE)
                                                                   : T("Listo. La primera vez, JUGAR prepara los shaders antes (unos minutos)"))
                                                       : g_status.c_str());
        pc_poll();
        if (pc_running()) {
            ImGui::ProgressBar(g_pc_frac, ImVec2(-150 * scale, 0), g_pc_text.c_str());
            ImGui::SameLine();
            if (ImGui::Button(g_pc_then_play ? T("Omitir") : T("Detener"), ImVec2(-1, 0))) {
                pc_stop();
                g_status = T("Precompilación detenida (se puede seguir otro día: lo ya hecho se conserva)");
                if (g_pc_then_play) g_pc_play_now = true;
            }
        }
        if (g_pc_play_now) {
            g_pc_play_now = false;
            launch();
            launched = g_launched_ok;
            if (launched) run = false;
        }
        ImGui::BeginDisabled(!have_game || !have_exe || launched || pc_running() || bd_running());
        if (play_button(T("JUGAR"), ImVec2(240 * scale, play_h))) {
            if (!pc_done()) pc_start(true);
            else { launch(); launched = g_launched_ok; if (launched) run = false; }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Precompilar shaders"), ImVec2(0, play_h))) pc_start(false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("Traduce y compila todos los shaders del juego por adelantado, para que la primera visita\n"
                              "a una zona no se detenga a compilarlos. Solo hace falta una vez (o tras actualizar el juego)."));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Guardar ajustes"), ImVec2(0, play_h))) g_status = save() ? T("Ajustes guardados en " INI_FILE) : T("No se pudo escribir " INI_FILE);
        ImGui::SameLine();
        if (ImGui::Button(T("Crear reporte"), ImVec2(0, play_h))) make_report();
        ImGui::SetItemTooltip(T("Junta en un .zip los logs de las dos últimas partidas, " INI_FILE " y el último informe de\n"
                              "cierre inesperado (sin partidas guardadas ni archivos del juego), para adjuntarlo a un reporte."));
        ImGui::SameLine();
        if (ImGui::Button(T("Salir"), ImVec2(0, play_h))) run = false;
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 6, 10, 20, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        bool shot_now = shot_bd_wait ? shot_bd_started && !bd_running() && ++frame_no > 5 : shot && ++frame_no == (shot_pc || shot_bd ? 300 : 5);
        if (shot && shot_now) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
            if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); }
            run = false;
        }
        SDL_RenderPresent(ren);
    }
    if (!launched) pc_stop();  // (closing the launcher stops a precompilation; what it wrote is kept)
    bd_stop();                 // [XC3] (and a build: build_all.sh starts again from its first step next time)
    tl_stop();                 // [XC3] (and a tools install: setup_windows.ps1 is safe to run again)
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
