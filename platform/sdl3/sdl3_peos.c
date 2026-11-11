// SDL3 on PeOS (d2decomped port, phase 4): the part of SDL 3.2's API that
// ports use, written over Starlight, with SDL's own headers (3.2.26, zlib
// license: the types, events and key codes are SDL's exactly).
//
// The window is the body of a Starlight window whose pixels are the app's
// own (SLUI SDK 1.0.7 and later, as Abisso's Lume): the app needs the "shm"
// capability. The renderer is in software: textures in memory, and at
// SDL_RenderPresent the last texture drawn is converted to Starlight's
// XRGB8888 and scaled (nearest) into the body, letterboxed under the
// logical presentation. Events come from Starlight's (keys as PC set-1
// scan codes, the pointer over the pixels, typed characters).
//
// Not here yet: gamepads and joysticks (none are found), colour cursors
// (NULL: the app draws its own), a window's fullscreen (Starlight decides),
// SDL_GPU. Audio devices are not SDL's on PeOS: WAV loading and sample
// conversion are, for apps that mix themselves.
#include <SDL3/SDL.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <peos/peos.h>
#include <peos/starlight_ui.h>
#include <peos/sysinfo_abi.h>
#include <peos/thread.h>

// ---- errors, hints, init ----

static char g_error[256];
static uint64_t g_start_ns;

bool SDL_SetError(SDL_PRINTF_FORMAT_STRING const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
    return false;
}

const char *SDL_GetError(void) { return g_error; }

bool SDL_SetHint(const char *name, const char *value) {
    (void)name;
    (void)value;
    return true;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool SDL_InitSubSystem(SDL_InitFlags flags) {
    (void)flags;
    if (!g_start_ns) g_start_ns = now_ns();
    return true;
}

bool SDL_Init(SDL_InitFlags flags) { return SDL_InitSubSystem(flags); }

void SDL_Quit(void) {}

int SDL_GetVersion(void) { return SDL_VERSION; }

const char *SDL_GetPlatform(void) { return "PeOS"; }

const char *SDL_GetCurrentVideoDriver(void) { return "peos"; }

void SDL_free(void *mem) { free(mem); }

int SDL_setenv_unsafe(const char *name, const char *value, int overwrite) {
    return setenv(name, value, overwrite);
}

// ---- time ----

Uint64 SDL_GetTicksNS(void) {
    if (!g_start_ns) g_start_ns = now_ns();
    return now_ns() - g_start_ns;
}

Uint64 SDL_GetTicks(void) { return SDL_GetTicksNS() / 1000000ull; }

void SDL_DelayNS(Uint64 ns) {
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

void SDL_Delay(Uint32 ms) { SDL_DelayNS((Uint64)ms * 1000000ull); }

// PeOS's sleep wakes on its timer to the nanosecond asked (the address wait's
// timeout): no spinning needed for a precise one.
void SDL_DelayPrecise(Uint64 ns) { SDL_DelayNS(ns); }

// ---- the system ----

int SDL_GetNumLogicalCPUCores(void) { return (int)peos_cpu_count(); }

int SDL_GetSystemRAM(void) {
    peos_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (peos_sysinfo(&info) < 0) return 0;
    return (int)(info.ram_total / (1024u * 1024u));
}

const char *SDL_GetBasePath(void) {
    static char path[PEOS_PATH_MAX + 2];
    if (!path[0]) {
        if (!getcwd(path, PEOS_PATH_MAX)) strcpy(path, "/");
        size_t len = strlen(path);
        if (len == 0 || path[len - 1] != '/') strcat(path, "/");
    }
    return path;
}

bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window) {
    (void)flags;
    (void)window;
    fprintf(stderr, "%s: %s\n", title ? title : "", message ? message : "");
    return true;
}

// ---- properties (none: every lookup answers its default) ----

SDL_PropertiesID SDL_GetRendererProperties(SDL_Renderer *renderer) { return renderer ? 1u : 0u; }

void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value) {
    (void)props;
    (void)name;
    return default_value;
}

const char *SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value) {
    (void)props;
    (void)name;
    return default_value;
}

const char *SDL_GetGPUDeviceDriver(SDL_GPUDevice *device) {
    (void)device;
    return NULL;
}

SDL_PropertiesID SDL_GetGPUDeviceProperties(SDL_GPUDevice *device) {
    (void)device;
    return 0;
}

// ---- events ----

#define EVENT_QUEUE 512
static SDL_Event g_events[EVENT_QUEUE];
static unsigned g_event_head, g_event_tail;

static void push(const SDL_Event *event) {
    unsigned next = (g_event_tail + 1u) % EVENT_QUEUE;
    if (next == g_event_head) return;   // full: the oldest are what the app reads first
    g_events[g_event_tail] = *event;
    g_event_tail = next;
}

bool SDL_PushEvent(SDL_Event *event) {
    if (!event) return false;
    if (!event->common.timestamp) event->common.timestamp = SDL_GetTicksNS();
    push(event);
    return true;
}

// ---- the window: Starlight's body, its pixels ----

struct SDL_Window {
    SDL_WindowID id;
    SDL_WindowFlags flags;
    uint32_t body_w, body_h;     // from Starlight (SLUI_EVENT_RESIZE)
    uint32_t surf_w, surf_h;     // the pixels asked for (a little larger)
    int surface_ready;
    int frame_sent;              // the SLUI frame that shows the pixels
    int text_input;              // SDL_StartTextInput
};

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;

static void send_frame(SDL_Window *w) {
    slui_begin_frame();
    if (w->surface_ready) slui_pixels(0u, 0u, w->body_w, w->body_h, 0u, 0u);
    else slui_rect(0u, 0u, w->body_w ? w->body_w : 1u, w->body_h ? w->body_h : 1u, 0u, 0u, 0u);
    slui_end_frame();
    w->frame_sent = 1;
}

static void window_event(SDL_EventType type, int data1, int data2) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.window.type = type;
    e.window.timestamp = SDL_GetTicksNS();
    e.window.windowID = g_window ? g_window->id : 0;
    e.window.data1 = data1;
    e.window.data2 = data2;
    push(&e);
}

static void layout(SDL_Window *w) {
    uint32_t bw = 0, bh = 0;
    slui_viewport(&bw, &bh);
    if (!bw || !bh) return;
    int changed = bw != w->body_w || bh != w->body_h;
    w->body_w = bw;
    w->body_h = bh;
    if (bw > w->surf_w || bh > w->surf_h || !w->surface_ready) {
        if (bw > w->surf_w) w->surf_w = bw + 64u;
        if (bh > w->surf_h) w->surf_h = bh + 64u;
        w->surface_ready = 0;
        slui_pixels_open(w->surf_w, w->surf_h);
    }
    w->frame_sent = 0;
    if (changed) {
        window_event(SDL_EVENT_WINDOW_RESIZED, (int)bw, (int)bh);
        window_event(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, (int)bw, (int)bh);
    }
}

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags) {
    (void)title;
    if (g_window) {
        SDL_SetError("PeOS: one window per app");
        return NULL;
    }
    SDL_Window *win = calloc(1, sizeof(*win));
    if (!win) {
        SDL_SetError("out of memory");
        return NULL;
    }
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    win->id = 1;
    win->flags = (flags & (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)) | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    // the size asked until Starlight says the body's (app.ini sets the window)
    win->body_w = w > 0 ? (uint32_t)w : 800u;
    win->body_h = h > 0 ? (uint32_t)h : 600u;
    g_window = win;
    send_frame(win);   // says hello: Starlight answers with the body's size
    win->body_w = win->body_h = 0;
    for (int i = 0; i < 200 && !win->body_w; i++) {   // up to 2 s for the first size
        slui_event_t ev;
        while (slui_poll_event(&ev)) {
            if (ev.type == SLUI_EVENT_RESIZE) layout(win);
            else if (ev.type == SLUI_EVENT_PIXELS) win->surface_ready = ev.value > 0u && ev.value2 > 0u;
        }
        if (!win->body_w) SDL_Delay(10);
    }
    if (!win->body_w) {   // no Starlight: keep the size asked
        win->body_w = w > 0 ? (uint32_t)w : 800u;
        win->body_h = h > 0 ? (uint32_t)h : 600u;
    }
    return win;
}

void SDL_DestroyWindow(SDL_Window *window) {
    if (!window) return;
    if (window == g_window) g_window = NULL;
    free(window);
}

SDL_WindowID SDL_GetWindowID(SDL_Window *window) { return window ? window->id : 0; }

SDL_Window *SDL_GetWindowFromEvent(const SDL_Event *event) {
    (void)event;
    return g_window;
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window) { return window ? window->flags : 0; }

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen) {
    (void)window;
    (void)fullscreen;
    return SDL_SetError("PeOS: Starlight sizes the window");
}

bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) {
    (void)window;
    (void)icon;
    return true;   // the app bundle's icon is the window's
}

bool SDL_RaiseWindow(SDL_Window *window) {
    (void)window;
    return true;
}

bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h) {
    if (!window) return SDL_SetError("no window");
    if (w) *w = (int)window->body_w;
    if (h) *h = (int)window->body_h;
    return true;
}

float SDL_GetWindowPixelDensity(SDL_Window *window) { return window ? 1.0f : 0.0f; }

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window) { return window ? 1u : 0u; }

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID display) {
    static SDL_DisplayMode mode;
    if (display != 1u) {
        SDL_SetError("no such display");
        return NULL;
    }
    mode.displayID = 1u;
    mode.format = SDL_PIXELFORMAT_XRGB8888;
    mode.w = g_window ? (int)g_window->body_w : 0;
    mode.h = g_window ? (int)g_window->body_h : 0;
    mode.pixel_density = 1.0f;
    mode.refresh_rate = 60.0f;
    mode.refresh_rate_numerator = 60;
    mode.refresh_rate_denominator = 1;
    return &mode;
}

bool SDL_StartTextInput(SDL_Window *window) {
    if (window) window->text_input = 1;
    return true;
}

bool SDL_StopTextInput(SDL_Window *window) {
    if (window) window->text_input = 0;
    return true;
}

bool SDL_SetTextInputArea(SDL_Window *window, const SDL_Rect *rect, int cursor) {
    (void)window;
    (void)rect;
    (void)cursor;
    return true;
}

// ---- keyboard ----

// PC set-1 scan codes (PeOS's key codes; 0xE0xx the extended ones, and
// PeOS's arrows and the like also without the 0xE0) to SDL's scan codes.
static SDL_Scancode scancode_of(uint16_t code) {
    static const SDL_Scancode plain[0x59] = {
        [0x01] = SDL_SCANCODE_ESCAPE, [0x02] = SDL_SCANCODE_1, [0x03] = SDL_SCANCODE_2, [0x04] = SDL_SCANCODE_3,
        [0x05] = SDL_SCANCODE_4, [0x06] = SDL_SCANCODE_5, [0x07] = SDL_SCANCODE_6, [0x08] = SDL_SCANCODE_7,
        [0x09] = SDL_SCANCODE_8, [0x0A] = SDL_SCANCODE_9, [0x0B] = SDL_SCANCODE_0, [0x0C] = SDL_SCANCODE_MINUS,
        [0x0D] = SDL_SCANCODE_EQUALS, [0x0E] = SDL_SCANCODE_BACKSPACE, [0x0F] = SDL_SCANCODE_TAB,
        [0x10] = SDL_SCANCODE_Q, [0x11] = SDL_SCANCODE_W, [0x12] = SDL_SCANCODE_E, [0x13] = SDL_SCANCODE_R,
        [0x14] = SDL_SCANCODE_T, [0x15] = SDL_SCANCODE_Y, [0x16] = SDL_SCANCODE_U, [0x17] = SDL_SCANCODE_I,
        [0x18] = SDL_SCANCODE_O, [0x19] = SDL_SCANCODE_P, [0x1A] = SDL_SCANCODE_LEFTBRACKET,
        [0x1B] = SDL_SCANCODE_RIGHTBRACKET, [0x1C] = SDL_SCANCODE_RETURN, [0x1D] = SDL_SCANCODE_LCTRL,
        [0x1E] = SDL_SCANCODE_A, [0x1F] = SDL_SCANCODE_S, [0x20] = SDL_SCANCODE_D, [0x21] = SDL_SCANCODE_F,
        [0x22] = SDL_SCANCODE_G, [0x23] = SDL_SCANCODE_H, [0x24] = SDL_SCANCODE_J, [0x25] = SDL_SCANCODE_K,
        [0x26] = SDL_SCANCODE_L, [0x27] = SDL_SCANCODE_SEMICOLON, [0x28] = SDL_SCANCODE_APOSTROPHE,
        [0x29] = SDL_SCANCODE_GRAVE, [0x2A] = SDL_SCANCODE_LSHIFT, [0x2B] = SDL_SCANCODE_BACKSLASH,
        [0x2C] = SDL_SCANCODE_Z, [0x2D] = SDL_SCANCODE_X, [0x2E] = SDL_SCANCODE_C, [0x2F] = SDL_SCANCODE_V,
        [0x30] = SDL_SCANCODE_B, [0x31] = SDL_SCANCODE_N, [0x32] = SDL_SCANCODE_M, [0x33] = SDL_SCANCODE_COMMA,
        [0x34] = SDL_SCANCODE_PERIOD, [0x35] = SDL_SCANCODE_SLASH, [0x36] = SDL_SCANCODE_RSHIFT,
        [0x37] = SDL_SCANCODE_KP_MULTIPLY, [0x38] = SDL_SCANCODE_LALT, [0x39] = SDL_SCANCODE_SPACE,
        [0x3A] = SDL_SCANCODE_CAPSLOCK, [0x3B] = SDL_SCANCODE_F1, [0x3C] = SDL_SCANCODE_F2, [0x3D] = SDL_SCANCODE_F3,
        [0x3E] = SDL_SCANCODE_F4, [0x3F] = SDL_SCANCODE_F5, [0x40] = SDL_SCANCODE_F6, [0x41] = SDL_SCANCODE_F7,
        [0x42] = SDL_SCANCODE_F8, [0x43] = SDL_SCANCODE_F9, [0x44] = SDL_SCANCODE_F10,
        [0x45] = SDL_SCANCODE_NUMLOCKCLEAR, [0x46] = SDL_SCANCODE_SCROLLLOCK,
        // PeOS gives its arrows, Home, End, Page Up and Down also without
        // the 0xE0 (PEOS_KEY_*_NORM): those are the navigation keys
        [0x47] = SDL_SCANCODE_HOME, [0x48] = SDL_SCANCODE_UP, [0x49] = SDL_SCANCODE_PAGEUP,
        [0x4A] = SDL_SCANCODE_KP_MINUS, [0x4B] = SDL_SCANCODE_LEFT, [0x4C] = SDL_SCANCODE_KP_5,
        [0x4D] = SDL_SCANCODE_RIGHT, [0x4E] = SDL_SCANCODE_KP_PLUS, [0x4F] = SDL_SCANCODE_END,
        [0x50] = SDL_SCANCODE_DOWN, [0x51] = SDL_SCANCODE_PAGEDOWN, [0x52] = SDL_SCANCODE_INSERT,
        [0x53] = SDL_SCANCODE_DELETE, [0x56] = SDL_SCANCODE_NONUSBACKSLASH, [0x57] = SDL_SCANCODE_F11,
        [0x58] = SDL_SCANCODE_F12,
    };
    if (code < 0x59u) return plain[code];
    switch (code) {
        case 0xE01C: return SDL_SCANCODE_KP_ENTER;
        case 0xE01D: return SDL_SCANCODE_RCTRL;
        case 0xE035: return SDL_SCANCODE_KP_DIVIDE;
        case 0xE038: return SDL_SCANCODE_RALT;
        case 0xE047: return SDL_SCANCODE_HOME;
        case 0xE048: return SDL_SCANCODE_UP;
        case 0xE049: return SDL_SCANCODE_PAGEUP;
        case 0xE04B: return SDL_SCANCODE_LEFT;
        case 0xE04D: return SDL_SCANCODE_RIGHT;
        case 0xE04F: return SDL_SCANCODE_END;
        case 0xE050: return SDL_SCANCODE_DOWN;
        case 0xE051: return SDL_SCANCODE_PAGEDOWN;
        case 0xE052: return SDL_SCANCODE_INSERT;
        case 0xE053: return SDL_SCANCODE_DELETE;
        case 0xE05B: return SDL_SCANCODE_LGUI;
        case 0xE05C: return SDL_SCANCODE_RGUI;
        case 0xE05D: return SDL_SCANCODE_APPLICATION;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}

// The US layout's keys, as SDL's default keymap (unshifted).
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode, SDL_Keymod modstate, bool key_event) {
    (void)modstate;
    (void)key_event;
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) return (SDL_Keycode)('a' + (scancode - SDL_SCANCODE_A));
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) return (SDL_Keycode)('1' + (scancode - SDL_SCANCODE_1));
    switch (scancode) {
        case SDL_SCANCODE_0: return SDLK_0;
        case SDL_SCANCODE_RETURN: return SDLK_RETURN;
        case SDL_SCANCODE_ESCAPE: return SDLK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
        case SDL_SCANCODE_TAB: return SDLK_TAB;
        case SDL_SCANCODE_SPACE: return SDLK_SPACE;
        case SDL_SCANCODE_MINUS: return SDLK_MINUS;
        case SDL_SCANCODE_EQUALS: return SDLK_EQUALS;
        case SDL_SCANCODE_LEFTBRACKET: return SDLK_LEFTBRACKET;
        case SDL_SCANCODE_RIGHTBRACKET: return SDLK_RIGHTBRACKET;
        case SDL_SCANCODE_BACKSLASH: return SDLK_BACKSLASH;
        case SDL_SCANCODE_NONUSBACKSLASH: return SDLK_BACKSLASH;
        case SDL_SCANCODE_SEMICOLON: return SDLK_SEMICOLON;
        case SDL_SCANCODE_APOSTROPHE: return SDLK_APOSTROPHE;
        case SDL_SCANCODE_GRAVE: return SDLK_GRAVE;
        case SDL_SCANCODE_COMMA: return SDLK_COMMA;
        case SDL_SCANCODE_PERIOD: return SDLK_PERIOD;
        case SDL_SCANCODE_SLASH: return SDLK_SLASH;
        default: return SDL_SCANCODE_TO_KEYCODE(scancode);   // what SDL's keymap does for the rest
    }
}

static bool g_keys[SDL_SCANCODE_COUNT];
static SDL_Keymod g_mods;

const bool *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = SDL_SCANCODE_COUNT;
    return g_keys;
}

SDL_Keymod SDL_GetModState(void) { return g_mods; }

static SDL_Keymod mod_of(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_LSHIFT: return SDL_KMOD_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return SDL_KMOD_RSHIFT;
        case SDL_SCANCODE_LCTRL: return SDL_KMOD_LCTRL;
        case SDL_SCANCODE_RCTRL: return SDL_KMOD_RCTRL;
        case SDL_SCANCODE_LALT: return SDL_KMOD_LALT;
        case SDL_SCANCODE_RALT: return SDL_KMOD_RALT;
        case SDL_SCANCODE_LGUI: return SDL_KMOD_LGUI;
        case SDL_SCANCODE_RGUI: return SDL_KMOD_RGUI;
        default: return SDL_KMOD_NONE;
    }
}

static void key_event(uint16_t code, bool down, bool repeat) {
    SDL_Scancode sc = scancode_of(code);
    if (sc == SDL_SCANCODE_UNKNOWN) return;
    g_keys[sc] = down;
    SDL_Keymod m = mod_of(sc);
    if (m) g_mods = down ? (SDL_Keymod)(g_mods | m) : (SDL_Keymod)(g_mods & ~m);
    if (sc == SDL_SCANCODE_CAPSLOCK && down && !repeat) g_mods ^= SDL_KMOD_CAPS;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = g_window ? g_window->id : 0;
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, g_mods, true);
    e.key.mod = g_mods;
    e.key.raw = code;
    e.key.down = down;
    e.key.repeat = repeat;
    push(&e);
}

// devctl's names: a letter or digit, else the few SDL names it uses
SDL_Keycode SDL_GetKeyFromName(const char *name) {
    if (!name || !*name) return SDLK_UNKNOWN;
    if (!name[1]) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return (SDL_Keycode)(unsigned char)c;
    }
    static const struct { const char *name; SDL_Keycode key; } named[] = {
        { "Return", SDLK_RETURN }, { "Escape", SDLK_ESCAPE }, { "Backspace", SDLK_BACKSPACE },
        { "Tab", SDLK_TAB }, { "Space", SDLK_SPACE }, { "Up", SDLK_UP }, { "Down", SDLK_DOWN },
        { "Left", SDLK_LEFT }, { "Right", SDLK_RIGHT }, { "F1", SDLK_F1 }, { "F2", SDLK_F2 },
        { "F3", SDLK_F3 }, { "F4", SDLK_F4 }, { "F5", SDLK_F5 }, { "F6", SDLK_F6 }, { "F7", SDLK_F7 },
        { "F8", SDLK_F8 }, { "F9", SDLK_F9 }, { "F10", SDLK_F10 }, { "F11", SDLK_F11 }, { "F12", SDLK_F12 },
        { "Left Shift", SDLK_LSHIFT }, { "Left Ctrl", SDLK_LCTRL }, { "Left Alt", SDLK_LALT },
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (!strcasecmp(name, named[i].name)) return named[i].key;
    return SDLK_UNKNOWN;
}

// ---- mouse ----

static float g_mouse_x, g_mouse_y;
static SDL_MouseButtonFlags g_buttons;
static bool g_cursor_shown = true;

SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) {
    if (x) *x = g_mouse_x;
    if (y) *y = g_mouse_y;
    return g_buttons;
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y) {
    (void)window;
    // Starlight moves the pointer, not the app: where the app thinks it is
    g_mouse_x = x;
    g_mouse_y = y;
}

// Starlight's pointer over the window's pixels: none while hidden (SLUI SDK
// 1.0.15), the arrow otherwise; said again once the window is there.
static void apply_cursor(void) {
    if (g_window) slui_pixels_cursor(g_cursor_shown ? SLUI_CURSOR_ARROW : SLUI_CURSOR_NONE);
}

bool SDL_HideCursor(void) {
    g_cursor_shown = false;
    apply_cursor();
    return true;
}

bool SDL_ShowCursor(void) {
    g_cursor_shown = true;
    apply_cursor();
    return true;
}

SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y) {
    (void)surface;
    (void)hot_x;
    (void)hot_y;
    SDL_SetError("PeOS: no colour cursors (draw it in the frame)");
    return NULL;
}

bool SDL_SetCursor(SDL_Cursor *cursor) {
    (void)cursor;
    return true;
}

void SDL_DestroyCursor(SDL_Cursor *cursor) { (void)cursor; }

static void pointer_event(const slui_event_t *ev) {
    float x = (float)ev->value, y = (float)ev->value2;
    SDL_WindowID wid = g_window ? g_window->id : 0;
    SDL_Event e;
    if (ev->id == SLUI_POINTER_WHEEL) {
        memset(&e, 0, sizeof(e));
        e.wheel.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.timestamp = SDL_GetTicksNS();
        e.wheel.windowID = wid;
        e.wheel.y = (float)ev->wheel;
        e.wheel.integer_y = ev->wheel;
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        e.wheel.mouse_x = x;
        e.wheel.mouse_y = y;
        push(&e);
        return;
    }
    // Starlight's held buttons: bit 0 left, 1 right, 2 middle
    SDL_MouseButtonFlags held = 0;
    if (ev->keycode & 1u) held |= SDL_BUTTON_LMASK;
    if (ev->keycode & 2u) held |= SDL_BUTTON_RMASK;
    if (ev->keycode & 4u) held |= SDL_BUTTON_MMASK;
    if (x != g_mouse_x || y != g_mouse_y) {
        memset(&e, 0, sizeof(e));
        e.motion.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.timestamp = SDL_GetTicksNS();
        e.motion.windowID = wid;
        e.motion.state = held;
        e.motion.x = x;
        e.motion.y = y;
        e.motion.xrel = x - g_mouse_x;
        e.motion.yrel = y - g_mouse_y;
        g_mouse_x = x;
        g_mouse_y = y;
        push(&e);
    }
    static const struct { SDL_MouseButtonFlags mask; Uint8 button; } map[] = {
        { SDL_BUTTON_LMASK, SDL_BUTTON_LEFT }, { SDL_BUTTON_RMASK, SDL_BUTTON_RIGHT }, { SDL_BUTTON_MMASK, SDL_BUTTON_MIDDLE },
    };
    for (size_t i = 0; i < 3; i++) {
        bool was = (g_buttons & map[i].mask) != 0, is = (held & map[i].mask) != 0;
        if (was == is) continue;
        memset(&e, 0, sizeof(e));
        e.button.type = is ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.timestamp = SDL_GetTicksNS();
        e.button.windowID = wid;
        e.button.button = map[i].button;
        e.button.down = is;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
        push(&e);
    }
    g_buttons = held;
}

// ---- text: the characters typed, while the app asked for them ----

#define TEXT_SLOTS 64
static char g_text[TEXT_SLOTS][8];
static unsigned g_text_next;

static void text_event(const slui_event_t *ev) {
    if (!g_window || !g_window->text_input || !ev->text || !ev->text_len || ev->text_len >= 8) return;
    char *slot = g_text[g_text_next++ % TEXT_SLOTS];   // lives while the event is read
    memcpy(slot, ev->text, ev->text_len);
    slot[ev->text_len] = '\0';
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.text.type = SDL_EVENT_TEXT_INPUT;
    e.text.timestamp = SDL_GetTicksNS();
    e.text.windowID = g_window->id;
    e.text.text = slot;
    push(&e);
}

// ---- Starlight's events, read into SDL's ----

static void pump(void) {
    if (!g_window) return;
    slui_event_t ev;
    while (slui_poll_event(&ev)) {
        switch (ev.type) {
            case SLUI_EVENT_RESIZE:
            case SLUI_EVENT_LANGUAGE:
                layout(g_window);
                break;
            case SLUI_EVENT_PIXELS:
                g_window->surface_ready = ev.value > 0u && ev.value2 > 0u;
                g_window->frame_sent = 0;
                window_event(SDL_EVENT_WINDOW_EXPOSED, 0, 0);
                break;
            case SLUI_EVENT_POINTER:
                pointer_event(&ev);
                break;
            case SLUI_EVENT_KEY_DOWN:
                key_event(ev.keycode, true, false);
                break;
            case SLUI_EVENT_KEY_REPEAT:
                key_event(ev.keycode, true, true);
                break;
            case SLUI_EVENT_KEY_UP:
                key_event(ev.keycode, false, false);
                break;
            case SLUI_EVENT_CHAR:
                text_event(&ev);
                break;
            default:
                break;
        }
    }
    if (!g_window->frame_sent && g_window->body_w) send_frame(g_window);
}

void SDL_PumpEvents(void) { pump(); }

bool SDL_PollEvent(SDL_Event *event) {
    if (g_event_head == g_event_tail) pump();
    if (g_event_head == g_event_tail) return false;
    if (event) {
        *event = g_events[g_event_head];
        g_event_head = (g_event_head + 1u) % EVENT_QUEUE;
    }
    return true;
}

// ---- the renderer: in software ----

// SDL 3.2's SDL_Texture is public (format, w, h, refcount): ours begins with it
typedef struct {
    SDL_Texture pub;
    int pitch;
    uint32_t *pixels;
    SDL_ScaleMode scale_mode;
} tex_t;
#define TEX(t) ((tex_t *)(t))

struct SDL_Renderer {
    SDL_Window *window;
    int logical_w, logical_h;
    SDL_RendererLogicalPresentation mode;
    SDL_Texture *shown;          // the last texture drawn since the present
    SDL_FRect shown_src, shown_dst;
    int shown_whole;             // the whole texture over the whole view
    uint32_t *row;               // a source row in XRGB
    int row_cap;
    int *xmap;                   // the body's columns to the texture's
    int xmap_w, xmap_src_w, xmap_dst_w;
};

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name) {
    (void)name;
    if (!window) {
        SDL_SetError("no window");
        return NULL;
    }
    SDL_Renderer *r = calloc(1, sizeof(*r));
    if (!r) {
        SDL_SetError("out of memory");
        return NULL;
    }
    r->window = window;
    g_renderer = r;
    return r;
}

void SDL_DestroyRenderer(SDL_Renderer *renderer) {
    if (!renderer) return;
    if (renderer == g_renderer) g_renderer = NULL;
    free(renderer->row);
    free(renderer->xmap);
    free(renderer);
}

const char *SDL_GetRendererName(SDL_Renderer *renderer) { return renderer ? "software" : NULL; }

bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync) {
    (void)renderer;
    (void)vsync;
    return true;   // Starlight shows the pixels when it can: the app paces itself
}

bool SDL_SetRenderLogicalPresentation(SDL_Renderer *renderer, int w, int h, SDL_RendererLogicalPresentation mode) {
    if (!renderer) return SDL_SetError("no renderer");
    renderer->logical_w = w;
    renderer->logical_h = h;
    renderer->mode = mode;
    return true;
}

// Where the logical view sits in the body, in the body's pixels.
static SDL_FRect view_rect(const SDL_Renderer *r) {
    SDL_FRect rect = { 0.0f, 0.0f, (float)r->window->body_w, (float)r->window->body_h };
    if (r->mode == SDL_LOGICAL_PRESENTATION_DISABLED || r->logical_w <= 0 || r->logical_h <= 0) return rect;
    float bw = (float)r->window->body_w, bh = (float)r->window->body_h;
    float sx = bw / (float)r->logical_w, sy = bh / (float)r->logical_h;
    float s = sx < sy ? sx : sy;
    if (r->mode == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE && s >= 1.0f) s = (float)(int)s;
    if (r->mode == SDL_LOGICAL_PRESENTATION_STRETCH) {
        return rect;
    }
    rect.w = (float)(int)((float)r->logical_w * s);
    rect.h = (float)(int)((float)r->logical_h * s);
    rect.x = (float)(int)((bw - rect.w) / 2.0f);
    rect.y = (float)(int)((bh - rect.h) / 2.0f);
    return rect;
}

bool SDL_GetRenderLogicalPresentationRect(SDL_Renderer *renderer, SDL_FRect *rect) {
    if (!renderer) return SDL_SetError("no renderer");
    if (rect) *rect = view_rect(renderer);
    return true;
}

bool SDL_RenderCoordinatesFromWindow(SDL_Renderer *renderer, float window_x, float window_y, float *x, float *y) {
    if (!renderer) return SDL_SetError("no renderer");
    SDL_FRect v = view_rect(renderer);
    float lw = renderer->logical_w > 0 ? (float)renderer->logical_w : v.w;
    float lh = renderer->logical_h > 0 ? (float)renderer->logical_h : v.h;
    if (x) *x = v.w > 0 ? (window_x - v.x) * lw / v.w : window_x;
    if (y) *y = v.h > 0 ? (window_y - v.y) * lh / v.h : window_y;
    return true;
}

bool SDL_RenderCoordinatesToWindow(SDL_Renderer *renderer, float x, float y, float *window_x, float *window_y) {
    if (!renderer) return SDL_SetError("no renderer");
    SDL_FRect v = view_rect(renderer);
    float lw = renderer->logical_w > 0 ? (float)renderer->logical_w : v.w;
    float lh = renderer->logical_h > 0 ? (float)renderer->logical_h : v.h;
    if (window_x) *window_x = v.x + x * v.w / lw;
    if (window_y) *window_y = v.y + y * v.h / lh;
    return true;
}

bool SDL_ConvertEventToRenderCoordinates(SDL_Renderer *renderer, SDL_Event *event) {
    if (!renderer || !event) return SDL_SetError("no renderer");
    switch (event->type) {
        case SDL_EVENT_MOUSE_MOTION: {
            float x0 = event->motion.x - event->motion.xrel, y0 = event->motion.y - event->motion.yrel;
            SDL_RenderCoordinatesFromWindow(renderer, event->motion.x, event->motion.y, &event->motion.x, &event->motion.y);
            SDL_RenderCoordinatesFromWindow(renderer, x0, y0, &x0, &y0);
            event->motion.xrel = event->motion.x - x0;
            event->motion.yrel = event->motion.y - y0;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            SDL_RenderCoordinatesFromWindow(renderer, event->button.x, event->button.y, &event->button.x, &event->button.y);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            SDL_RenderCoordinatesFromWindow(renderer, event->wheel.mouse_x, event->wheel.mouse_y, &event->wheel.mouse_x,
                                            &event->wheel.mouse_y);
            break;
        default:
            break;
    }
    return true;
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, SDL_PixelFormat format, SDL_TextureAccess access, int w, int h) {
    (void)access;
    if (!renderer || w <= 0 || h <= 0) {
        SDL_SetError("bad texture");
        return NULL;
    }
    if (format != SDL_PIXELFORMAT_RGBA32 && format != SDL_PIXELFORMAT_XRGB8888 && format != SDL_PIXELFORMAT_ARGB8888 &&
        format != SDL_PIXELFORMAT_XBGR8888) {
        SDL_SetError("PeOS: 32-bit RGB textures only");
        return NULL;
    }
    tex_t *t = calloc(1, sizeof(*t));
    if (!t) {
        SDL_SetError("out of memory");
        return NULL;
    }
    t->pixels = calloc((size_t)w * (size_t)h, 4);
    if (!t->pixels) {
        free(t);
        SDL_SetError("out of memory");
        return NULL;
    }
    t->pub.format = format;
    t->pub.w = w;
    t->pub.h = h;
    t->pub.refcount = 1;
    t->pitch = w * 4;
    t->scale_mode = SDL_SCALEMODE_NEAREST;
    return &t->pub;
}

void SDL_DestroyTexture(SDL_Texture *texture) {
    if (!texture) return;
    if (g_renderer && g_renderer->shown == texture) g_renderer->shown = NULL;
    free(TEX(texture)->pixels);
    free(TEX(texture));
}

bool SDL_SetTextureScaleMode(SDL_Texture *texture, SDL_ScaleMode mode) {
    if (!texture) return SDL_SetError("no texture");
    TEX(texture)->scale_mode = mode;   // nearest whatever it is, for now
    return true;
}

bool SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch) {
    if (!texture || !pixels) return SDL_SetError("no texture");
    SDL_Rect r = { 0, 0, texture->w, texture->h };
    if (rect) r = *rect;
    if (r.x < 0 || r.y < 0 || r.w < 0 || r.h < 0 || r.x + r.w > texture->w || r.y + r.h > texture->h)
        return SDL_SetError("rect outside the texture");
    for (int y = 0; y < r.h; y++)
        memcpy((uint8_t *)TEX(texture)->pixels + (size_t)(r.y + y) * (size_t)TEX(texture)->pitch + (size_t)r.x * 4u,
               (const uint8_t *)pixels + (size_t)y * (size_t)pitch, (size_t)r.w * 4u);
    return true;
}

bool SDL_RenderClear(SDL_Renderer *renderer) {
    if (!renderer) return SDL_SetError("no renderer");
    renderer->shown = NULL;
    return true;
}

bool SDL_RenderTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect) {
    if (!renderer || !texture) return SDL_SetError("no texture");
    renderer->shown = texture;
    renderer->shown_whole = !srcrect && !dstrect;
    renderer->shown_src = srcrect ? *srcrect : (SDL_FRect){ 0.0f, 0.0f, (float)texture->w, (float)texture->h };
    float lw = renderer->logical_w > 0 ? (float)renderer->logical_w : (float)renderer->window->body_w;
    float lh = renderer->logical_h > 0 ? (float)renderer->logical_h : (float)renderer->window->body_h;
    renderer->shown_dst = dstrect ? *dstrect : (SDL_FRect){ 0.0f, 0.0f, lw, lh };
    return true;
}

// A texture's pixel as Starlight's XRGB8888 (0x00RRGGBB)
static inline uint32_t to_xrgb(SDL_PixelFormat format, uint32_t p) {
    switch (format) {
        case SDL_PIXELFORMAT_RGBA32:     // bytes r, g, b, a: 0xAABBGGRR
        case SDL_PIXELFORMAT_XBGR8888:
            return ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu);
        default:                         // XRGB8888, ARGB8888
            return p & 0xffffffu;
    }
}

bool SDL_RenderPresent(SDL_Renderer *renderer) {
    if (!renderer) return SDL_SetError("no renderer");
    pump();
    SDL_Window *w = renderer->window;
    uint32_t sw = 0, sh = 0, pitch = 0;
    uint32_t *px = w->surface_ready ? slui_pixels_buffer(&sw, &sh, &pitch) : NULL;
    SDL_Texture *t = renderer->shown;
    if (!px || !t) return true;
    uint32_t bw = w->body_w < sw ? w->body_w : sw, bh = w->body_h < sh ? w->body_h : sh;
    uint32_t stride = pitch / 4u;

    // the texture's part, into the view's part of the body
    SDL_FRect v = view_rect(renderer);
    float lw = renderer->logical_w > 0 ? (float)renderer->logical_w : v.w;
    float lh = renderer->logical_h > 0 ? (float)renderer->logical_h : v.h;
    int dx0 = (int)(v.x + renderer->shown_dst.x * v.w / lw), dy0 = (int)(v.y + renderer->shown_dst.y * v.h / lh);
    int dw = (int)(renderer->shown_dst.w * v.w / lw), dh = (int)(renderer->shown_dst.h * v.h / lh);
    int sx0 = (int)renderer->shown_src.x, sy0 = (int)renderer->shown_src.y;
    int srcw = (int)renderer->shown_src.w, srch = (int)renderer->shown_src.h;
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx0 + dw > (int)bw) dw = (int)bw - dx0;
    if (dy0 + dh > (int)bh) dh = (int)bh - dy0;
    if (dw <= 0 || dh <= 0 || srcw <= 0 || srch <= 0) return true;

    if (renderer->row_cap < srcw) {
        uint32_t *row = realloc(renderer->row, (size_t)srcw * 4u);
        if (!row) return SDL_SetError("out of memory");
        renderer->row = row;
        renderer->row_cap = srcw;
    }
    if (renderer->xmap_w != dw || renderer->xmap_src_w != srcw || renderer->xmap_dst_w != dw) {
        int *map = realloc(renderer->xmap, (size_t)dw * sizeof(int));
        if (!map) return SDL_SetError("out of memory");
        renderer->xmap = map;
        for (int x = 0; x < dw; x++) map[x] = (int)((int64_t)x * srcw / dw);
        renderer->xmap_w = renderer->xmap_dst_w = dw;
        renderer->xmap_src_w = srcw;
    }

    // the letterbox's bars black
    for (uint32_t y = 0; y < bh; y++) {
        uint32_t *dst = px + (size_t)y * stride;
        if ((int)y < dy0 || (int)y >= dy0 + dh) {
            memset(dst, 0, (size_t)bw * 4u);
        } else {
            if (dx0 > 0) memset(dst, 0, (size_t)dx0 * 4u);
            if (dx0 + dw < (int)bw) memset(dst + dx0 + dw, 0, (size_t)(bw - (uint32_t)(dx0 + dw)) * 4u);
        }
    }
    int last_src_y = -1;
    for (int y = 0; y < dh; y++) {
        int src_y = sy0 + (int)((int64_t)y * srch / dh);
        uint32_t *dst = px + (size_t)(dy0 + y) * stride + dx0;
        if (src_y == last_src_y) {   // the row above, again
            memcpy(dst, dst - stride, (size_t)dw * 4u);
            continue;
        }
        last_src_y = src_y;
        if (src_y < 0 || src_y >= t->h) {
            memset(dst, 0, (size_t)dw * 4u);
            continue;
        }
        const uint32_t *src = (const uint32_t *)((const uint8_t *)TEX(t)->pixels + (size_t)src_y * (size_t)TEX(t)->pitch) + sx0;
        int n = srcw;
        if (sx0 + n > t->w) n = t->w - sx0;
        for (int x = 0; x < n; x++) renderer->row[x] = to_xrgb(t->format, src[x]);
        for (int x = n; x < srcw; x++) renderer->row[x] = 0;
        const int *map = renderer->xmap;
        for (int x = 0; x < dw; x++) dst[x] = renderer->row[map[x]];
    }
    slui_pixels_damage(0u, 0u, bw, bh);
    return true;
}

// ---- surfaces ----

SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch) {
    SDL_Surface *s = calloc(1, sizeof(*s));
    if (!s) {
        SDL_SetError("out of memory");
        return NULL;
    }
    s->flags = SDL_SURFACE_PREALLOCATED;
    s->format = format;
    s->w = width;
    s->h = height;
    s->pitch = pitch;
    s->pixels = pixels;
    s->refcount = 1;
    return s;
}

void SDL_DestroySurface(SDL_Surface *surface) {
    if (!surface || --surface->refcount > 0) return;
    if (!(surface->flags & SDL_SURFACE_PREALLOCATED)) free(surface->pixels);
    free(surface);
}

bool SDL_AddSurfaceAlternateImage(SDL_Surface *surface, SDL_Surface *image) {
    (void)surface;
    (void)image;
    return true;   // one density: the alternates are not needed
}

// ---- memory streams ----

struct SDL_IOStream {
    const uint8_t *data;
    size_t size, pos;
};

SDL_IOStream *SDL_IOFromConstMem(const void *mem, size_t size) {
    if (!mem && size) {
        SDL_SetError("no memory to read");
        return NULL;
    }
    SDL_IOStream *io = calloc(1, sizeof(*io));
    if (!io) {
        SDL_SetError("out of memory");
        return NULL;
    }
    io->data = mem;
    io->size = size;
    return io;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// A BMP of 24 or 32 bits a pixel, uncompressed (or BI_BITFIELDS in the
// usual order), into an ARGB8888 surface.
SDL_Surface *SDL_LoadBMP_IO(SDL_IOStream *src, bool closeio) {
    SDL_Surface *out = NULL;
    if (!src) {
        SDL_SetError("no stream");
        return NULL;
    }
    const uint8_t *d = src->data;
    size_t n = src->size;
    if (n < 54 || d[0] != 'B' || d[1] != 'M') {
        SDL_SetError("not a BMP");
        goto done;
    }
    uint32_t off = le32(d + 10);
    int32_t w = (int32_t)le32(d + 18), h = (int32_t)le32(d + 22);
    uint16_t bpp = le16(d + 28);
    uint32_t comp = le32(d + 30);
    int bottom_up = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) {
        SDL_SetError("BMP: 24 or 32 bits only");
        goto done;
    }
    size_t row = ((size_t)w * bpp / 8u + 3u) & ~(size_t)3u;
    if (off + row * (size_t)h > n) {
        SDL_SetError("BMP: short");
        goto done;
    }
    uint32_t *px = malloc((size_t)w * (size_t)h * 4u);
    if (!px) {
        SDL_SetError("out of memory");
        goto done;
    }
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *s = d + off + row * (size_t)(bottom_up ? h - 1 - y : y);
        for (int32_t x = 0; x < w; x++) {
            const uint8_t *p = s + (size_t)x * bpp / 8u;
            uint32_t a = bpp == 32 ? p[3] : 0xffu;
            px[(size_t)y * (size_t)w + (size_t)x] = a << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
        }
    }
    out = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ARGB8888, px, w * 4);
    if (out) out->flags &= ~SDL_SURFACE_PREALLOCATED;
    else free(px);
done:
    if (closeio) free(src);
    return out;
}

// ---- audio data (the device is the app's: peos/audio.h) ----

// A RIFF WAVE of PCM (8 or 16 bits, or 32-bit float), its samples copied.
bool SDL_LoadWAV_IO(SDL_IOStream *src, bool closeio, SDL_AudioSpec *spec, Uint8 **audio_buf, Uint32 *audio_len) {
    bool ok = false;
    if (!src || !spec || !audio_buf || !audio_len) {
        SDL_SetError("bad arguments");
        goto done;
    }
    const uint8_t *d = src->data;
    size_t n = src->size, at = 12;
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) {
        SDL_SetError("not a WAVE");
        goto done;
    }
    int have_fmt = 0;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    while (at + 8 <= n) {
        uint32_t size = le32(d + at + 4);
        const uint8_t *body = d + at + 8;
        if (at + 8 + size > n) size = (uint32_t)(n - at - 8);   // a short last chunk: what is there
        if (!memcmp(d + at, "fmt ", 4) && size >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            have_fmt = 1;
        } else if (!memcmp(d + at, "data", 4) && have_fmt) {
            if (format == 1 && bits == 8) spec->format = SDL_AUDIO_U8;
            else if (format == 1 && bits == 16) spec->format = SDL_AUDIO_S16LE;
            else if (format == 1 && bits == 32) spec->format = SDL_AUDIO_S32LE;
            else if (format == 3 && bits == 32) spec->format = SDL_AUDIO_F32LE;
            else {
                SDL_SetError("WAVE: PCM only (format %u, %u bits)", format, bits);
                goto done;
            }
            spec->channels = channels;
            spec->freq = (int)rate;
            *audio_buf = malloc(size ? size : 1);
            if (!*audio_buf) {
                SDL_SetError("out of memory");
                goto done;
            }
            memcpy(*audio_buf, body, size);
            *audio_len = size;
            ok = true;
            goto done;
        }
        at += 8 + size + (size & 1u);
    }
    SDL_SetError("WAVE: no data");
done:
    if (closeio) free(src);
    return ok;
}

static float sample_in(SDL_AudioFormat f, const uint8_t *p) {
    switch (f) {
        case SDL_AUDIO_U8: return ((float)p[0] - 128.0f) / 128.0f;
        case SDL_AUDIO_S8: return (float)(int8_t)p[0] / 128.0f;
        case SDL_AUDIO_S16LE: return (float)(int16_t)le16(p) / 32768.0f;
        case SDL_AUDIO_S32LE: return (float)((double)(int32_t)le32(p) / 2147483648.0);
        case SDL_AUDIO_F32LE: { float v; memcpy(&v, p, 4); return v; }
        default: return 0.0f;
    }
}

static void sample_out(SDL_AudioFormat f, uint8_t *p, float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    switch (f) {
        case SDL_AUDIO_U8: p[0] = (uint8_t)(int)(v * 127.0f + 128.0f); break;
        case SDL_AUDIO_S8: p[0] = (uint8_t)(int8_t)(int)(v * 127.0f); break;
        case SDL_AUDIO_S16LE: { int16_t s = (int16_t)(int)(v * 32767.0f); p[0] = (uint8_t)s; p[1] = (uint8_t)((uint16_t)s >> 8); break; }
        case SDL_AUDIO_S32LE: { int32_t s = (int32_t)((double)v * 2147483647.0); memcpy(p, &s, 4); break; }
        case SDL_AUDIO_F32LE: memcpy(p, &v, 4); break;
        default: break;
    }
}

// Format, channels (mono <-> stereo) and rate (linear), whole buffers.
bool SDL_ConvertAudioSamples(const SDL_AudioSpec *src_spec, const Uint8 *src_data, int src_len,
                             const SDL_AudioSpec *dst_spec, Uint8 **dst_data, int *dst_len) {
    if (!src_spec || !dst_spec || !dst_data || !dst_len || src_len < 0 || (!src_data && src_len))
        return SDL_SetError("bad arguments");
    int sin = (int)SDL_AUDIO_BYTESIZE(src_spec->format), sout = (int)SDL_AUDIO_BYTESIZE(dst_spec->format);
    int cin = src_spec->channels, cout = dst_spec->channels;
    if (sin <= 0 || sout <= 0 || cin < 1 || cin > 2 || cout < 1 || cout > 2 || src_spec->freq <= 0 || dst_spec->freq <= 0)
        return SDL_SetError("PeOS: 1 or 2 channels of PCM");
    size_t frames_in = (size_t)src_len / (size_t)(sin * cin);
    size_t frames_out = (size_t)((double)frames_in * dst_spec->freq / src_spec->freq);
    uint8_t *out = malloc(frames_out * (size_t)(sout * cout) + 1u);
    if (!out) return SDL_SetError("out of memory");
    for (size_t i = 0; i < frames_out; i++) {
        double at = (double)i * src_spec->freq / dst_spec->freq;
        size_t a = (size_t)at, b = a + 1 < frames_in ? a + 1 : a;
        float t = (float)(at - (double)a);
        float l, r;
        const uint8_t *pa = src_data + a * (size_t)(sin * cin), *pb = src_data + b * (size_t)(sin * cin);
        float la = sample_in(src_spec->format, pa), lb = sample_in(src_spec->format, pb);
        float ra = cin == 2 ? sample_in(src_spec->format, pa + sin) : la, rb = cin == 2 ? sample_in(src_spec->format, pb + sin) : lb;
        l = la + (lb - la) * t;
        r = ra + (rb - ra) * t;
        uint8_t *po = out + i * (size_t)(sout * cout);
        if (cout == 1) sample_out(dst_spec->format, po, (l + r) * 0.5f);
        else {
            sample_out(dst_spec->format, po, l);
            sample_out(dst_spec->format, po + sout, r);
        }
    }
    *dst_data = out;
    *dst_len = (int)(frames_out * (size_t)(sout * cout));
    return true;
}

// ---- gamepads and joysticks: none yet ----

SDL_JoystickID *SDL_GetGamepads(int *count) {
    if (count) *count = 0;
    return calloc(1, sizeof(SDL_JoystickID));   // a 0-terminated empty list, freed with SDL_free
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id) {
    (void)instance_id;
    SDL_SetError("PeOS: no gamepads yet");
    return NULL;
}

void SDL_CloseGamepad(SDL_Gamepad *gamepad) { (void)gamepad; }

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
    (void)gamepad;
    (void)axis;
    return 0;
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) {
    (void)gamepad;
    (void)button;
    return false;
}

const char *SDL_GetGamepadName(SDL_Gamepad *gamepad) {
    (void)gamepad;
    return NULL;
}

const char *SDL_GetGamepadSerial(SDL_Gamepad *gamepad) {
    (void)gamepad;
    return NULL;
}

Uint16 SDL_GetGamepadVendor(SDL_Gamepad *gamepad) {
    (void)gamepad;
    return 0;
}

Uint16 SDL_GetGamepadProduct(SDL_Gamepad *gamepad) {
    (void)gamepad;
    return 0;
}

SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) {
    (void)gamepad;
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}

SDL_GamepadType SDL_GetRealGamepadType(SDL_Gamepad *gamepad) {
    (void)gamepad;
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}

// SDL's names (for the key config, read even without a pad)
SDL_GamepadAxis SDL_GetGamepadAxisFromString(const char *str) {
    static const char *const names[] = { "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger" };
    for (int i = 0; str && i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (!strcasecmp(str, names[i])) return (SDL_GamepadAxis)i;
    return SDL_GAMEPAD_AXIS_INVALID;
}

SDL_GamepadButton SDL_GetGamepadButtonFromString(const char *str) {
    static const char *const names[] = { "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
                                         "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1",
                                         "paddle1", "paddle2", "paddle3", "paddle4", "touchpad", "misc2", "misc3",
                                         "misc4", "misc5", "misc6" };
    for (int i = 0; str && i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (!strcasecmp(str, names[i])) return (SDL_GamepadButton)i;
    return SDL_GAMEPAD_BUTTON_INVALID;
}

SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID instance_id) {
    (void)instance_id;
    SDL_SetError("PeOS: no joysticks yet");
    return NULL;
}

void SDL_CloseJoystick(SDL_Joystick *joystick) { (void)joystick; }

SDL_JoystickID SDL_GetJoystickID(SDL_Joystick *joystick) {
    (void)joystick;
    return 0;
}

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *desc) {
    (void)desc;
    SDL_SetError("PeOS: no virtual joysticks");
    return 0;
}

bool SDL_DetachVirtualJoystick(SDL_JoystickID instance_id) {
    (void)instance_id;
    return SDL_SetError("PeOS: no virtual joysticks");
}

bool SDL_SetJoystickVirtualAxis(SDL_Joystick *joystick, int axis, Sint16 value) {
    (void)joystick;
    (void)axis;
    (void)value;
    return SDL_SetError("PeOS: no virtual joysticks");
}

bool SDL_SetJoystickVirtualButton(SDL_Joystick *joystick, int button, bool down) {
    (void)joystick;
    (void)button;
    (void)down;
    return SDL_SetError("PeOS: no virtual joysticks");
}
