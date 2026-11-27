// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "option_rows.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "../text_utf8.h"
#include "text_clipboard.h"
#include "theme.h"
#include "../hle/cheatvm.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif

extern "C" {
#include "../hle.h"
#include "../settings.h"
#include "menu.h"
void input_reset(void);
void save_editor_draw(float scale);  // save_editor.mm
extern "C" int game_in_play(void);  // hle/warp.c
void save_editor_on_open(void);
void rando_tab_draw(float scale);
void launcher_mods_tab(float scale);  // launcher.mm: Settings > Mods, on the start screen
bool warp_tab_draw(float scale);  // warp_tab.mm: true when a warp started
extern "C" void cheats_draw(float scale);  // cheats.mm
extern "C" void difficulty_draw(float scale);  // difficulty.mm
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../paths.h"
#include "../gamebuild.h"
#include "../services/nfp.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_notice_active{false};
static pthread_mutex_t notice_lock = PTHREAD_MUTEX_INITIALIZER;
static char notice_text[2048];
extern "C" void menu_notice(const char *text) {
    pthread_mutex_lock(&notice_lock);
    snprintf(notice_text, sizeof notice_text, "%s", text);
    g_notice_active.store(true);
    pthread_mutex_unlock(&notice_lock);
}
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT, FOCUS } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        save_editor_on_open();
        nfp_rescan_amiibos();
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back.
static ImGuiKey key_from_pad(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
static bool capturing() { return g_notice_active.load() || g_open.load() || g_kbd.load() == 1; }
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return g_notice_active.load() || capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    clipboard_service(e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST || e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        QEvent q = {}; q.kind = QEvent::FOCUS; q.down = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED; push(q);
    }

    if (g_notice_active.load() &&
        ((e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
          (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_ESCAPE)) ||
         (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
          (e->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e->gbutton.button == SDL_GAMEPAD_BUTTON_EAST)))) {
        g_notice_active.store(false);
        input_reset();
        return true;
    }
    static bool back_down, start_down;
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) {
            // Modifier state first: ImGui (1.87+) takes Ctrl / Shift / Alt / Super only as ImGuiMod_* events, not from
            // the LeftCtrl etc. keys, so without these Ctrl+V / C / X / A / Z did nothing in text fields
            static const struct { ImGuiKey mod; SDL_Keymod sdl; } mods[] = {
                {ImGuiMod_Ctrl, SDL_KMOD_CTRL}, {ImGuiMod_Shift, SDL_KMOD_SHIFT},
                {ImGuiMod_Alt, SDL_KMOD_ALT}, {ImGuiMod_Super, SDL_KMOD_GUI}};
            for (const auto &m : mods) {
                QEvent q = {}; q.kind = QEvent::KEY; q.key = m.mod; q.down = (e->key.mod & m.sdl) != 0; push(q);
            }
            QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        {   // (diagnostic) LARECOMP_LOG_PAD=1: each controller button press / release as SDL reports it
            static int lp = -1;
            if (lp < 0) lp = getenv("LARECOMP_LOG_PAD") != NULL;
            if (lp) LOG("[pad] %s %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)e->gbutton.button), down ? "down" : "up");
        }
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        // Back + Start: not while L or R is held - the game's soft reset is L + R + Start + Back, and the menu taking the
        // combo made it hard to do - and not at all when it's turned off (Controls > Back + Start opens the menu)
        bool combo = back_down && start_down && g_settings.pad_menu_combo;
        if (combo) {
            SDL_Gamepad *gp = SDL_GetGamepadFromID(e->gbutton.which);
            if (gp && (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ||
                       SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)))
                combo = false;
        }
        // A Guide press while Back and Start are held is the controller's own chord, not the Guide button: several pads
        // (8BitDo ones among them) send Guide for Back + Start, which opened the menu even with the combo off.
        bool guide = e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE && !(back_down && start_down);
        if (down && !typing && (combo || guide)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_apply_window_requests(void *win) {
    clipboard_service(false);
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
        SDL_SetWindowFullscreen(w, g_settings.fullscreen);
    }
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -1;

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;  // (theme.h: slate and gold)
    c[ImGuiCol_WindowBg] = THEME_WINDOW;
    c[ImGuiCol_ChildBg] = THEME_CHILD;
    c[ImGuiCol_PopupBg] = THEME_POPUP;
    c[ImGuiCol_Text] = THEME_TEXT;
    c[ImGuiCol_TextDisabled] = THEME_QUIET;
    c[ImGuiCol_FrameBg] = THEME_FRAME;
    c[ImGuiCol_FrameBgHovered] = THEME_HOVER;
    c[ImGuiCol_FrameBgActive] = THEME_ACTIVE;
    c[ImGuiCol_Button] = THEME_FRAME;
    c[ImGuiCol_ButtonHovered] = THEME_HOVER;
    c[ImGuiCol_ButtonActive] = THEME_ACTIVE;
    c[ImGuiCol_Header] = THEME_HOVER;
    c[ImGuiCol_HeaderHovered] = THEME_ACTIVE;
    c[ImGuiCol_HeaderActive] = THEME_ACTIVE;
    c[ImGuiCol_Tab] = THEME_TAB;
    c[ImGuiCol_TabHovered] = THEME_HOVER;
    c[ImGuiCol_TabSelected] = THEME_TAB_SELECTED;
    c[ImGuiCol_TabSelectedOverline] = THEME_GOLD;
    c[ImGuiCol_CheckMark] = THEME_GOLD;
    c[ImGuiCol_SliderGrab] = THEME_GOLD_DIM;
    c[ImGuiCol_SliderGrabActive] = THEME_GOLD;
    c[ImGuiCol_NavCursor] = THEME_GOLD;
    c[ImGuiCol_Separator] = THEME_SEPARATOR;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}


// Korean game language only: merges a system font's Hangul into the font added just before (ImGui MergeMode), for the
// name entry. ~11,000 glyphs, so not for other languages (atlas size, start time). FE3H_KOR_FONT: another font file.
// The menu's fonts are built once, often before the language is switched to Korean (Quit to Start Screen + Play keeps
// this context): menu_render rebuilds them with Hangul when the name entry opens in Korean (kor_fonts_wanted).
static bool g_kor_tried;  // (merged, or no font found: not again)
static bool kor_fonts_wanted(void) { return !g_kor_tried && !strcmp(settings_language(), "ko"); }
static void merge_korean(ImGuiIO &io, float size) {
    if (!kor_fonts_wanted()) return;
    g_kor_tried = true;
    const char *paths[] = {getenv("FE3H_KOR_FONT"), "C:/Windows/Fonts/malgun.ttf",
                           "/System/Library/Fonts/AppleSDGothicNeo.ttc",
                           "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
                           "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                           "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"};
    for (const char *p : paths) {
        if (!p || !*p) continue;
        if (FILE *f = fopen(p, "rb")) fclose(f);  // (a missing file is an assertion in ImGui)
        else continue;
        ImFontConfig k;
        k.MergeMode = true;
        k.OversampleH = 1;  // (keeps the atlas small)
        k.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(p, size, &k, io.Fonts->GetGlyphRangesKorean());
        LOG("[menu] Korean name entry: Hangul from %s\n", p);
        return;
    }
    LOG("[menu] Korean name entry: no Hangul font found (FE3H_KOR_FONT=<font file> sets one)\n");
}

static void load_fonts(ImGuiIO &io);

#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = clipboard_get;
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = clipboard_set;
    load_fonts(io);
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

// The menu's fonts (init, and again with Hangul for the Korean name entry: menu_render)
static void load_fonts(ImGuiIO &io) {
    g_font = g_font_big = g_menu_font = g_menu_font_big = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    for (const char *p : paths) {
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) {
            ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b);
        }
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    merge_korean(io, 56);  // also try after the default-font fallback, once per process
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        case QEvent::FOCUS: io.AddFocusEvent(q.down); break;
        }
    }
}

// Section header in the big font.
static void header(const char *t) {
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(THEME_GOLD, "%s", t);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", t);
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", names[*value])) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(names[i], i == *value)) { *value = i; changed = true; }
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");  // (no aspect ratio choice: FE3H keeps its 16:9 framing, settings.c)
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_want_fullscreen_apply.store(true);
    }
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");
    {   // (nvn/objects.c present_interval_wait; LARECOMP_PRESENT_INTERVAL overrides it for a session)
        static const char *const fr[] = {"30 (as on the Switch)", "60"};
        int fi = g_settings.frame_rate == 30 ? 0 : 1;
        char fd[400];
        int fn = snprintf(fd, sizeof fd,
                          "The game runs at 30 frames per second on the Switch; here it runs at 60 by default. 30 can "
                          "be steadier and save battery on Steam Deck. The game plays at the same speed either way.");
        if (getenv("LARECOMP_PRESENT_INTERVAL")) {
            snprintf(fd + fn, sizeof fd - (size_t)fn, "\n\nSet for this session by LARECOMP_PRESENT_INTERVAL.");
            opt_disable_next();
        }
        if (opt_choice("Frame rate", &fi, fr, 2, 1, fd)) g_settings.frame_rate = fi ? 60 : 30;
    }

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "900p", "1x", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, RES_900, 1, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, "How sharp the image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: the Switch's 1080p times that. "
                                      "75%% / 50%% / 25%%: a share of the display, for slower machines.\n\nRendering at "
                                      "%.0f x %.0f.", 1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // The default is MetalFX on macOS and AMD FSR 1 elsewhere.
        constexpr int up_default = 2;
#ifdef __APPLE__
        static const char *ups[] = {"None - Bilinear", "Integer Scaling", "MetalFX"};
        static const int upv[] = {UPSCALE_BILINEAR, UPSCALE_INTEGER, UPSCALE_METALFX};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Integer Scaling: whole-number zoom with black borders for sharp pixels. "
                         "Bilinear: smooths between pixels.";
#else
        static const char *ups[] = {"None - Bilinear", "Integer Scaling", "AMD FSR 1"};
        static const int upv[] = {UPSCALE_BILINEAR, UPSCALE_INTEGER, UPSCALE_FSR1};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening. Integer Scaling: whole-number zoom with black borders for "
                         "sharp, even pixels (falls back to smooth scaling with Dynamic resolution or downsampling). "
                         "Bilinear: smooths between pixels.";
#endif
        int ui = up_default;
        for (int i = 0; i < 3; i++)
            if (g_settings.upscaler == upv[i]) { ui = i; break; }
        char udm[512];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 3, up_default, ud)) g_settings.upscaler = upv[ui];
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");
    static const char *waits[] = {"Off", "Balanced", "Full"};
    opt_choice("Wait for shaders", &g_settings.shader_wait, waits, 3, SHADER_WAIT_BALANCED,
               "What happens when something appears whose shaders aren't ready yet. Shaders seen in earlier sessions "
               "are prepared while the game starts, so this mostly matters for things you're seeing for the first "
               "time.\n\nBalanced: waits a few ms, then draws without them. Full: waits up to 50 ms, so nothing pops "
               "in, but a new area may hitch. Off: never waits; things can pop in for a frame or two.");

    opt_end();
}

// Cosmetics: tunic / heart colours, live (runtime/nvn/texpatch.c); never part of a seed
static void tab_cosmetics(void) {
    opt_begin("##cosmetics", ImGui::GetFontSize() / 34);
    opt_section("Colors");
    opt_choice("Tunic color", &g_settings.tunic, g_tunic_colours, N_TUNIC_COLOURS, 0,
               "Link's tunic color. Default: the game's own (or the randomizer seed's).");
    opt_choice("Heart color", &g_settings.hearts, g_heart_colours, N_HEART_COLOURS, 0,
               "The color of the hearts on the HUD.");
    if (opt_action("Random colors", "", "Picks a random tunic and heart color (any other color, Default included).")) {
        g_settings.tunic = (g_settings.tunic + 1 + rand() % (N_TUNIC_COLOURS - 1)) % N_TUNIC_COLOURS;
        g_settings.hearts = (g_settings.hearts + 1 + rand() % (N_HEART_COLOURS - 1)) % N_HEART_COLOURS;
    }
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Room reverb", &g_settings.reverb, true,
               "The game's room acoustics: the echo in halls, the cathedral and other large spaces.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's animated cutscenes. Applies the next time a movie starts.");
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0;
    const char *lost = in_game ? "\n\nProgress since your last save is lost." : "";
    char d1[200], d2[200];
    snprintf(d1, sizeof d1, "Leaves the game and goes back to the " RT_APP_NAME " start screen.%s", lost);
    snprintf(d2, sizeof d2, "Closes " RT_APP_NAME ".%s", lost);
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action("Quit to Start Screen", "", in_game ? d1 : "You're on the start screen.")) ask = 1;
        if (opt_action("Quit to Desktop", "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? "Yes, quit to Start Screen" : "Yes, quit to Desktop", "", ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(2);
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;
    }
    opt_end();
}

// Game: what the game itself is told (its language). Amiibo and cheats live here too.
static void tab_game(void) {
    static bool nfp_scanned = false;
    if (!nfp_scanned || ImGui::IsWindowAppearing()) {
        nfp_scanned = true;
        nfp_rescan_amiibos();
    }
    opt_begin("##game", ImGui::GetFontSize() / 34);
    opt_section("System");
    // what the game is told the console's language is: its text follows this (runtime/settings.c)
    char autol[64];
    snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
    const char *names[N_LANGUAGES];
    names[0] = autol;
    for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
    if (opt_choice("Language", &g_settings.language, names, N_LANGUAGES, 0,
                   "The console language the game is told, which picks its text language (the voice language is in "
                   "the game's own settings). Applies when the game restarts: Quit to Start Screen, then Play."))
        settings_save();
    // Amiibo (Amiibo/ folder in the data root): runtime/services/nfp.c
    opt_section("Amiibo");
    {
        static NfpAmiiboInfo amiibos[NFP_MAX_ENTRIES];
        static uint64_t amiibo_gen = 0;
        NfpUiState ast;
        nfp_ui_snapshot(amiibos, NFP_MAX_ENTRIES, &amiibo_gen, &ast);

        const char *anames[NFP_MAX_ENTRIES + 1];
        anames[0] = "None";
        for (int i = 0; i < ast.count; i++) anames[1 + i] = amiibos[i].name;

        char adesc[768];
        if (ast.count == 0) {
            snprintf(adesc, sizeof adesc,
                     "No valid Amiibo .bin files found.\n\n"
                     "Place compatible .bin dumps inside:\n%s/Amiibo/\n"
                     "then reopen this menu to pick one. Visiting the Garreg Mach Monastery Amiibo Gazebo with None selected is safe.",
                     rt_data_root());
        } else if (ast.selected_ui == 0) {
            snprintf(adesc, sizeof adesc,
                     "Choose a compatible Amiibo .bin from %s/Amiibo/ (%d detected).\n\n"
                     "With None selected, visiting the Garreg Mach Monastery Amiibo Gazebo waits safely without crashing. "
                     "After a completed gazebo scan, selection automatically returns to None.",
                     rt_data_root(), ast.count);
        } else {
            uint16_t char_id = (uint16_t)((ast.active_tag.char_id[0] << 8) | ast.active_tag.char_id[1]);
            const char *state_note = (ast.dev_state == 1 || ast.dev_state == 2 || ast.dev_state == 4)
                ? "Gazebo active: ready for immediate read."
                : "Armed: stays selected until read at the Garreg Mach Monastery Amiibo Gazebo (resets to None only after a completed read).";
            snprintf(adesc, sizeof adesc,
                     "File: %s\nCharacter ID: 0x%04X   Numbering ID: 0x%04X\n\n%s",
                     ast.active_tag.rel_path, char_id, ast.active_tag.numbering_id, state_note);
        }

        int sel = ast.selected_ui;
        if (opt_dropdown("Amiibo", &sel, anames, 1 + ast.count, 0, adesc)) nfp_ui_select(sel);
    }
    // cheats.txt (Atmosphere's format, in the project folder): runtime/hle/cheatvm.c
    int nc = cheatvm_count();
    opt_section("Cheats");
    if (nc == 0)
        opt_action("No cheats", "", "Put Atmosphere-format cheats in cheats.txt next to " RT_APP_NAME " (a [name] line, "
                   "then its codes), then restart. Cheats that patch the game's code also need the game code rebuilt "
                   "(Settings > Game Files > Rebuild game code; or from a terminal: `manage.py rebuild --root <install dir>`; "
                   "in a source checkout: `make gen-main` and `make`).");
    for (int i = 0; i < nc; i++) {
        CheatInfo ci;
        if (!cheatvm_info(i, &ci) || ci.master) continue;
        bool on = ci.on;
        std::string d = ci.desc && *ci.desc ? ci.desc : "From cheats.txt (no description: add # lines under its name).";
        if (ci.excludes && *ci.excludes) d += std::string("\n\nOnly one at a time: switching this on turns off ") + ci.excludes + ".";
        if (ci.unsupported) d += "\n\nThis cheat uses an opcode the cheat engine doesn't support, so it can't run.";
        else if (ci.needs_gen) d += gamebuild_available()
            ? "\n\nIt patches the game's code, and that patch isn't built in yet: Quit to Start Screen, then Settings > "
              "Game Files > Rebuild game code, and restart. You can also run `manage.py rebuild --root <install dir>` from a terminal."
            : "\n\nIt patches the game's code, and that patch isn't built in yet: run `make gen-main` and then `make`, "
              "and restart.";
        if (ci.unsupported || ci.needs_gen) opt_disable_next();
        if (opt_toggle(ci.name, &on, false, d.c_str())) { cheatvm_set(i, on); settings_save(); }
    }
    opt_end();
}

static void tab_about(void) {
    // (option rows fill the height they're given: the mouse rows get just theirs, the key list follows below)
    float fs = ImGui::GetFontSize(), sc = fs / 34;
    ImGui::BeginChild("##mousebox", ImVec2(0, fs * 10.6f + 44 * sc), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    opt_begin("##mouse", sc);
    opt_section("Mouse");
    static const char *mcam[] = {"Off", "Hold right button", "Always"};
    opt_choice("Mouse camera", &g_settings.mouse_cam, mcam, 3, MOUSE_CAM_HOLD,
               "Turn the camera with the mouse. Hold right button: while the right mouse button is held. Always: the "
               "cursor is hidden and the mouse always turns the camera, and right click is B (back). Left click is A "
               "(confirm) and the wheel steps up / down through lists (with Shift, or tilted: left / right), unless this is Off. The menu (F1 / Esc) gets the cursor back.");
    opt_slider("Mouse sensitivity", &g_settings.mouse_sens, 0.25f, 4.0f, 0.25f, 1.0f, "%gx", "How fast the mouse turns the camera.");
    opt_toggle("Invert mouse up / down", &g_settings.mouse_invert, false, "Moving the mouse up tilts the camera the other way.");
    opt_section("Controller");
    opt_toggle("Back + Start opens this menu", &g_settings.pad_menu_combo, true,
               "Holding the controller's Back and Start (Minus and Plus) together opens and closes this menu. It never "
               "does while L or R is held, so the game's soft reset (L + R + Start + Back) works either way. Off: only "
               "F1, Esc or the controller's Home / Xbox / PlayStation button open the menu (on a Steam Deck, keep this "
               "on, or use a keyboard key: its Steam button belongs to Steam).");
    opt_end();
    ImGui::EndChild();
    header("Controls");
    controls_bindings_draw(ImGui::GetFontSize() / 34);
    ImGui::Spacing();
    header("Menu");
    ImGui::TextUnformatted("Open / close this menu:  F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start");
    ImGui::TextUnformatted("(Back + Start: not while L / R are held - the game's soft reset - or when turned off above)");
    ImGui::TextUnformatted("Switch tabs:  L / R  (Q / E)        Back:  B  (Esc)");
    ImGui::TextUnformatted("Fullscreen:  F11 / Alt+Enter");
}

extern "C" bool inventory_overlay_wanted(void);
extern "C" bool tracker_overlay_wanted(void);
extern "C" void tracker_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool chaos_overlay_wanted(void);
extern "C" void chaos_overlay_draw(ImVec2 display, ImFont *font);
extern "C" void inventory_overlay_draw(ImVec2 display, ImFont *font);

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(THEME_GOLD, "Enter your name");
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        text_utf8_truncate(g_kbd_text, g_kbd_max);
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", text_utf8_count(g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    text_utf8_truncate(g_kbd_text, g_kbd_max);
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Game Files", -1, true};
    static const char *names[] = {"Video", "Audio", "Save Editor", "Randomizer", "Warp", "Cheats", "Game",
                                  "Cosmetics", "Controls", "Quit"};
    // Controls, Video, Audio, Game, Quit (ids stay: test hooks). Link's Awakening's pages - Save Editor, Randomizer,
    // Warp, its Cheats and Cosmetics - aren't shown: they read and patch that game's memory.
    static const int order[] = {8, 0, 1, 6, 9};
    for (int i : order)  // (Quit: in game only - the start screen has its own QUIT, and an eleventh tab cut names short)
        if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, in_game || (i != 2 && i != 4 && i != 5)};
    if (prelaunch) tabs[ntabs++] = {"Mods", 10, true};  // (start screen only, last: mods apply when the game starts)
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor on the tab bar goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (focused_tab != -99) g_focus_tab = tabs[i].id, g_focus_frames = 0;
        }
    }
    // Down with the cursor on the tab bar: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible && !any_popup &&
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    opt_focus_first(down_from_tab);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            snprintf(lbl, sizeof lbl, prelaunch ? " %s " : "  %s  ", t.name);  // (ten tabs: tighter, so none is cut short)
            ImGui::BeginDisabled(!t.on);  // (out of the game: Save Editor, Warp, Cheats)
            // focus (controller / keyboard): the tab lights up a little with a gold bar along its bottom, instead of
            // ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (lit) {
                ImGui::PushStyleColor(ImGuiCol_Tab, THEME_HOVER);
                ImGui::PushStyleColor(ImGuiCol_TabHovered, THEME_HOVER);
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(lit ? 3 : 1);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 4 * scale > 2 ? 4 * scale : 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - h), ImVec2(b.x - 2, b.y),
                                                          THEME_GOLD_U32);  // (the menu's gold)
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                // Down from the tab bar, pages that aren't option rows (Prelaunch, Controls, Warp): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8 || t.id == 4)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                // Down from the tab bar, pages that aren't option rows (Prelaunch, Controls, Warp): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8 || t.id == 4)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2:
                    if (game_in_play()) save_editor_draw(scale);
                    else ImGui::TextDisabled("Load a save file and get into the game to edit it.");
                    break;
                case 3: rando_tab_draw(scale); break;
                case 4: if (warp_tab_draw(scale)) set_open(false); break;
                case 5: cheats_draw(scale); break;
                case 6: tab_game(); break;
                case 7: tab_cosmetics(); break;
                case 8: tab_about(); break;
                case 9: tab_quit(scale, in_game); break;
                case 10: launcher_mods_tab(scale); break;
                }
                static const char *scroll_test = getenv("LARECOMP_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first(false);  // (a page without option rows: nothing took it)
        ImGui::EndTabBar();
    }
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load();
    bool inv = !open && inventory_overlay_wanted();
    bool trk = !open && tracker_overlay_wanted();
    bool chaos = !open && chaos_overlay_wanted();
    bool kbd = g_kbd.load() == 1;
    if (!g_notice_active.load() && !open && !kbd && !g_settings.show_fps && !inv && !trk && !chaos) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifndef __APPLE__
    if (kbd && kor_fonts_wanted()) {  // name entry in Korean, fonts built without Hangul: rebuild them with it
        io.Fonts->Clear();
        load_fonts(io);
        ImGui_ImplVulkan_CreateFontsTexture();  // (waits for the GPU, replaces the old font texture)
    }
#endif
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (g_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps", io.Framerate);
        ImGui::End();
    }

    if (trk) tracker_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (chaos) chaos_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (inv) {
        inventory_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    }

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        theme_trim(scale);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        ImGui::PushFont(g_font_big);
        ImGui::TextColored(THEME_GOLD, "Settings");
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? THEME_GOLD_U32 : THEME_TEXT_DIM_U32;
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    if (g_notice_active.load()) {
        char message[sizeof notice_text];
        pthread_mutex_lock(&notice_lock);
        snprintf(message, sizeof message, "%s", notice_text);
        pthread_mutex_unlock(&notice_lock);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f), ImGuiCond_Always, ImVec2(.5f,.5f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * .88f, io.DisplaySize.y * .7f), ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        ImGui::Begin("Save warning", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::BeginChild("##notice_text", ImVec2(0, -70 * scale));
        ImGui::TextWrapped("%s", message);
        ImGui::EndChild();
        if (ImGui::Button("Aceptar / A / Enter", ImVec2(-1, 50 * scale))) g_notice_active.store(false);
        ImGui::End();
    }
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
#endif
}
