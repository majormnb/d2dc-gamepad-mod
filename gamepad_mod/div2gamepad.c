/*
 * Div2Gamepad — native gamepad support for Divinity II: Developer's Cut.
 *
 * Drop-in replacement for Div2Patch.dll: forwards InitPatch to the real
 * payload (Div2PatchBase.dll, renamed Enhancer/Div2Patch build) so all
 * v0.97 + Enhancer features keep working, then applies our own gamepad
 * hooks. Config: Div2PatchGamepad.ini next to this DLL (base payload
 * keeps reading <BaseName>.ini of its own).
 *
 * Build: i686-w64-mingw32-g++ -shared -static -O2 -o Div2Patch.dll \
 *          div2gamepad.c div2gamepad.def
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <timeapi.h>

#define BASE_DLL_NAME   "Div2PatchBase.dll"
#define GAMEPAD_INI     "Div2PatchGamepad.ini"

static void log_line(const char *fmt, ...);
static void pad_dev_diag(void);

/* diag#99: per-event trace lines (token/btnmap/fire/b270/d9x) are
 * switched off by default. log_line opens and closes the log file per
 * line, and during active play those hooks produced ~170 lines/s - a
 * visible frame-time cost. Trace=1 in the ini brings them back. */
static int g_trace;
#define TLOG(...) do { if (g_trace) log_line(__VA_ARGS__); } while (0)

static HINSTANCE g_self;
static HMODULE g_base;
static char g_dll_dir[MAX_PATH];
static char g_log_path[MAX_PATH];

static int g_enable_gamepad = 1;
static int g_hook_pump = 1;

/* engine event pump HandleDeviceEvent @ VA 0x8EBB34 (base image 0x400000):
 * 7-byte prologue "sub esp,0x1c4; push esi" is replaced with a jump into
 * our stub; stub logs (eax=type, 4 stack dwords), then runs the original
 * bytes and jumps back to 0x8EBB3B. */
#define PUMP_VA        0x008EBB34u
#define PUMP_ORIG_BYTES_SIZE 7
/* action dispatcher @ VA 0x8E7710: thiscall Dispatch(u32 mode, Event* evt);
 * 5-byte prologue "push ebx; mov ebx,[esp+8]". Pad input reaches the
 * action map here (NOT through the pump — proven by N0.1 session). */
#define DISPATCH_VA    0x008E7710u
#define DISPATCH_ORIG_BYTES_SIZE 5
#define EXE_DEFAULT_BASE 0x400000u

static FILE *g_pump_fp;
static volatile long g_pump_events;
static FILE *g_disp_fp;
static int g_hook_dispatch = 1;

/* ---------------- B1: XInput polling + event synthesis ---------------- */

typedef struct XINPUT_GAMEPAD_T {
    unsigned short wButtons;
    unsigned char  bLeftTrigger, bRightTrigger;
    short          sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD_T;

typedef struct XINPUT_STATE_T {
    unsigned long  dwPacketNumber;
    XINPUT_GAMEPAD_T Gamepad;
} XINPUT_STATE_T;

typedef unsigned long (WINAPI *XInputGetState_fn)(unsigned long, XINPUT_STATE_T *);
typedef unsigned long (WINAPI *XInputSetState_fn)(unsigned long, void *);

static HMODULE g_xinput_lib;
static XInputGetState_fn g_xinput_get;
static XInputSetState_fn g_xinput_set;
static int g_synth = 1;
static int g_diag = 1;
static int g_delivery = 1; /* 1 = OS-level keybd_event (real input queue,
                             * identical to physical keys by construction);
                             * 0 = legacy pump-synth + PeekMessage inject */
static int g_rstick_cam = 1;     /* right stick -> camera (mouse deltas) */
static long g_cam_speed = 600;   /* menu cursor: mouse px/sec at full defl */
static long g_cam_world_speed = 800; /* in-world camera speed */
static int g_cam_invert_y = 0;
static volatile long g_cam_moves;
static volatile long g_pump_rate; /* 0x14FE ticks/sec: ~9 in world, ~100
                                   * in menus — our game-state detector */
static long g_deadzone = 9000;
static long g_trig_thresh = 60;    /* trigger threshold, percent 0..100 */
/* §149: 1 = RT attacks via the native L3 slot (old trigger_to_virt
 * path), 0 (default) = RT/LT are OS mouse clicks (RT->LMB, LT->RMB)
 * driven from the camera thread — clicks reach Scaleform menus and
 * inventory, which native acts do not. Both paths at once would
 * double-fire the attack. */
static int g_rt_native;
/* §158: 1 = force the engine's XInput rumble controller instead of the
 * DI variant (the DI variant cannot initialise force feedback on an
 * XInput pad — menu shows "Unable to initialise rumble on a direct
 * input device"). One-byte patch of the factory branch. */
static int g_rumble = 1;
/* §159: the engine never initialises its rumble controller on PC (the
 * factory is not called at all — neither in menus nor in the world),
 * so vibration is our own: XInputSetState pulses. Sources (2026-10-29):
 * damage taken (§201) and load/area-transition start (§204). The old
 * RT attack/confirm pulse was removed by user request. */
static int g_vibration = 1;
static long g_vib_strength = 60;
static volatile unsigned long g_vib_until_ms;
/* §160: the game's own RumbleIntensity RTPC (0..1, default 1.0),
 * captured by hooking the Wwise setter 0xF134C0 — the options slider
 * drives our pulse. */
static volatile float g_game_vib = 1.0f;
/* §149: fwd-declared here (defined with the bind tables below) — the
 * tick's native-owns row skip needs it before that point in the file */
static int g_paddefaults = 1;
/* §151: 1 = movement runs on the native virtual-slot carriers (dpad
 * canonicals 8..11 via MoveVirt) — the physical dpad then ALSO walks.
 * 0 = movement comes from the OS WASD rows (Synth=1) and the dpad
 * canonicals are free for the native Quickmap 5-8 binds. */
static int g_native_move = 1;
static unsigned long g_user_index = 0;
static volatile long g_tick_count;
static volatile long g_getkb_calls;
static int g_xinput_seen_ok;
static void *g_trampoline;      /* callable original-pump entry (in stub page) */
static void *g_synth_call;      /* cdecl->(eax=type) shim for the pump */
static unsigned g_pump_manager; /* captured from live events (a4) */
static volatile long g_synth_events;
static volatile long g_in_tick;      /* reentrancy guard for gamepad_tick */
#define PUMP_HIST_SIZE 0x2000       /* 0x14FE tick type is 5374 — needs room */
static volatile long g_pump_hist[PUMP_HIST_SIZE];
static int g_test_sticky;           /* ini TestSticky: 1 = self-test thread */
static int g_dbgcmd;                /* ini DbgCmd: 1 = file-queue command console (diag#83) */

#define XI_DPAD_UP    0x0001
#define XI_DPAD_DOWN  0x0002
#define XI_DPAD_LEFT  0x0004
#define XI_DPAD_RIGHT 0x0008
#define XI_START      0x0010
#define XI_BACK       0x0020
#define XI_LTHUMB     0x0040
#define XI_RTHUMB     0x0080
#define XI_LSHOULDER  0x0100
#define XI_RSHOULDER  0x0200
#define XI_A          0x1000
#define XI_B          0x2000
#define XI_X          0x4000
#define XI_Y          0x8000

/* §149: buttons that carry a native bind in k_pad_binds (A B Y X LB
 * RB R3). With Synth=0 their OS-synth rows must stay silent: the
 * native act already fires and an OS key on top doubles every press.
 * §155/§156: Start and Back are NOT here — they have no native binds
 * (delivered as the real ESC / M keys from the fast row poll so the
 * pause menu and the map close on the same button). */
#define NATIVE_OWNED_MASK (XI_A | XI_B | XI_X | XI_Y | \
                           XI_LSHOULDER | XI_RSHOULDER | XI_RTHUMB)

/* DIK / VK pairs used by synthesis (formats proven by live pump log).
 * Dual target: world (dik/vk) and menu (m_dik/m_vk) — menus are detected
 * by the camera thread's SetCursorPos-rate detector (g_in_menu). */
struct synth_key {
    unsigned       btn;      /* wButtons bit, or XI_TRIG_* virtual bit */
    unsigned char  dik;
    unsigned char  vk;
    unsigned char  flags;   /* PADF_*: target is a mouse button, vk/dik ignored */
    unsigned char  m_dik;   /* menu-mode target */
    unsigned char  m_vk;
};

#define PADF_MOUSE_L 0x01    /* synthesize left mouse button   */
#define PADF_MOUSE_R 0x02    /* synthesize right mouse button  */
#define PADF_OFF     0x04    /* button disabled via ini (NONE) */

/* virtual bits beyond 16-bit wButtons: triggers behave like buttons */
#define XI_TRIG_R    0x10000u
#define XI_TRIG_L    0x20000u

/* world/menu mode for the dual layout, mirrored from the camera
 * thread's detector (2-window confirm filters the known flapping) */
static volatile long g_in_menu = 1;

/* Default X360-console layout over the game's DEFAULT key binds, so a
 * fresh install needs no rebinding. These OS rows mirror the native
 * bind table (§150) and only drive the game when Synth=1: with
 * Synth=0 the native binds own the buttons (NATIVE_OWNED_MASK) and
 * only dpad/RT/LT rows stay live. Every row is [Layout]-overridable
 * (override applies to the WORLD target). */
static struct synth_key g_keys[] = {
    { XI_A,          0x04, 0x33, 0, 0x04, 0x33 },  /* A     -> '3' (slot 3) */
    { XI_B,          0x03, 0x32, 0, 0x03, 0x32 },  /* B     -> '2' (slot 2) */
    { XI_START,      0x01, 0x1B, 0, 0x01, 0x1B },  /* Start -> ESC         */
    { XI_BACK,       0x32, 0x4D, 0, 0x32, 0x4D },  /* Back  -> M (map)     */
    { XI_X,          0x05, 0x34, 0, 0x05, 0x34 },  /* X     -> '4' (slot 4) */
    { XI_Y,          0x02, 0x31, 0, 0x02, 0x31 },  /* Y     -> '1' (slot 1) */
    { XI_LSHOULDER,  0x12, 0x45, 0, 0x12, 0x45 },  /* LB    -> E (interact) */
    { XI_RSHOULDER,  0x39, 0x20, 0, 0x39, 0x20 },  /* RB    -> Space (jump) */
    { XI_RTHUMB,     0x1C, 0x0D, 0, 0x1C, 0x0D },  /* R3    -> Enter (tactical pause) */
    /* dpad: quickbar 5-8 everywhere (the world/menu detector flaps
     * in-world at every 400ms window — unusable for binding targets;
     * menus are navigated with the RS cursor + A/B instead) */
    { XI_DPAD_UP,    0x06, 0x35, 0, 0, 0 },
    { XI_DPAD_DOWN,  0x07, 0x36, 0, 0, 0 },
    { XI_DPAD_LEFT,  0x08, 0x37, 0, 0, 0 },
    { XI_DPAD_RIGHT, 0x09, 0x38, 0, 0, 0 },
    { XI_TRIG_R,     0x00, 0x01, PADF_MOUSE_L, 0, 0 }, /* RT -> LMB attack */
    { XI_TRIG_L,     0x00, 0x02, PADF_MOUSE_R, 0, 0 }, /* LT -> RMB        */
};
#define KEY_COUNT (sizeof(g_keys) / sizeof(g_keys[0]))
/* left stick -> WASD (m_vk kept 0: no menu variant until a reliable
 * world/menu detector exists) */
#define STICK_KEY_COUNT (KEY_COUNT + 4)
static struct synth_key g_stick_keys[4] = {
    { 0, 0x11, 0x57, 0, 0, 0 },  /* W */
    { 0, 0x1F, 0x53, 0, 0, 0 },  /* S */
    { 0, 0x1E, 0x41, 0, 0, 0 },  /* A */
    { 0, 0x20, 0x44, 0, 0, 0 },  /* D */
};
static unsigned char g_key_state[STICK_KEY_COUNT];
/* which target a held key was pressed with (1 = menu variant): a mode
 * flip between press and release must release the SAME key it pressed,
 * otherwise the old key stays logically down = "stuck" movement */
static unsigned char g_key_sent_menu[STICK_KEY_COUNT];
/* R3 double-tap: tap = lock key (R3 in [Layout]), tap-tap (<350ms) =
 * cancel key (R3Double in [Layout], default G, 0 disables) */
static int g_r3_idx = -1;
static unsigned char g_r3_alt_vk = 0x47, g_r3_alt_dik = 0x22;
static volatile long g_r3_press_alt;
static DWORD g_r3_last_up;
static unsigned char g_key_alt_sent[STICK_KEY_COUNT];

static void synth_key_event(unsigned vk, unsigned char dik, int down)
{
    typedef void (__stdcall *synth_fn)(unsigned, unsigned, unsigned,
                                       unsigned, unsigned, unsigned);
    unsigned type, a3;

    if (!g_synth_call || !g_pump_manager)
        return;
    type = down ? 0x100u : 0x101u;
    a3 = down ? ((unsigned)dik << 16) | 1u
              : (((unsigned)dik | 0xC000u) << 16) | 1u;
    /* pump reads up to 5 stack args (mouse cases use [esp+0x1dc]) — pass
     * the captured manager as a5 to keep every branch on a valid value */
    ((synth_fn)g_synth_call)(type, type, vk, a3, g_pump_manager,
                             g_pump_manager);
}

/* ---- dynamic-import interception: the game resolves some input APIs at
 * runtime via GetProcAddress (not in the import table) — most likely
 * GetAsyncKeyState for per-frame key polling. Log everything it pulls
 * from user32 and substitute our key-overlay versions. ---- */
#define GETPROCADDR_SLOT_VA 0x012B7064u

typedef SHORT (__stdcall *GAKS_fn)(int);
typedef FARPROC (WINAPI *GPA_fn)(HMODULE, LPCSTR);

static GPA_fn g_real_gpa;
static GAKS_fn g_real_gaks;
static GAKS_fn g_real_gks;
static HMODULE g_user32_mod;
static volatile long g_gpa_logged;
static volatile long g_gaks_calls, g_gks_calls;

static int vk_held_by_us(unsigned vk)
{
    int i;

    for (i = 0; i < (int)STICK_KEY_COUNT; i++) {
        unsigned char k = i < (int)KEY_COUNT ? g_keys[i].vk
                                             : g_stick_keys[i - KEY_COUNT].vk;

        if (k == vk && g_key_state[i])
            return 1;
    }
    return 0;
}

static SHORT __stdcall hk_GetAsyncKeyState(int vk)
{
    SHORT r = 0;

    if (g_real_gaks)
        r = g_real_gaks(vk);
    if (InterlockedIncrement(&g_gaks_calls) == 1)
        log_line("gaks: FIRST call vk=%02X (channel hot)", vk);
    if (vk_held_by_us((unsigned)vk))
        r |= (SHORT)0x8000;
    return r;
}

static SHORT __stdcall hk_GetKeyState(int vk)
{
    SHORT r = 0;

    if (g_real_gks)
        r = g_real_gks(vk);
    if (InterlockedIncrement(&g_gks_calls) == 1)
        log_line("gks: FIRST call vk=%02X (channel hot)", vk);
    if (vk_held_by_us((unsigned)vk))
        r |= (SHORT)0x8000;
    return r;
}

static FARPROC WINAPI hk_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC r;

    if (!g_real_gpa)
        return NULL;
    r = g_real_gpa(mod, name);
    if (mod && g_user32_mod && mod == g_user32_mod && name &&
        HIWORD((uintptr_t)name)) {
        if (strcmp(name, "GetAsyncKeyState") == 0 && r) {
            if (!g_real_gaks)
                g_real_gaks = (GAKS_fn)r;
            log_line("gpa: game asked for GetAsyncKeyState -> substituted");
            return (FARPROC)&hk_GetAsyncKeyState;
        }
        if (strcmp(name, "GetKeyState") == 0 && r) {
            if (!g_real_gks)
                g_real_gks = (GAKS_fn)r;
            log_line("gpa: game asked for GetKeyState -> substituted");
            return (FARPROC)&hk_GetKeyState;
        }
        if (InterlockedIncrement(&g_gpa_logged) < 60)
            log_line("gpa: user32!%s", name);
    }
    return r;
}
#define PEEKMSG_SLOT_VA 0x012B7A14u
#define SETCURSOR_SLOT_VA 0x012B7A3Cu

/* cursor-anchoring detector: the engine re-anchors the cursor via
 * SetCursorPos in world mode (its call cluster sits right next to the
 * event pump, 0x8EB30B) and almost never in menus — this separates
 * camera speed from menu-cursor speed. */
typedef BOOL (__stdcall *SetCursorPos_fn)(int, int);
static SetCursorPos_fn g_real_setcursor;
static volatile long g_setcur_calls;

static BOOL __stdcall hk_SetCursorPos(int x, int y)
{
    InterlockedIncrement(&g_setcur_calls);
    /* diag#71: log the re-anchor pattern (rate-limited) - the camera
     * is mouse-emulated, so the cursor position IS the camera
     * direction; a pad button press appears to reset it (camera
     * "recenters"). This shows what the game does on a press. */
    {
        static long last_log;
        static int lx = -1, ly = -1;
        long now = GetTickCount();

        if (g_diag && (x != lx || y != ly) && now - last_log > 100) {
            lx = x;
            ly = y;
            last_log = now;
            log_line("setcur: %d,%d", x, y);
        }
    }
    if (!g_real_setcursor)
        return FALSE;
    return g_real_setcursor(x, y);
}

typedef BOOL (__stdcall *PeekMessageA_fn)(MSG *, HWND, UINT, UINT, UINT);
static PeekMessageA_fn g_real_peekmsg;
static HWND g_game_hwnd;
static volatile long g_peek_calls;
static volatile long g_peek_msgs;

/* SPSC edge queue: producer = pump-thread tick, consumer = message loop */
struct wm_edge {
    unsigned char vk, dik, down, pad;
};
static volatile long g_edge_head, g_edge_tail;
static struct wm_edge g_edge_q[64];
static volatile long g_wm_injected;

static void queue_edge(unsigned vk, unsigned dik, int down)
{
    long head = g_edge_head;
    long tail = g_edge_tail;

    if (head - tail >= (long)(sizeof(g_edge_q) / sizeof(g_edge_q[0])))
        return;
    g_edge_q[head & 63].vk = (unsigned char)vk;
    g_edge_q[head & 63].dik = (unsigned char)dik;
    g_edge_q[head & 63].down = (unsigned char)(down ? 1 : 0);
    __sync_synchronize();
    g_edge_head = head + 1;
}

static BOOL __stdcall hk_PeekMessageA(MSG *msg, HWND hwnd, UINT lo,
                                      UINT hi, UINT remove)
{
    BOOL r;
    static int first = 1;

    if (!g_real_peekmsg)
        return FALSE;
    r = g_real_peekmsg(msg, hwnd, lo, hi, remove);
    InterlockedIncrement(&g_peek_calls);
    if (r)
        InterlockedIncrement(&g_peek_msgs);
    if (first) {
        first = 0;
        log_line("peek: first call hwnd=%p remove=%u r=%d",
                 (void *)hwnd, remove, (int)r);
    }
    if (r && msg && msg->hwnd && !g_game_hwnd) {
        g_game_hwnd = msg->hwnd;
        log_line("peek: game hwnd=%p", (void *)g_game_hwnd);
    }
    if ((g_peek_calls % 20000) == 0)
        log_line("peek: calls=%ld msgs=%ld injected=%ld qh=%ld qt=%ld",
                 g_peek_calls, g_peek_msgs, g_wm_injected,
                 g_edge_head, g_edge_tail);

    if (!r && msg && (remove & 1) /* PM_REMOVE */ &&
        g_edge_head != g_edge_tail) {
        struct wm_edge e = g_edge_q[g_edge_tail & 63];
        __sync_synchronize();
        g_edge_tail = g_edge_tail + 1;
        POINT pt;

        GetCursorPos(&pt);
        msg->hwnd = g_game_hwnd ? g_game_hwnd : GetForegroundWindow();
        msg->message = e.down ? WM_KEYDOWN : WM_KEYUP;
        msg->wParam = e.vk;
        msg->lParam = 1u | ((unsigned)e.dik << 16);
        if (!e.down)
            msg->lParam |= 0xC0000000u;
        msg->time = GetTickCount();
        msg->pt = pt;
        InterlockedIncrement(&g_wm_injected);
        return TRUE;
    }
    return r;
}

/* OS-level synthesis: puts a REAL keystroke into the Windows input
 * queue (GetQueueStatus sees it, the game loop fetches it, WndProc
 * processes it) — indistinguishable from a physical key. This is the
 * only delivery that survives a GetQueueStatus-gated world loop. */

/* ---- Delivery=2: PostMessage into the game window ----
 * The legacy path (synth_key_event + queue_edge) substituted our key
 * messages only when the game's queue was EMPTY (`!r && PM_REMOVE`),
 * and in the world the queue is never empty - that is why our keys
 * were never picked up there (diag#16-#20).
 * PostMessage puts a NORMAL keyboard message into the game's thread
 * queue: the game handles it exactly like a hardware key, but the
 * event never reaches the OS input stack - so the Steam overlay does
 * not react and the screen does not recenter. */
static int is_extended_dik(unsigned char dik)
{
    switch (dik) {
    case 0x1C: case 0x2D: case 0x35: case 0x38:      /* numpad */
    case 0x4B: case 0x4C: case 0x4D: case 0x50:      /* arrows */
    case 0x5B: case 0x5C: case 0x5D: case 0x6F:
    case 0x90: case 0xA3: case 0xA5:
        return 1;
    }
    return 0;
}

static void post_key_msg(unsigned vk, unsigned char dik, int down)
{
    /* the hwnd captured from the first PeekMessage turned out to be a
     * SERVICE window, not the game's main one (the notes said so
     * long ago) - so post to the foreground window as well, and to
     * both if they differ. */
    HWND fg = GetForegroundWindow();
    HWND gh = g_game_hwnd;
    LPARAM lp;
    UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
    static int logged;

    lp = 1 | ((LPARAM)(dik & 0xFF) << 16);
    if (is_extended_dik(dik))
        lp |= (LPARAM)1 << 24;
    if (!down)
        lp |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    if (!logged) {
        logged = 1;
        log_line("post: targets fg=%08X game=%08X", (unsigned)(uintptr_t)fg,
                 (unsigned)(uintptr_t)gh);
    }
    if (fg && IsWindow(fg))
        PostMessageA(fg, msg, (WPARAM)vk, lp);
    if (gh && gh != fg && IsWindow(gh))
        PostMessageA(gh, msg, (WPARAM)vk, lp);
}

static void send_key_os(unsigned vk, unsigned char dik, int down)
{
    keybd_event(vk, dik, down ? 0u : KEYEVENTF_KEYUP, 0);
}

static void send_mouse_os(unsigned char flags, int down)
{
    mouse_event(flags & PADF_MOUSE_L
                    ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
                    : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP),
                0, 0, 0, 0);
}

static void apply_key(int idx, int want)
{
    const struct synth_key *k;
    unsigned char dik, vk;
    int i, use_menu;

    if (idx < (int)KEY_COUNT) {
        k = &g_keys[idx];
        i = idx;
    } else {
        k = &g_stick_keys[idx - KEY_COUNT];
        i = idx;
    }
    g_key_state[i] = (unsigned char)want;
    InterlockedIncrement(&g_synth_events);
    if (k->flags & PADF_OFF)
        return;
    if (k->flags & (PADF_MOUSE_L | PADF_MOUSE_R)) {
        send_mouse_os(k->flags, want);   /* mouse: OS-only, both modes */
        return;
    }
    /* R3 double-tap alt key: pick at press, mirror at release */
    if (i == g_r3_idx && g_r3_alt_vk) {
        int alt = want ? (int)g_r3_press_alt : g_key_alt_sent[i];

        if (want)
            g_key_alt_sent[i] = (unsigned char)alt;
        if (alt) {
            unsigned char avk = g_r3_alt_vk, adik = g_r3_alt_dik;

            if (g_delivery == 1) {
                send_key_os(avk, adik, want);
            } else if (g_delivery == 2) {
                post_key_msg(avk, adik, want);
            } else {
                synth_key_event(avk, adik, want);
                queue_edge(avk, adik, want);
            }
            return;
        }
    }
    /* dual target: pick at PRESS time, remember, and release exactly
     * what was pressed even if the mode flips mid-hold */
    use_menu = (g_in_menu && k->m_vk);
    if (!want)
        use_menu = g_key_sent_menu[i];
    else
        g_key_sent_menu[i] = (unsigned char)use_menu;
    if (use_menu) {
        dik = k->m_dik;
        vk = k->m_vk;
    } else {
        dik = k->dik;
        vk = k->vk;
    }
    if (g_delivery == 1) {
        send_key_os(vk, dik, want);
    } else if (g_delivery == 2) {
        post_key_msg(vk, dik, want);
    } else {
        synth_key_event(vk, dik, want);  /* engine event pump channel */
        queue_edge(vk, dik, want);       /* PeekMessage injection channel */
    }
}

/* §149: analog trigger as a button with hysteresis — ON at
 * TriggerThreshold, OFF at half of it. A hard threshold flaps while
 * the trigger hovers (user's RT: 96 flaps in one diag#70 session) and
 * every flap would re-fire a click. Percent semantics, same as
 * trigger_to_virt always used. */
static int trig_as_btn(unsigned char raw, int down)
{
    int pct = (int)raw * 100 / 255;

    return down ? pct >= g_trig_thresh / 2 : pct >= g_trig_thresh;
}

/* §149: trigger mouse clicks (RT -> LMB, LT -> RMB) for the pad rows
 * flagged PADF_MOUSE_*. Driven from the camera thread at ~125 Hz: the
 * engine pump tick samples only ~9 Hz everywhere and the DI8 hook is
 * Synth-gated, so neither can carry menu clicks. OS-only path
 * (mouse_event inside apply_key) — no engine objects touched, safe
 * off the game thread, and the click reaches Scaleform menus and the
 * inventory, which native acts never do. The caller has already
 * verified input focus (another app's clicks must never be forged). */
/* §164: the in-game RumbleIntensity slider is written to
 * %LOCALAPPDATA%\Divinity 2\Profile\settings.dv2 and nothing reads
 * it at runtime (the Wwise setter is never called — verified). The file
 * is a small serialized blob: the audio settings floats start at 0x1E
 * (MusicVolume/SpeechVolume/EffectsVolume) and RumbleIntensity is the
 * float at 0x2A (default 1.0; observed 1.0 -> 0.0 -> 0.85 across
 * sessions). Read it with a small cache — the camera thread calls this
 * every few seconds, so slider changes apply after the game saves the
 * settings. Guarded: plausible size and value range only. */
static void vib_read_setting(void)
{
    static unsigned long last_check;
    static int checked_once;
    unsigned long now = GetTickCount();
    char path[MAX_PATH], base[MAX_PATH];
    FILE *f;
    unsigned char buf[0x40];
    size_t n;
    float v;

    if (checked_once && now - last_check < 3000)
        return;
    checked_once = 1;
    last_check = now;
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH))
        return;
    snprintf(path, MAX_PATH, "%s/Divinity 2/Profile/settings.dv2",
             base);
    f = fopen(path, "rb");
    if (!f) {
        static int warned;

        if (!warned) {
            warned = 1;
            log_line("vib: settings file open failed (%s)", path);
        }
        return;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 0x30)
        return;
    memcpy(&v, buf + 0x2A, 4);   /* floats start at 0x1E: 0x1E/0x22/0x26/0x2A */
    if (!(v >= 0.0f && v <= 1.0f))
        return;                          /* layout shifted or garbage */
    if (v != g_game_vib) {
        g_game_vib = v;
        log_line("vib: settings RumbleIntensity = %ld%%",
                 (long)(v * 100.0f + 0.5f));
    }
}

static void vib_pulse_ms(long pct, long ms)
{
    unsigned short vib[2];
    float gv;

    if (!g_vibration || !g_xinput_set || !g_xinput_seen_ok)
        return;
    if (pct <= 0 || pct > 100)
        pct = g_vib_strength;
    gv = g_game_vib;                    /* in-game RumbleIntensity 0..1 */
    if (gv < 0.0f)
        gv = 0.0f;
    if (gv > 1.0f)
        gv = 1.0f;
    pct = (long)((float)pct * gv);
    {
        static long last_pct = -1;

        if (pct != last_pct) {
            last_pct = pct;
            log_line("vib: pulse strength = %ld%% (game=%.2f strength=%ld)",
                     pct, gv, g_vib_strength);
        }
    }
    if (pct <= 0) {                     /* slider at zero: stay silent */
        unsigned short z[2] = { 0, 0 };

        g_xinput_set(g_user_index, z);
        g_vib_until_ms = 0;
        return;
    }
    vib[0] = (unsigned short)(pct * 600);
    vib[1] = (unsigned short)(pct * 900);
    g_xinput_set(g_user_index, vib);
    g_vib_until_ms = GetTickCount() + (unsigned long)ms;
}

static void vib_pulse(long pct)
{
    vib_pulse_ms(pct, 140);
}

/* §204: vibration at the START of a load / area transition (user
 * choice: only the start, no arrival pulse). Drivers: the first
 * "openPopUp" of a load session (the success dialog fires it again —
 * excluded by the existing session-window branch) and
 * "RegionSwap_Start" for door/area transitions. 5 s debounce merges
 * the two signals of one transition into a single pulse. */
static int g_vib_load = 1;            /* ini VibLoad */
static long g_vib_load_start = 0;     /* percent; 0 = take VibStrength */
static volatile unsigned long g_vib_load_last_ms;

static void vib_load_pulse(const char *why)
{
    unsigned long now = GetTickCount();

    if (!g_vib_load)
        return;
    if (g_vib_load_last_ms &&
        (long)(now - g_vib_load_last_ms) < 5000)
        return;                          /* one transition, one pulse */
    g_vib_load_last_ms = now;
    vib_pulse_ms(g_vib_load_start, 200);
    log_line("vibload: start pulse (%s)", why);
}

static void mousebtn_poll(const XINPUT_STATE_T *st)
{
    static int rt_dn, lt_dn, logged;
    int i;

    rt_dn = trig_as_btn(st->Gamepad.bRightTrigger, rt_dn);
    lt_dn = trig_as_btn(st->Gamepad.bLeftTrigger, lt_dn);
    for (i = 0; i < (int)KEY_COUNT; i++) {
        unsigned fl = g_keys[i].flags;
        int want;

        if (!(fl & (PADF_MOUSE_L | PADF_MOUSE_R)) || (fl & PADF_OFF))
            continue;      /* keyboard rows: the pump tick owns them */
        if (g_keys[i].btn != XI_TRIG_R && g_keys[i].btn != XI_TRIG_L)
            continue;      /* Layout mouse remaps: pump-tick fallback */
        if (g_rt_native && g_keys[i].btn == XI_TRIG_R)
            continue;      /* RT on the native L3 slot (old path) */
        want = g_keys[i].btn == XI_TRIG_R ? rt_dn : lt_dn;
        if (want != g_key_state[i])
            apply_key(i, want);
    }
    if (!logged && (rt_dn || lt_dn)) {
        logged = 1;
        log_line("clicks: RT/LT poll fired (camera thread, 125 Hz)");
    }
}

/* §203: menu list navigation — REMOVED per user decision (2026-10-29):
 * the cursor-step was a crutch, not native navigation. Native PC-menu
 * pad navigation does not exist (NOTES §202); menus stay on the
 * right-stick cursor + RT click as before. Deliberately kept as a
 * marker so the experiment is not resurrected blindly. */

/* §154/§155: fast OS rows at 125 Hz — the dpad Quickmap 5-8 and Start
 * (real ESC). With Synth=0 the DI8 GetDeviceState hook is not
 * installed, so gamepad_tick_real runs only on the engine pump
 * (measured ~4-9 Hz in the world) — short taps fell between the ticks
 * and never fired (user report: "на крестовине ничего не работает").
 * Delivery is OS-only (keybd_event), safe off the game thread. The
 * pump loop keeps these rows only as a fallback without this thread. */
static void fastrow_poll(const XINPUT_STATE_T *st)
{
    static unsigned char seen;
    static const char *dn[6] = { "UP", "DOWN", "LEFT", "RIGHT",
                                 "START", "BACK" };
    int i;

    for (i = 0; i < (int)KEY_COUNT; i++) {
        unsigned fl = g_keys[i].flags;
        int want, d;

        if (!(g_keys[i].btn & (XI_DPAD_UP | XI_DPAD_DOWN |
                               XI_DPAD_LEFT | XI_DPAD_RIGHT |
                               XI_START | XI_BACK)))
            continue;
        if (fl & PADF_OFF)
            continue;
        want = (st->Gamepad.wButtons & g_keys[i].btn) != 0;
        d = (g_keys[i].btn & XI_START) ? 4 :
            (g_keys[i].btn & XI_BACK) ? 5 :
            (g_keys[i].btn & XI_DPAD_DOWN) ? 1 :
            (g_keys[i].btn & XI_DPAD_LEFT) ? 2 :
            (g_keys[i].btn & XI_DPAD_RIGHT) ? 3 : 0;
        if (want && !g_key_state[i] && !(seen & g_keys[i].btn)) {
            seen |= (unsigned char)g_keys[i].btn;
            log_line("%s: %s -> VK%02X delivery=%d",
                     d >= 4 ? (d == 4 ? "start" : "back") : "dpad", dn[d],
                     g_keys[i].vk, g_delivery);
        }
        if (want != g_key_state[i])
            apply_key(i, want);
    }
}

/* diag#70: the pad must be detected BEFORE the synthesis-shim check.
 * With Synth=0 there is no shim (g_synth_call stays NULL) and the
 * old order returned before the XInput read - so g_xinput_seen_ok
 * stayed 0 for the whole session, which silently killed the trigger
 * digitizer, the right-stick camera and the menu/world detector
 * (all three gate on that flag). Returns 1 with *st filled when a
 * pad is connected. */
static int xinput_detect(XINPUT_STATE_T *st)
{
    int idx, found = -1;

    if (!g_xinput_get)
        return 0;
    if (g_xinput_get(g_user_index, st) == 0) {
        if (!g_xinput_seen_ok) {
            g_xinput_seen_ok = 1;
            log_line("xinput: pad CONNECTED (index %lu)", g_user_index);
        }
        return 1;
    }
    for (idx = 0; idx < 4; idx++) {
        XINPUT_STATE_T probe;

        if (g_xinput_get(idx, &probe) == 0) {
            found = idx;
            break;
        }
    }
    if (found >= 0) {
        if ((unsigned long)found != g_user_index) {
            log_line("xinput: pad found at user index %d", found);
            g_user_index = found;
        }
        g_xinput_get(g_user_index, st);
        if (!g_xinput_seen_ok) {
            g_xinput_seen_ok = 1;
            log_line("xinput: pad CONNECTED (index %d)", found);
        }
        return 1;
    }
    if (g_xinput_seen_ok) {
        g_xinput_seen_ok = 0;
        log_line("xinput: pad DISCONNECTED");
    }
    return 0;
}

/* ---- diag#95: direction bools of the ACTIVE mode listener. The walk
 * path reads listener+8..+0xB (the four directions held); we log them on
 * change plus a 1s heartbeat while any is up, together with the g6 route
 * flag (0x14F13C0+0x52). A release that reached the bus but did not
 * clear its bool becomes directly visible here. See NOTES §122/§124. ---- */
static void d97_dump(void);

static void d95_poll(void)
{
    static DWORD last_ms;
    static unsigned l_mode, l_lst, l_b, l_f52;
    static int have;
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned sub2 = 0, inner = 0, mode = 0, lst = 0, b = 0, f52 = 0;
    DWORD now = GetTickCount();

    if (!g_diag)                    /* §204: pure diagnostics */
        return;
    sub2 = *(unsigned *)(exe + (0x14F1704u - EXE_DEFAULT_BASE));
    if (sub2 > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)sub2, 0x20))
        inner = *(unsigned *)(uintptr_t)(sub2 + 0x14);
    if (inner > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)inner, 0x40)) {
        static const unsigned char moff[13] = {
            0, 0x0C, 0x10, 0x14, 0x18, 0x20, 0x1C,
            0x24, 0x28, 0x2C, 0x30, 0x34, 0x38
        };
        mode = *(unsigned *)(uintptr_t)(inner + 4);
        if (mode >= 1 && mode <= 0x0C)
            lst = *(unsigned *)(uintptr_t)(inner + moff[mode]);
        if (lst > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)lst, 0x10)) {
            b = (unsigned)(*(unsigned char *)(uintptr_t)(lst + 8)) |
                (unsigned)(*(unsigned char *)(uintptr_t)(lst + 9)) << 1 |
                (unsigned)(*(unsigned char *)(uintptr_t)(lst + 0xA)) << 2 |
                (unsigned)(*(unsigned char *)(uintptr_t)(lst + 0xB)) << 3;
        } else {
            lst = 0;
        }
    }
    {
        unsigned g = *(unsigned *)(exe + (0x14F13C0u - EXE_DEFAULT_BASE));
        if (g > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)(g + 0x52), 1))
            f52 = *(unsigned char *)(uintptr_t)(g + 0x52);
    }
    if (((!have || mode != l_mode || lst != l_lst || b != l_b ||
          f52 != l_f52) && now - last_ms >= 50) ||
        (b != 0 && now - last_ms >= 1000)) {
        static int dumped;

        last_ms = now;
        l_mode = mode; l_lst = lst; l_b = b; l_f52 = f52;
        have = 1;
        log_line("d95: mode=%d lst=%08X b=%u%u%u%u f52=%u",
                 (int)mode, lst, b & 1, (b >> 1) & 1, (b >> 2) & 1,
                 (b >> 3) & 1, f52);
        if (!dumped && mode == 3) {
            dumped = 1;
            d97_dump();
        }
    }
}

static void gamepad_tick_real(void)
{
    XINPUT_STATE_T st;
    unsigned btn, want_mask = 0;
    long lx, ly;
    int i;

    InterlockedIncrement(&g_tick_count);
    pad_dev_diag();
    d95_poll();

    xinput_detect(&st);

    /* §149: the synth shim is only needed by delivery mode 0 — with
     * Synth=0 the tick still drives the dpad/LT/RT rows (PostMessage
     * and mouse paths need no shim; synth_key_event null-checks
     * itself). This return used to kill the whole OS path, so with
     * Synth=0 the triggers could not click in menus at all. */
    if (!g_xinput_get) {
        if (g_diag && (g_tick_count % 1000) == 0)
            log_line("diag: ticks=%ld xinput=%p shim=%p mgr=%08X kb=%ld",
                     g_tick_count, (void *)g_xinput_get, g_synth_call,
                     g_pump_manager, g_getkb_calls);
        return;
    }
    if (!g_xinput_seen_ok) {
        if (g_diag && (g_tick_count % 1000) == 0)
            log_line("diag: ticks=%ld NOT_CONNECTED idx=%lu kb=%ld "
                     "synth=%ld",
                     g_tick_count, g_user_index, g_getkb_calls,
                     g_synth_events);
        return;
    }
    btn = st.Gamepad.wButtons;
    /* §149: analog triggers as virtual buttons with hysteresis. The
     * old raw-value compare (bTrigger > 60 == 23% real travel) fired
     * too light AND flapped on every wobble; percent semantics now,
     * identical to mousebtn_poll / trigger_to_virt. */
    {
        static int rt_b, lt_b;

        rt_b = trig_as_btn(st.Gamepad.bRightTrigger, rt_b);
        lt_b = trig_as_btn(st.Gamepad.bLeftTrigger, lt_b);
        if (rt_b)
            btn |= XI_TRIG_R;
        if (lt_b)
            btn |= XI_TRIG_L;
    }

    if (g_diag && (g_tick_count % 500) == 0) {
        unsigned held = 0;
        int k;

        for (k = 0; k < (int)STICK_KEY_COUNT; k++)
            if (g_key_state[k])
                held |= 1u << k;
        log_line("diag: t=%ld idx=%lu kb=%ld synth=%ld held=%03X btn=%04X",
                 g_tick_count, g_user_index, g_getkb_calls, g_synth_events,
                 held, btn);
    }

    {
        DWORD fg_pid = 0;
        HWND fg = GetForegroundWindow();

        if (fg)
            GetWindowThreadProcessId(fg, &fg_pid);
        if (!fg || fg_pid != GetCurrentProcessId()) {
            /* another app has focus: force-release everything, never
             * leak keys out. (Note: comparing against a captured hwnd
             * is wrong — the captured one may be an aux window, which
             * zeroed out ALL synthesis for a whole session.) */
            for (i = 0; i < (int)STICK_KEY_COUNT; i++)
                if (g_key_state[i])
                    apply_key(i, 0);
            return;
        }
    }

    for (i = 0; i < (int)KEY_COUNT; i++) {
        int want;

        if (g_keys[i].flags & PADF_OFF)
            continue;
        /* §149: trigger mouse rows are owned by the camera thread
         * (125 Hz, works in menus); the pump tick drives them only as
         * a fallback when there is no camera thread (RStickCam=0). */
        if ((g_keys[i].flags & (PADF_MOUSE_L | PADF_MOUSE_R)) &&
            (g_keys[i].btn == XI_TRIG_R || g_keys[i].btn == XI_TRIG_L) &&
            g_rstick_cam)
            continue;
        /* §149/§151: with native binds active the native act already
         * fires on these buttons; an OS key on top would double every
         * press. Holds for Synth=1 too — the OS path then only adds the
         * WASD stick rows, the trigger clicks and, in WASD-movement
         * mode, the dpad rows. */
        if (!(g_keys[i].flags & (PADF_MOUSE_L | PADF_MOUSE_R)) &&
            g_paddefaults && (g_keys[i].btn & NATIVE_OWNED_MASK))
            continue;
        /* §151: native dpad Quickmap binds own the dpad in that mode */
        if (g_paddefaults && !g_native_move &&
            (g_keys[i].btn & (XI_DPAD_UP | XI_DPAD_DOWN |
                              XI_DPAD_LEFT | XI_DPAD_RIGHT)))
            continue;
        /* §154-§156: the camera thread (125 Hz) owns the dpad rows,
         * Start and Back — this tick is pump-driven (~4-9 Hz with
         * Synth=0) and short taps fall through it entirely */
        if (g_rstick_cam &&
            (g_keys[i].btn & (XI_DPAD_UP | XI_DPAD_DOWN |
                              XI_DPAD_LEFT | XI_DPAD_RIGHT |
                              XI_START | XI_BACK)))
            continue;
        want = (btn & g_keys[i].btn) != 0;
        if (want != g_key_state[i]) {
            if (i == g_r3_idx && g_r3_alt_vk) {
                if (want)
                    g_r3_press_alt =
                        (GetTickCount() - g_r3_last_up < 350) ? 1 : 0;
                else
                    g_r3_last_up = GetTickCount();
            }
            apply_key(i, want);
        }
    }

    /* left stick -> WASD with deadzone. §149: Synth=1 only — with
     * Synth=0 movement is native (stick_to_virt slots + MoveVirt
     * binds) and WASD on top would double it. */
    if (g_synth) {
    lx = st.Gamepad.sThumbLX;
    ly = st.Gamepad.sThumbLY;
    if (abs(lx) > g_deadzone || abs(ly) > g_deadzone) {
        if (ly >  g_deadzone) want_mask |= 1u << (KEY_COUNT + 0); /* W */
        if (ly < -g_deadzone) want_mask |= 1u << (KEY_COUNT + 1); /* S */
        if (lx < -g_deadzone) want_mask |= 1u << (KEY_COUNT + 2); /* A */
        if (lx >  g_deadzone) want_mask |= 1u << (KEY_COUNT + 3); /* D */
    }
    for (i = 0; i < 4; i++) {
        static unsigned raw_prev[4];
        int idx = (int)KEY_COUNT + i;
        int on = (want_mask >> idx) & 1;
        int eff;

        /* 2-sample debounce: stick wobble when pressing buttons must not
         * flap movement keys (proven by log: 12 down/up cycles in 5s) */
        if (on != (int)raw_prev[i])
            eff = g_key_state[idx];   /* first sample of a change: hold */
        else
            eff = on;                 /* stable for 2 samples: apply */
        raw_prev[i] = (unsigned)on;
        if (eff != g_key_state[idx])
            apply_key(idx, eff);
    }
    }
}

/* reentrancy guard: ticks may come from the 0x14FE pump event (menus)
 * and from the pad GetDeviceState poll (in-world) */
static void gamepad_tick(void)
{
    if (g_in_tick)
        return;
    g_in_tick = 1;
    gamepad_tick_real();
    g_in_tick = 0;
}

/* forward refs for the M2 pad-binds block below */
static void apply_own_pad_binds(unsigned char *exe, void *mgr);
#define MGR_GLOBAL_VA 0x014F16FCu
/* game-thread application: the background thread only ARMS the flag
 * once the binding manager exists and boot bind traffic has settled;
 * the pump hook (engine's own thread) performs the binds. Applying
 * from our own thread crashed the game: binding into occupied
 * actions triggers a synchronous ~80-calls-per-bind UI slot resync,
 * which raced the menu renderer (diag#7 crash on menu cursor move). */
static volatile long g_want_pad_binds, g_pad_binds_done,
                     g_own_binds_running;
static volatile long g_pump_hook_installed;
/* consecutive Bind calls with slot==-1: the btnRevert/reset signature
 * (defaults re-apply; runtime rebinds use explicit slots 0/1) */
static volatile long g_reset_burst;
/* §157: incremented by the reset-function hook (0x82CC10). The burst
 * heuristic above misses the options "reset to defaults": it clears the
 * table without Bind(-1) traffic, so the direct counter is the reliable
 * reset signal. */
volatile long g_reset_seen;
static volatile unsigned long g_reset_seen_ms;
/* §167: the wwise 'openPopUp' event marks a save load. Our own writers
 * (bind re-apply, flag force) stay out of the load window: the game
 * black-screened a load once with our reset handling racing it. */
static volatile unsigned long g_load_ms;
static volatile int g_defer_reapply;

extern "C" void __cdecl pump_tick_c(unsigned type, const unsigned *args)
{
    if (type < PUMP_HIST_SIZE)
        InterlockedIncrement(&g_pump_hist[type]);
    if (g_pump_fp) {
        InterlockedIncrement(&g_pump_events);
        fprintf(g_pump_fp, "%lu PUMP t=%03X a=%08X %08X %08X %08X\n",
                GetTickCount(), type, args[0], args[1], args[2], args[3]);
    }
    if (args && args[3])
        g_pump_manager = args[3];
    /* one-shot native pad binds: applied HERE on the engine's own
     * thread once the background armer flags readiness (diag#7
     * crashed applying them from our thread mid-frame) */
    if (g_want_pad_binds && !g_pad_binds_done) {
        unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
        void *mgr = *(void * volatile *)(exe + (MGR_GLOBAL_VA -
                                                EXE_DEFAULT_BASE));

        if (mgr) {
            g_pad_binds_done = 1;
            apply_own_pad_binds(exe, mgr);
        }
    }
    /* 0x14FE = internal per-frame UI tick (~100/s) — our poll cadence.
     * Its measured rate doubles as a game-state detector: ~9/s in the
     * world vs ~100/s in menus (10x margin, no hysteresis needed). */
    if (type == 0x14FE) {
        static DWORD win_start;
        static long win_count;
        DWORD now_ms = GetTickCount();

        win_count++;
        if (!win_start) {
            win_start = now_ms;
        } else if (now_ms - win_start >= 500) {
            g_pump_rate = win_count * 1000 / (now_ms - win_start);
            win_start = now_ms;
            win_count = 0;
        }
        if (g_synth)
            gamepad_tick();
    }
}

static void load_xinput(void)
{
    if (g_xinput_get)
        return;
    g_xinput_lib = LoadLibraryA("xinput1_4.dll");
    if (!g_xinput_lib)
        g_xinput_lib = LoadLibraryA("xinput1_3.dll");
    if (!g_xinput_lib)
        g_xinput_lib = LoadLibraryA("xinput9_1_0.dll");
    if (g_xinput_lib)
        g_xinput_get = (XInputGetState_fn)GetProcAddress(
            g_xinput_lib, "XInputGetState");
    if (g_xinput_lib)
        g_xinput_set = (XInputSetState_fn)GetProcAddress(
            g_xinput_lib, "XInputSetState");
    log_line("xinput: lib=%p get=%p set=%p", g_xinput_lib,
             (void *)g_xinput_get, (void *)g_xinput_set);
    /* §158-diag: the engine's XInput rumble variant gates SetVibration
     * on IsConnected, which requires nonzero vibration caps. Log them
     * once — zero fields would explain "no rumble" with a live pad. */
    if (g_xinput_lib) {
        typedef unsigned long (WINAPI *caps_fn)(unsigned long,
                                                unsigned long, void *);
        caps_fn caps = (caps_fn)GetProcAddress(g_xinput_lib,
                                               "XInputGetCapabilities");

        if (caps) {
            unsigned char buf[20];
            unsigned long rc;

            memset(buf, 0, sizeof(buf));
            rc = caps(g_user_index, 1, buf);
            log_line("rumble: caps rc=%lu wx=%u dw=%u", rc,
                     (unsigned)*(unsigned short *)(buf + 12),
                     (unsigned)*(unsigned short *)(buf + 14));
        }
    }
}

/* ------------- B1.2: DirectInput keyboard state injection ------------- */

/* exe IAT slot for DINPUT8.dll!DirectInput8Create (VA, retail build) */
#define DICREATE_SLOT_VA 0x012B7018u
/* exe IAT slot for USER32.dll!GetKeyboardState — the engine's actual
 * keyboard source (wrapper at 0x8EAC60 calls it; DirectInput is not used
 * for the keyboard at all) */
#define GETKBSTATE_SLOT_VA 0x012B7A40u

static const unsigned char kGuidSysKeyboard[16] = {
    0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
    0xBF, 0x08, 0x7F, 0x00, 0x2A, 0xE1, 0x03, 0x1E
};
static const unsigned char kGuidSysMouse[16] = {
    0x6F, 0x1D, 0x2B, 0x60, 0xD5, 0xA0, 0x11, 0xCF,
    0xBF, 0x08, 0x7F, 0x00, 0x2A, 0xE1, 0x03, 0x1E
};

typedef long (__stdcall *DirectInput8Create_fn)(void *, unsigned long,
                                                const void *, void **,
                                                void *);
typedef BOOL (__stdcall *GetKeyboardState_fn)(unsigned char *);
typedef long (__fastcall *dic_CreateDevice_fn)(void *, int, const void *,
                                               const void *, void **,
                                                void *);
typedef long (__fastcall *did_GetDeviceState_fn)(void *, int,
                                                 unsigned long, void *);
typedef long (__fastcall *did_GetDeviceData_fn)(void *, int, void *,
                                                unsigned long *,
                                                unsigned long);

#define MAX_PAD_DEVS 32
struct pad_dev_slot {
    void *dev;
    void **vtable;
    did_GetDeviceState_fn orig_state;
    unsigned long cb;
    unsigned last_hash;
    int logged;
};

static struct pad_dev_slot g_pads[MAX_PAD_DEVS];
static int g_pad_count;
/* distinct device vtables seen in CreateDevice (diagnostic: module map) */
static void **g_vt_seen[8];
static int g_vt_count;

/* pad GetDeviceState hook: the game polls the pad every frame in-world
 * (native binds prove it) — our synthesis cadence. The vtable is shared
 * per device class, so one saved original serves all churned devices. */
static did_GetDeviceState_fn g_pad_orig_state;
static volatile long g_pad_polls;
static unsigned g_pad_last_hash;
static int g_pad_dumps;

/* naked GetDeviceData (vtbl 10) tracer: the engine calls this slot with a
 * non-standard convention (observed ECX=vtable, shifted stack args; a C
 * fastcall hook crashed on stack cleanup). The thunk only MOVes the call
 * context into globals and jumps to the original with the stack and all
 * callee-visible registers untouched — safe under any convention. The
 * watchdog prints a snapshot whenever the return address changes. */
extern "C" {
    volatile long g_gdd_calls;
    void *volatile g_gdd_orig;
    volatile unsigned g_gdd_ecx, g_gdd_edx, g_gdd_ret;
    volatile unsigned g_gdd_stk[8];
    void gdd_thunk(void); /* file-scope asm below */
    /* same tracer shape for slot 9 (GetDeviceState) — separate orig so one
     * vtable can carry both slots safely */
    volatile long g_gds_calls;
    void *volatile g_gds_orig;
    volatile unsigned g_gds_ecx, g_gds_edx, g_gds_ret;
    volatile unsigned g_gds_stk[8];
    void gds_thunk(void);
}

#define NAKED_TRACE(pfx)                                         \
__asm__(                                                        \
    ".globl _" #pfx "_thunk\n"                                  \
"_" #pfx "_thunk:\n"                                            \
    "popl  %eax\n"                                              \
    "movl  %eax, _g_" #pfx "_ret\n"                             \
    "movl  %ecx, _g_" #pfx "_ecx\n"                             \
    "movl  %edx, _g_" #pfx "_edx\n"                             \
    "movl  (%esp), %eax\n"                                      \
    "movl  %eax, _g_" #pfx "_stk+0\n"                           \
    "movl  4(%esp), %eax\n"                                     \
    "movl  %eax, _g_" #pfx "_stk+4\n"                           \
    "movl  8(%esp), %eax\n"                                     \
    "movl  %eax, _g_" #pfx "_stk+8\n"                           \
    "movl  12(%esp), %eax\n"                                    \
    "movl  %eax, _g_" #pfx "_stk+12\n"                          \
    "movl  16(%esp), %eax\n"                                    \
    "movl  %eax, _g_" #pfx "_stk+16\n"                          \
    "movl  20(%esp), %eax\n"                                    \
    "movl  %eax, _g_" #pfx "_stk+20\n"                          \
    "movl  24(%esp), %eax\n"                                    \
    "movl  %eax, _g_" #pfx "_stk+24\n"                          \
    "movl  28(%esp), %eax\n"                                    \
    "movl  %eax, _g_" #pfx "_stk+28\n"                          \
    "incl  _g_" #pfx "_calls\n"                                 \
    "movl  _g_" #pfx "_ret, %eax\n"                             \
    "pushl %eax\n"                                              \
    "jmp  *_g_" #pfx "_orig\n"                                  \
);

NAKED_TRACE(gdd)
NAKED_TRACE(gds)

static unsigned buf_hash(const unsigned char *p, unsigned long n)
{
    unsigned h = 2166136261u;
    unsigned long i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}


static DirectInput8Create_fn g_real_dicreate;
static GetKeyboardState_fn g_real_getkbstate;
static dic_CreateDevice_fn g_real_di8_createdevice;
static did_GetDeviceState_fn g_real_kb_getstate;
static void *g_kb_device;         /* IDirectInputDevice8 (keyboard) */
static void *g_mouse_device;      /* kept for future cursor synthesis (B4) */

static int guid_eq(const void *a, const unsigned char *b)
{
    return memcmp(a, b, 16) == 0;
}

static void patch_vtable_entry(void *obj, int index, void *our_fn,
                               void **out_orig)
{
    void ***vt = (void ***)obj;
    DWORD old;

    if (!obj || !*vt)
        return;
    if ((*vt)[index] == our_fn)
        return; /* already patched: keep the saved original untouched */
    *out_orig = (*vt)[index];
    if (VirtualProtect(&(*vt)[index], sizeof(void *),
                       PAGE_EXECUTE_READWRITE, &old)) {
        (*vt)[index] = our_fn;
        VirtualProtect(&(*vt)[index], sizeof(void *), old, &old);
    }
}

static long __fastcall hk_kb_GetDeviceState(void *self, int edx_dummy,
                                            unsigned long len, void *buf)
{
    long hr;
    int i;

    if (!g_real_kb_getstate)
        return 0x8007000EL /* E_FAIL: unhooked */;
    hr = g_real_kb_getstate(self, edx_dummy, len, buf);
    if (hr >= 0 && len == 256 && buf) {
        unsigned char *kb = (unsigned char *)buf;

        for (i = 0; i < (int)STICK_KEY_COUNT; i++) {
            unsigned char dik = i < (int)KEY_COUNT ? g_keys[i].dik
                                                   : g_stick_keys[i - KEY_COUNT].dik;

            if (g_key_state[i])
                kb[dik] |= 0x80;
        }
    }
    return hr;
}

/* per-frame cadence: runs on the game thread whenever the engine polls
 * the pad (in-world). Also dumps the state layout on change (Diag). */
static long __fastcall hk_pad_GetDeviceState(void *self, int edx_dummy,
                                             unsigned long cb, void *buf)
{
    long hr;

    if (!g_pad_orig_state)
        return 0x8007000EL;
    hr = g_pad_orig_state(self, edx_dummy, cb, buf);
    InterlockedIncrement(&g_pad_polls);
    if (g_pad_polls == 1)
        log_line("padstate: FIRST self=%p cb=%lu buf=%p hr=%08lX",
                 self, cb, buf, (unsigned long)hr);
    if (g_diag && hr >= 0 && buf && cb >= 0x10 && !IsBadWritePtr(buf, cb)) {
        unsigned n = cb < 0x30 ? (unsigned)cb : 0x30;
        unsigned h = buf_hash((const unsigned char *)buf, n);

        if (h != g_pad_last_hash && g_pad_dumps < 40) {
            const unsigned char *p = (const unsigned char *)buf;
            int k;

            g_pad_last_hash = h;
            g_pad_dumps++;
            for (k = 0; k < (int)n; k += 16)
                log_line("padstate[%d] +%02X: %02X %02X %02X %02X %02X %02X "
                         "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                         g_pad_dumps, k, p[k], p[k+1], p[k+2], p[k+3],
                         p[k+4], p[k+5], p[k+6], p[k+7], p[k+8], p[k+9],
                         p[k+10], p[k+11], p[k+12], p[k+13], p[k+14],
                         p[k+15]);
        }
    }
    gamepad_tick();
    return hr;
}

static long __fastcall hk_di8_CreateDevice(void *self, int edx_dummy,
                                           const void *rguid, const void *riid,
                                           void **out, void *unk)
{
    long hr;

    if (!g_real_di8_createdevice)
        return 0x8007000EL;
    hr = g_real_di8_createdevice(self, edx_dummy, rguid, riid, out, unk);

    if (hr >= 0 && out && *out) {
        char b1[64], b2[64];
        unsigned ra = (unsigned)__builtin_return_address(0);
        unsigned base = (unsigned)GetModuleHandleA(NULL);

        /* dump ALL FOUR arg pointers + contents: the calling convention of
         * this slot is nonstandard (a previous session proved the first
         * logged "GUID" was actually a pointer to a COM object), so dump
         * everything and sort it out offline */
#define FMT16(p, buf) do { \
            if ((p) && !IsBadReadPtr((p), 16)) { \
                const unsigned char *q_ = (const unsigned char *)(p); \
                snprintf((buf), sizeof(b1), \
                         "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X%02X%02X", \
                         q_[3], q_[2], q_[1], q_[0], q_[5], q_[4], q_[7], q_[6], \
                         q_[8], q_[9], q_[10], q_[11], q_[12], q_[13], q_[14], q_[15]); \
            } else { \
                snprintf((buf), sizeof(b1), "<bad>"); \
            } \
        } while (0)
        FMT16(rguid, b1);
        FMT16(riid, b2);
        log_line("dinput: CD ra=%08X va=%08X rguid=%p[%s] riid=%p[%s] "
                 "out=%p unk=%p dev=%p",
                 ra, ra - base + 0x400000, rguid, b1, riid, b2, out, unk,
                 *out);
#undef FMT16

        /* patch the read path ONLY on the first non-kb/mouse device — its
         * vtable is shared by all enumerated HID instances (the gamepad
         * family). One global orig per slot = one vtable max (prev11 crash).
         * The real GUID arrives in our riid param; GUIDs here are INSTANCE
         * GUIDs (Data4 tail = BFC7-444553540000), so classify by Data1..3:
         * SysMouse 6F1D2B60-D5A0-11CF, SysKeyboard 6F1D2B61-D5A0-11CF. */
        if (!IsBadReadPtr(riid, 8) && g_pad_count < MAX_PAD_DEVS) {
            static const unsigned char kMouse8[8] = {
                0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11 };
            static const unsigned char kKbd8[8] = {
                0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11 };
            void ***vt = (void ***)*out;
            int is_mouse = memcmp(riid, kMouse8, 8) == 0;
            int is_kbd = memcmp(riid, kKbd8, 8) == 0;
            int i, new_vt = 1;

            for (i = 0; i < g_vt_count; i++)
                if (g_vt_seen[i] == *vt)
                    new_vt = 0;
            if (new_vt && g_vt_count < (int)(sizeof(g_vt_seen) / sizeof(g_vt_seen[0]))) {
                HMODULE hm = NULL;
                char mod[MAX_PATH] = "?";

                g_vt_seen[g_vt_count++] = *vt;
                if (GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)*vt, &hm) && hm)
                    GetModuleFileNameA(hm, mod, sizeof(mod));
                log_line("dinput: vtbl=%p class=%s module=%s", (void *)*vt,
                         is_mouse ? "sysmouse" : is_kbd ? "syskeyboard"
                                                        : "hid-instance",
                         mod);
            }
            g_pads[g_pad_count].dev = *out;
            g_pads[g_pad_count].vtable = *vt;
            g_pads[g_pad_count].orig_state = NULL;
            g_pad_count++;
            /* naked tracers, convention-proof (pure forward): slot 10 =
             * buffered GetDeviceData, slot 9 = immediate GetDeviceState.
             * ONE vtable only — the first hid-instance device. */
            if (!is_mouse && !is_kbd) {
                if (!g_gdd_orig) {
                    patch_vtable_entry(*out, 10, (void *)&gdd_thunk,
                                       (void **)&g_gdd_orig);
                    if (g_gdd_orig)
                        log_line("paddat: GetDeviceData tracer on vtbl=%p",
                                 (void *)*vt);
                }
                if (!g_gds_orig) {
                    patch_vtable_entry(*out, 9, (void *)&gds_thunk,
                                       (void **)&g_gds_orig);
                    if (g_gds_orig)
                        log_line("paddat: GetDeviceState tracer on vtbl=%p",
                                 (void *)*vt);
                }
            }
        }
    }
    return hr;
}

static long __stdcall hk_DirectInput8Create(void *hinst, unsigned long ver,
                                            const void *riid, void **out,
                                            void *unk)
{
    long hr;

    if (!g_real_dicreate)
        return 0x8007000EL;
    hr = g_real_dicreate(hinst, ver, riid, out, unk);

    if (hr >= 0 && out && *out && !g_real_di8_createdevice) {
        patch_vtable_entry(*out, 3, (void *)&hk_di8_CreateDevice,
                           (void **)&g_real_di8_createdevice);
        if (g_real_di8_createdevice)
            log_line("dinput: IDirectInput8 hooked (CreateDevice)");
    }
    return hr;
}

/* ---------------- Larian PadDevice (NiInputGamePad) capture ---------------- */

/* engine PadDevice ctor @ VA 0xFBAEC0:
 *   mov eax,ecx; mov cl,[esp+4]; mov [eax],0x13DC0D4; mov [eax+4],cl; ret 4
 * 6-byte head is replaced with a cave that captures `this`. */
#define PADCTOR_VA 0x00FBAEC0u
#define PADCTOR_HEAD 6

/* Token::AppendName @ VA 0x8EC300 (virtual; the ONLY bind-slot text path):
 *   sub esp,0x1C; push edi; mov edi,ecx; cmp [edi+4],0 — group==0 => the
 * slot renders empty. Token = {vptr?, group@+4, index@+8, combo@+0xC};
 * groups: 1=pad buttons(20) 2=mouse(10) 3=pad axes 4=keyboard(145).
 * Diagnostic: log every distinct token the UI asks to name. */
#define APPENDNAME_VA 0x008EC300u
#define APPENDNAME_HEAD 6

static volatile long g_an_calls;

extern "C" void __cdecl appendname_c(unsigned self)
{
    static unsigned last_g, last_i, last_c;
    unsigned g, i, c;

    InterlockedIncrement(&g_an_calls);
    if (IsBadReadPtr((void *)(self + 4), 12))
        return;
    g = *(unsigned *)(self + 4);
    i = *(unsigned *)(self + 8);
    c = *(unsigned *)(self + 0xC);
    if (g != last_g || i != last_i || c != last_c) {
        last_g = g;
        last_i = i;
        last_c = c;
        TLOG("token: AppendName group=%u idx=%u combo=%u", g, i, c);
    }
}
static int install_appendname_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (APPENDNAME_VA - EXE_DEFAULT_BASE);
    unsigned char orig[APPENDNAME_HEAD];
    unsigned char *stub, *c;
    DWORD old;
    long rel;

    memcpy(orig, target, sizeof(orig));
    /* expected: 83 EC 1C 57 8B F9 (sub esp,0x1C; push edi; mov edi,ecx) */
    if (!(orig[0] == 0x83 && orig[1] == 0xEC && orig[2] == 0x1C &&
          orig[3] == 0x57 && orig[4] == 0x8B && orig[5] == 0xF9)) {
        log_line("token hook: prologue mismatch at %p: %02X %02X %02X %02X %02X %02X",
                 target, orig[0], orig[1], orig[2], orig[3], orig[4], orig[5]);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x51;                                  /* push ecx (this) */
    *c++ = 0xE8;                                  /* call appendname_c */
    rel = (long)((uintptr_t)&appendname_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;        /* add esp, 4 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, orig, APPENDNAME_HEAD); c += APPENDNAME_HEAD;
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + APPENDNAME_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, APPENDNAME_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    VirtualProtect(target, APPENDNAME_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, APPENDNAME_HEAD);
    log_line("token hook installed: target=%p stub=%p", target, stub);
    return 1;
}

/* ---- Token diagnostics round 2 -------------------------------------
 * AppendName (0x8EC300) installs fine but is NEVER called while the
 * settings UI renders bind slots -> slot text comes from another path.
 * Hook the remaining token surface:
 *   GetName   0x8EC090  81 EC 90 00 00 00   sub esp,0x90 (cookie prologue)
 *   operator= 0x8EC2B0  8B 44 24 04 8B 50 04  mov eax,[esp+4]; mov edx,[eax+4]
 *             (thiscall: ecx=dst, [esp+4]=src; copies +4/+8/+0xC)
 *   Parse     0x8EC350  81 EC FC 00 00 00   sub esp,0xFC (cookie prologue)
 * Heads are position-independent (no reloc fixups needed). */
static volatile long g_tfn_calls[3];

extern "C" void __cdecl token_getname_c(unsigned self)
{
    static unsigned lg = 0xFFFFFFFFu, li, lc;
    unsigned g, i, c;

    InterlockedIncrement(&g_tfn_calls[0]);
    if (IsBadReadPtr((void *)(self + 4), 12))
        return;
    g = *(unsigned *)(self + 4);
    i = *(unsigned *)(self + 8);
    c = *(unsigned *)(self + 0xC);
    if (g != lg || i != li || c != lc) {
        lg = g; li = i; lc = c;
        TLOG("token: GetName g=%u i=%u c=%u", g, i, c);
    }
}

extern "C" void __cdecl token_opeq_c(unsigned self)
{
    static unsigned lg = 0xFFFFFFFFu, li, lc, sg = 0xFFFFFFFFu;
    unsigned g, i, c, src;

    InterlockedIncrement(&g_tfn_calls[1]);
    if (IsBadReadPtr((void *)(self + 4), 12))
        return;
    g = *(unsigned *)(self + 4);
    i = *(unsigned *)(self + 8);
    c = *(unsigned *)(self + 0xC);
    /* arg0 (src token) above our saved frame:
     * [ret][ecx][pushad x8][pushfd][caller ret][arg0] */
    src = ((unsigned *)(uintptr_t)&self)[11];
    if (g != lg || i != li || c != lc || src != sg) {
        lg = g; li = i; lc = c; sg = src;
        if (src > 0x10000 && !IsBadReadPtr((void *)(src + 4), 12))
            TLOG("token: opeq dst(g=%u i=%u c=%u) <- src(g=%u i=%u c=%u)",
                     g, i, c, *(unsigned *)(src + 4), *(unsigned *)(src + 8),
                     *(unsigned *)(src + 0xC));
        else
            TLOG("token: opeq dst(g=%u i=%u c=%u) <- src=%08X",
                     g, i, c, src);
    }
}

extern "C" void __cdecl token_parse_c(unsigned self)
{
    static unsigned lg = 0xFFFFFFFFu, li, lc, la;
    unsigned g, i, c, arg0;
    char txt[80];

    InterlockedIncrement(&g_tfn_calls[2]);
    if (IsBadReadPtr((void *)(self + 4), 12))
        return;
    g = *(unsigned *)(self + 4);
    i = *(unsigned *)(self + 8);
    c = *(unsigned *)(self + 0xC);
    arg0 = ((unsigned *)(uintptr_t)&self)[11];
    if (g != lg || i != li || c != lc || arg0 != la) {
        lg = g; li = i; lc = c; la = arg0;
        txt[0] = 0;
        if (arg0 > 0x10000 && !IsBadStringPtrA((LPCSTR)arg0, 64))
            lstrcpynA(txt, (const char *)arg0, 64);
        TLOG("token: Parse g=%u i=%u c=%u str=\"%s\"", g, i, c, txt);
    }
}

static int install_thiscall_hook(unsigned va, unsigned head,
                                 const unsigned char *expect,
                                 void (__cdecl *cb)(unsigned),
                                 const char *name, int stack_arg)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (va - EXE_DEFAULT_BASE);
    unsigned char orig[8];
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    memcpy(orig, target, head);
    for (k = 0; k < head; k++) {
        if (orig[k] != expect[k]) {
            log_line("%s hook: prologue mismatch at %p byte %u: %02X != %02X",
                     name, target, k, orig[k], expect[k]);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    if (stack_arg) {
        /* cdecl: the argument sits on the stack right above the
         * return address -> [esp+0x28] after pushfd+pushad */
        *c++ = 0xFF; *c++ = 0x74; *c++ = 0x24; *c++ = 0x28;
    } else {
        *c++ = 0x51;                              /* push ecx (this) */
    }
    *c++ = 0xE8;                                  /* call cb (cdecl) */
    rel = (long)((uintptr_t)cb - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;        /* add esp, 4 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, orig, head); c += head;             /* replay head */
    *c++ = 0xE9;                                  /* jmp target+head */
    rel = (long)((uintptr_t)(target + head) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, head, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    for (k = 5; k < head; k++)
        target[k] = 0x90;
    VirtualProtect(target, head, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, head);
    log_line("%s hook installed: target=%p stub=%p", name, target, stub);
    return 1;
}

static void install_tokenfn_hooks(void)
{
    static const struct {
        const char *name;
        unsigned va, head;
        unsigned char expect[7];
        void (__cdecl *cb)(unsigned);
        int stack_arg;   /* 1 = the arg is on the stack (cdecl) */
    } fns[] = {
        { "GetName", 0x008EC090u, 6,
          { 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x00 }, token_getname_c, 0 },
        { "opeq",    0x008EC2B0u, 7,
          { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x50, 0x04 }, token_opeq_c, 1 },
        { "Parse",   0x008EC350u, 6,
          { 0x81, 0xEC, 0xFC, 0x00, 0x00, 0x00, 0x00 }, token_parse_c, 0 },
    };
    unsigned i;

    for (i = 0; i < 3; i++)
        install_thiscall_hook(fns[i].va, fns[i].head, fns[i].expect,
                              fns[i].cb, fns[i].name, fns[i].stack_arg);
}

/* -------- M2 axes: live evidence -------- */
/* NiInputDI8GamePad per-event handler 0xD1A960: thiscall(ecx=pad,
 * [esp+4]=dwOfs, [esp+0xC]=dwData). Jump table at 0xD1AEA8 maps
 * dwOfs 0..0x14 identically: 0-7 ANALOG CHANNELS (0xD11760 stores
 * dwData into pad+0x38 channel array, stride 0x20, invert mask
 * pad+0x40), 8 = POV (virtual buttons 0-3), 9..0x14 = half-axes
 * (virtual buttons 8..0x13, digitized against a threshold).
 * diag#10 proved sticks stream into channels 0-3. The pad format
 * static 0x15034B0 (getter 0xD1BFE0) never validated as built. */
#define EVH_VA 0x00D1A960u
#define EVH_HEAD 7u
#define PADFMT_VA 0x015034B0u
static volatile long g_ofs_hist[0x15];
static volatile long g_evh_calls;
static volatile DWORD g_last_pad_evt_ms;  /* §144: last real DI8 event */
volatile long g_camhold_skip_flag;        /* §144: read by the align stub */
int g_cam_hold_ext = 1;                   /* §144: CamHold, written from ini/dbgcmd */
static volatile unsigned g_last_pov = 0xFFFFFFFFu; /* diag#73 */
/* dwData range per dwOfs separates axes (0..65535) from buttons
 * (0x80/0) and digitized half-axes (0/0xFF); pad object is kept for
 * the channel dump (pad+0x38 -> 8 channels, stride 0x20:
 * raw@0, processed@4, prev@8/C, min@0x10, max@0x14) */
static volatile long g_dw_seen[0x15];
static volatile unsigned g_dw_min[0x15], g_dw_max[0x15];
static volatile unsigned g_pad_obj;
/* sign-aware per-channel ranges from the event stream (dwData of
 * dwOfs 0..7 = raw channel value): identifies which physical input
 * drives which channel — snapshots miss short blips, these don't */
static volatile long g_ch_smin[8], g_ch_smax[8];
static volatile long g_ch_seen[8];
static void stick_to_virt(void);
static void stick_to_virt_g(void);
static void trigger_to_virt(unsigned char *p);
static void sticky_fix_tick(void);

/* diag#53: on PC the game never polls the DI8 pad's axis state
 * (the channel array pad+0x38 stays 0), so Paddev::Update has
 * nothing to fill the query pairs from and GetAxisValue always
 * returns 0 -> gav(0..3) = 0 -> MOVC computes X=Y=0 -> no movement.
 * The EVENTS, however, do carry the axes: dwOfs 0..3 = LX/LY/RX/RY
 * with a signed percent value (-100..100). We keep the last value
 * of each and write them into the paddev query pairs ourselves -
 * the engine then reads them through its own GetAxisValue. */
static volatile int g_axis_raw[4];

extern "C" void __cdecl evh_c(unsigned self, unsigned dwofs, unsigned dwdata)
{
    g_evh_calls++;
    /* §148: arm the camera-align skip RIGHT HERE, on the game thread,
     * in the same frame as the engine's own event processing - the
     * camera thread's 8 ms tick can be too late for a one-shot align. */
    {
        g_last_pad_evt_ms = GetTickCount();
        if (g_cam_hold_ext)
            g_camhold_skip_flag = 1;
    }
    g_pad_obj = self;
    if (dwofs == 8)
        g_last_pov = dwdata;   /* diag#73: last POV angle (DI8 deg) */
    if (dwofs < 4)
        g_axis_raw[dwofs] = (int)(long)dwdata;
    if (dwofs < 0x15) {
        g_ofs_hist[dwofs]++;
        if (!g_dw_seen[dwofs]) {
            g_dw_seen[dwofs] = 1;
            g_dw_min[dwofs] = dwdata;
            g_dw_max[dwofs] = dwdata;
        } else {
            if (dwdata < g_dw_min[dwofs])
                g_dw_min[dwofs] = dwdata;
            if (dwdata > g_dw_max[dwofs])
                g_dw_max[dwofs] = dwdata;
        }
    }
    if (dwofs < 8) {
        long v = (long)(int)dwdata;

        if (!g_ch_seen[dwofs]) {
            g_ch_seen[dwofs] = 1;
            g_ch_smin[dwofs] = v;
            g_ch_smax[dwofs] = v;
        } else {
            if (v < g_ch_smin[dwofs])
                g_ch_smin[dwofs] = v;
            if (v > g_ch_smax[dwofs])
                g_ch_smax[dwofs] = v;
        }
    }
    /* diag#66: measure the engine's real "event -> virt slot" map.
     * The engine processes the event AFTER our observer returns, so
     * the diff between consecutive snapshots belongs to the PREVIOUS
     * event. Stick axes (dwOfs 0..3) flood the stream and never
     * write virt slots, so any delta is attributed to the last
     * non-axis event. Run with StickVirt=0 so our own writes do not
     * pollute the array. */
    {
        static unsigned char snap[20 * 0x10];
        static int have_snap;
        static unsigned pend_ofs = 0xFFFFFFFFu, pend_data;
        unsigned char *padp = (unsigned char *)(uintptr_t)g_pad_obj;

        if (padp && !IsBadReadPtr(padp, 0x160)) {
            unsigned va = *(unsigned *)(padp + 0x30);

            if (va > 0x10000 && !IsBadReadPtr((void *)va, 20 * 0x10)) {
                unsigned char *cur = (unsigned char *)va;

                if (have_snap) {
                    unsigned changed = 0;
                    int s;

                    for (s = 0; s < 20; s++)
                        if (*(unsigned *)(cur + s * 0x10) !=
                            *(unsigned *)(snap + s * 0x10))
                            changed |= 1u << s;
                    if (changed && pend_ofs != 0xFFFFFFFFu && g_trace) {
                        char mb[140];
                        int ml = 0, t;

                        for (t = 0; t < 20; t++)
                            if (changed & (1u << t))
                                ml += snprintf(mb + ml,
                                               sizeof(mb) - ml,
                                               "%X:%02X ", t,
                                               *(unsigned char *)
                                                   (cur + t * 0x10));
                        mb[ml > 0 ? ml - 1 : 0] = 0;
                        TLOG("btnmap: dwOfs=%02X dwData=%08X -> "
                                 "virt %s",
                                 pend_ofs, pend_data, mb);
                    }
                }
                memcpy(snap, cur, 20 * 0x10);
                have_snap = 1;
            } else {
                have_snap = 0;
            }
        } else {
            have_snap = 0;
        }
        if (dwofs > 3) { /* not a stick axis: candidate writer */
            pend_ofs = dwofs;
            pend_data = dwdata;
        }
    }
    /* §149: RT is an OS LMB click now (mousebtn_poll, camera thread).
     * The native L3-slot digitization stays behind RTNative=1 as the
     * old-path escape hatch; running both would double-fire attacks. */
    if (g_rt_native)
        trigger_to_virt((unsigned char *)(uintptr_t)self);
    stick_to_virt_g();
}

/* calibration (diag#17): channels 0=LX 1=LY 2=RX 3=RY, 5=LT(+)/RT(−).
 * diag#18 post-mortem: PROCESSED values of channels 0/1 are always 0
 * (their out-range calibration +0x18/+0x1C is never set on PC — the
 * ctor 0xD1B460 only writes in-min/max; channels 2/3 get their out
 * range from elsewhere and work). RAW (+0) is written unconditionally
 * by 0xD18540, so digitize from RAW. Down via 0xD12430(pad,idx,0xFF),
 * release via the engine's own up-path 0xD124B0(pad,idx) — same calls
 * the real event handler uses for dwOfs 9..0x14. */
static unsigned g_synth_virt47;
/* diag#98 (§128): one direction-slot transition per CONSUMED action
 * event. diag#97 gated on Paddev::Update entries, but the pad update
 * can run without consuming the dirty bit, so two slot changes still
 * landed in one consumption pass - and the engine drops the second g6
 * of a pass (d97a/d96b proof: the second event enters 0x8344D0 but
 * never reaches the mode listener). g_b270_calls counts the bind
 * manager's emissions (0x82B270) - the exact consumption signal. */
static volatile long g_b270_calls;
static volatile long g_last_dir_b270 = -1;
static DWORD g_last_dir_ms;
static volatile long g_pup_calls;   /* Paddev::Update entries (diag dump) */

/* diag#98 (§128): last-resort release clear. When the engine decides a
 * movement release (0x82B270 value=0, vkey 0..3) but its dispatch loses
 * the event, the direction bool on the mode-3 listener stays 1 and the
 * hero keeps running. Resolve the listener exactly like d95_poll and
 * clear bool[vkey] ourselves; pressing is left to the engine so a lost
 * press can never start movement. */
static void d98_release_clear(unsigned vkey)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned sub2, inner, mode, lst;

    if (vkey > 3)
        return;
    sub2 = *(unsigned *)(exe + (0x14F1704u - EXE_DEFAULT_BASE));
    if (sub2 <= 0x10000 || IsBadReadPtr((void *)(uintptr_t)sub2, 0x20))
        return;
    inner = *(unsigned *)(uintptr_t)(sub2 + 0x14);
    if (inner <= 0x10000 || IsBadReadPtr((void *)(uintptr_t)inner, 0x40))
        return;
    mode = *(unsigned *)(uintptr_t)(inner + 4);
    if (mode != 3)
        return;
    lst = *(unsigned *)(uintptr_t)(inner + 0x14);
    if (lst <= 0x10000 || IsBadReadPtr((void *)(uintptr_t)(lst + 0x0C), 1))
        return;
    if (*(unsigned char *)(uintptr_t)(lst + 8 + vkey) != 0) {
        *(unsigned char *)(uintptr_t)(lst + 8 + vkey) = 0;
        {
            static long n;
            if (++n <= 200)
                TLOG("d98: forceclr k=%u lst=%08X", vkey, lst);
        }
    }
}
static volatile long g_stk_busy;
/* declared here (used by stick_to_virt below, defined with the
 * other diag#35 natives near post_tick_c) */
static int g_stick_virt;
static int g_stick_src;      /* 0 = auto (XInput else DI8), 1 = DI8, 2 = XInput */
static int g_stick_dead = 35; /* stick digitize threshold, % of +-100.
                               * diag#73: 20 was TOO SMALL - the user's
                               * stick rests at -15..-25 after releasing
                               * a direction, flapped around the
                               * threshold and the engine latched the
                               * last direction ("the hero keeps
                               * running"). 35 clears that range. */
static int g_move_virt[4];   /* CANONICAL X360 button ids for the four
                              * movement directions, ini MoveVirt=U,D,L,R
                              * (default 10,11,8,9 = D-pad U/D/L/R).
                              * diag#66 btnmap PROVED the g1 bind idx is
                              * the CANONICAL id, not the virt-array
                              * slot: the live consumer polls
                              * va[k_canon_virt[canonical]]. Writing the
                              * array happens at the SLOT, so the two
                              * namespaces are translated by
                              * k_canon_virt (all 14 buttons measured). */
static const unsigned char k_canon_virt[14] = {
    /* canonical:  0=A 1=B 2=Y 3=X 4=BACK 5=START 6=LB 7=RB */
    /*             8=dpadL 9=dpadR 10=dpadU 11=dpadD 12=L3 13=R3 */
    9, 0xB, 8, 0xA, 0xF, 0xE, 4, 6, 2, 3, 0, 1, 0xC, 0xD
};
static volatile long g_stk_writes;
static volatile int g_stk_lx, g_stk_ly;
static volatile char g_stk_src;

static void stick_to_virt(void)
{
    /* diag#65: canonical X360 virt slots (PADNAMES 0x14CFB30 +
     * dispatch table 0x14CF030):
     *   0 = D-pad UP     1 = D-pad DOWN   2 = D-pad LEFT  3 = D-pad RIGHT
     *   4 = LB           6 = RB           8 = Y           9 = A
     *   A = X            B = B            C = L3          D = R3
     *   E = START        F = BACK         (5 and 7 are unused)
     * diag#64 mapped LEFT->4 (LB) RIGHT->C (L3) UP->6 (RB)
     * DOWN->0 (D-pad UP) - the hero then also walked when the player
     * pressed LB/RB/L3/D-pad-up ("movement on different buttons").
     * The stick now feeds the D-pad slots, which the physical D-pad
     * uses as well: stick and D-pad agree. */
    unsigned char dir_virt[4];
    typedef void (__thiscall *virtset_fn)(void *, int, int);
    typedef void (__thiscall *virtclr_fn)(void *, int);
    unsigned char *p, *exe;
    unsigned chp, want = 0, k;
    int lx, ly, use_xi;
    char src = 'D';

    if (!g_stick_virt || !g_pad_obj)
        return;
    p = (unsigned char *)(uintptr_t)g_pad_obj;
    if (IsBadReadPtr(p, 0x140))
        return;
    /* direction canonical ids come from ini MoveVirt (U,D,L,R); the
     * physical write goes to the virt SLOT the engine maps that
     * canonical button to (k_canon_virt, btnmap-proven diag#66) */
    {
        static const int ord[4] = { 2, 3, 0, 1 }; /* dir -> MoveVirt idx:
                                                     LEFT,RIGHT,UP,DOWN */
        int k2;

        for (k2 = 0; k2 < 4; k2++) {
            int canon = g_move_virt[ord[k2]];

            if (canon < 0 || canon >= 14)
                return; /* misconfigured MoveVirt - refuse to write */
            dir_virt[k2] = k_canon_virt[canon];
        }
    }
    for (k = 0; k < 4; k++)
        if (dir_virt[k] > 0x13)
            return; /* safety: slot out of the 20-entry array */

    /* stick values: XInput is the unambiguous source - sThumbLX IS
     * the left stick by definition. The DI8 channel layout was never
     * proven (channels 0/1 may be either stick) and the engine's own
     * axis path (GetAxisValue) is dead on PC, so "the sticks are
     * swapped" could not be explained by the engine. StickSrc:
     * 0 = auto (XInput when seen, else DI8), 1 = DI8, 2 = XInput. */
    lx = ly = 0;
    use_xi = (g_stick_src == 2) ||
             (g_stick_src == 0 && g_xinput_get && g_xinput_seen_ok);
    if (use_xi) {
        XINPUT_STATE_T xst;

        if (g_xinput_get(g_user_index, &xst) == 0) {
            lx = (int)(xst.Gamepad.sThumbLX * 100L / 32767L);
            ly = -(int)(xst.Gamepad.sThumbLY * 100L / 32767L);
            src = 'X';
        } else {
            use_xi = 0;
        }
    }
    if (!use_xi) {
        chp = *(unsigned *)(p + 0x38);
        if (chp > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)chp, 2 * 0x20)) {
            lx = (int)*(unsigned *)(chp + 0);
            ly = (int)*(unsigned *)(chp + 0x20);
        }
    }
    g_stk_src = src;
    g_stk_lx = lx;
    g_stk_ly = ly;
    if (lx < -g_stick_dead) want |= 1u << 0;   /* left   */
    if (lx > g_stick_dead) want |= 1u << 1;    /* right  */
    if (ly < -g_stick_dead) want |= 1u << 2;   /* up     */
    if (ly > g_stick_dead) want |= 1u << 3;    /* down   */
    if (want == g_synth_virt47)
        return;
    exe = (unsigned char *)GetModuleHandleA(NULL);
    /* diag#91: serialize direction edges. The engine mishandles TWO
     * simultaneous slot transitions - a fast diagonal flick changes
     * two bits in one event (user-proven trigger of the sticky hero;
     * paired stickdir lines with identical lx/ly in the logs). Emit
     * at most ONE bit transition per call; the remaining bits go out
     * on the next call (next DI8 event or next frame via the pup
     * flush - max delay ~16 ms, imperceptible).
     * diag#97 (§126): the call-level cut was NOT enough - both bits
     * still landed inside one Paddev::Update, which emits two g6
     * events and the engine drops the second (proven: d96b has only
     * the first). diag#98 (§128): Paddev::Update entries are the
     * wrong clock too (the body may skip the dirty bit). Gate on the
     * bind manager's emission (b270) instead: a new slot change is
     * only written after the previous one has been consumed. */
    {
        unsigned emitted = 0;

        for (k = 0; k < 4; k++) {
            unsigned bit = 1u << k;

            if ((want & bit) == (g_synth_virt47 & bit))
                continue;
            if (emitted)
                continue;           /* deferred to the next call */
            {
                static const char *dirname[4] = { "LEFT", "RIGHT", "UP",
                                                  "DOWN" };
                long pc = g_b270_calls;
                DWORD nw = GetTickCount();

                if (pc == g_last_dir_b270 && nw - g_last_dir_ms < 100)
                    break;          /* previous transition not consumed */
                g_last_dir_b270 = pc;
                g_last_dir_ms = nw;
                log_line("stickdir: %s %s (src=%c lx=%d ly=%d)",
                         dirname[k], (want & bit) ? "on" : "off", src,
                         lx, ly);
                if (want & bit)
                    ((virtset_fn)(exe +
                                  (0x00D12430u - EXE_DEFAULT_BASE)))
                        (p, (int)dir_virt[k], 0xFF);
                else
                    ((virtclr_fn)(exe +
                                  (0x00D124B0u - EXE_DEFAULT_BASE)))
                        (p, (int)dir_virt[k]);
                /* mirror the event handler's per-virt dirty bit
                 * ([pad+0x158] |= 1<<virt in every dwOfs 9..0x14
                 * case): without it the engine's virt->action
                 * dispatch never sees our transitions (diag#27
                 * breakthrough) */
                *(unsigned *)(p + 0x158) |= 1u << dir_virt[k];
                g_stk_writes++;
            }
            g_synth_virt47 ^= bit;
            emitted = 1;
        }
    }
}

/* diag#91: frame-rate flush of deferred direction bits - the DI8
 * event stream can stall when the stick parks, but pup_c runs every
 * frame, so a deferred bit never waits longer than one frame. The
 * busy-guard keeps the two callers (evh thread + game thread) from
 * interleaving a half-emitted state. */
static void stick_to_virt_g(void)
{
    if (InterlockedExchange(&g_stk_busy, 1))
        return;
    stick_to_virt();
    g_stk_busy = 0;
}

/* ---- diag#88 v2: sticky-hero fix (§106 verdict, §108 correction) ----
 * The latched movement magnitude is ANALOG slot 0x12 (18 decimal —
 * btnmap prints slot ids in HEX; §106 misread "12" as decimal and
 * diag#88 v1 wiped slot 0xC by mistake). The engine ramps slot
 * 0x12's value byte per input event with acceleration; a stalled
 * event stream (stick perfectly centered, no jitter) freezes it and
 * the hero keeps running. While the stick is parked we replay the
 * engine's own hard release for that slot: vclr 0xD124B0 zeroes
 * value/state/timestamp and clears the slot's down bits in
 * pad+0x10/+0x24 (§108). NOT touched: slot 0xC (the L3/RT-attack
 * carrier, diag#70 — its FF/00 flapping in btnmap is our own
 * trigger digitization, not the latch). */
static int g_sticky_fix = 1;
static volatile int g_rt_down;
static volatile long g_d88_zaps, g_d88_skips;
static volatile unsigned g_d88_last;

static void sticky_fix_tick(void)
{
    typedef void (__thiscall *virtclr_fn)(void *, int);
    unsigned char *p = (unsigned char *)(uintptr_t)g_pad_obj;
    unsigned char *exe;
    unsigned base, cur;
    int cx = 0, cy = 0, have_xi = 0;
    static DWORD last_log;
    static int armed;

    if (!armed) {
        armed = 1;
        if (g_sticky_fix)
            log_line("d88: sticky fix v2 armed (slot 0x12, dead=%d)",
                     g_stick_dead);
    }
    if (!g_sticky_fix || !g_stick_virt || !p)
        return;
    if (IsBadReadPtr(p, 0x34))
        return;
    base = *(unsigned *)(p + 0x30);
    if (base <= 0x10000 ||
        IsBadReadPtr((void *)(uintptr_t)(base + 0x12 * 0x10), 4))
        return;
    if (*(unsigned *)(p + 0x2c) <= 0x12)
        return;                     /* truncated slot array - refuse */
    /* stick parked? XInput is authoritative and independent of the
     * (possibly stalled) DI8 event stream */
    if (g_xinput_get && g_xinput_seen_ok) {
        XINPUT_STATE_T xst;

        if (g_xinput_get(g_user_index, &xst) == 0) {
            cx = (int)(xst.Gamepad.sThumbLX * 100L / 32767L);
            cy = (int)(xst.Gamepad.sThumbLY * 100L / 32767L);
            have_xi = 1;
        }
    }
    if (have_xi) {
        if (cx < -g_stick_dead || cx > g_stick_dead ||
            cy < -g_stick_dead || cy > g_stick_dead)
            return;                 /* deflected: charge is wanted */
    } else if (g_synth_virt47 || g_stk_lx < -g_stick_dead ||
               g_stk_lx > g_stick_dead || g_stk_ly < -g_stick_dead ||
               g_stk_ly > g_stick_dead) {
        return;      /* no XInput: trust the last digitized state */
    }
    cur = *(unsigned char *)(base + 0x12 * 0x10);
    if (!cur)
        return;
    exe = (unsigned char *)GetModuleHandleA(NULL);
    ((virtclr_fn)(exe + (0x00D124B0u - EXE_DEFAULT_BASE)))(p, 0x12);
    g_d88_zaps++;
    g_d88_last = cur;
    if (GetTickCount() - last_log < 200) {
        g_d88_skips++;
        return;
    }
    log_line("d88: zap va[0x12]=%02X->0 (zaps=%ld skip=%ld)", cur,
             g_d88_zaps, g_d88_skips);
    g_d88_skips = 0;
    last_log = GetTickCount();
}

/* diag#70: RT -> attack. act 0x4 (the game's LMB action) is bound
 * to canonical 12 (L3) and we digitize the right trigger into L3's
 * slot. The dispatch table 0x14CF030 has no canonical ids for the
 * triggers (14 entries, verified in the exe), so a real button has
 * to carry the trigger - L3 is the carrier (the physical L3 also
 * attacks). Kept independent of StickVirt: the trigger must work
 * even when the stick digitization is off. */
static void trigger_to_virt(unsigned char *p)
{
    typedef void (__thiscall *virtset_fn)(void *, int, int);
    typedef void (__thiscall *virtclr_fn)(void *, int);
    XINPUT_STATE_T xst;
    unsigned char *exe;
    unsigned slot = k_canon_virt[12];
    int rt, wr;

    if (!g_xinput_get || !g_xinput_seen_ok || !p)
        return;
    if (g_xinput_get(g_user_index, &xst) != 0)
        return;
    rt = (int)(xst.Gamepad.bRightTrigger * 100) / 255;
    /* hysteresis: ON at g_trig_thresh, OFF at half of it - the user's
     * RT hovered 54..66% (96 flaps in the diag#70 session) and each
     * flap re-fired the attack (act 0x4) */
    wr = g_rt_down ? rt >= g_trig_thresh / 2 : rt >= g_trig_thresh;
    if (wr == g_rt_down)
        return;
    g_rt_down = wr;
    exe = (unsigned char *)GetModuleHandleA(NULL);
    log_line("trig: RT=%d%% -> slot %X %s", rt, slot,
             wr ? "on" : "off");
    if (wr)
        ((virtset_fn)(exe + (0x00D12430u - EXE_DEFAULT_BASE)))
            (p, (int)slot, 0xFF);
    else
        ((virtclr_fn)(exe + (0x00D124B0u - EXE_DEFAULT_BASE)))
            (p, (int)slot);
    /* dirty bit, exactly like the event handler does */
    *(unsigned *)(p + 0x158) |= 1u << slot;
    g_stk_writes++;
}

/* ---- split-brain bridge v2 (diag#16): replace the query ----
 * diag#15 wrote bits into [paddev+4] believing it a flat bitmask —
 * WRONG: that region holds pointers (the decoder 0x735350
 * dereferences them); the write corrupted one and crashed the
 * game. v2 goes over the wrapper's head: REPLACE 0x6EAB70
 * (InputManager::IsPadButtonDown(idx), thiscall, ret $4, bool al)
 * so every caller (Token::IsDown g1 + 5 engine sites) reads the
 * live polled pad directly. idx 0-3 dpad, 8-0x13 buttons from the
 * live virt array; 4-7 = left stick digitized from channels 0/1
 * (free slots, movement binds live there). */
static volatile long g_ispb_calls, g_ispb_hits;
static volatile long g_ispb_idx[0x14], g_ispb_true[0x14];
extern "C" unsigned __cdecl ispb_c(unsigned idx)
{
    unsigned char *p;
    unsigned va, chp;
    unsigned ret;

    InterlockedIncrement(&g_ispb_calls);
    if (!g_pad_obj || idx >= 20)
        return 0;
    InterlockedIncrement(&g_ispb_idx[idx]);
    p = (unsigned char *)(uintptr_t)g_pad_obj;
    if (IsBadReadPtr(p, 0x140))
        return 0;
    ret = 0;
    /* diag#69: callers pass the CANONICAL button id, not the array
     * slot. Proof from the diag#68 session log: the menu checker
     * polled ispb(5) 9 times with 0 true - canonical 5 is START,
     * whose live slot is E; our old va[5] read (an unused slot)
     * could never see the button, which is why Start opened the
     * menu via the action bind but never closed it. Translate the
     * way the engine's own consumer does (k_canon_virt). */
    va = *(unsigned *)(p + 0x30);
    if (va > 0x10000 && !IsBadReadPtr((void *)va, 0x140)) {
        unsigned slot = (idx < 14) ? k_canon_virt[idx] : idx;

        ret = *(unsigned char *)(va + slot * 0x10) != 0;
    }
    (void)chp;
    InterlockedIncrement(&g_ispb_hits);
    if (ret)
        InterlockedIncrement(&g_ispb_true[idx]);
    return ret;
}

static volatile unsigned g_ispb_res;

static int install_ispb_hook(void)
{
    /* mov 0xc(ecx),ecx; test ecx,ecx; je +0x14 */
    static const unsigned char expect[7] =
        { 0x8B, 0x49, 0x0C, 0x85, 0xC9, 0x74, 0x14 };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x006EAB70u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < 7; k++) {
        if (target[k] != expect[k]) {
            log_line("ispb hook: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* mov eax,[esp+0x28] = idx */
    *c++ = 0x50;                                  /* push eax */
    *c++ = 0xE8;                                  /* call ispb_c */
    rel = (long)((uintptr_t)&ispb_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;        /* add esp, 4 */
    *c++ = 0xA3;                                  /* mov [g_ispb_res], eax */
    { unsigned gaddr = (unsigned)(uintptr_t)&g_ispb_res;
      memcpy(c, &gaddr, 4); c += 4; }             /* A3 takes moffs32 (absolute) */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    *c++ = 0xA1;                                  /* mov eax, [g_ispb_res] */
    { unsigned gaddr = (unsigned)(uintptr_t)&g_ispb_res;
      memcpy(c, &gaddr, 4); c += 4; }             /* A1 takes moffs32 (absolute) */
    *c++ = 0x24; *c++ = 0x01;                     /* and al, 1 */
    *c++ = 0xC2; *c++ = 0x04; *c++ = 0x00;        /* ret $4 (thiscall, 1 arg) */

    if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90; target[6] = 0x90;
    VirtualProtect(target, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 7);
    log_line("ispb hook installed (replaces 0x6EAB70): target=%p stub=%p",
             target, stub);
    return 1;
}

/* diag#74: setter watch — inline-hook BOTH virt setters (0xD12430
 * down / 0xD124B0 release) and log every call with the caller's
 * return address. The stub replays the prologue but splits its
 * short `jae` (its target lies far past the stub): common path
 * (idx < count) continues the replay and jmps back right after the
 * replaced bytes; the rare overflow path far-jmps to the function's
 * own early-ret. Calls made by this DLL show a ret inside
 * Div2Patch.dll; engine callers show a Divinity2.exe VA — that
 * separates "our writes" from every engine writer of the slots. */
static volatile long g_vset_calls, g_vset_dropped;
static volatile long g_vclr_calls, g_vclr_dropped;
static volatile DWORD g_last_clr[4];   /* diag#82 release timestamps */
static volatile unsigned g_vc_ctrl;    /* fwd: set by the vc2 hook */

/* §152: physical-dpad redirect. The engine's own DI8 handler writes
 * virt slots 0..3 for the POV hat — the very slots the movement
 * quartet polls (MoveVirt canonicals 8..11 -> virt 0..3), so a real
 * dpad press made the hero walk. We cannot move the quartet (those are
 * the only pollable slots), so instead every NON-our-module write to
 * 0..3 is pushed to the dead slots 16..19: nothing polls them, the
 * engine's own bookkeeping still sees a write, and our stick
 * digitizer (return address inside this DLL) passes through
 * untouched. The vclr (release) path needs the same redirection or a
 * physical dpad release would clear a held stick direction. */
static int g_vset_redirect_logged;
static unsigned vset_remap(int idx, unsigned ret)
{
    static unsigned lo, hi;

    if (!g_native_move || idx < 0 || idx > 3)
        return (unsigned)idx;
    if (!lo) {
        unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);

        if (exe) {
            unsigned char *pe = exe + *(unsigned *)(exe + 0x3C);

            lo = (unsigned)(uintptr_t)exe;
            hi = lo + *(unsigned *)(pe + 0x50);   /* SizeOfImage */
        }
    }
    /* §152-FIX: redirect only callers INSIDE the exe image (the
     * engine's own POV handler). The first version had this comparison
     * inverted: our stick_to_virt calls (return address in this DLL)
     * were redirected to the dead slots — left stick died — while the
     * engine's dpad writes passed, so the cross walked AND the stick
     * did not. Symptom signature: "stick dead + dpad walks". */
    if (ret >= lo && ret < hi) {
        if (!g_vset_redirect_logged) {
            g_vset_redirect_logged = 1;
            log_line("vset: dpad redirect active (idx %d -> %d ret=%08X)",
                     idx, idx + 0x10, ret);
        }
        return (unsigned)(idx + 0x10);
    }
    /* first pass-through of one of the four carrier slots = the stick
     * digitizer reaching the engine (proof the redirect left us alone) */
    {
        static int pass_logged;

        if (!pass_logged) {
            pass_logged = 1;
            log_line("vset: stick write passes (idx %d ret=%08X)", idx, ret);
        }
    }
    return (unsigned)idx;
}

static unsigned __cdecl vset_c(void *pad, int idx, int val, unsigned ret)
{
    static DWORD win_ms;
    static long sent;
    DWORD now = GetTickCount();
    unsigned out = vset_remap(idx, ret);

    InterlockedIncrement(&g_vset_calls);
    if (idx < 0 || idx >= 20)
        return out;
    if (now - win_ms >= 1000) {
        win_ms = now;
        sent = 0;
    }
    if (sent >= 120) {
        InterlockedIncrement(&g_vset_dropped);
        return out;
    }
    sent++;
    TLOG("vset: idx=%X val=%02X pad=%08X ret=%08X", idx,
             (unsigned)val & 0xFF, (unsigned)(uintptr_t)pad, ret);
    return out;
}

static unsigned __cdecl vclr_c(void *pad, int idx, unsigned ret)
{
    static DWORD win_ms;
    static long sent;
    DWORD now = GetTickCount();
    unsigned out = vset_remap(idx, ret);

    InterlockedIncrement(&g_vclr_calls);
    /* diag#82: timestamp the four movement-slot releases - the
     * diagonal-release signature (idx 2 + idx 0 close together)
     * arms the page-guard capture */
    if (idx >= 0 && idx < 4)
        g_last_clr[idx] = now;
    if (idx < 0 || idx >= 20)
        return out;
    if (now - win_ms >= 1000) {
        win_ms = now;
        sent = 0;
    }
    if (sent >= 120) {
        InterlockedIncrement(&g_vclr_dropped);
        return out;
    }
    sent++;
    TLOG("vclr: idx=%X pad=%08X ret=%08X", idx,
             (unsigned)(uintptr_t)pad, ret);
    return out;
}

/* ---------------- diag#84: fire-token + NiInputDevice::Update watch --
 * §90 statics: the dispatcher (0x733Exx core) walks the 14 canonical
 * IDs, and for each rising edge calls IsNewPress -> token(flag=0) ->
 * fire 0x8E7710(ctx,1,tok); for each falling edge IsReleased ->
 * token(flag=1) -> the same fire. If a REAL sticky episode shows the
 * release vclr but NO fire(flag=1) token, the edge was consumed by
 * the per-frame now->prev commit (NiInputDevice::Update, vtbl+0x1C,
 * 0xD11ED0) running between the vclr and the dispatcher poll - the
 * race hypothesis for the latch. The fire watch makes every token
 * visible (with the token header dwords: canon id + press/release
 * flag); the pump watch counts Update calls and logs the first
 * caller (resolves the virtual caller dynamically). */
static volatile long g_fire_calls, g_fire_dropped;
static volatile long g_pump84_calls;
static volatile long g_pump84_logged;

/* diag#90 v2 (§115): the river of tiny type-4 mouse deltas is the
 * prime suspect for the sticky hero. v1 dropped at the switch
 * 0x8E7660 - WRONG PLACE: most tokens travel via the queue
 * (0x8E7710 -> 0x8E81E0) whose replay bypasses the switch, so the
 * drop never fired while the river flowed. v2 drops at the GATEWAY
 * 0x8E7710 (ret 8, verified): while the stick is parked and NOT in
 * a menu (flag *(0x14F13C0)+0x52) small delta tokens (|dx|<=8,
 * a1==1, tok[0]==4) are answered with an emulated stdcall return.
 * Real mouse gestures (large deltas) always pass. */
static int g_river_fix = 1;
static volatile long g_river_drops, g_river_seen;
static volatile int g_drop_this;

static int stick_parked_now(void)
{
    int cx = 0, cy = 0, have = 0;

    if (g_xinput_get && g_xinput_seen_ok) {
        XINPUT_STATE_T xst;

        if (g_xinput_get(g_user_index, &xst) == 0) {
            cx = (int)(xst.Gamepad.sThumbLX * 100L / 32767L);
            cy = (int)(xst.Gamepad.sThumbLY * 100L / 32767L);
            have = 1;
        }
    }
    if (have)
        return cx >= -g_stick_dead && cx <= g_stick_dead &&
               cy >= -g_stick_dead && cy <= g_stick_dead;
    return !g_synth_virt47;
}

static void __cdecl fire84_c(unsigned this_, unsigned a1, unsigned tok,
                             unsigned ret)
{
    static DWORD win_ms;
    static long sent;
    DWORD now = GetTickCount();
    unsigned t0 = 0, t1 = 0, t2 = 0, t3 = 0;

    InterlockedIncrement(&g_fire_calls);
    if (tok > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)tok, 16)) {
        t0 = *(unsigned *)(uintptr_t)(tok + 0x0);
        t1 = *(unsigned *)(uintptr_t)(tok + 0x4);
        t2 = *(unsigned *)(uintptr_t)(tok + 0x8);
        t3 = *(unsigned *)(uintptr_t)(tok + 0xC);
    }
    /* diag#93: comparator tokens (g5 idx 0xE..0x13) and the g6
     * movement summary are rare and decisive - the 60-line/s river
     * limiter must never eat them (the 10133/10134 chatter pair in
     * the stuck log only survived by luck). */
    if (a1 == 1 && (t0 == 6 || (t0 == 5 && t1 >= 0xE && t1 <= 0x13))) {
        static long keep;
        if (keep < 4000) {
            keep++;
            TLOG("fireC: g%u i=%X a=%X b=%08X ret=%08X",
                     t0, t1, t2, t3, ret);
        }
    }
    if (a1 == 1 && t0 == 6) {
        /* diag#97 (§126): which branch would 0x8E7710 take? The
         * dispatcher owns two thread ids (this[0]/this[1]) and one
         * queue per id ([this+0xC]/[this+0x10]). 1 = immediate,
         * 2 = queued, 0 = dropped. */
        unsigned own0 = 0, own1 = 0, q1 = 0;
        int route = 0;
        DWORD tid = GetCurrentThreadId();

        if (this_ > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)this_, 0x14)) {
            own0 = *(unsigned *)(uintptr_t)this_;
            own1 = *(unsigned *)(uintptr_t)(this_ + 4);
            q1 = *(unsigned *)(uintptr_t)(this_ + 0xC);
            if (a1 == 0)
                route = (tid == own1) ? 1 : ((tid == own0 && q1) ? 2 : 0);
            else
                route = (tid == own0) ? 1 : ((tid == own1 && q1) ? 2 : 0);
        }
        TLOG("d97f: v=%u k=%u route=%d tid=%u own=%u/%u q1=%08X",
                 t1, t2, route, (unsigned)tid, own0, own1, q1);
    }
    if (now - win_ms >= 1000) {
        win_ms = now;
        sent = 0;
    }
    if (sent >= 60) {
        InterlockedIncrement(&g_fire_dropped);
    } else {
        sent++;
        TLOG("fire: this=%08X a1=%X tok=%08X [%08X %08X %08X %08X] "
                 "ret=%08X",
                 this_, a1, tok, t0, t1, t2, t3, ret);
    }
    g_drop_this = 0;
    if (g_river_fix && a1 == 1 && t0 == 4) {
        static unsigned exe84;
        unsigned mgrp;
        int dx = (int)(int)t1;
        int menu = 0;

        if (!exe84)
            exe84 = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
        InterlockedIncrement(&g_river_seen);
        mgrp = *(unsigned *)(exe84 + (0x014F13C0u - EXE_DEFAULT_BASE));
        if (mgrp > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)mgrp,
                                             0x54))
            menu = *(unsigned char *)(mgrp + 0x52) != 0;
        if (!menu && dx >= -8 && dx <= 8 && stick_parked_now()) {
            g_drop_this = 1;
            if (InterlockedIncrement(&g_river_drops) <= 200)
                log_line("d90: drop river dx=%d (drops=%ld)", dx,
                         g_river_drops);
        }
    }
}

static void __cdecl pump84_c(unsigned ret)
{
    InterlockedIncrement(&g_pump84_calls);
    if (!InterlockedExchange(&g_pump84_logged, 1))
        log_line("pump84: first Update ret=%08X (exeVA=%08X)", ret,
                 ret - (unsigned)(uintptr_t)GetModuleHandleA(NULL) +
                     0x400000u);
}

/* ---------------- diag#82: page-guard accessor trap ----------------
 * The sticky latch lives in state none of our instruments see: both
 * known direction writers are exonerated (vc2 pushes zeros even
 * during real walking, vc3 is never called) and the controller's
 * visible fields are idle constants. This trap puts PAGE_NOACCESS
 * on the slots page and the move-controller page for a few seconds
 * right after a REAL diagonal release (the user lets go, the hero
 * keeps running) and records the EIP of every instruction that
 * touches them - the unknown reader/writer driving the latch is
 * guaranteed to be in that list. */
#define GUARD_MAXPAGES 2
#define GUARD_MAXEIP 96
static volatile long g_guard_armed;
static volatile unsigned g_guard_page[GUARD_MAXPAGES];
static volatile long g_guard_eip[GUARD_MAXEIP];
static volatile long g_guard_hits[GUARD_MAXEIP];
static volatile long g_guard_dir[GUARD_MAXEIP];   /* 0=R 1=W (diag#82c) */
static volatile long g_guard_neip;
static volatile long g_guard_traps;
static volatile long g_guard_captures;
static volatile long g_guard_force;  /* diag#83: armguard command flag */
/* diag#86c: auto-arm (diagonal-release signature) is OFF by default:
 * arming the controller page killed the game <1 s after arming in two
 * real sessions (86a TF scheme, 86b sampling scheme - same death, log
 * ends at the armed line). 1 = research mode, opt-in via ini. */
static int g_guard_auto;
static volatile DWORD g_guard_deadline;
static volatile unsigned g_guard_va0;   /* slots array base (diag#82c:
                                         * the array lives at
                                         * *(pad+0x30), NOT on the pad
                                         * page - guarding the pad page
                                         * caught only heap neighbors) */
static volatile unsigned g_guard_va1;   /* diag#86: controller object
                                         * base, the page1 target range
                                         * [va1, va1+0x50) */

/* diag#86: re-protect sampler. The old TF single-step scheme was
 * safe with one cold page; arming the HOT controller page crashed
 * the game inside the first second (nested exception storm - log
 * ended right at the armed line, WER recorded nothing). Sampling:
 * a trap only records the EIP and lifts the guard; this thread
 * puts PAGE_NOACCESS back every ~12 ms for the whole window. Every
 * repeating accessor (60 fps loops) is still caught. */
static DWORD WINAPI guard_sampler_thread(void *param)
{
    DWORD deadline = (DWORD)(uintptr_t)param;

    while (GetTickCount() <= deadline) {
        DWORD dummy;
        int i;

        Sleep(12);
        if (!g_guard_armed)
            break;
        for (i = 0; i < GUARD_MAXPAGES; i++)
            if (g_guard_page[i])
                VirtualProtect((void *)(uintptr_t)g_guard_page[i], 0x1000,
                               PAGE_NOACCESS, &dummy);
    }
    return 0;
}

static LONG WINAPI guard_veh(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    PCONTEXT ctx = ep->ContextRecord;
    DWORD dummy;
    int i;

    /* SINGLE_STEP is always OURS (nothing else sets TF here) and one
     * may arrive right AFTER the capture window closed - returning
     * CONTINUE_SEARCH for it crashed the game exactly at the sticky
     * moment (diag#82 first deploy). Always swallow it and clear
     * TF. */
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        ctx->EFlags &= ~0x100u;
        if (g_guard_armed) {
            if (GetTickCount() <= g_guard_deadline) {
                for (i = 0; i < GUARD_MAXPAGES; i++)
                    if (g_guard_page[i])
                        VirtualProtect(
                            (void *)(uintptr_t)g_guard_page[i], 0x1000,
                            PAGE_NOACCESS, &dummy);
            } else {
                g_guard_armed = 0;
                for (i = 0; i < GUARD_MAXPAGES; i++)
                    if (g_guard_page[i])
                        VirtualProtect(
                            (void *)(uintptr_t)g_guard_page[i], 0x1000,
                            PAGE_READWRITE, &dummy);
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!g_guard_armed)
        return EXCEPTION_CONTINUE_SEARCH;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2) {
        unsigned addr = (unsigned)er->ExceptionInformation[1];
        unsigned eip = (unsigned)ctx->Eip;

        if (GetTickCount() > g_guard_deadline) {
            g_guard_armed = 0;
            for (i = 0; i < GUARD_MAXPAGES; i++)
                if (g_guard_page[i])
                    VirtualProtect((void *)(uintptr_t)g_guard_page[i],
                                   0x1000, PAGE_READWRITE, &dummy);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        for (i = 0; i < GUARD_MAXPAGES; i++) {
            if (g_guard_page[i] && addr >= g_guard_page[i] &&
                addr < g_guard_page[i] + 0x1000) {
                int j, slot = -1;

                /* diag#82c: record ONLY touches of the actual target
                 * ranges - the guarded pages hold unrelated heap
                 * neighbors whose traffic drowned the first capture.
                 * diag#86: TWO ranges now - the 16-slot array
                 * [g_guard_va0, +0x100) and the controller object
                 * [g_guard_va1, +0x50). ExceptionInformation[0]:
                 * 0=read, 1=write */
                if ((addr >= g_guard_va0 && addr < g_guard_va0 + 0x100) ||
                    (g_guard_va1 && addr >= g_guard_va1 &&
                     addr < g_guard_va1 + 0x50)) {
                    for (j = 0; j < GUARD_MAXEIP; j++)
                        if (!g_guard_eip[j] ||
                            (unsigned)g_guard_eip[j] == eip) {
                            slot = j;
                            break;
                        }
                    if (slot >= 0) {
                        g_guard_eip[slot] = (long)eip;
                        g_guard_dir[slot] =
                            (long)er->ExceptionInformation[0];
                        InterlockedIncrement(&g_guard_hits[slot]);
                    }
                }
                InterlockedIncrement(&g_guard_traps);
                /* diag#86b: lift the guard and let the sampler thread
                 * re-protect ~12 ms later - NO trap-flag single step
                 * (that crashed the game on the hot controller page) */
                for (j = 0; j < GUARD_MAXPAGES; j++)
                    if (g_guard_page[j])
                        VirtualProtect(
                            (void *)(uintptr_t)g_guard_page[j], 0x1000,
                            PAGE_READWRITE, &dummy);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI guard_armer_thread(void *param)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    DWORD last_capture = 0;
    int captures = 0;

    (void)param;
    for (;;) {
        DWORD now = GetTickCount();
        unsigned char *p;
        int force;
        DWORD dummy;

        Sleep(250);
        /* diag#86c: never touch the target pages while a window is
         * live - the old code rewrote g_guard_page[] every 250 ms and
         * a mid-window rewrite orphaned NOACCESS pages the VEH no
         * longer recognized (their AVs became fatal) */
        if (g_guard_armed)
            continue;
        p = (unsigned char *)(uintptr_t)g_pad_obj;
        if (!p || IsBadReadPtr(p, 0x160))
            continue;
        /* diag#82c: guard the SLOTS ARRAY page (va = *(pad+0x30)),
         * not the pad object page (heap neighbors drowned capture 1) */
        {
            unsigned va = *(unsigned *)(p + 0x30);

            if (va < 0x10000 || IsBadReadPtr((void *)(uintptr_t)va, 0x140))
                continue;
            g_guard_va0 = va;
            g_guard_page[0] = va & ~0xFFFu;
        }
        /* diag#83: the console's "armguard" bypasses the diagonal
         * release signature and the cooldown/capture budget - the
         * operator decides when and how many times to capture.
         * diag#86c: force=1 (armguard) captures the SLOTS page only
         * - proven safe across 82/83 and the 86b smoke (which had
         * page1=0 because the pad was dormant); force=2 (armguard2)
         * adds the controller page, which killed the game <1 s after
         * arming in BOTH real sessions (86a TF scheme, 86b sampling;
         * log ends at the armed line, WER: NtWaitForSingleObject+0xC) */
        force = InterlockedExchange(&g_guard_force, 0);
        if (!force && !g_guard_auto)
            continue;
        /* diag#86: page1 = the move-controller page (g_vc_ctrl).
         * §92: every input layer (slots, act levels, pad channels,
         * tokens) is clean during a real sticky - the driver state
         * lives below; ctrl+0x38..0x40 flicker between live world
         * coords and zeros while +0x44/+0x48 hold the frozen target.
         * Naming the ctrl page's accessors during a capture = naming
         * the movement driver. Page1 only when explicitly asked for. */
        g_guard_page[1] = 0;
        g_guard_va1 = 0;
        if ((force == 2 || g_guard_auto) && g_vc_ctrl > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)g_vc_ctrl, 0x50)) {
            g_guard_page[1] = g_vc_ctrl & ~0xFFFu;
            g_guard_va1 = g_vc_ctrl;
        }
        if (!force) {
            if (!g_last_clr[2] || !g_last_clr[0])
                continue;
            /* diagonal release signature: both 2 and 0 released within
             * the last 4 s, close together, and at least 120 s / one
             * capture since the last capture */
            if (now - g_last_clr[2] > 4000 || now - g_last_clr[0] > 4000)
                continue;
            if (g_last_clr[2] > g_last_clr[0] ?
                g_last_clr[2] - g_last_clr[0] > 2500 :
                g_last_clr[0] - g_last_clr[2] > 2500)
                continue;
            if (last_capture &&
                now - last_capture < 120000)
                continue;
            if (captures >= 10)
                continue;
        }

        memset((void *)g_guard_eip, 0, sizeof(g_guard_eip));
        memset((void *)g_guard_hits, 0, sizeof(g_guard_hits));
        memset((void *)g_guard_dir, 0, sizeof(g_guard_dir));
        g_guard_traps = 0;
        g_guard_neip = 0;
        log_line("GUARD: armed page0=%08X page1=%08X (%s)",
                 g_guard_page[0], g_guard_page[1],
                 force ? (force == 2 ? "manual both pages - DANGER"
                                     : "manual slots only")
                       : "auto diagonal - GuardAuto");
        g_guard_deadline = now + 6000;
        g_guard_armed = 1;
        VirtualProtect((void *)(uintptr_t)g_guard_page[0], 0x1000,
                       PAGE_NOACCESS, &dummy);
        if (g_guard_page[1])
            VirtualProtect((void *)(uintptr_t)g_guard_page[1], 0x1000,
                           PAGE_NOACCESS, &dummy);
        CreateThread(NULL, 0, guard_sampler_thread,
                     (void *)(uintptr_t)g_guard_deadline, 0, NULL);
        /* partial dumps every second: if the capture crashes the
         * game mid-window (as the first deploy did), the data
         * captured so far still lands in the log */
        {
            int s, j;

            for (s = 1; s <= 6; s++) {
                Sleep(1000);
                log_line("GUARD: t=%ds traps=%ld", s, g_guard_traps);
                for (j = 0; j < GUARD_MAXEIP; j++)
                    if (g_guard_eip[j] && g_guard_hits[j] > 0)
                        log_line("GUARD: t=%ds eip=%08X %c hits=%ld", s,
                                 (unsigned)g_guard_eip[j],
                                 g_guard_dir[j] ? 'W' : 'R',
                                 g_guard_hits[j]);
            }
        }
        g_guard_armed = 0;
        VirtualProtect((void *)(uintptr_t)g_guard_page[0], 0x1000,
                       PAGE_READWRITE, &dummy);
        if (g_guard_page[1])
            VirtualProtect((void *)(uintptr_t)g_guard_page[1], 0x1000,
                           PAGE_READWRITE, &dummy);
        {
            int j, n = 0;

            for (j = 0; j < GUARD_MAXEIP; j++)
                if (g_guard_eip[j])
                    n++;
            log_line("GUARD: done traps=%ld accessors=%d", g_guard_traps,
                     n);
            for (j = 0; j < GUARD_MAXEIP; j++)
                if (g_guard_eip[j] && g_guard_hits[j] > 0)
                    log_line("GUARD: eip=%08X exeVA=%08X %c hits=%ld",
                             (unsigned)g_guard_eip[j],
                             (unsigned)g_guard_eip[j] - (unsigned)(uintptr_t)
                                 exe + 0x400000u,
                             g_guard_dir[j] ? 'W' : 'R',
                             g_guard_hits[j]);
        }
        last_capture = GetTickCount();
        captures++;
        g_guard_captures = captures;
    }
}


static int install_vset_watch(void)
{
    /* mov eax,ecx; mov ecx,[esp+4]; cmp ecx,[eax+2C]; jae;
     * mov dl,[esp+8] */
    static const unsigned char expect[15] = {
        0x8B, 0xC1, 0x8B, 0x4C, 0x24, 0x04, 0x3B, 0x48, 0x2C,
        0x73, 0x70, 0x8A, 0x54, 0x24, 0x08
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x00D12430u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("vset watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 384, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x18; /* pad */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x28; /* idx */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x2C; /* val */
    *c++ = 0x8B; *c++ = 0x7C; *c++ = 0x24; *c++ = 0x24; /* ret */
    *c++ = 0x57; *c++ = 0x52; *c++ = 0x51; *c++ = 0x50; /* push r,v,i,p */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&vset_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x10;        /* add esp, 16 */
    *c++ = 0x89; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* §152: store
                                  * remapped idx back to the arg slot */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    *c++ = 0x8B; *c++ = 0xC1;                     /* mov eax,ecx */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x04; /* mov ecx,[esp+4] */
    *c++ = 0x3B; *c++ = 0x48; *c++ = 0x2C;        /* cmp ecx,[eax+2C] */
    *c++ = 0x72; *c++ = 0x05;                     /* jb +5 (was: jae far) */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 0x7B) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;                   /* early-ret d124ab */
    *c++ = 0x8A; *c++ = 0x54; *c++ = 0x24; *c++ = 0x08; /* mov dl,[esp+8] */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + sizeof(expect)) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;                   /* back to d1243f */
    if (!VirtualProtect(target, sizeof(expect), PAGE_EXECUTE_READWRITE,
                        &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, sizeof(expect) - 5);
    VirtualProtect(target, sizeof(expect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expect));
    log_line("vset watch installed: target=%p stub=%p", target, stub);
    return 1;
}

static int install_vclr_watch(void)
{
    /* push esi; mov esi,[esp+8]; mov edx,ecx; cmp esi,[edx+2C]; jae */
    static const unsigned char expect[12] = {
        0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0xD1, 0x3B, 0x72,
        0x2C, 0x73, 0x7A
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x00D124B0u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("vclr watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 384, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x18; /* pad */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x28; /* idx */
    *c++ = 0x8B; *c++ = 0x7C; *c++ = 0x24; *c++ = 0x24; /* ret */
    *c++ = 0x57; *c++ = 0x51; *c++ = 0x50;        /* push r,i,p */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&vclr_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;  /* §148 FIX: 3 args = 12 */        /* add esp, 16 */
    *c++ = 0x89; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* §152: store
                                  * remapped idx back to the arg slot */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    *c++ = 0x56;                                  /* push esi */
    *c++ = 0x8B; *c++ = 0x74; *c++ = 0x24; *c++ = 0x08; /* mov esi,[esp+8] */
    *c++ = 0x8B; *c++ = 0xD1;                     /* mov edx,ecx */
    *c++ = 0x3B; *c++ = 0x72; *c++ = 0x2C;        /* cmp esi,[edx+2C] */
    *c++ = 0x72; *c++ = 0x05;                     /* jb +5 (was: jae far) */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 0x86) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;                   /* early-ret d12536 */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 12) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;                   /* back to d124bc */
    if (!VirtualProtect(target, sizeof(expect), PAGE_EXECUTE_READWRITE,
                        &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, sizeof(expect) - 5);
    VirtualProtect(target, sizeof(expect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 12);
    log_line("vclr watch installed: target=%p stub=%p", target, stub);
    return 1;
}

static int install_fire84_watch(void)
{
    /* push ebx; mov ebx,[esp+8] - exactly 5 bytes, clean replay cut */
    static const unsigned char expect[5] = {
        0x53, 0x8B, 0x5C, 0x24, 0x08
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x008E7710u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("fire84 watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 256, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x18; /* this */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x28; /* a1 */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x2C; /* tok */
    *c++ = 0x8B; *c++ = 0x7C; *c++ = 0x24; *c++ = 0x24; /* ret */
    *c++ = 0x57; *c++ = 0x52; *c++ = 0x51; *c++ = 0x50; /* push r,t,a,p */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&fire84_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x10;        /* add esp, 16 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    /* diag#90 v2: if fire84_c flagged this token, emulate the
     * stdcall epilogue "ret 8" instead of dispatching it */
    *c++ = 0x83; *c++ = 0x3D;                     /* cmp dword.. */
    *(unsigned *)(uintptr_t)c = (unsigned)(uintptr_t)&g_drop_this;
    c += 4;
    *c++ = 0x01;
    *c++ = 0x75; *c++ = 0x03;                     /* jne +3 */
    *c++ = 0xC2; *c++ = 0x08; *c++ = 0x00;        /* ret 8 */
    *c++ = 0x53;                                  /* push ebx (replay) */
    *c++ = 0x8B; *c++ = 0x5C; *c++ = 0x24; *c++ = 0x08; /* mov ebx,[esp+8] */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 5) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, sizeof(expect), PAGE_EXECUTE_READWRITE,
                        &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, sizeof(expect) - 5);
    VirtualProtect(target, sizeof(expect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expect));
    log_line("fire84 watch installed: target=%p stub=%p", target, stub);
    return 1;
}

static int install_pump84_watch(void)
{
    /* push esi; xor esi,esi; push edi; cmp [ecx+2C],esi - 7 bytes:
     * the 5-byte jmp would cut the cmp in half, replay covers it */
    static const unsigned char expect[7] = {
        0x56, 0x33, 0xF6, 0x57, 0x39, 0x71, 0x2C
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x00D11ED0u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("pump84 watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 192, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x18; /* this (ecx) */
    *c++ = 0x8B; *c++ = 0x7C; *c++ = 0x24; *c++ = 0x24; /* ret */
    *c++ = 0x50; *c++ = 0x57;                     /* push p(this), r(ret) */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&pump84_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;        /* add esp, 8 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    *c++ = 0x56;                                  /* push esi (replay) */
    *c++ = 0x33; *c++ = 0xF6;                     /* xor esi,esi */
    *c++ = 0x57;                                  /* push edi */
    *c++ = 0x39; *c++ = 0x71; *c++ = 0x2C;        /* cmp [ecx+2C],esi */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 7) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, sizeof(expect), PAGE_EXECUTE_READWRITE,
                        &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, sizeof(expect) - 5);
    VirtualProtect(target, sizeof(expect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expect));
    log_line("pump84 watch installed: target=%p stub=%p", target, stub);
    return 1;
}

/* ---- diag#94: 0x82B270 - the bind manager's virtual-key emitter,
 * the single choke point where a movement release can be lost for
 * good (see NOTES-input.md §122). value: 1=down, 0=up; vkey: engine
 * virtual key, 0..3 are the four movement directions. A release dies
 * here either by dedup (A[vkey].4C latch already equals value) or by
 * the live-input poll 0x82B1E0 (some defining key still down). We
 * log all four facts; the poll is a pure device read, safe to repeat
 * from the hook. ---- */

typedef int (__fastcall *b270_poll_t)(void *ecx_, void *edx_,
                                      unsigned vkey);

extern "C" void __cdecl b270_c(unsigned this_, unsigned value,
                               unsigned vkey)
{
    static unsigned char *exe_base;
    static long keep;
    unsigned latch = 0xFFFFFFFFu, poll = 0xFFFFFFFFu;

    InterlockedIncrement(&g_b270_calls);
    if (value == 0)
        d98_release_clear(vkey);      /* diag#98 sticky safety net */
    if (keep >= 4000)
        return;
    keep++;
    if (!exe_base)
        exe_base = (unsigned char *)GetModuleHandleA(NULL);
    if (this_ > 0x10000 && vkey < 0x61 &&
        !IsBadReadPtr((void *)(uintptr_t)this_, 0x80)) {
        unsigned abeg = *(unsigned *)(uintptr_t)(this_ + 0x0C);
        unsigned aend = *(unsigned *)(uintptr_t)(this_ + 0x10);
        unsigned char *taba = (unsigned char *)(uintptr_t)abeg;
        unsigned char *tbb = *(unsigned char **)(uintptr_t)(this_ + 0x74);
        unsigned char *tbe = *(unsigned char **)(uintptr_t)(this_ + 0x78);
        if (abeg && aend > abeg && aend - abeg > vkey * 0x54 &&
            !IsBadReadPtr(taba + (uintptr_t)vkey * 0x54 + 0x4C, 4))
            latch = *(unsigned *)(uintptr_t)(taba +
                                             (uintptr_t)vkey * 0x54 + 0x4C);
        if (tbb && tbe > tbb && (tbe - tbb) > (int)vkey * 0x54 &&
            !IsBadReadPtr(tbb, 0x40)) {
            b270_poll_t poll_fn = (b270_poll_t)(uintptr_t)(
                exe_base + (0x0082B1E0u - EXE_DEFAULT_BASE));
            poll = (unsigned)poll_fn((void *)(uintptr_t)this_, NULL, vkey);
        }
    }
    TLOG("b270: v=%u k=%u latch=%08X poll=%u", value, vkey, latch,
             poll);
}

static int install_b270_watch(void)
{
    /* sub esp,38h; push ebx; push esi - exactly 5 bytes, clean cut */
    static const unsigned char expect[5] = {
        0x83, 0xEC, 0x38, 0x53, 0x56
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x0082B270u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("b270 watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 128, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* value */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x2C; /* vkey */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x18; /* this (saved ecx) */
    *c++ = 0x51; *c++ = 0x50; *c++ = 0x52;        /* push vkey,value,this */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&b270_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;        /* add esp, 12 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, sizeof(expect)); c += sizeof(expect);
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 5) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, sizeof(expect), PAGE_EXECUTE_READWRITE,
                        &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, sizeof(expect) - 5);
    VirtualProtect(target, sizeof(expect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expect));
    log_line("b270 watch installed: target=%p stub=%p", target, stub);
    return 1;
}

/* ---- diag#96: the g6 consumer chain. With f52=1 every g6 event goes
 * to 0x8E7D20, which walks subscribers UI -> bindmgr -> modal ->
 * dispatcher -> menu and STOPS at the first that returns true; only
 * then the gameplay subscriber (the mode listener) sees the token. A
 * release swallowed by an early subscriber never reaches the listener
 * and its direction bool stays up. d96a logs the chain input and
 * resolves the five early-out handlers once; d96b logs what actually
 * reaches the mode-3 listener's vtbl+8 (0x9F2550). ---- */
static unsigned vt4(unsigned obj)
{
    unsigned vt;

    if (obj <= 0x10000 || IsBadReadPtr((void *)(uintptr_t)obj, 4))
        return 0;
    vt = *(unsigned *)(uintptr_t)obj;
    if (vt <= 0x10000 || IsBadReadPtr((void *)(uintptr_t)(vt + 4), 4))
        return 0;
    return *(unsigned *)(uintptr_t)(vt + 4);
}

static volatile long g_d96a_hits;

extern "C" void __cdecl d96a_c(unsigned tok)
{
    static long keep;
    unsigned t1, t2;

    InterlockedIncrement(&g_d96a_hits);
    if (keep >= 4000)
        return;
    if (tok <= 0x10000 || IsBadReadPtr((void *)(uintptr_t)tok, 12))
        return;
    if (*(unsigned *)(uintptr_t)tok != 6)
        return;
    t1 = *(unsigned *)(uintptr_t)(tok + 4);
    t2 = *(unsigned *)(uintptr_t)(tok + 8);
    keep++;
    TLOG("d96a: v=%u k=%u", t1, t2);
    if (keep == 1) {
        unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
        unsigned A = *(unsigned *)(exe + (0x14F1700u - EXE_DEFAULT_BASE));
        unsigned B = *(unsigned *)(exe + (0x14F16FCu - EXE_DEFAULT_BASE));
        unsigned C = *(unsigned *)(exe + (0x14F13F0u - EXE_DEFAULT_BASE));
        unsigned D = *(unsigned *)(exe + (0x14F13E4u - EXE_DEFAULT_BASE));
        unsigned E = *(unsigned *)(exe + (0x14F13D0u - EXE_DEFAULT_BASE));
        unsigned G = *(unsigned *)(exe + (0x14F1704u - EXE_DEFAULT_BASE));
        unsigned D2 = (D > 0x10000 &&
                       !IsBadReadPtr((void *)(uintptr_t)(D + 4), 4)) ?
            *(unsigned *)(uintptr_t)(D + 4) : 0;
        int f = (int)(signed char)*(unsigned char *)
            (exe + (0x14F605Cu - EXE_DEFAULT_BASE));

        log_line("d96subs: A=%08X:%08X B=%08X:%08X C=%08X:%08X "
                 "D=%08X:%08X Dp4=%08X:%08X E=%08X:%08X G=%08X:%08X "
                 "f605c=%d",
                 A, vt4(A), B, vt4(B), C, vt4(C), D, vt4(D),
                 D2, vt4(D2), E, vt4(E), G, vt4(G), f);
    }
}

extern "C" void __cdecl d96b_c(unsigned lst, unsigned tok)
{
    static long keep;
    unsigned t0, t1, t2;

    if (keep >= 4000)
        return;
    if (tok <= 0x10000 || IsBadReadPtr((void *)(uintptr_t)tok, 12))
        return;
    t0 = *(unsigned *)(uintptr_t)tok;
    t1 = *(unsigned *)(uintptr_t)(tok + 4);
    t2 = *(unsigned *)(uintptr_t)(tok + 8);
    keep++;
    TLOG("d96b: lst=%08X t=%u v=%u k=%u", lst, t0, t1, t2);
}

static int install_d96a_watch(void)
{
    /* mov ecx,[014F1700h] - the operand is relocated at runtime, so the
     * check is semantic and the replay copies the live bytes */
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x008E7D20u - EXE_DEFAULT_BASE);
    unsigned char saved[6];
    unsigned char *stub, *c;
    DWORD old;
    long rel;

    if (IsBadReadPtr(target, 6) || target[0] != 0x8B ||
        target[1] != 0x0D ||
        *(unsigned *)(target + 2) !=
            (unsigned)(uintptr_t)(exe + (0x14F1700u - EXE_DEFAULT_BASE))) {
        log_line("d96a watch: head mismatch at %p", target);
        return 0;
    }
    memcpy(saved, target, 6);
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* token */
    *c++ = 0x50;                                  /* push token */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&d96a_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;        /* add esp, 4 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, saved, 6); c += 6;                  /* replay live bytes */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 6) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    VirtualProtect(target, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    log_line("d96a watch installed: target=%p stub=%p", target, stub);
    return 1;
}

static int install_d96b_watch(void)
{
    /* push esi; mov esi,[esp+8] - exactly 5 bytes, clean cut */
    static const unsigned char expect[5] = {
        0x56, 0x8B, 0x74, 0x24, 0x08
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x009F2550u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("d96b watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x28; /* token */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x18; /* this (ecx) */
    *c++ = 0x52; *c++ = 0x51;                     /* push token,this */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&d96b_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;        /* add esp, 8 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, sizeof(expect)); c += sizeof(expect);
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 5) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, sizeof(expect), PAGE_EXECUTE_READWRITE,
                        &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    VirtualProtect(target, sizeof(expect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expect));
    log_line("d96b watch installed: target=%p stub=%p", target, stub);
    return 1;
}

/* ---- diag#97 (§126): where does the SECOND same-tick g6 event die?
 * d96a proved 0x8E7D20 never runs, so the events take the q==0 branch
 * of the group switch (single subscriber DAT_014f13d8). d97a logs every
 * event entering the gameplay g6 handler 0x8344D0; d97e logs every
 * group-6 switch call with q; d97_dump prints the subscriber objects
 * and the hook liveness once. ---- */
static volatile long g_d97a_hits, g_d97e_hits;

static void __cdecl d97a_c(unsigned this_, unsigned tok)
{
    unsigned v = 0, k = 0, x = 0, t10 = 0;

    InterlockedIncrement(&g_d97a_hits);
    if (tok > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)tok, 12)) {
        v = *(unsigned *)(uintptr_t)tok;
        k = *(unsigned *)(uintptr_t)(tok + 4);
        x = *(unsigned *)(uintptr_t)(tok + 8);
    }
    if (this_ > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)(this_ + 0x14), 4))
        t10 = *(unsigned *)(uintptr_t)(this_ + 0x10);
    TLOG("d97a: this=%08X v=%u k=%u x=%08X t10=%08X",
             this_, v, k, x, t10);
}

static void __cdecl d97e_c(unsigned q, unsigned tok)
{
    unsigned v = 0, k = 0, x = 0;

    InterlockedIncrement(&g_d97e_hits);
    if (tok > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)tok, 12)) {
        v = *(unsigned *)(uintptr_t)tok;
        k = *(unsigned *)(uintptr_t)(tok + 4);
        x = *(unsigned *)(uintptr_t)(tok + 8);
    }
    TLOG("d97e: q=%u v=%u k=%u x=%08X", q, v, k, x);
}

static int install_d97a_watch(void)
{
    /* push esi; mov esi,ecx; mov ecx,[esi+14h] - 6 bytes, clean cut */
    static const unsigned char expect[6] = {
        0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x14
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x008344D0u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("d97a watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* tok */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x18; /* this */
    *c++ = 0x50; *c++ = 0x51;                     /* push tok,this */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&d97a_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;        /* add esp, 8 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, sizeof(expect)); c += sizeof(expect);
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 6) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, sizeof(expect), PAGE_EXECUTE_READWRITE,
                        &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    VirtualProtect(target, sizeof(expect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expect));
    log_line("d97a watch installed: target=%p stub=%p", target, stub);
    return 1;
}

static int install_d97e_watch(void)
{
    /* mov eax,[esp+8]; sub eax,0 - 7 bytes. The JE at +7 stays at its
     * original address and consumes the flags our replay sets. */
    static const unsigned char expect[7] = {
        0x8B, 0x44, 0x24, 0x08, 0x83, 0xE8, 0x00
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x008E8160u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("d97e watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x2C; /* q */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x28; /* tok */
    *c++ = 0x52; *c++ = 0x50;                     /* push tok,q */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&d97e_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;        /* add esp, 8 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, sizeof(expect)); c += sizeof(expect);
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 7) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    log_line("d97e watch installed: target=%p stub=%p", target, stub);
    return 1;
}

/* ---- diag#98 (§128): which branch of 0x8344D0 does each event take?
 * 0x83453D = early-out (xor al,al; pop esi; ret 4) - the listener is
 * never called; 0x834543 = the normal listener path. A payload seen at
 * 0x834543 without a following d96b means the listener selection
 * inside the normal path skipped the mode-3 object. ---- */
static volatile long g_d98a_hits, g_d98b_hits;

static void __cdecl d98a_c(unsigned tok)
{
    unsigned v = 0, k = 0;

    InterlockedIncrement(&g_d98a_hits);
    if (tok > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)tok, 8)) {
        v = *(unsigned *)(uintptr_t)tok;
        k = *(unsigned *)(uintptr_t)(tok + 4);
    }
    TLOG("d98a: norm v=%u k=%u", v, k);
}

static void __cdecl d98b_c(unsigned tok)
{
    unsigned v = 0, k = 0;

    InterlockedIncrement(&g_d98b_hits);
    if (tok > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)tok, 8)) {
        v = *(unsigned *)(uintptr_t)tok;
        k = *(unsigned *)(uintptr_t)(tok + 4);
    }
    TLOG("d98b: early v=%u k=%u", v, k);
}

static int install_d98a_watch(void)
{
    /* push ebx; push edi; mov ecx,esi; xor bl,bl - 6 bytes, the CALL
     * at +6 stays in place */
    static const unsigned char expect[6] = {
        0x53, 0x57, 0x8B, 0xCE, 0x32, 0xDB
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x00834543u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("d98a watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x2C; /* tok */
    *c++ = 0x50;                                  /* push tok */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&d98a_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;        /* add esp, 4 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, sizeof(expect)); c += sizeof(expect);
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 6) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    log_line("d98a watch installed: target=%p stub=%p", target, stub);
    return 1;
}

static int install_d98b_watch(void)
{
    /* xor al,al; pop esi; ret 4 - replay returns to the caller */
    static const unsigned char expect[6] = {
        0x32, 0xC0, 0x5E, 0xC2, 0x04, 0x00
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x0083453Du - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < sizeof(expect); k++) {
        if (target[k] != expect[k]) {
            log_line("d98b watch: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x2C; /* tok */
    *c++ = 0x50;                                  /* push tok */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&d98b_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;        /* add esp, 4 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, sizeof(expect)); c += sizeof(expect);
    *c++ = 0xE9;                                  /* dead: ret 4 above */
    rel = (long)((uintptr_t)(target + 6) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    log_line("d98b watch installed: target=%p stub=%p", target, stub);
    return 1;
}

static void d97_dump(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned A = *(unsigned *)(exe + (0x14F1700u - EXE_DEFAULT_BASE));
    unsigned B = *(unsigned *)(exe + (0x14F16FCu - EXE_DEFAULT_BASE));
    unsigned C = *(unsigned *)(exe + (0x14F13F0u - EXE_DEFAULT_BASE));
    unsigned D = *(unsigned *)(exe + (0x14F13E4u - EXE_DEFAULT_BASE));
    unsigned E = *(unsigned *)(exe + (0x14F13D0u - EXE_DEFAULT_BASE));
    unsigned G = *(unsigned *)(exe + (0x14F1704u - EXE_DEFAULT_BASE));
    unsigned H = *(unsigned *)(exe + (0x14F13D8u - EXE_DEFAULT_BASE));
    unsigned D2 = 0;
    unsigned grp = *(unsigned *)(exe + (0x14F13C0u - EXE_DEFAULT_BASE));

    if (D > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)(D + 4), 4))
        D2 = *(unsigned *)(uintptr_t)(D + 4);
    log_line("d97g: A=%08X:%08X B=%08X:%08X C=%08X:%08X "
             "D=%08X:%08X Dp4=%08X E=%08X:%08X G=%08X:%08X H=%08X:%08X",
             A, vt4(A), B, vt4(B), C, vt4(C), D, vt4(D), D2, E, vt4(E),
             G, vt4(G), H, vt4(H));
    log_line("d97h: f52=%d f54=%d f605c=%d h7d20=%02X h44d0=%02X "
             "h8160=%02X d96a=%ld d97a=%ld d97e=%ld d98a=%ld d98b=%ld "
             "pup=%ld",
             (grp > 0x10000 &&
              !IsBadReadPtr((void *)(uintptr_t)(grp + 0x54), 1)) ?
                 (int)*(unsigned char *)(uintptr_t)(grp + 0x52) : -1,
             (grp > 0x10000 &&
              !IsBadReadPtr((void *)(uintptr_t)(grp + 0x54), 1)) ?
                 (int)*(unsigned char *)(uintptr_t)(grp + 0x54) : -1,
             (int)*(signed char *)(exe + (0x14F605Cu - EXE_DEFAULT_BASE)),
             exe[0x8E7D20u - EXE_DEFAULT_BASE],
             exe[0x8344D0u - EXE_DEFAULT_BASE],
             exe[0x8E8160u - EXE_DEFAULT_BASE],
             g_d96a_hits, g_d97a_hits, g_d97e_hits, g_d98a_hits,
             g_d98b_hits, g_pup_calls);
}

static int install_evh_hook(void)
{
    /* mov eax,[esp+4]; push esi; mov esi,ecx */
    static const unsigned char expect[EVH_HEAD] =
        { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1 };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (EVH_VA - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < EVH_HEAD; k++) {
        if (target[k] != expect[k]) {
            log_line("evh hook: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    /* §148 FIX: the §143 caller-log build pushed a 4th argument (the
     * return address) but the cleanup still removed only 12 bytes, so
     * popad/popfd restored registers from a shifted stack and the
     * engine resumed with garbage -> the "first pad event" crash
     * (EIP=0x202). Three args, 12 bytes, is the balanced original. */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* mov eax,[esp+0x28] = arg1 (dwOfs) */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x2C; /* mov edx,[esp+0x2C] = arg2 (dwData) */
    *c++ = 0x52;                                  /* push edx (dwData) */
    *c++ = 0x50;                                  /* push eax (dwOfs) */
    *c++ = 0x51;                                  /* push ecx (this) */
    *c++ = 0xE8;                                  /* call evh_c */
    rel = (long)((uintptr_t)&evh_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;        /* add esp, 12 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, EVH_HEAD); c += EVH_HEAD;   /* replay prologue */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + EVH_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, EVH_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90; target[6] = 0x90;
    VirtualProtect(target, EVH_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, EVH_HEAD);
    log_line("evh hook installed: target=%p stub=%p", target, stub);
    return 1;
}

/* one-shot dump of the runtime-built pad DIDATAFORMAT + histogram
 * of handler dwOfs, printed by the watchdog every 5 s */
static int g_fmt_dumped;
static int g_fmt_raw_logged;
static void dump_pad_format(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned fmt = (unsigned)(uintptr_t)exe + (PADFMT_VA - EXE_DEFAULT_BASE);
    unsigned size, objsize, flags, datasize, numobjs, rgodf;
    unsigned i;

    if (g_fmt_dumped || IsBadReadPtr((void *)fmt, 24))
        return;
    size = *(unsigned *)fmt;
    objsize = *(unsigned *)(fmt + 4);
    flags = *(unsigned *)(fmt + 8);
    datasize = *(unsigned *)(fmt + 12);
    numobjs = *(unsigned *)(fmt + 16);
    rgodf = *(unsigned *)(fmt + 20);
    /* diag#10: the validated decode never fired - log the raw header
     * once so we see whether the format is ever built at all */
    if (!g_fmt_raw_logged) {
        g_fmt_raw_logged = 1;
        log_line("padfmt raw: size=%d objsize=%d flags=%d data=%d "
                 "objs=%d rgodf=%08X", size, objsize, flags, datasize,
                 numobjs, rgodf);
    }
    if (objsize != 0x10 || numobjs == 0 || numobjs > 60 ||
        rgodf < 0x400000 || IsBadReadPtr((void *)rgodf, numobjs * 0x10))
        return; /* not built yet (or garbage) - try again next tick */
    g_fmt_dumped = 1;
    /* fall through to the decoded dump below */
    log_line("padfmt: size=%d flags=%d data=%d objs=%d rgodf=%08X",
             size, flags, datasize, numobjs, rgodf);
    for (i = 0; i < numobjs && i < 24; i++) {
        unsigned e = rgodf + i * 0x10;
        unsigned guid = *(unsigned *)e, ofs = *(unsigned *)(e + 4);
        unsigned typ = *(unsigned *)(e + 8), fl = *(unsigned *)(e + 12);
        const char *tname = (typ & 0xFF) == 1 ? "AXIS" :
                            (typ & 0xFF) == 2 ? "BTN" :
                            (typ & 0xFF) == 3 ? "POV" : "???";
        log_line("  obj[%u] guid=%08X ofs=0x%X %s inst=%u fl=0x%X",
                 i, guid, ofs, tname, (typ >> 8) & 0xFFFF, fl);
    }
}


/* one-shot dump of the action-side query chain, decoded statically:
 * Token::IsDown (0x8EC5A0): group1 -> inputmgr [0x14F13E4] ->
 * [+0xC] sub -> [+4] paddev; gate [paddev+0x4C]!=3, idx 0..0x13,
 * then a state-vector lookup at (paddev+4). If paddev differs from
 * the polled pad object or +0x4C==3 or the vector is empty, pad
 * actions are dead regardless of binds. */
#define INPUTMGR_VA 0x014F13E4u
static int g_chain_dumped;
static unsigned g_qvec_begin, g_qvec_end;
static void module_of(unsigned addr, char *out, unsigned outsz)
{
    HMODULE m = NULL;

    out[0] = 0;
    if (!addr)
        return;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && m)
        GetModuleFileNameA(m, out, outsz);
}

static void dump_input_chain(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned im, sub, pad, i;
    char nm1[MAX_PATH], nm2[MAX_PATH];

    if (g_chain_dumped || !g_pad_obj)
        return; /* wait until the pad has been polled at least once */
    im = *(unsigned *)(exe + (INPUTMGR_VA - EXE_DEFAULT_BASE));
    if (!im || IsBadReadPtr((void *)im, 0x10))
        return;
    sub = *(unsigned *)(im + 0xC);
    if (!sub || IsBadReadPtr((void *)sub, 8))
        return;
    pad = *(unsigned *)(sub + 4);
    if (!pad || IsBadReadPtr((void *)pad, 0x60))
        return;
    g_chain_dumped = 1;
    g_qvec_begin = *(unsigned *)(pad + 4);
    g_qvec_end = *(unsigned *)(pad + 0x1C);
    module_of(*(unsigned *)pad, nm1, sizeof(nm1));
    module_of(*(unsigned *)(uintptr_t)g_pad_obj, nm2, sizeof(nm2));
    log_line("qchain: inputmgr=%08X sub=%08X paddev=%08X vt=%08X mod=%s "
             "(polled pad=%08X vt=%08X mod=%s %s)",
             im, sub, pad, *(unsigned *)pad, nm1,
             (unsigned)g_pad_obj, *(unsigned *)(uintptr_t)g_pad_obj, nm2,
             pad == g_pad_obj ? "SAME" : "DIFFERENT");
    {
        char buf[400];
        int len = 0;

        for (i = 0; i < 0x60 / 4; i++)
            len += snprintf(buf + len, sizeof(buf) - len, "%08X ",
                            *(unsigned *)(pad + i * 4));
        buf[len > 0 ? len - 1 : 0] = 0;
        log_line("paddev dwords: %s", buf);
    }
    if (g_qvec_begin > 0x10000 && g_qvec_end >= g_qvec_begin &&
        !IsBadReadPtr((void *)g_qvec_begin, 0xA0)) {
        char buf[400];
        int len = 0;
        unsigned n = g_qvec_end - g_qvec_begin;

        for (i = 0; i < 0xA0 / 4; i++)
            len += snprintf(buf + len, sizeof(buf) - len, "%08X ",
                            *(unsigned *)(g_qvec_begin + i * 4));
        buf[len > 0 ? len - 1 : 0] = 0;
        log_line("qvec: span=%u bytes: %s", n, buf);
    }
    if (!IsBadReadPtr((void *)(uintptr_t)g_pad_obj, 0x140)) {
        unsigned char *p = (unsigned char *)(uintptr_t)g_pad_obj;
        unsigned va = *(unsigned *)(p + 0x30);
        char buf[256];
        int len = 0, k;

        if (va > 0x10000 && !IsBadReadPtr((void *)va, 0x140)) {
            for (k = 0; k < 20; k++)
                len += snprintf(buf + len, sizeof(buf) - len,
                                "%X:%X ", (unsigned)k,
                                *(unsigned char *)(va + k * 0x10));
            buf[len > 0 ? len - 1 : 0] = 0;
            log_line("virtbtn cur[]: %s", buf);
        }
    }
}

/* ---- diag#19: paddev mode (THE gate) ----
 * Static decode: Paddev::Update 0x733CC0 (called every frame from the
 * input tick 0x7355F0) only fills the query-side axis pairs
 * [paddev+0x28..0x44] (pair0=axes0/1 LX/LY, pair1=2/3 RX/RY, pair2=
 * triggers) — and the button vector<bool> at [paddev+4] — when
 * [paddev+0x4C] == 2 (pad mode). In mode 1 (keyboard) it instead
 * scans the live pads for a whitelisted button (table 0x14CF030,
 * value 0xE = virt 0xE) and auto-switches to mode 2 on press. On PC
 * the pairs never move (mode stuck at 1), so every group-3 axis
 * token — movement AND camera, the engine's own X360 defaults —
 * evaluates to 0 through 0x6EAFF0 -> 0x733B90. Update reads the live
 * pad from the device array [sys+0x48+idx*4] (sys = the object the
 * pad ctor 0xD1B460 registers into; idx = [paddev+0x48]).
 * This hookless helper: find sys by matching g_pad_obj in candidate
 * arrays, then force [paddev+0x4C]=2 and [paddev+0x48]=our index, and
 * log mode/pairs every watchdog tick so we SEE the query side move. */
static int g_padmode_force = 1;
static int g_force_menu = 1;   /* diag#70: force pad mode 2 even when
                                * the game switches it to 3 (menus) */
static volatile unsigned g_sys_obj, g_sys_idx;
static volatile unsigned g_paddev_obj;
/* M4-camera (§141): zero the pad device's PROCESSED AXIS FLOATS right
 * after Paddev::Update. The probe showed the real axis values live at
 * pd+0x2C/+0x30/+0x34 (NOT the *(pd+0x38) array tried in §136), and
 * they are the only pad state that changes when the physical stick is
 * touched; the camera recenter is tied to them. */
static int g_zero_pad_axes = 1;
static volatile unsigned g_pup_retaddr;

static void zero_pad_axes_tick(void)
{
    unsigned char *pd = (unsigned char *)(uintptr_t)g_paddev_obj;

    if (!g_zero_pad_axes || !pd || IsBadReadPtr(pd, 0x3C))
        return;
    if (IsBadWritePtr((void *)(uintptr_t)(pd + 0x2C), 0x0C))
        return;
    /* +0x38 is the channel-ARRAY POINTER (d93) - never touch it! */
    *(float *)(pd + 0x2C) = 0.0f;
    *(float *)(pd + 0x30) = 0.0f;
    *(float *)(pd + 0x34) = 0.0f;
}

static void __cdecl post_pup_c(void)
{
    zero_pad_axes_tick();
}

/* M4-camera (§140): differential input-state probe. Any PHYSICAL pad
 * event recenters the camera, while our direct virt-slot writes do not
 * - so the engine must record some pad-activity marker in its input
 * state. This dumps the candidate objects on change (rate-limited) so
 * a user session with real presses shows which field flips. */
static int g_dump_input = 0;
static unsigned g_im_vec[24];   /* input manager dwords +0..+0x5C */
static unsigned g_sub_vec[24];  /* manager sub-object dwords */
static unsigned g_pd_vec[24];   /* pad device dwords +0x24..+0x80 */

static void dump_input_state(void)
{
    static unsigned prev[80];
    static int have;
    static DWORD last_ms;
    static long lines;
    unsigned vals[80];
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned im, sub, pd;
    int n = 0, i;
    DWORD now = GetTickCount();

    if (!g_dump_input || lines >= 900)
        return;
    im = *(unsigned *)(exe + (0x014F13E4u - EXE_DEFAULT_BASE));
    sub = (im > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)im, 0x40)) ?
        *(unsigned *)(uintptr_t)(im + 0xC) : 0;
    pd = g_paddev_obj;
    vals[n++] = im;
    vals[n++] = sub;
    vals[n++] = pd;
    vals[n++] = (unsigned)*(unsigned char *)
        (exe + (0x014F18FCu - EXE_DEFAULT_BASE));
    if (im > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)im, 0x60))
        for (i = 0; i < 24; i++)
            vals[n++] = *(unsigned *)(uintptr_t)(im + i * 4);
    else
        for (i = 0; i < 24; i++)
            vals[n++] = 0;
    if (sub > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)sub, 0x60))
        for (i = 0; i < 24; i++)
            vals[n++] = *(unsigned *)(uintptr_t)(sub + i * 4);
    else
        for (i = 0; i < 24; i++)
            vals[n++] = 0;
    if (pd > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)pd, 0x84))
        for (i = 0; i < 24; i++)
            vals[n++] = *(unsigned *)(uintptr_t)(pd + 0x24 + i * 4);
    else
        for (i = 0; i < 24; i++)
            vals[n++] = 0;
    if (have && !memcmp(prev, vals, n * 4))
        return;
    if (have && now - last_ms < 400)
        goto keep;
    last_ms = now;
    {
        char buf[640];
        int bl = 0;

        for (i = 0; i < n && bl < (int)sizeof(buf) - 24; i++)
            if (!have || prev[i] != vals[i])
                bl += snprintf(buf + bl, sizeof(buf) - bl, " %d:%08X",
                               i, vals[i]);
        if (bl)
            log_line("dumi: t=%lu %s", (unsigned long)now, buf);
        lines++;
    }
keep:
    memcpy(prev, vals, n * 4);
    have = 1;
}

/* M4-camera (§144): the recenter-on-pad-event hunt. The DI8 device
 * update (0xD1B5A0) writes NO globals, so the camera cannot learn
 * about pad events directly - it must be a stored input-scheme flag,
 * an action, or the look-delta accumulator the player controller
 * feeds into the input manager every frame. This probe samples all
 * three candidates once per ~250 ms and logs a line only when a value
 * changed, so ONE short session with the real pad ("pitch up, press
 * A, press B, press RT") names the culprit.
 *   0x14F13D8+0x14  control scheme (the object the 0x71E620 event-0x41
 *                   handler switches; 0x71E490 applies it)
 *   0x14F6055       "pad scheme active" - read by the player controller
 *                   (0x9F1F12) and the character turn code (0x9469FD)
 *   0x14CF748       camera pitch target (written when the camera resets)
 *   0x14CF744       camera pitch companion
 *   0x14F1428+0xF4  look delta the controller injects into the input
 *                   manager as mouse movement every frame (0x9F2792)
 *   listener+8..+D  the four movement direction bools + 2 aux flags
 * Log field indices: 0 magic 1 0x14F6055 2 0x14CF744 3 0x14CF748
 * 4 look-dx 5 look-dy 6 dirs 7 listener 8 mode 9 listener+4
 * 10 listener+0x10 11 scheme 12..18 paddev(+0x28..+0x34,+0x44,+0x48,
 * +0x4C) 19 input manager. */
static int g_cam_probe = 0;
static int g_scheme_off = 0;     /* candidate fix: keep 0x14F6055=0 */
static volatile long g_scheme_writes;
static volatile long g_cam_probe_now;   /* dbgcmd "camstate": one line now */

static void cam_probe_tick(void)
{
    static unsigned prev[24];
    static int have;
    static DWORD last_ms;
    static long lines;
    unsigned vals[24];
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned im, sub2, inner, mode, lst, lc;
    int n = 0, i, force;
    DWORD now = GetTickCount();

    force = (g_cam_probe_now != 0);
    if (!g_cam_probe && !force)
        return;
    if (lines >= 2000 || !exe)
        return;
    if (!force && have && now - last_ms < 16)
        return;
    last_ms = now;
    im = *(unsigned *)(exe + (0x014F13E4u - EXE_DEFAULT_BASE));
    sub2 = *(unsigned *)(exe + (0x014F1704u - EXE_DEFAULT_BASE));
    inner = (sub2 > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)sub2, 0x20)) ?
        *(unsigned *)(uintptr_t)(sub2 + 0x14) : 0;
    mode = (inner > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)inner, 0x40)) ?
        *(unsigned *)(uintptr_t)(inner + 4) : 0;
    lst = (mode == 3 && inner > 0x10000 &&
           !IsBadReadPtr((void *)(uintptr_t)(inner + 0x14), 4)) ?
        *(unsigned *)(uintptr_t)(inner + 0x14) : 0;
    lc = *(unsigned *)(exe + (0x014F1428u - EXE_DEFAULT_BASE));
    vals[n++] = 0xCA5E0000u;
    vals[n++] = *(unsigned *)(exe + (0x014F6055u - EXE_DEFAULT_BASE));
    vals[n++] = *(unsigned *)(exe + (0x014CF744u - EXE_DEFAULT_BASE));
    vals[n++] = *(unsigned *)(exe + (0x014CF748u - EXE_DEFAULT_BASE));
    vals[n++] = (lc > 0x10000 &&
                 !IsBadReadPtr((void *)(uintptr_t)(lc + 0xF4), 8)) ?
        *(unsigned *)(uintptr_t)(lc + 0xF4) : 0;
    vals[n++] = (lc > 0x10000 &&
                 !IsBadReadPtr((void *)(uintptr_t)(lc + 0xF4), 8)) ?
        *(unsigned *)(uintptr_t)(lc + 0xF8) : 0;
    vals[n++] = (lst > 0x10000 &&
                 !IsBadReadPtr((void *)(uintptr_t)(lst + 8), 6)) ?
        (unsigned)*(unsigned char *)(uintptr_t)(lst + 8) |
        ((unsigned)*(unsigned char *)(uintptr_t)(lst + 9) << 8) |
        ((unsigned)*(unsigned char *)(uintptr_t)(lst + 0xA) << 16) |
        ((unsigned)*(unsigned char *)(uintptr_t)(lst + 0xB) << 24) : 0;
    vals[n++] = lst;
    vals[n++] = mode;
    /* listener+4 = the "camera align active" handle the per-frame
     * update resolves before calling 0x9F23A0; +0x10 = the toggle the
     * 0x3B payload flips */
    vals[n++] = (lst > 0x10000 &&
                 !IsBadReadPtr((void *)(uintptr_t)(lst + 4), 8)) ?
        *(unsigned *)(uintptr_t)(lst + 4) : 0;
    vals[n++] = (lst > 0x10000 &&
                 !IsBadReadPtr((void *)(uintptr_t)(lst + 0x10), 1)) ?
        (unsigned)*(unsigned char *)(uintptr_t)(lst + 0x10) : 0;
    /* the scheme object the event-0x41 handler toggles */
    {
        unsigned so = *(unsigned *)(exe + (0x014F13D8u - EXE_DEFAULT_BASE));

        vals[n++] = (so > 0x10000 &&
                     !IsBadReadPtr((void *)(uintptr_t)so, 0x20)) ?
            *(unsigned *)(uintptr_t)(so + 0x14) : 0;
    }
    /* pad device axes + button masks, so a button press is visible */
    if (g_paddev_obj && !IsBadReadPtr((void *)(uintptr_t)g_paddev_obj, 0x50)) {
        unsigned char *pd = (unsigned char *)(uintptr_t)g_paddev_obj;

        for (i = 0; i < 4; i++)
            vals[n++] = *(unsigned *)(pd + 0x28 + i * 4);
        vals[n++] = *(unsigned *)(pd + 0x44);
        vals[n++] = *(unsigned *)(pd + 0x48);
        vals[n++] = *(unsigned *)(pd + 0x4C);
    } else {
        for (i = 0; i < 7; i++)
            vals[n++] = 0;
    }
    vals[n++] = im;
    if (!force && have && !memcmp(prev, vals, n * 4))
        return;
    {
        char buf[520];
        int bl = 0;

        for (i = 0; i < n && bl < (int)sizeof(buf) - 24; i++)
            if (force || !have || prev[i] != vals[i])
                bl += snprintf(buf + bl, sizeof(buf) - bl, " %d:%08X",
                               i, vals[i]);
        if (bl) {
            log_line("camp%s: t=%lu %s", force ? "*" : "",
                     (unsigned long)now, buf);
            lines++;
        }
    }
    memcpy(prev, vals, n * 4);
    have = 1;
    g_cam_probe_now = 0;
}

/* candidate fix (§144): the engine's control-scheme flag. Diag#12
 * proved forcing the OTHER flag (0x14F18FC) broke the load, so this
 * one is written every tick from ours - it is a pure "readers see
 * keyboard/mouse scheme" override, the scheme objects themselves are
 * untouched, so the engine stays consistent. */
static void pad_scheme_tick(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);

    if (!g_scheme_off || !exe)
        return;
    if (*(unsigned char *)(exe + (0x014F6055u - EXE_DEFAULT_BASE)) != 0) {
        *(unsigned char *)(exe + (0x014F6055u - EXE_DEFAULT_BASE)) = 0;
        g_scheme_writes++;
    }
}

/* ---- M4-camera fix candidate A (§144): CamHold ----
 * The mode-3 listener's per-frame update (0x9F25F0) calls the camera
 * ALIGN function 0x9F23A0 (single verified call site 0x9F27D1) which
 * writes the hero's camera-orbit params (yaw offset 0, pitch/distance
 * consts) - "camera back behind the hero". A real pad event makes the
 * engine run that align once, which the player sees as "every pad
 * press recenters the camera". We skip the call while no movement is
 * active and a real DI8 pad event arrived in the last 200 ms.
 *
 * Implementation note (crash #1): a C `thiscall` replacement called
 * from the engine crashed on its first pad event (AV addr 0x202,
 * ecx=0x0A) even though the call landed on our function, so the call
 * is redirected to a FLAG-CHECKING ASM STUB instead: the stub only
 * reads one dword and either returns to the engine or jumps to the
 * original 0x9F23A0. It touches no registers except EFLAGS, so the
 * engine's calling convention cannot be disturbed. */
#define g_cam_hold g_cam_hold_ext
static int g_cam_diag = 0;
static int g_no_threads = 0;  /* §147 bisect: 1 = hooks only, no background threads */
static int g_lean_diag = 0;  /* §147 bisect: 1 = install the diag watches */   /* §145 bisect: 1 = run the camera-thread diag ticks */
static volatile long g_camhold_skips, g_camhold_runs;
#define CAM_ALIGN_VA 0x009F23A0u
#define CAM_ALIGN_CALL_VA 0x009F27D1u

/* camera thread: maintain the stub flag (pad event <200 ms and the
 * player is not moving). evh_c raises it right in the input update,
 * so the very frame of the press already sees it. */
static void camhold_tick(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned sub2, inner, mode, lst;
    int dirs = 0, moving;
    DWORD dt;

    if (g_cam_hold == 2) {   /* §148 diagnostic: always skip the align */
        g_camhold_skip_flag = 1;
        if (!g_camhold_skip_flag)
            ;
        return;
    }
    if (!g_cam_hold || !exe) {
        if (g_camhold_skip_flag)
            g_camhold_skip_flag = 0;
        return;
    }
    {
        static long seen_calls;
        long calls = g_evh_calls;

        if (calls != seen_calls) {
            seen_calls = calls;
            g_last_pad_evt_ms = GetTickCount();
        }
    }
    dt = GetTickCount() - g_last_pad_evt_ms;
    sub2 = *(unsigned *)(exe + (0x014F1704u - EXE_DEFAULT_BASE));
    inner = (sub2 > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)sub2, 0x20)) ?
        *(unsigned *)(uintptr_t)(sub2 + 0x14) : 0;
    mode = (inner > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)inner, 0x40)) ?
        *(unsigned *)(uintptr_t)(inner + 4) : 0;
    lst = (mode == 3 && inner > 0x10000 &&
           !IsBadReadPtr((void *)(uintptr_t)(inner + 0x14), 4)) ?
        *(unsigned *)(uintptr_t)(inner + 0x14) : 0;
    if (lst > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)(lst + 8), 4))
        dirs = *(unsigned char *)(uintptr_t)(lst + 8) |
               *(unsigned char *)(uintptr_t)(lst + 9) |
               *(unsigned char *)(uintptr_t)(lst + 0xA) |
               *(unsigned char *)(uintptr_t)(lst + 0xB);
    moving = dirs || g_stk_lx > g_stick_dead || g_stk_lx < -g_stick_dead ||
             g_stk_ly > g_stick_dead || g_stk_ly < -g_stick_dead;
    /* §148: the reset is a SCHEME-CHANGE action, not the per-frame camera
     * follow, so it is skipped for ANY pad event inside the window -
     * including while running (the user asked exactly for that). The
     * `moving` flag is kept for the log only. */
    if (g_last_pad_evt_ms && dt < 200) {
        if (!g_camhold_skip_flag) {
            g_camhold_skip_flag = 1;
            if (++g_camhold_skips <= 60 || (g_camhold_skips % 100) == 0)
                log_line("camhold: skip window on (dirs=%d dt=%lu n=%ld)",
                         dirs, (unsigned long)dt, g_camhold_skips);
        }
    } else if (g_camhold_skip_flag) {
        g_camhold_skip_flag = 0;
    }
}

/* ---- §148 diagnostic: who RESETS the camera offsets ----
 * hero+0x18C = yaw offset, hero+0x190 = pitch offset (setters
 * 0x90FA20/0x90FA30, resetters 0x90FA40/0x90FA50). The align call we
 * skip at 0x9F27D1 is NOT the recenter (proved by camhold=2: always
 * skip, the camera still snapped). Watch the two reset functions and
 * log the caller when the reset happens right after a pad event. */
static volatile long g_camreset_hits[4];
static volatile long g_camreset_logs;
extern "C" void __cdecl camreset_c(unsigned id, unsigned ret)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    DWORD dt = GetTickCount() - g_last_pad_evt_ms;

    if (id < 4)
        InterlockedIncrement(&g_camreset_hits[id]);
    if (g_last_pad_evt_ms && dt < 400 && g_camreset_logs < 80) {
        g_camreset_logs++;
        log_line("camreset: id=%u ret=%08X exeVA=%08X dt=%lu",
                 id, ret, ret - (unsigned)(uintptr_t)exe + 0x400000u,
                 (unsigned long)dt);
    }
}

static int install_camreset_watch(unsigned va, unsigned len, unsigned id)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (va - EXE_DEFAULT_BASE);
    unsigned char orig[16];
    unsigned char *stub, *c;
    DWORD old;
    long rel;

    if (len < 5 || len > 16 || IsBadReadPtr(target, len))
        return 0;
    memcpy(orig, target, len);
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                    /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x24; /* eax=[esp+24] ret */
    *c++ = 0x50;                                 /* push ret */
    *c++ = 0x68; *(unsigned *)c = id; c += 4;     /* push id */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&camreset_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;       /* add esp,8 */
    *c++ = 0x61; *c++ = 0x9D;                    /* popad; popfd */
    memcpy(c, orig, len); c += len;              /* replay */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + len) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    if (len > 5)
        memset(target + 5, 0x90, len - 5);
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    log_line("camreset watch %u: va=%08X len=%u stub=%p", id, va, len,
             (void *)stub);
    return 1;
}

static int install_camhold_hook(void)
{
    /* §148: the real recenter. The pad event switches the input scheme;
     * the MoveCollector's scheme handler 0xA8B6D0 then resets the hero's
     * camera offsets: 0x946220(hero) at 0xA8B6EF (yaw) and 0x946230(hero)
     * at 0xA8B6F6 (pitch). Both call sites are patched to a flag-checking
     * stub: while g_camhold_skip_flag is set the call is skipped (the
     * camera stays where the player put it), otherwise it runs. */
    static const struct { unsigned va; unsigned target; } sites[2] = {
        { 0x00A8B6EFu, 0x00946220u },   /* camera yaw offset = 0 */
        { 0x00A8B6F6u, 0x00946230u }    /* camera pitch offset = 0 */
    };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    int i, ok = 0;

    for (i = 0; i < 2; i++) {
        unsigned char *site = exe + (sites[i].va - EXE_DEFAULT_BASE);
        unsigned char *orig = exe + (sites[i].target - EXE_DEFAULT_BASE);
        unsigned char *stub;
        long rel, rel2;
        DWORD old, rel_old;

        if (site[0] != 0xE8 || site[-2] != 0x8B || site[-1] != 0xCE) {
            log_line("camhold: site %08X mismatch %02X %02X %02X",
                     sites[i].va, site[-2], site[-1], site[0]);
            continue;
        }
        rel_old = *(long *)(site + 1);
        if ((unsigned)(site + 5 + rel_old) != (unsigned)(uintptr_t)orig) {
            log_line("camhold: site %08X target -> %08X (want %08X)",
                     sites[i].va, (unsigned)(uintptr_t)(site + 5 + rel_old),
                     (unsigned)(uintptr_t)orig);
            continue;
        }
        stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT |
                                             MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
        if (!stub)
            continue;
        /* cmp dword [flag],0 ; jne +5 ; jmp orig ; ret */
        stub[0] = 0x83;
        stub[1] = 0x3D;
        *(unsigned *)(stub + 2) = (unsigned)(uintptr_t)&g_camhold_skip_flag;
        stub[6] = 0x00;
        stub[7] = 0x75;
        stub[8] = 0x05;
        stub[9] = 0xE9;
        rel2 = (long)((uintptr_t)orig - (uintptr_t)(stub + 14));
        *(long *)(stub + 10) = rel2;
        stub[14] = 0xC3;
        FlushInstructionCache(GetCurrentProcess(), stub, 16);
        rel = (long)((uintptr_t)stub - (uintptr_t)(site + 5));
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
            log_line("camhold: VirtualProtect failed %lu", GetLastError());
            continue;
        }
        site[0] = 0xE8;
        site[1] = (unsigned char)(rel & 0xFF);
        site[2] = (unsigned char)((rel >> 8) & 0xFF);
        site[3] = (unsigned char)((rel >> 16) & 0xFF);
        site[4] = (unsigned char)((rel >> 24) & 0xFF);
        VirtualProtect(site, 5, old, &rel_old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        log_line("camhold: site %08X -> stub %p -> orig %p", sites[i].va,
                 (void *)stub, (void *)orig);
        ok++;
    }
    return ok;
}

static volatile long g_inputtick_calls;
static unsigned g_padchain_logged;

static void paddev_diag(unsigned char *exe)
{
    unsigned im, sub, pad, cands[8], nc = 0, i, k;

    if (!g_pad_obj)
        return;
    im = *(unsigned *)(exe + (INPUTMGR_VA - EXE_DEFAULT_BASE));
    if (!im || IsBadReadPtr((void *)im, 0x14))
        return;
    sub = *(unsigned *)(im + 0xC);
    if (!sub || IsBadReadPtr((void *)sub, 8))
        return;
    pad = *(unsigned *)(sub + 4);
    if (!pad || IsBadReadPtr((void *)pad, 0x54))
        return;
    g_paddev_obj = pad;
    if (!g_sys_obj) {
        cands[nc++] = im;
        cands[nc++] = *(unsigned *)(im + 0);
        cands[nc++] = *(unsigned *)(im + 4);
        cands[nc++] = *(unsigned *)(im + 8);
        cands[nc++] = sub;
        cands[nc++] = *(unsigned *)(sub + 0);
        cands[nc++] = *(unsigned *)(sub + 8);
        cands[nc++] = *(unsigned *)(sub + 0xC);
        for (i = 0; i < nc && !g_sys_obj; i++) {
            unsigned c = cands[i];

            if (c <= 0x10000 || IsBadReadPtr((void *)c, 0x68))
                continue;
            for (k = 0; k < 8; k++)
                if (*(unsigned *)(c + 0x48 + k * 4) == g_pad_obj) {
                    g_sys_obj = c;
                    g_sys_idx = k;
                    break;
                }
        }
        if (g_sys_obj)
            log_line("padchain: sys=%08X (pad idx %u, pad+0x20=%u) "
                     "paddev=%08X mode=%u dev=%u lock=%u im10=%08X",
                     g_sys_obj, g_sys_idx,
                     *(unsigned *)((unsigned char *)(uintptr_t)g_pad_obj
                                   + 0x20),
                     pad, *(unsigned char *)(pad + 0x4C),
                     *(unsigned *)(pad + 0x48),
                     *(unsigned char *)(pad + 0x50),
                     *(unsigned *)(im + 0x10));
    }
    if (g_padmode_force && g_sys_obj) {
        unsigned mode = *(unsigned char *)(pad + 0x4C);
        unsigned dev = *(unsigned *)(pad + 0x48);

        /* mode 3 = the engine's own "no pad" state (menus on PC).
         * The OLD code refused to fight it ("the hero would walk in
         * the menus"), but GetAxisValue is dead on PC now, so that
         * side effect is gone - while the pad sits in mode 3 the
         * menus see NOTHING (ispb always false, Start cannot close
         * the menu, the D-pad does not navigate). diag#70: force
         * mode 2 everywhere (ForceMenu) and see whether the menus
         * wake up. */
        if ((mode != 2 || dev != g_sys_idx) &&
            (mode != 3 || g_force_menu)) {
            *(unsigned char *)(pad + 0x4C) = 2;
            *(unsigned *)(pad + 0x48) = g_sys_idx;
            log_line("paddev: FORCED pad mode %u->2 (was dev=%u)",
                     mode, dev);
        }
    }
    {
        /* pairs: f0=+0x28 f1=+0x2C f2=+0x30 f3=+0x34 f4=+0x38 f5=+0x3C */
        float pr[6];
        int changed;
        static unsigned last_mode = 0xFFFFFFFFu;

        memcpy(pr, (void *)(pad + 0x28), sizeof(pr));
        changed = pr[0] != 0.0f || pr[1] != 0.0f || pr[2] != 0.0f ||
                  pr[3] != 0.0f;
        if (last_mode != *(unsigned char *)(pad + 0x4C)) {
            last_mode = *(unsigned char *)(pad + 0x4C);
            changed = 1;   /* log mode changes even without movement */
        }
        if (changed || !g_padchain_logged) {
            log_line("paddev: mode=%u dev=%u pairs LX=%.1f LY=%.1f RX=%.1f"
                     " RY=%.1f T0=%.1f T1=%.1f tick=%ld",
                     *(unsigned char *)(pad + 0x4C),
                     *(unsigned *)(pad + 0x48),
                     pr[0], pr[1], pr[2], pr[3], pr[4], pr[5],
                     g_inputtick_calls);
            g_padchain_logged = 1;
        }
    }
}

/* observer on the per-frame input tick 0x7355F0 (the function that
 * calls Paddev::Update every frame; gates on [container+0x10]).
 * Head: 56 8B F1 83 3E 00 74 5C (push esi; mov esi,ecx; cmp [esi],0;
 * je). Counting calls proves the whole query chain is pumped.
 * diag#22: the stub ALSO ticks the pad movement/camera collectors
 * ourselves - diag#21 proved the engine NEVER calls them on PC (the
 * camera one 0xA8B780 is fully orphaned in the PC build, the movement
 * one 0xA8B620 sits on never-invoked vtable slots). Both are plain
 * thiscalls that only read [this+4] (avatar handle) and push axis
 * values into the avatar's movement controller (0xA8BA00 path), so
 * we call them with the collector object found at runtime. */
#define TICK_VA   0x007355F0u
#define TICK_HEAD 8u
#define MOVC_VA   0x00A8B620u
#define CAMC_VA   0x00A8B780u
extern "C" void __cdecl post_tick_c(void);
static int g_run_collectors = 0;
static int g_run_camc = 0;
/* diag#35 native analog sticks: tick the singleton input-bundle's
 * own MoveCollector (the X360 per-frame consumer of gav acts 0-3)
 * after every engine input tick. StickVirt gates the old digital
 * virt-digitization (stick_to_virt) so it cannot confound tests. */
static int g_native_stick = 0;
static int g_native_gate = 0;
/* diag#46: drive the engine's own tickInput (0x6A7DB0) per frame
 * from the pup hook - wakes the original X360 wiring without
 * touching any platform flags */
static int g_tick_drive = 0;
static volatile unsigned g_bundle_col;
static unsigned g_bundle_handle;
static volatile long g_nstick_ticks;
static volatile unsigned g_mov_collector;
static unsigned g_collector_handle;
static unsigned g_collector_scans;
static volatile unsigned g_tick_retaddr;

/* the entity handle table the collectors resolve through (0x818330):
 * [tbl+0x30] = ptr array, [tbl+0x3C] = count, handle dword = index */
static int handle_resolves(unsigned h, unsigned char *exe)
{
    unsigned tbl = *(unsigned *)(exe + (0x014F1758u - EXE_DEFAULT_BASE));
    unsigned arr, cnt;

    if (!tbl || tbl < 0x10000 || IsBadReadPtr((void *)tbl, 0x40))
        return 0;
    cnt = *(unsigned *)(tbl + 0x3C);
    arr = *(unsigned *)(tbl + 0x30);
    if (cnt == 0 || cnt > 0x100000 || arr < 0x10000)
        return 0;
    if (h >= cnt)
        return 0;
    if (IsBadReadPtr((void *)(uintptr_t)(arr + h * 4), 4))
        return 0;
    return *(unsigned *)(uintptr_t)(arr + h * 4) != 0;
}

static void find_collectors(unsigned char *exe)
{
    static unsigned char buf[0x100000];
    unsigned mov_vt = (unsigned)(uintptr_t)exe +
                      (0x0133B87Cu - EXE_DEFAULT_BASE);
    unsigned ctor_vt = (unsigned)(uintptr_t)exe +
                      (0x0134A7A0u - EXE_DEFAULT_BASE);
    SYSTEM_INFO si;
    unsigned char *p, *end;
    MEMORY_BASIC_INFORMATION mbi;

    if (g_mov_collector || g_collector_scans >= 60)
        return;
    g_collector_scans++;
    GetSystemInfo(&si);
    p = (unsigned char *)si.lpMinimumApplicationAddress;
    end = (unsigned char *)si.lpMaximumApplicationAddress;
    while (p < end) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            mbi.RegionSize >= 0x1000 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                            PAGE_WRITECOPY)) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            unsigned char *r = (unsigned char *)mbi.BaseAddress;
            SIZE_T left = mbi.RegionSize;

            while (left) {
                SIZE_T want = left < sizeof(buf) ? left : sizeof(buf);
                SIZE_T got = 0;
                SIZE_T o;

                /* ReadProcessMemory never faults: pages can decommit
                 * under us while the game thread churns the heap
                 * (diag#22 crash: raw reads raced a free) */
                if (!ReadProcessMemory(GetCurrentProcess(), r, buf,
                                       want, &got) || got < 8)
                    break;
                for (o = 0; o + 0x10 <= got; o += 4) {
                    unsigned v = *(unsigned *)(buf + o);
                    unsigned h;

                    if (v != mov_vt && v != ctor_vt)
                        continue;
                    h = *(unsigned *)(buf + o + 4);
                    /* ctor zeroes +8/+9/+0xC; a stack slot holding
                     * the vtable value (false positive) won't match */
                    if (*(unsigned *)(buf + o + 0xC) != 0)
                        continue;
                    if (!handle_resolves(h, exe))
                        continue;
                    g_mov_collector = (unsigned)(uintptr_t)(r + o);
                    g_collector_handle = h;
                    log_line("collector: obj=%08X vt=%08X handle=%08X "
                             "(scan %u)", g_mov_collector, v, h,
                             g_collector_scans);
                    return;
                }
                r += got;
                left -= got;
            }
        }
        if (mbi.RegionSize == 0)
            break;
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    if (g_collector_scans == 60)
        log_line("collector: NOT FOUND after %u scans", g_collector_scans);
}

/* The singleton input wrapper [0x14F1704] (created at boot, ctor
 * 0x834070; heap-scan collectors may belong to random entities -
 * diag#25c crash) holds the input-component bundle at +0x14. The
 * bundle's +0x10 slot is the game's own MoveCollector (vtable
 * 0x133B87C, ctor 0x9F1680 inside bundle ctor 0x9F1A20). Ticking it
 * (0xA8B620) restores the X360 chain: Tick -> gav(acts 0..3) ->
 * slot g3 tokens -> GetAxisValue(pad, idx) -> processed floats
 * pad+0x28(LX)/+0x2C(LY), fed by CalibFix. */
static void find_bundle_collector(unsigned char *exe)
{
    unsigned wrap, bundle, col, vt, h;

    if (g_bundle_col)
        return;
    wrap = *(unsigned *)(exe + (0x014F1704u - EXE_DEFAULT_BASE));
    if (wrap < 0x10000 || IsBadReadPtr((void *)(uintptr_t)wrap, 0x18))
        return;
    bundle = *(unsigned *)(uintptr_t)(wrap + 0x14);
    if (bundle < 0x10000 || IsBadReadPtr((void *)(uintptr_t)bundle, 0x14))
        return;
    col = *(unsigned *)(uintptr_t)(bundle + 0x10);
    if (col < 0x10000 || IsBadReadPtr((void *)(uintptr_t)col, 0x14))
        return;
    vt = *(unsigned *)(uintptr_t)col;
    if (vt != (unsigned)(uintptr_t)exe +
                (0x0133B87Cu - EXE_DEFAULT_BASE)) {
        static long logged;

        if (!logged) {
            logged = 1;
            log_line("bundle col: slot+0x10 vt=%08X != move vt - skip",
                     vt);
        }
        return;
    }
    h = *(unsigned *)(uintptr_t)(col + 4);
    if (!handle_resolves(h, exe)) {
        static long logged;

        if (!logged) {
            logged = 1;
            log_line("bundle col: obj=%08X handle=%08X NOT resolving",
                     col, h);
        }
        return;
    }
    g_bundle_col = col;
    g_bundle_handle = h;
    log_line("bundle col: FOUND obj=%08X handle=%08X (heap-scan was "
             "%08X/%08X)", col, h, g_mov_collector, g_collector_handle);
}

/* diag#37: the "input tick" 0x7355F0 our post-trampoline hung on is
 * NEVER called on PC (wd: t=0 all session) - the fourth dead input
 * function. The per-frame path that IS alive is Paddev::Update
 * 0x733CC0 (the pairs refresh every frame). Plain head hook: our C
 * runs BEFORE the body (one frame of stick latency - irrelevant),
 * prologue `sub esp,0x2C8` is 6 relref-free bytes. GetAxisValue's
 * own [paddev+0x4C]==3 gate zeroes the axes in menus, so the tick
 * is self-gating. */
static void native_stick_tick(unsigned char *exe);
/* diag#49: the native movement tick - call the engine's own MoveCollector
 * tick (MOVC 0xA8B620) once per frame from the pup hook (game thread).
 * MOVC(this, float, float) - the callee pops 8 bytes (ret $8). */
/* ---- diag#48: entity-resolver instrument (read-only) ---- */
#define RESOLVE_VA1  0x00818330u
#define RESOLVE_VA2  0x008182B0u
#define RESOLVE_HEAD 6u
#define RES_MAXH     2048u
#define RES_MAXMGR   8u
#define RES_MAXSITE  256u
#define RES_SNAPF    8u

struct res_mgr_s {
    unsigned mgr;
    long calls;
    unsigned arr;
    unsigned cnt;
    unsigned *hcnt;        /* per-handle resolve counts */
    unsigned *hent;        /* per-handle entity pointer */
    float *hsnap;          /* per-handle float snapshot */
    unsigned char *hseen;
};

struct res_site_s {
    unsigned site;         /* caller return address (exe VA) */
    long calls;
    unsigned mgr;
    unsigned handle;
    unsigned ent;
};

static struct res_mgr_s g_res_mgrs[RES_MAXMGR];
static struct res_site_s g_res_sites[RES_MAXSITE];
static unsigned g_exe_base;

/* ---- diag#56: the move controller's direction writer ----
 * 0x8FD700(this=move controller, arg=ptr to 3 floats) copies the
 * direction into ctrl+0x38..+0x40. BOTH the keyboard path and the
 * native X360 path go through it - so hooking it tells us which
 * controller each path drives. If the keyboard writes to controller A
 * and our stick writes to controller B, we are driving the wrong
 * entity and must find the right handle. */
static volatile long g_vc_calls;
static volatile unsigned g_vc_ctrl2;
static volatile float g_vc_vec[3];

/* diag#78: continuous rate-capped log of the direction writer with
 * the caller's return address and the controller's stored vector
 * (ctrl+0x38..0x40, copied unconditionally by 0x8FD700). During a
 * sticky episode: nonzero pushes name the latched state's owner
 * (ret); zero pushes that fail to stop the hero point at the
 * stored vector as the latch (the writer stopped being called or
 * something re-pushes the old vector). */
extern "C" void __cdecl vc_c(unsigned ctrl, unsigned vecp, unsigned ret)
{
    static DWORD last_ms;
    static long skipped;
    DWORD now = GetTickCount();
    float v0 = 0.0f, v1 = 0.0f, v2 = 0.0f;
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;

    InterlockedIncrement(&g_vc_calls);
    if (!ctrl || ctrl < 0x10000)
        return;
    if (vecp < 0x10000 || IsBadReadPtr((void *)(uintptr_t)vecp, 12))
        return;
    v0 = *(float *)(uintptr_t)(vecp + 0);
    v1 = *(float *)(uintptr_t)(vecp + 4);
    v2 = *(float *)(uintptr_t)(vecp + 8);
    if (ctrl != g_vc_ctrl) {
        g_vc_ctrl2 = g_vc_ctrl;
        g_vc_ctrl = ctrl;
        log_line("vc: controller=%08X (was %08X)", ctrl, g_vc_ctrl2);
    }
    g_vc_vec[0] = v0;
    g_vc_vec[1] = v1;
    g_vc_vec[2] = v2;
    if (!IsBadReadPtr((void *)(uintptr_t)ctrl, 0x44)) {
        s0 = *(float *)(uintptr_t)(ctrl + 0x38);
        s1 = *(float *)(uintptr_t)(ctrl + 0x3C);
        s2 = *(float *)(uintptr_t)(ctrl + 0x40);
    }
    if (now - last_ms < 100) {
        skipped++;
        return;
    }
    last_ms = now;
    log_line("vc2: ctrl=%08X push=%.4f,%.4f,%.4f stored=%.4f,%.4f,%.4f "
             "ret=%08X skip=%ld", ctrl, v0, v1, v2, s0, s1, s2, ret,
             skipped);
    skipped = 0;
}

#define VC_VA   0x008FD700u
/* The 5-byte jmp at the entry overwrites the first 5 bytes, so the
 * replay must cover whole instructions: "8b 44 24 04" (4) + "8b 10"
 * (2) = 6 bytes, and the stub jumps to target+6.
 * NB: HEAD must be > 5 - memset(target+5, 0x90, HEAD-5) underflows
 * to SIZE_MAX for HEAD==4 (that killed the game at startup). */
#define VC_HEAD 6u
static int install_vc_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (VC_VA - EXE_DEFAULT_BASE);
    unsigned char expect[VC_HEAD] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x10 };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, VC_HEAD) ||
        memcmp(target, expect, VC_HEAD) != 0) {
        log_line("vc hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x7C; *c++ = 0x24; *c++ = 0x24; /* mov edi,[esp+24] ret */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* mov eax,[esp+28] vecp */
    *c++ = 0x57;                                  /* push ret */
    *c++ = 0x51;                                  /* push ecx (ctrl) */
    *c++ = 0x50;                                  /* push eax (vecp) */
    *c++ = 0xE8;                                  /* call vc_c */
    rel = (long)((uintptr_t)&vc_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;         /* add esp,12 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, VC_HEAD); c += VC_HEAD;     /* replay head */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + VC_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, VC_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    if (VC_HEAD > 5)
        memset(target + 5, 0x90, VC_HEAD - 5);
    VirtualProtect(target, VC_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, VC_HEAD);
    log_line("vc hook installed (0x8FD700): target=%p stub=%p",
             target, stub);
    return 1;
}

/* diag#79: the SECOND direction writer 0x8FD720 — same 6-byte head,
 * writes vecp[0..2] into ctrl+0x44..0x4C. The first writer (0x8FD700,
 * +0x38..+0x40) turned out to be called only from one site with zero
 * vectors, so the real movement push must come through here (or not
 * at all — that is the answer too). */
static volatile long g_vc3_calls;
static volatile unsigned g_vc3_ctrl;

extern "C" void __cdecl vc3_c(unsigned ctrl, unsigned vecp, unsigned ret)
{
    static DWORD last_ms;
    static long skipped;
    DWORD now = GetTickCount();
    float v0 = 0.0f, v1 = 0.0f, v2 = 0.0f;
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;

    InterlockedIncrement(&g_vc3_calls);
    if (!ctrl || ctrl < 0x10000)
        return;
    if (vecp < 0x10000 || IsBadReadPtr((void *)(uintptr_t)vecp, 12))
        return;
    v0 = *(float *)(uintptr_t)(vecp + 0);
    v1 = *(float *)(uintptr_t)(vecp + 4);
    v2 = *(float *)(uintptr_t)(vecp + 8);
    if (ctrl != g_vc3_ctrl) {
        g_vc3_ctrl = ctrl;
        log_line("vc3: controller=%08X", ctrl);
    }
    if (!IsBadReadPtr((void *)(uintptr_t)ctrl, 0x50)) {
        s0 = *(float *)(uintptr_t)(ctrl + 0x44);
        s1 = *(float *)(uintptr_t)(ctrl + 0x48);
        s2 = *(float *)(uintptr_t)(ctrl + 0x4C);
    }
    if (now - last_ms < 100) {
        skipped++;
        return;
    }
    last_ms = now;
    log_line("vc3: ctrl=%08X push=%.4f,%.4f,%.4f stored=%.4f,%.4f,%.4f "
             "ret=%08X skip=%ld", ctrl, v0, v1, v2, s0, s1, s2, ret,
             skipped);
    skipped = 0;
}

static int install_vc3_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x008FD720u - EXE_DEFAULT_BASE);
    unsigned char expect[VC_HEAD] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x10 };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, VC_HEAD) ||
        memcmp(target, expect, VC_HEAD) != 0) {
        log_line("vc3 hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x7C; *c++ = 0x24; *c++ = 0x24; /* mov edi,[esp+24] ret */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* mov eax,[esp+28] vecp */
    *c++ = 0x57;                                  /* push ret */
    *c++ = 0x51;                                  /* push ecx (ctrl) */
    *c++ = 0x50;                                  /* push eax (vecp) */
    *c++ = 0xE8;                                  /* call vc3_c */
    rel = (long)((uintptr_t)&vc3_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;         /* add esp,12 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, VC_HEAD); c += VC_HEAD;     /* replay head */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + VC_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, VC_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, VC_HEAD - 5);
    VirtualProtect(target, VC_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, VC_HEAD);
    log_line("vc3 hook installed (0x8FD720): target=%p stub=%p",
             target, stub);
    return 1;
}

/* ---- diag#87: motor probes (§103 movement pipeline) ----
 * Static map: MOVC::Tick -> apply 0xA8BA00 pushes cmd 6 into the
 * ctrl command queue (0x8FE890); the dispatcher 0x8FEED0 (ctrl
 * vtbl[0] @0x132ED20) drains it every frame; case 6 calls the
 * motor 0x8FEA00(dt) which consumes the current vector via
 * 0x8FDF30 (-> action->vtbl+0x64) and the target via
 * action->vtbl+0x60(&ctrl+0x44, decay).  Three probes:
 *   m1 = motor entry 0x8FEA00 (thiscall, dt on stack): periodic
 *        dump of the whole ctrl state incl. both command queues;
 *        flags STICKY when the direction writer has been pushing
 *        zeros for >600 ms while the motor keeps running.
 *   m2 = call site of vtbl+0x64 inside 0x8FDF30 (patch 0x8FDF6D,
 *        replay `mov edx,[edi+0x64]; mov ecx,eax`, jump back onto
 *        `call edx`): logs the concrete callee VA (the direction
 *        consumer implementor) and the pushed direction.
 *   m3 = call site of vtbl+0x60 inside 0x8FEA00 (patch 0x8FEAB3,
 *        replay `mov edx,[eax]; mov edx,[edx+0x60]`, jump back
 *        onto `push ecx`): logs the callee VA, the target vector
 *        (ctrl+0x44..0x4C) and the decay flag. */
static volatile long g_m1_calls, g_m2_calls, g_m3_calls;
static volatile unsigned g_impl64, g_impl60, g_m1_ctrl;

extern "C" void __cdecl m1_c(unsigned ctrl, unsigned dt, unsigned ret)
{
    static DWORD last_ms, zero_ms;
    static long skipped;
    DWORD now = GetTickCount();
    float cp[3], cur[3], tgt[3], t30, t54, t58;
    unsigned qac, qbc, qa0 = 0, qa1 = 0, qa2 = 0, qa3 = 0, qa4 = 0,
             qa5 = 0, qa6 = 0, qa7 = 0, qabuf = 0;
    const char *sticky = "";
    float fdt = *(float *)&dt;

    InterlockedIncrement(&g_m1_calls);
    if (ctrl < 0x10000 || IsBadReadPtr((void *)(uintptr_t)ctrl, 0xA8))
        return;
    g_m1_ctrl = ctrl;
    if (g_vc_vec[0] == 0.0f && g_vc_vec[1] == 0.0f) {
        if (!zero_ms)
            zero_ms = now;
        else if (now - zero_ms > 600)
            sticky = " STICKY?";
    } else {
        zero_ms = 0;
    }
    if (now - last_ms < 100 && !sticky[0]) {
        skipped++;
        return;
    }
    if (sticky[0] && now - last_ms < 400) {
        skipped++;
        return;
    }
    last_ms = now;
    memcpy(cp, (void *)(uintptr_t)(ctrl + 0x10), 12);
    memcpy(cur, (void *)(uintptr_t)(ctrl + 0x38), 12);
    memcpy(tgt, (void *)(uintptr_t)(ctrl + 0x44), 12);
    t30 = *(float *)(uintptr_t)(ctrl + 0x30);
    t54 = *(float *)(uintptr_t)(ctrl + 0x54);
    t58 = *(float *)(uintptr_t)(ctrl + 0x58);
    qabuf = *(unsigned *)(uintptr_t)(ctrl + 0x60);
    qac = *(unsigned *)(uintptr_t)(ctrl + 0x68);
    qbc = *(unsigned *)(uintptr_t)(ctrl + 0x78);
    if (qac > 100)
        qac = 100;
    if (qabuf > 0x10000 && qac && !IsBadReadPtr((void *)(uintptr_t)qabuf,
                                                qac * 4)) {
        unsigned n = qac < 8 ? qac : 8;
        unsigned v[8];

        memcpy(v, (void *)(uintptr_t)qabuf, n * 4);
        qa0 = v[0]; qa1 = v[1]; qa2 = v[2]; qa3 = v[3];
        qa4 = v[4]; qa5 = v[5]; qa6 = v[6]; qa7 = v[7];
    }
    log_line("m1:%s ctrl=%08X dt=%.4f ret=%08X cp=%.3f,%.3f,%.3f "
             "f1C=%X b2D=%X t30=%.3f/%X cur=%.3f,%.3f,%.3f "
             "tgt=%.3f,%.3f,%.3f t54=%.3f t58=%.3f qa=%u[%X,%X,%X,%X,"
             "%X,%X,%X,%X] qb=%u skip=%ld",
             sticky, ctrl, fdt, ret, cp[0], cp[1], cp[2],
             *(unsigned *)(uintptr_t)(ctrl + 0x1C),
             *(unsigned char *)(uintptr_t)(ctrl + 0x2D),
             t30, *(unsigned *)(uintptr_t)(ctrl + 0x34),
             cur[0], cur[1], cur[2], tgt[0], tgt[1], tgt[2], t54, t58,
             qac, qa0, qa1, qa2, qa3, qa4, qa5, qa6, qa7, qbc, skipped);
    skipped = 0;
}

extern "C" void __cdecl m2_c(unsigned action, unsigned x, unsigned y,
                             unsigned z, unsigned ret)
{
    static DWORD last_ms;
    static long skipped;
    unsigned vtbl, callee = 0;
    float fx = *(float *)&x, fy = *(float *)&y, fz = *(float *)&z;
    DWORD now = GetTickCount();

    InterlockedIncrement(&g_m2_calls);
    if (action < 0x10000 || IsBadReadPtr((void *)(uintptr_t)action, 4))
        return;
    vtbl = *(unsigned *)(uintptr_t)action;
    if (IsBadReadPtr((void *)(uintptr_t)vtbl, 0x68))
        return;
    callee = *(unsigned *)(uintptr_t)(vtbl + 0x64);
    if (callee != g_impl64) {
        g_impl64 = callee;
        log_line("m2: impl64=%08X action=%08X vtbl=%08X",
                 callee, action, vtbl);
    }
    if (now - last_ms < 100) {
        skipped++;
        return;
    }
    last_ms = now;
    log_line("m2: dir=%.4f,%.4f,%.4f ret=%08X skip=%ld",
             fx, fy, fz, ret, skipped);
    skipped = 0;
}

extern "C" void __cdecl m3_c(unsigned action, unsigned ctrl,
                             unsigned decay)
{
    static DWORD last_ms;
    static long skipped;
    unsigned vtbl, callee = 0;
    float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f;
    DWORD now = GetTickCount();

    InterlockedIncrement(&g_m3_calls);
    if (action < 0x10000 || IsBadReadPtr((void *)(uintptr_t)action, 4))
        return;
    vtbl = *(unsigned *)(uintptr_t)action;
    if (IsBadReadPtr((void *)(uintptr_t)vtbl, 0x64))
        return;
    callee = *(unsigned *)(uintptr_t)(vtbl + 0x60);
    if (callee != g_impl60) {
        g_impl60 = callee;
        log_line("m3: impl60=%08X action=%08X vtbl=%08X",
                 callee, action, vtbl);
    }
    if (ctrl > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)(ctrl + 0x44),
                                        12)) {
        t0 = *(float *)(uintptr_t)(ctrl + 0x44);
        t1 = *(float *)(uintptr_t)(ctrl + 0x48);
        t2 = *(float *)(uintptr_t)(ctrl + 0x4C);
    }
    if (now - last_ms < 100) {
        skipped++;
        return;
    }
    last_ms = now;
    log_line("m3: tgt=%.4f,%.4f,%.4f decay=%u skip=%ld",
             t0, t1, t2, decay & 0xFF, skipped);
    skipped = 0;
}

#define M2_VA   0x008FDF6Du   /* mov edx,[edi+0x64]; mov ecx,eax; call edx */
#define M2_HEAD 5u
#define M3_VA   0x008FEAB3u   /* mov edx,[eax]; mov edx,[edx+0x60]; push.. */
#define M3_HEAD 5u
#define M1_VA   0x008FEA00u   /* motor entry */
#define M1_HEAD 10u
static int install_motor_hooks(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char expect2[M2_HEAD] = { 0x8B, 0x57, 0x64, 0x8B, 0xC8 };
    unsigned char expect3[M3_HEAD] = { 0x8B, 0x10, 0x8B, 0x52, 0x60 };
    unsigned char expect1[M1_HEAD] = { 0x51, 0x56, 0x8B, 0xF1, 0x8B,
                                       0x0D, 0xC4, 0x13, 0x4F, 0x01 };
    int ok = 0;
    /* NB: bytes 6..9 of the motor head are an absolute address
     * (MOV ECX,[0x14F13C4]) that the loader RELOCATES - only the
     * 6-byte prefix is stable. The stub replays the LIVE bytes. */

    /* m2: callee + direction into action->vtbl+0x64 */
    {
        unsigned char *target = exe + (M2_VA - EXE_DEFAULT_BASE);
        unsigned char *stub, *c;
        long rel;
        DWORD old;

        if (IsBadReadPtr(target, M2_HEAD) ||
            memcmp(target, expect2, M2_HEAD) != 0) {
            log_line("m2 hook SKIP: head mismatch at %p", target);
        } else if ((stub = (unsigned char *)VirtualAlloc(
                       NULL, 128, MEM_COMMIT | MEM_RESERVE,
                       PAGE_EXECUTE_READWRITE)) != NULL) {
            c = stub;
            *c++ = 0x9C; *c++ = 0x60;               /* pushfd; pushad */
            *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x24; /* eax=x */
            *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x28; /* ecx=y */
            *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x2C; /* edx=z */
            *c++ = 0x8B; *c++ = 0x74; *c++ = 0x24; *c++ = 0x3C; /* esi=ret */
            *c++ = 0x8B; *c++ = 0x7C; *c++ = 0x24; *c++ = 0x1C; /* edi=act */
            *c++ = 0x56;                              /* push ret */
            *c++ = 0x52;                              /* push z */
            *c++ = 0x51;                              /* push y */
            *c++ = 0x50;                              /* push x */
            *c++ = 0x57;                              /* push action */
            *c++ = 0xE8;                              /* call m2_c */
            rel = (long)((uintptr_t)&m2_c - (uintptr_t)(c + 4));
            memcpy(c, &rel, 4); c += 4;
            *c++ = 0x83; *c++ = 0xC4; *c++ = 0x14;     /* add esp,0x14 */
            *c++ = 0x61; *c++ = 0x9D;                  /* popad; popfd */
            memcpy(c, expect2, M2_HEAD); c += M2_HEAD;
            *c++ = 0xE9;
            rel = (long)((uintptr_t)(target + M2_HEAD) -
                         (uintptr_t)(c + 4));
            memcpy(c, &rel, 4); c += 4;
            if (VirtualProtect(target, M2_HEAD, PAGE_EXECUTE_READWRITE,
                               &old)) {
                target[0] = 0xE9;
                rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
                memcpy(target + 1, &rel, 4);
                VirtualProtect(target, M2_HEAD, old, &old);
                FlushInstructionCache(GetCurrentProcess(), target,
                                      M2_HEAD);
                log_line("m2 hook installed (0x8FDF6D): target=%p "
                         "stub=%p", target, stub);
                ok++;
            }
        }
    }
    /* m3: target vector into action->vtbl+0x60 */
    {
        unsigned char *target = exe + (M3_VA - EXE_DEFAULT_BASE);
        unsigned char *stub, *c;
        long rel;
        DWORD old;

        if (IsBadReadPtr(target, M3_HEAD) ||
            memcmp(target, expect3, M3_HEAD) != 0) {
            log_line("m3 hook SKIP: head mismatch at %p", target);
        } else if ((stub = (unsigned char *)VirtualAlloc(
                       NULL, 128, MEM_COMMIT | MEM_RESERVE,
                       PAGE_EXECUTE_READWRITE)) != NULL) {
            c = stub;
            *c++ = 0x9C; *c++ = 0x60;               /* pushfd; pushad */
            *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x1C; /* eax=act */
            *c++ = 0x8B; *c++ = 0x74; *c++ = 0x24; *c++ = 0x04; /* esi=ctrl */
            *c++ = 0x0F; *c++ = 0xB6; *c++ = 0x54; *c++ = 0x24;
            *c++ = 0x30;                               /* movzx edx,decay */
            *c++ = 0x52;                              /* push decay */
            *c++ = 0x56;                              /* push ctrl */
            *c++ = 0x50;                              /* push action */
            *c++ = 0xE8;                              /* call m3_c */
            rel = (long)((uintptr_t)&m3_c - (uintptr_t)(c + 4));
            memcpy(c, &rel, 4); c += 4;
            *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;     /* add esp,12 */
            *c++ = 0x61; *c++ = 0x9D;                  /* popad; popfd */
            memcpy(c, expect3, M3_HEAD); c += M3_HEAD;
            *c++ = 0xE9;
            rel = (long)((uintptr_t)(target + M3_HEAD) -
                         (uintptr_t)(c + 4));
            memcpy(c, &rel, 4); c += 4;
            if (VirtualProtect(target, M3_HEAD, PAGE_EXECUTE_READWRITE,
                               &old)) {
                target[0] = 0xE9;
                rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
                memcpy(target + 1, &rel, 4);
                VirtualProtect(target, M3_HEAD, old, &old);
                FlushInstructionCache(GetCurrentProcess(), target,
                                      M3_HEAD);
                log_line("m3 hook installed (0x8FEAB3): target=%p "
                         "stub=%p", target, stub);
                ok++;
            }
        }
    }
    /* m1: motor entry - full ctrl state dump */
    {
        unsigned char *target = exe + (M1_VA - EXE_DEFAULT_BASE);
        unsigned char *stub, *c;
        long rel;
        DWORD old;

        if (IsBadReadPtr(target, M1_HEAD) ||
            memcmp(target, expect1, 6) != 0) {
            log_line("m1 hook SKIP: head mismatch at %p", target);
        } else if ((stub = (unsigned char *)VirtualAlloc(
                       NULL, 128, MEM_COMMIT | MEM_RESERVE,
                       PAGE_EXECUTE_READWRITE)) != NULL) {
            c = stub;
            *c++ = 0x9C; *c++ = 0x60;               /* pushfd; pushad */
            *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x24; /* eax=ret */
            *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x28; /* ecx=dt */
            *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x18; /* edx=ctrl */
            *c++ = 0x50;                              /* push ret */
            *c++ = 0x51;                              /* push dt */
            *c++ = 0x52;                              /* push ctrl */
            *c++ = 0xE8;                              /* call m1_c */
            rel = (long)((uintptr_t)&m1_c - (uintptr_t)(c + 4));
            memcpy(c, &rel, 4); c += 4;
            *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;     /* add esp,12 */
            *c++ = 0x61; *c++ = 0x9D;                  /* popad; popfd */
            memcpy(c, target, M1_HEAD); c += M1_HEAD; /* live bytes */
            *c++ = 0xE9;
            rel = (long)((uintptr_t)(target + M1_HEAD) -
                         (uintptr_t)(c + 4));
            memcpy(c, &rel, 4); c += 4;
            if (VirtualProtect(target, M1_HEAD, PAGE_EXECUTE_READWRITE,
                               &old)) {
                target[0] = 0xE9;
                rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
                memcpy(target + 1, &rel, 4);
                memset(target + 5, 0x90, M1_HEAD - 5);
                VirtualProtect(target, M1_HEAD, old, &old);
                FlushInstructionCache(GetCurrentProcess(), target,
                                      M1_HEAD);
                log_line("m1 hook installed (0x8FEA00): target=%p "
                         "stub=%p", target, stub);
                ok++;
            }
        }
    }
    return ok;
}

/* ---- diag#89: the move-to-target executor 0x6EFD90 (§110) ----
 * In gamepad mode the per-frame movement driver 0x75AA40 steers the
 * hero toward a stored world-space target; 0x6EFD90 is the executor
 * (stores the target at this+0x284, re-paths when farther than 15
 * units). The sticky hero = a stale far target nobody refreshes when
 * the stick goes quiet (§109/§110). m6 logs the two position trios it
 * is handed plus the DAT_014EF900 global - a stuck session will show
 * the frozen target directly. */
static volatile long g_m6_calls;

extern "C" void __cdecl m6_c(unsigned ctrl, unsigned p1, unsigned p2,
                             unsigned p3, unsigned p4, unsigned ret)
{
    static DWORD last_ms;
    static long skipped;
    static unsigned prev_h;
    unsigned exe = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    unsigned glob = exe + (0x014EF900u - EXE_DEFAULT_BASE);
    float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, u0 = 0.0f, u1 = 0.0f,
          u2 = 0.0f, g0 = 0.0f, g1 = 0.0f, g2 = 0.0f;
    unsigned h;
    DWORD now = GetTickCount();

    InterlockedIncrement(&g_m6_calls);
    if (p3 > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)p3, 12)) {
        t0 = *(float *)(uintptr_t)p3;
        t1 = *(float *)(uintptr_t)(p3 + 4);
        t2 = *(float *)(uintptr_t)(p3 + 8);
    }
    if (p4 > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)p4, 12)) {
        u0 = *(float *)(uintptr_t)p4;
        u1 = *(float *)(uintptr_t)(p4 + 4);
        u2 = *(float *)(uintptr_t)(p4 + 8);
    }
    if (!IsBadReadPtr((void *)(uintptr_t)glob, 12)) {
        g0 = *(float *)(uintptr_t)glob;
        g1 = *(float *)(uintptr_t)(glob + 4);
        g2 = *(float *)(uintptr_t)(glob + 8);
    }
    h = (unsigned)(t0 * 4.0f) ^ ((unsigned)(t1 * 4.0f) << 8) ^
        (unsigned)(u0 * 4.0f) ^ ((unsigned)(u1 * 4.0f) << 8);
    if (h == prev_h && now - last_ms < 2000) {
        skipped++;
        return;
    }
    prev_h = h;
    last_ms = now;
    log_line("m6: this=%08X p12=%08X,%08X tgt=(%.1f,%.1f,%.1f) "
             "p4=(%.1f,%.1f,%.1f) glob14EF900=(%.1f,%.1f,%.1f) "
             "ret=%08X skip=%ld", ctrl, p1, p2, t0, t1, t2, u0, u1,
             u2, g0, g1, g2, ret, skipped);
    skipped = 0;
}

#define M6_VA   0x006EFD90u
#define M6_HEAD 13u
static int install_m6_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (M6_VA - EXE_DEFAULT_BASE);
    unsigned char expect[9] = { 0x83, 0xEC, 0x24, 0x53, 0x57, 0x8B,
                                0xF9, 0x8B, 0x0D };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, M6_HEAD) ||
        memcmp(target, expect, sizeof(expect)) != 0) {
        log_line("m6 hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                          /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x74; *c++ = 0x24; *c++ = 0x18; /* esi=[esp+18] this */
    *c++ = 0x8B; *c++ = 0x7C; *c++ = 0x24; *c++ = 0x24; /* edi=[esp+24] ret */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* eax=[esp+28] p1 */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x2C; /* ecx=[esp+2C] p2 */
    *c++ = 0x8B; *c++ = 0x5C; *c++ = 0x24; *c++ = 0x30; /* ebx=[esp+30] p3 */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x34; /* edx=[esp+34] p4 */
    *c++ = 0x57; *c++ = 0x52; *c++ = 0x53; *c++ = 0x51;
    *c++ = 0x50; *c++ = 0x56;                  /* push ret,p4,p3,p2,p1,this */
    *c++ = 0xE8;                               /* call m6_c */
    rel = (long)((uintptr_t)&m6_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x18;     /* add esp,24 */
    *c++ = 0x61; *c++ = 0x9D;                  /* popad; popfd */
    /* NB: head bytes 9..12 are a loader-relocated absolute address
     * (MOV ECX,[0x14F13C4]) - replay the LIVE bytes, compare the
     * 9-byte stable prefix only. */
    memcpy(c, target, M6_HEAD); c += M6_HEAD;
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + M6_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, M6_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, M6_HEAD - 5);
    VirtualProtect(target, M6_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, M6_HEAD);
    log_line("m6 hook installed (0x6EFD90): target=%p stub=%p", target,
             stub);
    return 1;
}

/* ---- diag#49: native movement tick (MOVC) ----
 * MOVC 0xA8B620 is the X360 MoveCollector's per-frame tick:
 *   void __thiscall MOVC(MoveCollector *this, float a, float b);
 * the callee pops the two stack floats (`ret $8`) - a call without
 * them eats 8 bytes of the caller's stack (the old diag#22/35 crash).
 * It resolves [this+4] in the LIVENESS space (0x8C92C0 -> 0x8C9330 ->
 * 0x818330; a null entity = early exit), reads the action values
 * gav(0..3) (forward/back/strafe) and calls this->vt+0x28 (apply
 * 0xA8BA00) and this->vt+0x24 (0xA8C1B0, camera).
 */
#define MOVC_TICK_VA      0x00A8B620u
#define APPLY_SPACE_VA    0x014F13C4u
#define LIVENESS_SPACE_VA 0x014F1758u
#define GATE_MGR_VA       0x014F13C0u
#define RESOLVE_VA        0x00818330u

static int g_move_tick;
/* fill the paddev query pairs (LX/LY) from the raw DI8 channels -
 * on PC the engine's processed values for channels 0/1 never leave 0 */
static int g_axis_inject;
static int g_axis_dead = 10;
static int g_axis_flip;   /* bit0 = invert X, bit1 = invert Y */
static int g_mt_probe;    /* 1 = extra engine calls in the tick (diag) */
static volatile long g_mt_calls;
static volatile long g_mt_live;
static volatile long g_mt_skipped;
static int g_mt_applied;
static volatile unsigned g_mt_ent;
static volatile unsigned g_mt_ctrl;
static volatile float g_mt_a[3];
static void move_collector_tick(unsigned char *exe);

typedef unsigned (__thiscall *resolve_fn)(void *, unsigned *);

/* 0x818330: entity* resolve(manager, &handle) - ecx=manager, one
 * stack arg (&handle), callee pops it (ret $4) */
static unsigned mt_resolve(unsigned mgr, unsigned *hptr)
{
    typedef unsigned (__thiscall *res_fn)(void *, unsigned *);
    res_fn f = (res_fn)(void *)(uintptr_t)(g_exe_base + (RESOLVE_VA -
                                                        EXE_DEFAULT_BASE));

    return f((void *)(uintptr_t)mgr, hptr);
}

/* MOVC(this, a, b): ecx=this, two stack floats (b pushed first),
 * callee pops them (ret $8) */
static void mt_call(unsigned col, float a, float b)
{
    typedef void (__thiscall *movc_fn)(void *, float, float);
    movc_fn f = (movc_fn)(void *)(uintptr_t)(g_exe_base + (MOVC_TICK_VA -
                                                          EXE_DEFAULT_BASE));

    f((void *)(uintptr_t)col, a, b);
}

/* ---- e170 (0x756170) counter: the "entity is in a special state"
 * check the apply path uses ---- */
static volatile long g_e170_calls;

extern "C" void __cdecl e170_c(unsigned self)
{
    (void)self;
    InterlockedIncrement(&g_e170_calls);
}

/* ---- diag#46: drive the engine's own input tick (0x6A7DB0) ---- */
static volatile long g_tdrive_calls;

static void native_tick_drive(unsigned char *exe)
{
    typedef void (__cdecl *tick_fn)(float dt1, float dt0);
    static int tlog;

    if (!tlog) {
        tlog = 1;
        log_line("TDIV: NativeTickDrive ON - calling engine tickInput "
                 "0x6A7DB0 (dt 1/60,1/60) every frame");
    }
    ((tick_fn)(void *)(uintptr_t)
        (exe + (0x006A7DB0u - EXE_DEFAULT_BASE)))(1.0f / 60.0f,
                                                  1.0f / 60.0f);
    g_tdrive_calls++;
}

/* ---- the per-frame hook point (Paddev::Update 0x733CC0) ---- */
/* diag#92: the consumer here is a state machine (this+0x4c = 1 armed,
 * 2 active, 3 analog-ignored; this+0x48 = device index). The analog
 * direction comparators (tokens 0xE..0x13, threshold 10.0) only run
 * in state 2 - a state blip while bits are set would freeze them
 * "pressed" forever: the sticky-hero candidate. The stub pushes the
 * saved ECX (this) so we can watch state + the eight cached axis
 * floats (this+0x28..0x44) every frame. */
extern "C" void __cdecl pup_c(unsigned this_)
{
    static unsigned char *exe;
    static long calls;
    static int armed_logged;

    calls++;
    InterlockedIncrement(&g_pup_calls);   /* diag#97: per-poll gate */
    if (!exe)
        exe = (unsigned char *)GetModuleHandleA(NULL);
    /* diag#93: d92 locked onto the FIRST consumer instance, which
     * stayed dormant (state 0) all session - the active gameplay
     * consumer is a different `this`. Track up to four instances'
     * state transitions, and watch the live pad device's eight
     * channels (floats at *(pad+0x38) + i*0x20 + 4, count pad+0x34)
     * against our own XInput percent: the comparator chatter seen in
     * the stuck log (token 0x12 press+release while lx=45 was held)
     * must appear here as a channel dip while our=%d,%d stays hot -
     * or the comparator's value source is not the channel we think. */
    if (this_ > 0x10000 &&
        !IsBadReadPtr((void *)(uintptr_t)this_, 0x50)) {
        static unsigned d93_this[4];
        static unsigned d93_st[4] = {0xFFFFFFFFu, 0xFFFFFFFFu,
                                     0xFFFFFFFFu, 0xFFFFFFFFu};
        static DWORD d93_tr[4], d93_chhot;
        static float d93_prev[8];
        static int d93_seen;
        DWORD now93 = GetTickCount();
        int s = -1, i;

        for (i = 0; i < 4; i++)
            if (d93_this[i] == this_)
                s = i;
        if (s < 0)
            for (i = 0; i < 4; i++)
                if (!d93_this[i]) {
                    d93_this[i] = this_;
                    s = i;
                    log_line("d93: inst%d this=%08X", s, this_);
                    break;
                }
        if (s >= 0) {
            unsigned st = *(unsigned *)(uintptr_t)(this_ + 0x4c);

            if (st != d93_st[s] && now93 - d93_tr[s] >= 60) {
                d93_tr[s] = now93;
                log_line("d93: inst%d state %u->%u", s, d93_st[s], st);
                d93_st[s] = st;
            }
        }
        if (g_paddev_obj &&
            !IsBadReadPtr((void *)(uintptr_t)g_paddev_obj, 0x40)) {
            unsigned carr = *(unsigned *)(uintptr_t)(g_paddev_obj + 0x38);
            unsigned cnt = *(unsigned *)(uintptr_t)(g_paddev_obj + 0x34);
            float f[8];
            int cross = 0, hot = 0;

            if (carr > 0x10000 && cnt > 0 && cnt <= 0x40 &&
                !IsBadReadPtr((void *)(uintptr_t)carr, 8 * 0x20 + 4)) {
                if (cnt > 8)
                    cnt = 8;
                for (i = 0; i < 8; i++) {
                    f[i] = (i < (int)cnt) ?
                        *(float *)(uintptr_t)(carr + i * 0x20 + 4) : 0.0f;
                    if (f[i] > 9.9f || f[i] < -9.9f)
                        hot = 1;
                    if (d93_seen &&
                        (((d93_prev[i] >= 10.0f) != (f[i] >= 10.0f)) ||
                         ((d93_prev[i] <= -10.0f) != (f[i] <= -10.0f))))
                        cross = 1;
                    d93_prev[i] = f[i];
                }
                d93_seen = 1;
                if (cross)
                    log_line("d93ch: ch=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
                             "%.1f,%.1f our=%d,%d",
                             f[0], f[1], f[2], f[3], f[4], f[5], f[6],
                             f[7], g_stk_lx, g_stk_ly);
                else if (hot && now93 - d93_chhot >= 300) {
                    d93_chhot = now93;
                    log_line("d93ch: hot ch=%.1f,%.1f,%.1f,%.1f,%.1f,"
                             "%.1f,%.1f,%.1f our=%d,%d",
                             f[0], f[1], f[2], f[3], f[4], f[5], f[6],
                             f[7], g_stk_lx, g_stk_ly);
                }
            }
        }
    }
    if (g_native_stick == 2) {
        if (!armed_logged) {
            armed_logged = 1;
            log_line("PUP: first Paddev::Update call (tick %ld) - "
                     "NATIVE2 armed here", calls);
        }
        native_stick_tick(exe);
    }
    if (g_tick_drive)
        native_tick_drive(exe);
    if (g_move_tick)
        move_collector_tick(exe);
    stick_to_virt_g();
    sticky_fix_tick();
}

/* ---- vib-on-damage (§201): call-site detour on the hitDamage popup
 * display. Updater FUN_0093ab60 shows each popup entry exactly once:
 * if entry->0x64 == 0 it does
 *   0x93AC73: call 0xA783D0(ecx=movie, entry=esi, float, float,
 *                           entry->0x38 = color dword)
 * then Invoke("setTime") and sets entry->0x64 = 1. One call = one popup.
 * The popup color 0xFF0000 (red) is damage TO the player (Frida capture:
 * hero only walking, enemies hitting, every popup red, ent=293 = hero).
 * We redirect the E8 to a stub that peeks the entry and jumps to the
 * UNTOUCHED 0xA783D0 (the §200 head-hook on that function crashed). */
#define VIBD_SITE 0x0093AC73u
#define VIBD_FUNC 0x00A783D0u
#define VIBD_RED  0x00FF0000u                /* popup color = player hit */
static int g_vib_on_damage = 1;
static int g_vib_damage_str = 0;        /* 0 = VibStrength x slider */
static volatile long g_vibd_last_ms;
static volatile unsigned g_vibd_seen;
static volatile unsigned g_vibd_logged;

static void __cdecl vib_damage_c(unsigned entry)
{
    DWORD now = GetTickCount();
    unsigned color = 0;

    if (entry > 0x10000 && entry < 0x7FFF0000 &&
        !IsBadReadPtr((void *)(uintptr_t)entry, 0x68))
        color = *(unsigned *)(uintptr_t)(entry + 0x38);
    if (g_vibd_logged < 12)                  /* learn the popup palette */
        log_line("vibd: popup #%u color=%06X entry=%08X",
                 g_vibd_logged++, color, entry);
    if (color != VIBD_RED)
        return;                              /* not damage to the player */
    if ((long)(now - g_vibd_last_ms) < 150)
        return;                              /* burst rate-limit */
    g_vibd_last_ms = now;
    g_vibd_seen++;
    vib_pulse(g_vib_damage_str);             /* 0 = VibStrength x slider */
    if (g_vibd_seen <= 8)
        log_line("vibd: hit on player #%u", g_vibd_seen);
}

static int install_vibd_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *site = exe + (VIBD_SITE - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    long rel, expect_rel;
    DWORD old;

    expect_rel = (long)(VIBD_FUNC - (VIBD_SITE + 5));
    if (IsBadReadPtr(site, 5) || site[0] != 0xE8 ||
        *(long *)(site + 1) != expect_rel) {
        log_line("vibd hook SKIP: call-site mismatch at %p", site);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                    /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28;
                                                 /* mov eax,[esp+28h]=entry */
    *c++ = 0x50;                                 /* push eax */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&vib_damage_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;       /* add esp,4 (cdecl) */
    *c++ = 0x61; *c++ = 0x9D;                    /* popad; popfd */
    *c++ = 0xE9;                                 /* jmp untouched original */
    rel = (long)((uintptr_t)(exe + (VIBD_FUNC - EXE_DEFAULT_BASE)) -
                 (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (rel > 0x7FFFFFFFL || rel < -0x7FFFFFFFL) {
        log_line("vibd hook SKIP: stub out of range");
        VirtualFree(stub, 0, MEM_RELEASE);
        return 0;
    }
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    site[0] = 0xE8;
    *(long *)(site + 1) = (long)((uintptr_t)stub - (uintptr_t)(site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log_line("vibd hook installed (call-site 0x93AC73): site=%p stub=%p",
             site, stub);
    return 1;
}

#define PUP_VA   0x00733CC0u
#define PUP_HEAD 6u
static int install_pup_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (PUP_VA - EXE_DEFAULT_BASE);
    unsigned char expect[PUP_HEAD] = { 0x81, 0xEC, 0xC8, 0x02, 0x00,
                                       0x00 };
    unsigned char *stub, *c, *post, *swap_slot;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, PUP_HEAD) ||
        memcmp(target, expect, PUP_HEAD) != 0) {
        log_line("pup hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    post = (unsigned char *)VirtualAlloc(NULL, 32, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!post)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                    /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x24;
    *c++ = 0xA3;
    *(unsigned *)c = (unsigned)(uintptr_t)&g_pup_retaddr; c += 4;
    /* §146: NO return-address swap. The post trampoline (a §141
     * leftover) stored the return address in ONE global and replaced
     * the stack slot on every Paddev::Update entry; it destabilised
     * the engine's DI8 device init (crash 0xD1A87B on the first pad
     * event, EIP=0x202). With the trampoline off the frame stays
     * stock and g_pup_retaddr is still recorded for diagnostics. */
    (void)swap_slot;
    (void)post;
    *c++ = 0xFF; *c++ = 0x74; *c++ = 0x24; *c++ = 0x14;
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&pup_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;
    *c++ = 0x61; *c++ = 0x9D;
    memcpy(c, expect, PUP_HEAD); c += PUP_HEAD;
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + PUP_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    c = post;
    *c++ = 0x9C; *c++ = 0x60;
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&post_pup_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x61; *c++ = 0x9D;
    *c++ = 0xFF; *c++ = 0x25;
    *(unsigned *)c = (unsigned)(uintptr_t)&g_pup_retaddr; c += 4;
    if (!VirtualProtect(target, PUP_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    if (PUP_HEAD > 5)
        memset(target + 5, 0x90, PUP_HEAD - 5);
    VirtualProtect(target, PUP_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, PUP_HEAD);
    log_line("pup hook installed (0x733CC0): target=%p stub=%p post=%p",
             target, stub, post);
    return 1;
}

/* ---- apply 0xA8BA00: the movement write into the avatar's move
 * controller. diag#50: we also capture the three float arguments
 * MOVC computed from the action values ---- */
static volatile long g_apply_calls;
static volatile unsigned g_apply_h;
static volatile unsigned g_apply_ent;
static volatile float g_apply_ax, g_apply_ay;

extern "C" void __cdecl apply_c(unsigned mctrl, float ax, float ay,
                                float az)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned h, ent, mgr, prev_ent;
    resolve_fn rf;

    if (mctrl < 0x10000 || IsBadReadPtr((void *)(uintptr_t)mctrl, 8))
        return;
    h = *(unsigned *)(uintptr_t)(mctrl + 4);
    mgr = *(unsigned *)(exe + (0x014F13C4u - EXE_DEFAULT_BASE));
    ent = 0;
    if (mgr > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)mgr, 0x40)) {
        rf = (resolve_fn)(void *)(uintptr_t)
             (exe + (0x00818330u - EXE_DEFAULT_BASE));
        ent = rf((void *)(uintptr_t)mgr, &h);
    }
    prev_ent = g_apply_ent;
    g_apply_calls++;
    g_apply_h = h;
    g_apply_ent = ent;
    if (g_apply_calls <= 40 || ent != prev_ent)
        log_line("apply: mc=%08X handle=%08X ent=%08X (call %ld)",
                 mctrl, h, ent, g_apply_calls);
    if (g_apply_calls <= 40 || ax != g_apply_ax || ay != g_apply_ay) {
        g_apply_ax = ax;
        g_apply_ay = ay;
        log_line("apply args: X=%.4f Y=%.4f Z=%.4f (call %ld)",
                 ax, ay, az, g_apply_calls);
    }
}

#define APPLY_VA   0x00A8BA00u
#define APPLY_HEAD 6u
static int install_apply_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (APPLY_VA - EXE_DEFAULT_BASE);
    unsigned char expect[APPLY_HEAD] = { 0x83, 0xEC, 0x20, 0x55, 0x8B,
                                         0xE9 };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, APPLY_HEAD) ||
        memcmp(target, expect, APPLY_HEAD) != 0) {
        log_line("apply hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C;                                  /* pushfd */
    *c++ = 0x60;                                  /* pushad */
    /* stack at entry: [esp]=ret, [esp+4]=X, [esp+8]=Y, [esp+12]=Z;
     * after pushfd+pushad esp is 0x24 lower -> +0x28/+0x2C/+0x30 */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x18; /* mov eax,[esp+0x18]=ecx */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x28; /* mov edx,[esp+0x28]=X */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x2C; /* mov ecx,[esp+0x2C]=Y */
    *c++ = 0x8B; *c++ = 0x5C; *c++ = 0x24; *c++ = 0x30; /* mov ebx,[esp+0x30]=Z */
    *c++ = 0x53;                                  /* push ebx (Z) */
    *c++ = 0x51;                                  /* push ecx (Y) */
    *c++ = 0x52;                                  /* push edx (X) */
    *c++ = 0x50;                                  /* push eax (this) */
    *c++ = 0xE8;                                  /* call apply_c */
    rel = (long)((uintptr_t)&apply_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x10;        /* add esp,16 */
    *c++ = 0x61;                                  /* popad */
    *c++ = 0x9D;                                  /* popfd */
    memcpy(c, expect, APPLY_HEAD); c += APPLY_HEAD;   /* replay prologue */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + APPLY_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, APPLY_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    if (APPLY_HEAD > 5)
        memset(target + 5, 0x90, APPLY_HEAD - 5);
    VirtualProtect(target, APPLY_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, APPLY_HEAD);
    log_line("apply hook installed (0xA8BA00): target=%p stub=%p",
             target, stub);
    return 1;
}


/* diag#50: MOVC's second call (this->vt+0x24 = 0xA8C1B0) drives the
 * camera from the X360 camera actions. On PC the camera is mouse
 * driven, so the native camera write fights the mouse and the
 * camera feels sluggish. MoveCam=0 replaces the slot with a no-op
 * (0xA8C1B0 has no other call sites - vtable only). */
static int g_move_cam;

/* __thiscall, NOT __stdcall: the engine passes  in ECX and only
 * the two floats on the stack (0xA8C1B0 ends with `ret $8`). A
 * __stdcall build emits `ret $0xc` and eats 4 extra bytes of the
 * caller's stack EVERY FRAME - that was the diag#50 crash. */
static void __thiscall mt_nocam(void *self, float a, float b)
{
    (void)self;
    (void)a;
    (void)b;
}

static int patch_collector_camera(unsigned char *exe)
{
    unsigned vt = (unsigned)(uintptr_t)(exe + (0x0133B87Cu -
                                                EXE_DEFAULT_BASE));
    unsigned cur;
    DWORD old;

    if (!vt || IsBadReadPtr((void *)(uintptr_t)vt, 0x40))
        return 0;
    cur = *(unsigned *)(uintptr_t)(vt + 0x24);
    if (cur != (unsigned)(uintptr_t)(exe + (0x00A8C1B0u -
                                           EXE_DEFAULT_BASE))) {
        log_line("MT: vtable slot +0x24 = %08X (expected 0xA8C1B0) - "
                 "camera patch skipped", cur);
        return 0;
    }
    if (!VirtualProtect((void *)(uintptr_t)(vt + 0x24), 4,
                        PAGE_EXECUTE_READWRITE, &old))
        return 0;
    *(unsigned *)(uintptr_t)(vt + 0x24) = (unsigned)(uintptr_t)&mt_nocam;
    VirtualProtect((void *)(uintptr_t)(vt + 0x24), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(),
                          (void *)(uintptr_t)(vt + 0x24), 4);
    log_line("MT: collector vtable +0x24 -> noop (native camera off)");
    return 1;
}

/* GetAxisValue 0x733B90(paddev, idx): returns the float the engine
 * would give for that half-axis (idx 0=X+, 1=X-, 2=Y+, 3=Y-) */
static float mt_getaxis(void *paddev, int idx)
{
    typedef float (__thiscall *ax_fn)(void *, int);
    ax_fn f = (ax_fn)(void *)(uintptr_t)(g_exe_base + (0x00733B90u -
                                                      EXE_DEFAULT_BASE));

    if (!paddev || IsBadReadPtr(paddev, 0x60))
        return 0.0f;
    return f(paddev, idx);
}

/* entity->vt[0x58]() -> the move controller object */
static unsigned mt_getctrl(unsigned ent, unsigned vt)
{
    typedef void *(__thiscall *get_fn)(void *);
    void *p;

    if (!vt || IsBadReadPtr((void *)(uintptr_t)vt, 0x60))
        return 0;
    p = ((get_fn)(void *)(uintptr_t)(vt + 0x58))((void *)(uintptr_t)ent);
    return (unsigned)(uintptr_t)p;
}

static void move_collector_tick(unsigned char *exe)
{
    static long nlog;
    static volatile int in_tick;      /* re-entrancy guard */
    unsigned col, h;
    unsigned ent = 0, mgr, ctrl = 0, gate = 0xFF;
    unsigned live_ent = 0, node;
    float b0 = 0.0f, b1 = 0.0f, a0 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float gv[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    int pfx = 0, pfy = 0, c0 = 0, c1 = 0, r0 = 0, r1 = 0;
    int ix = 0, iy = 0, k;
    float ep0 = 0.0f, ep1 = 0.0f, wp0 = 0.0f, wp1 = 0.0f;
    float rawx = 0.0f, rawy = 0.0f;
    unsigned char *pp;
    unsigned chp = 0;

    if (in_tick) {            /* never re-enter: the tick calls engine
                               * code that may call back into us */
        g_mt_skipped++;
        return;
    }
    in_tick = 1;
    if (!g_bundle_col) {
        find_bundle_collector(exe);
        if (!g_bundle_col) {
            in_tick = 0;
            return;
        }
    }
    col = g_bundle_col;
    h = *(unsigned *)(uintptr_t)(col + 4);
    if (h == 0) {          /* not attached yet - do NOT tick handle 0 */
        g_mt_skipped++;
        in_tick = 0;
        return;
    }
    if (g_mt_probe) {
        /* --- input side: DI8 raw channels -> paddev query pairs --- */
        pp = (unsigned char *)(uintptr_t)g_pad_obj;
        if (pp && !IsBadReadPtr(pp, 0x160)) {
            chp = *(unsigned *)(uintptr_t)(pp + 0x38);
            if (chp > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)chp, 0x60)) {
                c0 = (int)(*(float *)(uintptr_t)(chp + 4) * 100.0f);
                c1 = (int)(*(float *)(uintptr_t)(chp + 0x24) * 100.0f);
                r0 = *(int *)(uintptr_t)(chp + 0);
                r1 = *(int *)(uintptr_t)(chp + 0x20);
            }
        }
        pp = (unsigned char *)(uintptr_t)g_paddev_obj;
        if (pp && !IsBadReadPtr(pp, 0x38)) {
            pfx = (int)(*(float *)(pp + 0x28) * 100.0f);
            pfy = (int)(*(float *)(pp + 0x2C) * 100.0f);
            rawx = *(float *)(pp + 0x28);
            rawy = *(float *)(pp + 0x2C);
            /* the old channel-array injection is gone: the
             * channel array is always 0 on PC. The event-sourced
             * injection below is the only one. */
            /* NB: do NOT call GetAxisValue here - our own calls would
             * pollute the axq histogram (which must show ONLY the
             * engine's queries) and we would read the pair floats
             * directly instead. The engine rectifies them into
             * half-axes: idx0/idx2 = positive, idx1/idx3 = negative. */
            gv[0] = *(float *)(pp + 0x28);
            gv[1] = -*(float *)(pp + 0x28);
            gv[2] = *(float *)(pp + 0x2C);
            gv[3] = -*(float *)(pp + 0x2C);
        }
        mgr = *(unsigned *)(exe + (APPLY_SPACE_VA - EXE_DEFAULT_BASE));
        if (mgr > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)mgr, 0x40)) {
            ent = mt_resolve(mgr, &h);
            g_mt_ent = ent;
            if (ent > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)ent, 0x60)) {
                /* NB: entity->vt[0x58] is NOT called any more - its
                 * calling convention is not the same for every entity
                 * class (diag#51 crash). The move controller is read
                 * by the apply hook instead. */
                node = *(unsigned *)(uintptr_t)(ent + 0x100);
                if (node > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)node, 0x100)) {
                    px = *(float *)(uintptr_t)(node + 0xF0);
                    py = *(float *)(uintptr_t)(node + 0xF4);
                    pz = *(float *)(uintptr_t)(node + 0xF8);
                }
            }
        }
        mgr = *(unsigned *)(exe + (LIVENESS_SPACE_VA - EXE_DEFAULT_BASE));
        if (mgr > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)mgr, 0x40)) {
            live_ent = mt_resolve(mgr, &h);
            if (live_ent > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)live_ent, 0x140)) {
                node = *(unsigned *)(uintptr_t)(live_ent + 0x100);
                if (node > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)node, 0x100)) {
                    lx = *(float *)(uintptr_t)(node + 0xF0);
                    ly = *(float *)(uintptr_t)(node + 0xF4);
                    lz = *(float *)(uintptr_t)(node + 0xF8);
                }
            }
        }
        if (ctrl > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)ctrl, 0x50)) {
            b0 = *(float *)(uintptr_t)(ctrl + 0x44);
            b1 = *(float *)(uintptr_t)(ctrl + 0x48);
        }
    }
    /* --- input: event-sourced axes -> paddev query pairs --- */
    if (g_axis_inject) {
        unsigned char *pd = (unsigned char *)(uintptr_t)g_paddev_obj;

        if (pd && !IsBadReadPtr(pd, 0x38)) {
            int a[4];
            int j;

            /* the engine's own value BEFORE our write (it fills the
             * pairs from the device channels - which stay 0 on PC) */
            ep0 = *(float *)(pd + 0x28);
            ep1 = *(float *)(pd + 0x2C);
            for (j = 0; j < 4; j++) {
                int v = g_axis_raw[j];

                if (v > 100)
                    v = 100;
                if (v < -100)
                    v = -100;
                if (v > -g_axis_dead && v < g_axis_dead)
                    v = 0;
                if ((g_axis_flip & 1) && j < 2)
                    v = -v;
                if ((g_axis_flip & 2) && j >= 2)
                    v = -v;
                a[j] = v;
            }
            /* ONLY the left stick: the right stick belongs to the
             * mouse camera, and overwriting it makes the camera
             * stick at one point */
            *(float *)(pd + 0x28) = (float)a[0] / 100.0f;
            *(float *)(pd + 0x2C) = (float)a[1] / 100.0f;
            wp0 = (float)a[0] / 100.0f;
            wp1 = (float)a[1] / 100.0f;
            ix = a[0];
            iy = a[1];
        }
    }
    /* --- one-shot: the collector needs the handle in the APPLY
     * space, not the liveness one. The two managers index the SAME
     * entity objects in a DIFFERENT order (diag#44): the handle the
     * engine gave the collector (0x293) is a liveness index, and
     * apply resolves it in the apply space -> a different entity
     * (no rotation -> the direction vector comes out zero -> the
     * hero never moves). So: take the avatar's entity pointer from
     * the liveness array and find THAT POINTER in the apply array -
     * its index is the handle apply needs.
     * Pure memory reads + one 4-byte write. */
    if (!g_mt_applied) {
        static long last_try;
        unsigned l_mgr = *(unsigned *)(exe + (LIVENESS_SPACE_VA -
                                              EXE_DEFAULT_BASE));
        unsigned a_mgr = *(unsigned *)(exe + (APPLY_SPACE_VA -
                                              EXE_DEFAULT_BASE));
        unsigned l_arr = 0, l_cnt = 0, a_arr = 0, a_cnt = 0;
        unsigned i, liv = 0, ah = 0;

        if (g_mt_calls && (g_mt_calls - last_try) < 60)
            goto skip_lookup;
        last_try = g_mt_calls;
        if (l_mgr > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)l_mgr, 0x40)) {
            l_arr = *(unsigned *)(uintptr_t)(l_mgr + 0x30);
            l_cnt = *(unsigned *)(uintptr_t)(l_mgr + 0x3C);
        }
        if (l_arr > 0x10000 && l_cnt && l_cnt <= 0x100000 && h < l_cnt &&
            !IsBadReadPtr((void *)(uintptr_t)(l_arr), l_cnt * 4))
            liv = *(unsigned *)(uintptr_t)(l_arr + h * 4);
        if (a_mgr > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)a_mgr, 0x40)) {
            a_arr = *(unsigned *)(uintptr_t)(a_mgr + 0x30);
            a_cnt = *(unsigned *)(uintptr_t)(a_mgr + 0x3C);
        }
        /* the apply entity keeps ITS LIVENESS handle at +0x148 (that
         * is exactly what 0x756170 does: add $0x148,%ecx and resolve
         * it in [0x14F1758]). So the avatar's apply entity is the one
         * whose +0x148 == our handle. */
        if (a_arr > 0x10000 && a_cnt && a_cnt <= 0x100000 &&
            !IsBadReadPtr((void *)(uintptr_t)(a_arr), a_cnt * 4)) {
            for (i = 0; i < a_cnt; i++) {
                unsigned e2 = *(unsigned *)(uintptr_t)(a_arr + i * 4);

                if (e2 < 0x10000 ||
                    IsBadReadPtr((void *)(uintptr_t)e2, 0x150))
                    continue;
                if (*(unsigned *)(uintptr_t)(e2 + 0x148) == h) {
                    ah = i;
                    liv = e2;
                    break;
                }
            }
        }
        log_line("MT: spaces: liv_ent=%08X apply_handle=%08X "
                 "(collector had %08X, l_cnt=%u a_cnt=%u)",
                 liv, ah, h, l_cnt, a_cnt);
        if (ah && ah != h) {
            /* MOVC's null check resolves the handle in the LIVENESS
             * space, so the apply index must exist there too */
            if (ah < l_cnt) {
                *(unsigned *)(uintptr_t)(col + 4) = ah;
                g_mt_applied = 1;
                log_line("MT: collector handle -> %08X (apply space) "
                         "- the avatar", ah);
            } else {
                log_line("MT: apply handle %08X >= liveness count %08X "
                         "- unusable for MOVC", ah, l_cnt);
            }
        } else if (!ah) {
            log_line("MT: avatar entity %08X not found in the apply "
                     "array - keeping the old handle", liv);
        }
    }
skip_lookup:
    mt_call(col, 0.0f, 0.0f);
    g_mt_calls++;
    in_tick = 0;
    if (!g_mt_probe) {
        if (((nlog++) % 60) == 0)
            log_line("MT: col=%08X h=%08X calls=%ld",
                     col, h, g_mt_calls);
        return;
    }
    if (ctrl > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)ctrl, 0x50)) {
        a0 = *(float *)(uintptr_t)(ctrl + 0x44);
        a1 = *(float *)(uintptr_t)(ctrl + 0x48);
        a2 = *(float *)(uintptr_t)(ctrl + 0x4C);
    }
    g_mt_a[0] = a0;
    g_mt_a[1] = a1;
    g_mt_a[2] = a2;
    if (a0 != b0 || a1 != b1)
        g_mt_live++;
    {
        unsigned gc = *(unsigned *)(exe + (GATE_MGR_VA - EXE_DEFAULT_BASE));

        if (gc > 0x10000 &&
            !IsBadReadPtr((void *)(uintptr_t)gc, 0x40))
            gate = *(unsigned char *)(uintptr_t)(gc + 0x19);
    }
    if (((nlog++) % 30) == 0 || ((a0 != b0 || a1 != b1) &&
                                (nlog % 5) == 0)) {
        log_line("MT: col=%08X h=%08X ent=%08X live=%08X ctrl=%08X "
                 "gate=%02X ev=%d,%d,%d,%d wp=%.4f,%.4f ep=%.4f,%.4f "
                 "pair=%.4f,%.4f gv=%.4f,%.4f,%.4f,%.4f | (%.2f,%.2f)->"
                 "(%.2f,%.2f,%.2f) pos=%.0f,%.0f,%.0f "
                 "livepos=%.0f,%.0f,%.0f live=%ld calls=%ld",
                 col, h, ent, live_ent, ctrl, gate,
                 g_axis_raw[0], g_axis_raw[1], g_axis_raw[2],
                 g_axis_raw[3], wp0, wp1, ep0, ep1,
                 rawx, rawy, gv[0], gv[1], gv[2], gv[3],
                 b0, b1, a0, a1, a2, px, py, pz, lx, ly, lz,
                 g_mt_live, g_mt_calls);
    }
}

static struct res_mgr_s *res_slot(unsigned mgr)
{
    int i;
    struct res_mgr_s *free_slot = NULL;

    for (i = 0; i < RES_MAXMGR; i++) {
        if (g_res_mgrs[i].mgr == mgr)
            return &g_res_mgrs[i];
        if (!g_res_mgrs[i].mgr && !free_slot)
            free_slot = &g_res_mgrs[i];
    }
    if (!free_slot)
        return NULL;
    free_slot->mgr = mgr;
    free_slot->hcnt = (unsigned *)calloc(RES_MAXH, sizeof(unsigned));
    free_slot->hent = (unsigned *)calloc(RES_MAXH, sizeof(unsigned));
    free_slot->hsnap = (float *)calloc(RES_MAXH * RES_SNAPF, sizeof(float));
    free_slot->hseen = (unsigned char *)calloc(RES_MAXH, 1);
    if (!free_slot->hcnt || !free_slot->hent || !free_slot->hsnap ||
        !free_slot->hseen) {
        free_slot->mgr = 0;
        return NULL;
    }
    return free_slot;
}

static void res_site(unsigned site, unsigned mgr, unsigned h, unsigned ent)
{
    int i;
    struct res_site_s *free_slot = NULL;

    for (i = 0; i < RES_MAXSITE; i++) {
        if (g_res_sites[i].site == site) {
            g_res_sites[i].calls++;
            g_res_sites[i].mgr = mgr;
            g_res_sites[i].handle = h;
            g_res_sites[i].ent = ent;
            return;
        }
        if (!g_res_sites[i].site && !free_slot)
            free_slot = &g_res_sites[i];
    }
    if (!free_slot)
        return;
    free_slot->site = site;
    free_slot->calls = 1;
    free_slot->mgr = mgr;
    free_slot->handle = h;
    free_slot->ent = ent;
}

extern "C" void __cdecl resolve_c(unsigned mgr, unsigned hptr, unsigned who)
{
    struct res_mgr_s *s;
    unsigned h, arr, cnt;

    if (mgr < 0x10000 || hptr < 0x10000)
        return;
    s = res_slot(mgr);
    if (!s)
        return;
    s->calls++;
    h = *(unsigned *)(uintptr_t)hptr;
    if (h >= RES_MAXH)
        return;
    arr = s->arr;
    cnt = s->cnt;
    if (!arr || !cnt) {
        /* first time we see this manager: cache its table */
        if (IsBadReadPtr((void *)(uintptr_t)(mgr + 0x30), 0x10))
            return;
        arr = *(unsigned *)(uintptr_t)(mgr + 0x30);
        cnt = *(unsigned *)(uintptr_t)(mgr + 0x3C);
        if (arr < 0x10000 || cnt == 0 || cnt > 0x100000)
            return;
        s->arr = arr;
        s->cnt = cnt;
    }
    if (IsBadReadPtr((void *)(uintptr_t)(arr + h * 4), 4))
        return;
    s->hcnt[h]++;
    s->hent[h] = *(unsigned *)(uintptr_t)(arr + h * 4);
    res_site(who, mgr, h, s->hent[h]);
}

static int install_resolve_hook_va(unsigned va, const char *tag)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (va - EXE_DEFAULT_BASE);
    unsigned char expect[RESOLVE_HEAD] = { 0x8B, 0x44, 0x24, 0x04, 0x56,
                                            0x57 };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    g_exe_base = (unsigned)(uintptr_t)exe;
    if (IsBadReadPtr((void *)(uintptr_t)target, RESOLVE_HEAD) ||
        memcmp(target, expect, RESOLVE_HEAD) != 0) {
        log_line("resolve hook %s SKIP: head mismatch at %p", tag, target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C;                                  /* pushfd */
    *c++ = 0x60;                                  /* pushad */
    /* after pushfd+pushad: saved ecx at [esp+0x18], the caller's return
     * address at [esp+0x24], the incoming argument (pointer to handle)
     * at [esp+0x28].  All three are read BEFORE any push, so the
     * offsets stay valid. */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* mov eax,[esp+28] */
    *c++ = 0x8B; *c++ = 0x5C; *c++ = 0x24; *c++ = 0x18; /* mov ebx,[esp+18] */
    *c++ = 0x8B; *c++ = 0x6C; *c++ = 0x24; *c++ = 0x24; /* mov ebp,[esp+24] */
    *c++ = 0x55;                                  /* push ebp (caller ret) */
    *c++ = 0x50;                                  /* push eax (hptr) */
    *c++ = 0x53;                                  /* push ebx (mgr) */
    *c++ = 0xE8;                                  /* call resolve_c */
    rel = (long)((uintptr_t)&resolve_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;        /* add esp,12 */
    *c++ = 0x61;                                  /* popad */
    *c++ = 0x9D;                                  /* popfd */
    memcpy(c, expect, RESOLVE_HEAD); c += RESOLVE_HEAD;
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + RESOLVE_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, RESOLVE_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, RESOLVE_HEAD - 5);
    VirtualProtect(target, RESOLVE_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, RESOLVE_HEAD);
    log_line("resolve hook %s installed (%08X): target=%p stub=%p",
             tag, va, target, stub);
    return 1;
}

static int install_resolve_hook(void)
{
    int a = install_resolve_hook_va(RESOLVE_VA1, "r330");
    int b = install_resolve_hook_va(RESOLVE_VA2, "r2b0");

    return a || b;
}

/* diag#48: snapshot the float block that holds an entity's position.
 * The engine's coordinate getter (0x753230) walks ent+0x100 -> node and
 * reads node+0xE8/0xD4 as a vector, so the floats of interest live in
 * the node.  Instead of guessing which of them is X we snapshot the
 * whole block and compare it between passes: ANY change proves the
 * entity moved, and we never have to know the layout. */
static int res_snap(unsigned ent, float *out)
{
    unsigned p;
    int i;

    if (ent < 0x10000 || IsBadReadPtr((void *)(uintptr_t)ent, 0x104))
        return 0;
    p = *(unsigned *)(uintptr_t)(ent + 0x100);
    if (p < 0x10000 || IsBadReadPtr((void *)(uintptr_t)(p + 0xD0), 0x30))
        return 0;
    for (i = 0; i < RES_SNAPF; i++)
        out[i] = *(float *)(uintptr_t)(p + 0xD0 + i * 4);
    return 1;
}

#define E170_VA   0x00756170u
#define E170_HEAD 6u
static int install_e170_hook(void){
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (E170_VA - EXE_DEFAULT_BASE);
    unsigned char expect[E170_HEAD] = { 0x81, 0xC1, 0x48, 0x01, 0x00,
                                        0x00 };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, E170_HEAD) ||
        memcmp(target, expect, E170_HEAD) != 0) {
        log_line("e170 hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C;                                  /* pushfd */
    *c++ = 0x60;                                  /* pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x18; /* mov eax,[esp+0x18]=saved ecx */
    *c++ = 0x50;                                  /* push eax */
    *c++ = 0xE8;                                  /* call e170_c */
    rel = (long)((uintptr_t)&e170_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;        /* add esp,4 */
    *c++ = 0x61;                                  /* popad */
    *c++ = 0x9D;                                  /* popfd */
    memcpy(c, expect, E170_HEAD); c += E170_HEAD; /* replay prologue */
    *c++ = 0xE9;                                  /* jmp target+6 */
    rel = (long)((uintptr_t)(target + E170_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, E170_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    VirtualProtect(target, E170_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, E170_HEAD);
    log_line("e170 hook installed (0x756170): target=%p stub=%p",
             target, stub);
    return 1;
}

static int install_tick_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (TICK_VA - EXE_DEFAULT_BASE);
    unsigned char expect[TICK_HEAD] = { 0x56, 0x8B, 0xF1, 0x83, 0x3E,
                                        0x00, 0x74, 0x5C };
    unsigned char *stub, *c, *post, *swap_slot;
    long rel;
    unsigned char *je_skip;
    DWORD old;

    if (IsBadReadPtr(target, TICK_HEAD) ||
        memcmp(target, expect, TICK_HEAD) != 0) {
        log_line("tick hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 160, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C;                                  /* pushfd */
    *c++ = 0x60;                                  /* pushad */
    *c++ = 0xFF; *c++ = 0x05;                     /* inc dword [abs] */
    *(unsigned *)c = (unsigned)(uintptr_t)&g_inputtick_calls; c += 4;
    /* diag#25: swap the return address so the collectors run AFTER
     * the tick body (diag#23 called them at entry, before devices
     * were polled and Paddev::Update refreshed the pairs - crashed
     * on the first call). 0x7355F0 has a single `pop esi; ret`, so
     * every return lands in the post trampoline. */
    *c++ = 0x80; *c++ = 0x3D;                     /* cmp byte [abs],0 */
    *(unsigned *)c = (unsigned)(uintptr_t)&g_run_collectors; c += 4;
    *c++ = 0x00;
    je_skip = c; c += 2;                          /* je Lnoswap - patch */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x24; /* mov eax,[esp+0x24] */
    *c++ = 0xA3;                                  /* mov [abs],eax */
    *(unsigned *)c = (unsigned)(uintptr_t)&g_tick_retaddr; c += 4;
    *c++ = 0xC7; *c++ = 0x44; *c++ = 0x24; *c++ = 0x24; /* mov [esp+0x24],imm32 */
    swap_slot = c; c += 4;                        /* patched below */
    je_skip[0] = 0x74;
    je_skip[1] = (unsigned char)(c - (je_skip + 2));
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    /* replay the prologue with the internal je RETARGETED: original
     * `74 5C` at target+6 would land at stub+8+0x5C - wrong place.
     * Replay the first 6 bytes, then: je NOT taken ([esi]!=0, the
     * common case) falls into the FIRST jmp = run the body at
     * target+8; je taken skips it to the original je destination
     * target+8+0x5C. (diag#22 first build had these two jmps
     * swapped - the input tick body never ran and ALL input died.) */
    memcpy(c, expect, 6); c += 6;                 /* 56 8B F1 83 3E 00 */
    *c++ = 0x74; *c++ = 0x05;                     /* je +5 -> skip body */
    *c++ = 0xE9;                                  /* run body: target+8 */
    rel = (long)((uintptr_t)(target + TICK_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0xE9;                                  /* [esi]==0: target+0x64 */
    rel = (long)((uintptr_t)(target + TICK_HEAD + 0x5C) -
                 (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    /* post-return trampoline: everything (gating, logging, calls)
     * lives in post_tick_c; the asm just preserves state and jumps
     * back to the saved return address */
    post = (unsigned char *)VirtualAlloc(NULL, 32, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!post)
        return 0;
    *(unsigned *)swap_slot = (unsigned)(uintptr_t)post;
    c = post;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0xE8;                                  /* call post_tick_c */
    rel = (long)((uintptr_t)&post_tick_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    *c++ = 0xFF; *c++ = 0x25;                     /* jmp dword [abs] */
    *(unsigned *)c = (unsigned)(uintptr_t)&g_tick_retaddr; c += 4;

    if (!VirtualProtect(target, TICK_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90; target[6] = 0x90; target[7] = 0x90;
    VirtualProtect(target, TICK_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, TICK_HEAD);
    log_line("tick hook v3 installed (0x7355F0): target=%p stub=%p "
             "post=%p", target, stub, post);
    return 1;
}

/* ---- diag#20: axis-query probe + left-stick calibration fix ----
 * diag#19 result: paddev mode was ALREADY 2 (the mode-2 "gate" was a
 * wrong hypothesis), pair1 (RX/RY) fills live, but pair0 (LX/LY) stays
 * 0.0 - Paddev::Update maps PROCESSED channel values into the pairs,
 * and channels 0/1 never get their out-range calibration (+0x18/+0x1C)
 * on PC (ctor 0xD1B460 writes only in-min/max; 2/3 get theirs from
 * elsewhere). Fix: copy the ch2/3 out-ranges into 0/1 once they exist.
 * Probe: head-hook on GetAxisValue 0x733B90 (thiscall, one stack arg
 * idx, ret 4; the real entry gate is [ecx+0x4C]==3 -> 0.0, not
 * "mode!=2" as noted earlier) counting per-idx queries - shows which
 * half-axes the game actually asks for in-world (movement idx0-3,
 * camera idx4-7). */
static int g_calib_fix = 1;
static int g_calib_logged;
static volatile long g_axq_calls;
static volatile long g_axq_idx[0x10];
static long g_axq_prev;
static long g_axq_previ[0x10];

static void pad_calib_fix(void)
{
    unsigned char *pad = (unsigned char *)(uintptr_t)g_pad_obj;
    unsigned char *c;
    unsigned k;

    if (!g_calib_fix || !g_pad_obj || IsBadReadPtr(pad, 0x140))
        return;
    c = (unsigned char *)(uintptr_t)*(unsigned *)(pad + 0x38);
    if ((uintptr_t)c <= 0x10000 || IsBadReadPtr(c, 6 * 0x20))
        return;
    /* source: ch2 (RX) must already have a non-degenerate out-range */
    if (*(int *)(c + 2 * 0x20 + 0x18) == *(int *)(c + 2 * 0x20 + 0x1C))
        return; /* not set yet - retry on a later watchdog tick */
    for (k = 0; k < 2; k++) {
        unsigned dst = k * 0x20, src = (k + 2) * 0x20;

        if (*(int *)(c + dst + 0x18) != *(int *)(c + dst + 0x1C))
            continue; /* already calibrated (engine set it) - leave alone */
        if (!g_calib_logged)
            log_line("padcal BEFORE: ch%u in=%08X..%08X out=%08X..%08X",
                     k, *(unsigned *)(c + dst + 0x10),
                     *(unsigned *)(c + dst + 0x14),
                     *(unsigned *)(c + dst + 0x18),
                     *(unsigned *)(c + dst + 0x1C));
        *(int *)(c + dst + 0x18) = *(int *)(c + src + 0x18);
        *(int *)(c + dst + 0x1C) = *(int *)(c + src + 0x1C);
    }
    if (!g_calib_logged) {
        g_calib_logged = 1;
        log_line("padcal: ch0/1 out-range <- ch2/3 "
                 "(ch2=%08X..%08X ch3=%08X..%08X; ch4=%08X..%08X "
                 "ch5=%08X..%08X)",
                 *(unsigned *)(c + 2 * 0x20 + 0x18),
                 *(unsigned *)(c + 2 * 0x20 + 0x1C),
                 *(unsigned *)(c + 3 * 0x20 + 0x18),
                 *(unsigned *)(c + 3 * 0x20 + 0x1C),
                 *(unsigned *)(c + 4 * 0x20 + 0x18),
                 *(unsigned *)(c + 4 * 0x20 + 0x1C),
                 *(unsigned *)(c + 5 * 0x20 + 0x18),
                 *(unsigned *)(c + 5 * 0x20 + 0x1C));
    }
}

static volatile unsigned g_axq_this[0x10];

extern "C" void __cdecl axq_c(unsigned idx, unsigned self)
{
    InterlockedIncrement(&g_axq_calls);
    if (idx < 0x10) {
        InterlockedIncrement(&g_axq_idx[idx]);
        if (!g_axq_this[idx])
            g_axq_this[idx] = self;
    }
}

#define AXQ_VA   0x00733B90u
#define AXQ_HEAD 9u
static int install_axq_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (AXQ_VA - EXE_DEFAULT_BASE);
    unsigned char expect[AXQ_HEAD] = { 0x83, 0xEC, 0x10, 0x83, 0x79,
                                       0x4C, 0x03, 0x75, 0x08 };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, AXQ_HEAD) ||
        memcmp(target, expect, AXQ_HEAD) != 0) {
        log_line("axq hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                           /* pushfd; pushad */
    *c++ = 0x51;                                        /* push ecx (pad) */
    *c++ = 0xFF; *c++ = 0x74; *c++ = 0x24; *c++ = 0x2C; /* push [esp+0x2C] */
    *c++ = 0xE8;                                        /* call axq_c */
    rel = (long)((uintptr_t)&axq_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;              /* add esp,8 */
    *c++ = 0x61; *c++ = 0x9D;                           /* popad; popfd */
    *c++ = 0x83; *c++ = 0xEC; *c++ = 0x10;              /* sub esp,0x10 */
    *c++ = 0x83; *c++ = 0x79; *c++ = 0x4C; *c++ = 0x03; /* cmp [ecx+4C],3 */
    *c++ = 0x75; *c++ = 0x05;                           /* jne +5 */
    *c++ = 0xE9;                                        /* jmp target+9 */
    rel = (long)((uintptr_t)(target + 9) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0xE9;                                        /* jmp target+0x11 */
    rel = (long)((uintptr_t)(target + AXQ_HEAD + 8) -
                 (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, AXQ_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    if (AXQ_HEAD > 5)
        memset(target + 5, 0x90, AXQ_HEAD - 5);
    VirtualProtect(target, AXQ_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, AXQ_HEAD);
    log_line("axq hook installed (0x733B90): target=%p stub=%p",
             target, stub);
    return 1;
}

/* ---- diag#21: is the axis query chain even RUNNING? ----
 * diag#20 result: pairs now fill (LX=-100 seen) but axq never fired -
 * GetAxisValue 0x733B90 is never called, so nobody queries the axis
 * tokens. Statics: the consumers are two VIRTUAL collectors - movement
 * 0xA8B620 (vt 0x134A7A0+0x18, queries acts 0,1,3,2) and camera
 * 0xA8B780 (acts 0x2F,0x30,0x32,0x31, deadzone via 0x134A7CC); both
 * guard on entity resolution (0x8C92C0 -> 0x8C9330([this+4]) -> 0).
 * GetActionAxisValue 0x6EAFF0 may also early-exit via the per-action
 * "disabled" dword [elem+0x4C]==1 (0x82B110 check, returns 1.0).
 * Probes: entry counters on both collectors, per-action histogram on
 * 0x6EAFF0, and a change-triggered dump of the manager token slots
 * (mgr=[0x14F16FC], array [mgr+0x74], stride 0x54: slotA +0x24,
 * slotB +0x38, group at +4/idx at +8 inside a slot). */
static volatile long g_movc_calls, g_camc_calls;
static volatile long g_gav_calls;
static volatile long g_gav_act[0x61];
static long g_movc_prev, g_camc_prev, g_gav_prev;
static long g_gav_pact[0x61];

extern "C" void __cdecl gav_c(unsigned act)
{
    InterlockedIncrement(&g_gav_calls);
    if (act < 0x61)
        InterlockedIncrement(&g_gav_act[act]);
}

static int install_inc_hook(const char *name, unsigned va,
                            const unsigned char *expect, unsigned head,
                            volatile long *counter)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (va - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, head) || memcmp(target, expect, head) != 0) {
        log_line("%s hook SKIP: head mismatch at %p", name, target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                  /* pushfd; pushad */
    *c++ = 0xFF; *c++ = 0x05;                  /* inc dword [abs] */
    *(unsigned *)c = (unsigned)(uintptr_t)counter; c += 4;
    *c++ = 0x61; *c++ = 0x9D;                  /* popad; popfd */
    memcpy(c, expect, head); c += head;        /* replay prologue */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + head) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, head, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    if (head > 5)
        memset(target + 5, 0x90, head - 5);
    VirtualProtect(target, head, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, head);
    log_line("%s hook installed (va=%08X): target=%p stub=%p",
             name, va, target, stub);
    return 1;
}

#define GAV_VA   0x006EAFF0u
#define GAV_HEAD 8u
/* diag#33: the definitive action-fire instrument. 0x8E7660 is the
 * common token-group switch every action fire goes through - pad
 * buttons (g1) AND keyboard (g4). Its head is 6 clean linear bytes.
 * token layout here: [0]=group, [4]=idx; second stack arg = state
 * (1=down, 0=up). Logging g1+g4 fires with the stickdir journal
 * shows exactly which token each input produces. */
static volatile long g_fire_logs;

extern "C" void __cdecl firelog_c(unsigned group, unsigned idx, int st)
{
    if ((group == 1 || group == 4 || group == 3) &&
        InterlockedIncrement(&g_fire_logs) <= 300)
        log_line("fire: g=%u i=%X st=%d", group, idx, st);
}

#define FIRE_VA   0x008E7660u
#define FIRE_HEAD 6u
static int install_firelog_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (FIRE_VA - EXE_DEFAULT_BASE);
    unsigned char expect[FIRE_HEAD] = { 0x8B, 0x44, 0x24, 0x04, 0x8B,
                                        0x08 };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, FIRE_HEAD) ||
        memcmp(target, expect, FIRE_HEAD) != 0) {
        log_line("firelog hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                           /* pushfd; pushad */
    /* mov edx,[esp+0x24]=token; group=[edx]; idx=[edx+4];
     * state=[esp+0x28] */
    *c++ = 0x8B; *c++ = 0x54; *c++ = 0x24; *c++ = 0x24; /* mov edx,[esp+0x24] */
    *c++ = 0x8B; *c++ = 0x0A;                           /* mov ecx,[edx] */
    *c++ = 0x8B; *c++ = 0x52; *c++ = 0x04;              /* mov edx,[edx+4] */
    *c++ = 0xFF; *c++ = 0x74; *c++ = 0x24; *c++ = 0x28; /* push [esp+0x28] */
    *c++ = 0x52;                                        /* push edx (idx) */
    *c++ = 0x51;                                        /* push ecx (group) */
    *c++ = 0xE8;                                        /* call firelog_c */
    rel = (long)((uintptr_t)&firelog_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x0C;              /* add esp,12 */
    *c++ = 0x61; *c++ = 0x9D;                           /* popad; popfd */
    /* diag#90: if firelog_c flagged this token, emulate the stdcall
     * epilogue "ret 8" instead of dispatching it */
    *c++ = 0x83; *c++ = 0x3D;                           /* cmp dword.. */
    *(unsigned *)(uintptr_t)c = (unsigned)(uintptr_t)&g_drop_this;
    c += 4;
    *c++ = 0x01;
    *c++ = 0x75; *c++ = 0x03;                           /* jne +3 */
    *c++ = 0xC2; *c++ = 0x08; *c++ = 0x00;              /* ret 8 */
    memcpy(c, expect, FIRE_HEAD); c += FIRE_HEAD;       /* replay */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + FIRE_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, FIRE_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    memset(target + 5, 0x90, FIRE_HEAD - 5);
    VirtualProtect(target, FIRE_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, FIRE_HEAD);
    log_line("firelog hook installed (0x8E7660): target=%p stub=%p",
             target, stub);
    return 1;
}

static int install_gav_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (GAV_VA - EXE_DEFAULT_BASE);
    unsigned char expect[GAV_HEAD] = { 0x56, 0x8B, 0x74, 0x24, 0x08,
                                       0x57, 0x8B, 0xF9 };
    unsigned char *stub, *c;
    long rel;
    DWORD old;

    if (IsBadReadPtr(target, GAV_HEAD) ||
        memcmp(target, expect, GAV_HEAD) != 0) {
        log_line("gav hook SKIP: head mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                           /* pushfd; pushad */
    *c++ = 0xFF; *c++ = 0x74; *c++ = 0x24; *c++ = 0x28; /* push [esp+0x28] */
    *c++ = 0xE8;                                        /* call gav_c */
    rel = (long)((uintptr_t)&gav_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;              /* add esp,4 */
    *c++ = 0x61; *c++ = 0x9D;                           /* popad; popfd */
    memcpy(c, expect, GAV_HEAD); c += GAV_HEAD;         /* replay */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + GAV_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, GAV_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    if (GAV_HEAD > 5)
        memset(target + 5, 0x90, GAV_HEAD - 5);
    VirtualProtect(target, GAV_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, GAV_HEAD);
    log_line("gav hook installed (0x6EAFF0): target=%p stub=%p",
             target, stub);
    return 1;
}

/* change-triggered dump of the engine token slots for ALL actions
 * (0..0x60): diag#25c proved hand-bound pad buttons DO drive the
 * character (user walks on triggers), but the 12-action dump never
 * showed their tokens - so the working binds live on actions/slots
 * we have not looked at. Dump everything that is bound. */
static void tk_dump(unsigned char *exe)
{
    static unsigned prev_hash;
    unsigned mgr, arr, asz, hash = 0, k;
    char buf[4096];
    int len = 0;

    mgr = *(unsigned *)(exe + (0x014F16FCu - EXE_DEFAULT_BASE));
    if (!mgr || IsBadReadPtr((void *)mgr, 0x78))
        return;
    arr = *(unsigned *)(mgr + 0x74);
    asz = *(unsigned *)(mgr + 0x78) - arr;
    if (arr < 0x10000 || asz < 0x61 * 0x54 ||
        IsBadReadPtr((void *)arr, 0x61 * 0x54))
        return;
    for (k = 0; k < 0x61; k++) {
        unsigned char *e = (unsigned char *)arr + k * 0x54;

        hash = hash * 131 + *(unsigned *)(e + 0x28);
        hash = hash * 131 + *(unsigned *)(e + 0x2C);
        hash = hash * 131 + *(unsigned *)(e + 0x3C);
        hash = hash * 131 + *(unsigned *)(e + 0x40);
    }
    if (hash == prev_hash)
        return;
    prev_hash = hash;
    for (k = 0; k < 0x61; k++) {
        unsigned char *e = (unsigned char *)arr + k * 0x54;
        unsigned ag = *(unsigned *)(e + 0x28), ai = *(unsigned *)(e + 0x2C);
        unsigned bg = *(unsigned *)(e + 0x3C), bi = *(unsigned *)(e + 0x40);

        if (!ag && !ai && !bg && !bi)
            continue;
        if (len > (int)sizeof(buf) - 64) {
            buf[len] = 0;
            log_line("tk: %s", buf);
            len = 0;
        }
        len += snprintf(buf + len, sizeof(buf) - len,
                        "%X:A%X/%X B%X/%X; ", k, ag, ai, bg, bi);
    }
    if (len) {
        buf[len] = 0;
        log_line("tk: %s", buf);
    }
}

/* last-gasp crash logger: diag#22/#23 both crashed inside the
 * collector experiment and the log just stopped - the fault address
 * would have named the culprit. log_line opens the file per call, so
 * it is safe from an exception filter. */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);
    unsigned addr = (unsigned)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;

    log_line("CRASH: code=%08X addr=%08X exeVA=%08X ecx=%08X edx=%08X",
             (unsigned)ep->ExceptionRecord->ExceptionCode, addr,
             addr >= base ? addr - base + 0x400000 : 0,
             ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* diag#25b: the SetUnhandledExceptionFilter never fired (the game or
 * its runtime overrides it) - use a vectored handler, which always
 * runs FIRST and cannot be replaced. Skip C++ EH noise, cap output. */
static volatile long g_veh_logs;
static LONG WINAPI veh_handler(EXCEPTION_POINTERS *ep)
{
    unsigned code = (unsigned)ep->ExceptionRecord->ExceptionCode;
    unsigned addr = (unsigned)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    unsigned base = (unsigned)(uintptr_t)GetModuleHandleA(NULL);

    if (code != 0xE06D7363u &&
        InterlockedIncrement(&g_veh_logs) <= 20)
        log_line("VEH: code=%08X addr=%08X exeVA=%08X ecx=%08X",
                 code, addr, addr >= base ? addr - base + 0x400000 : 0,
                 ep->ContextRecord->Ecx);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* diag#36: resolve the AVATAR entity handle through the engine's own
 * live chain (used by the camera-follow check 0x9F2287):
 *   player = 0x6EFD50([0x14F13D0]); ent = *([player+0xC4] + 0xC);
 *   type = 0x8FD430(ent) - the engine accepts 0x14/0x17 as avatar.
 * The collector wants a HANDLE, not a pointer: handles index the
 * entity table ([[0x14F1758]+0x30][h], count [+0x3C]) - scan it for
 * the entity pointer. Inject into the heap-scan collector and tick
 * it: the diag#25c crash wrote movement into a garbage-resolved
 * entity; the avatar is the intended target. Runs on the game
 * thread (post_tick_c), never from the watchdog. */
/* census cache shared between the one-shot vtable census and the
 * dword hunt inside native_stick_tick */
unsigned g_c2_types[64];
unsigned g_c2_counts[64];
unsigned g_c2_nt;

static void native_stick_tick(unsigned char *exe)
{
    typedef void *__thiscall (*player_fn)(void *);
    typedef int __thiscall (*type_fn)(void *);
    /* the collector ticks take (this, float, float) - the callee pops
     * the two stack floats (ret $8); calling them without the args
     * ate 8 bytes of the caller's stack (old diag#22/35 crash) */
    typedef void (__thiscall *col_fn)(void *, float, float);
    static int stage;                 /* 0 resolve, 2 ticking */
    static long throttle;

    if (!g_mov_collector)
        return;
    if (stage == 0) {
        unsigned tbl, arr, cnt, sess, player, tref = 0;
        unsigned cands[8];
        unsigned cand_n = 0;
        unsigned k, off, v, e, t;

        if ((++throttle & 0x3F) != 0)
            return;
        /* one-shot breadcrumbs at every silent gate (diag#39 session:
         * census never ran - one of these bailed without a trace) */
        {
            static int traced;

            if (!traced) {
                traced = 1;
                tbl = *(unsigned *)(exe + (0x014F1758u -
                                          EXE_DEFAULT_BASE));
                log_line("NATIVE2: gates tbl=%08X cnt=%08X arr=%08X "
                         "sess=%08X", tbl,
                         (tbl > 0x10000 &&
                          !IsBadReadPtr((void *)(uintptr_t)tbl, 0x40))
                             ? *(unsigned *)(uintptr_t)(tbl + 0x3C)
                             : 0,
                         (tbl > 0x10000 &&
                          !IsBadReadPtr((void *)(uintptr_t)tbl, 0x40))
                             ? *(unsigned *)(uintptr_t)(tbl + 0x30)
                             : 0,
                         *(unsigned *)(exe + (0x014F13D0u -
                                             EXE_DEFAULT_BASE)));
            }
        }
        /* handle space decoded from the engine resolver 0x818330:
         * handle = signed index into [[0x14F1758]+0x30], bounds
         * [+0x3C]. The camera-chain entity pointer is NOT in this
         * array (diag#38), so instead: census the table types and
         * hunt for the avatar's HANDLE stored inside the player
         * object (engines keep handles, not pointers, in owners) */
        tbl = *(unsigned *)(exe + (0x014F1758u - EXE_DEFAULT_BASE));
        if (tbl < 0x10000 || IsBadReadPtr((void *)(uintptr_t)tbl, 0x40))
            return;
        cnt = *(unsigned *)(uintptr_t)(tbl + 0x3C);
        arr = *(unsigned *)(uintptr_t)(tbl + 0x30);
        if (cnt == 0 || cnt > 0x4000 || arr < 0x10000 ||
            IsBadReadPtr((void *)(uintptr_t)arr, cnt * 4)) {
            static long b1;

            if (!b1) {
                b1 = 1;
                log_line("NATIVE2: bail table cnt=%u arr=%08X", cnt,
                         arr);
            }
            return;
        }
        sess = *(unsigned *)(exe + (0x014F13D0u - EXE_DEFAULT_BASE));
        if (sess < 0x10000 || IsBadReadPtr((void *)(uintptr_t)sess, 4)) {
            static long b2;

            if (!b2) {
                b2 = 1;
                log_line("NATIVE2: bail sess");
            }
            return;
        }
        player = (unsigned)(uintptr_t)((player_fn)(void *)(uintptr_t)
                  (exe + (0x006EFD50u - EXE_DEFAULT_BASE)))
            ((void *)(uintptr_t)sess);
        if (!player || player < 0x10000 ||
            IsBadReadPtr((void *)(uintptr_t)player, 0x40)) {
            static long b3;

            if (!b3) {
                b3 = 1;
                log_line("NATIVE2: bail player=%08X", player);
            }
            return;
        }
        /* reference type from the live camera chain (type 13 in the
         * diag#38 session) - the avatar's table twin must match it */
        {
            unsigned c4 = *(unsigned *)(uintptr_t)(player + 0xC4);

            if (c4 > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)c4, 0x10)) {
                unsigned ent = *(unsigned *)(uintptr_t)(c4 + 0xC);

                if (ent > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)ent, 4))
                    tref = (unsigned)((type_fn)(void *)(uintptr_t)
                           (exe + (0x008FD430u - EXE_DEFAULT_BASE)))
                        ((void *)(uintptr_t)ent);
            }
        }
        /* diag#44: crash post-mortem - the two managers have
         * DIFFERENT index spaces (arr2[0x293] was a wrong entity,
         * its vt+0x58 returned garbage -> [edi+0x2D] smash). But
         * the apply entity must carry a +0x148 handle of the
         * [0x14F1758] space (apply checks the world flag through
         * 0x756170). So: resolve the PLAYER entity via arr1 (the
         * player's own +0x148 handle), then find that POINTER in
         * arr2 - that index is the avatar handle in apply space. */
        {
            unsigned tbl2, arr2, cnt2, e2, vt2, m, ph;
            unsigned pent = 0, h2 = 0xFFFFFFFFu, i2;

            tbl2 = *(unsigned *)(exe + (0x014F13C4u - EXE_DEFAULT_BASE));
            if (tbl2 < 0x10000 ||
                IsBadReadPtr((void *)(uintptr_t)tbl2, 0x40))
                return;
            cnt2 = *(unsigned *)(uintptr_t)(tbl2 + 0x3C);
            arr2 = *(unsigned *)(uintptr_t)(tbl2 + 0x30);
            if (cnt2 == 0 || cnt2 > 0x8000 || arr2 < 0x10000 ||
                IsBadReadPtr((void *)(uintptr_t)arr2, 0x40))
                return;
            /* player entity pointer via the [0x14F1758] space */
            if (!IsBadReadPtr((void *)(uintptr_t)(player + 0x148), 4)) {
                unsigned h1 = *(unsigned *)(uintptr_t)(player + 0x148);

                if (h1 < cnt) {
                    pent = *(unsigned *)(uintptr_t)(arr + h1 * 4);
                    if (pent < 0x10000)
                        pent = 0;
                }
            }
            if (!pent) {
                static long w0;

                if (!w0) {
                    w0 = 1;
                    log_line("NATIVE2: player entity not resolvable "
                             "yet - retry");
                }
                return;
            }
            /* locate the same pointer inside arr2 */
            for (i2 = 0; i2 < cnt2; i2++) {
                unsigned v2 = *(unsigned *)(uintptr_t)(arr2 + i2 * 4);

                if (v2 == pent) {
                    h2 = i2;
                    break;
                }
            }
            if (h2 == 0xFFFFFFFFu) {
                static long w1;

                if (!w1) {
                    w1 = 1;
                    log_line("NATIVE2: player ent %08X NOT in arr2 - "
                             "apply space mismatch", pent);
                }
                return;
            }
            /* validate the found entry thoroughly before ticking */
            e2 = pent;
            vt2 = *(unsigned *)(uintptr_t)e2;
            if (vt2 < (unsigned)(uintptr_t)exe ||
                IsBadReadPtr((void *)(uintptr_t)vt2, 0x64)) {
                static long w2;

                if (!w2) {
                    w2 = 1;
                    log_line("NATIVE2: pent vt=%08X BAD", vt2);
                }
                return;
            }
            m = *(unsigned *)(uintptr_t)(vt2 + 0x58);
            if (m < (unsigned)(uintptr_t)exe ||
                IsBadReadPtr((void *)(uintptr_t)m, 4)) {
                static long w3;

                if (!w3) {
                    w3 = 1;
                    log_line("NATIVE2: pent vt+0x58=%08X BAD", m);
                }
                return;
            }
            ph = h2;
            log_line("NATIVE2: player ent %08X = arr2[%X] - INJECT+"
                     "TICK (was %X)", pent, ph, g_collector_handle);
            *(unsigned *)(uintptr_t)(g_mov_collector + 4) = ph;
            stage = 2;
        }
    }
    ((col_fn)(void *)(uintptr_t)(exe + (MOVC_VA - EXE_DEFAULT_BASE)))
        ((void *)(uintptr_t)g_mov_collector, 0.0f, 0.0f);
    g_nstick_ticks++;
}

/* post-tick collector driver: called from the return-address
 * trampoline after the input tick body finished. First-call
 * breadcrumbs localize a hard crash to a stage even when no
 * exception is loggable. */
extern "C" void __cdecl post_tick_c(void)
{
    /* the collector ticks take (this, float, float) - the callee pops
     * the two stack floats (ret $8); calling them without the args
     * ate 8 bytes of the caller's stack (old diag#22/35 crash) */
    typedef void (__thiscall *col_fn)(void *, float, float);
    static unsigned char *exe;
    static int stage;

    if (!exe)
        exe = (unsigned char *)GetModuleHandleA(NULL);
    /* diag#37: 0x7355F0 never fires on PC (wd: t=0) - the live
     * per-frame entry is the pup hook (Paddev::Update head), which
     * calls native_stick_tick itself. This branch stays for the
     * bundle experiment if the wrapper singleton ever exists. */
    if (g_native_stick == 1) {
        if (!g_bundle_col)
            find_bundle_collector(exe);
        if (g_bundle_col) {
            static int nstage;

            if (nstage == 0) {
                nstage = 1;
                log_line("NATIVE: FIRST moveCol tick obj=%08X handle=%08X",
                         g_bundle_col, g_bundle_handle);
            }
            ((col_fn)(void *)(uintptr_t)(exe + (MOVC_VA - EXE_DEFAULT_BASE)))
                ((void *)(uintptr_t)g_bundle_col, 0.0f, 0.0f);
            g_nstick_ticks++;
            if (nstage == 1) {
                nstage = 2;
                log_line("NATIVE: first tick returned OK");
            }
        }
    }
    if (!g_run_collectors || !g_mov_collector)
        return;
    if (stage == 0) {
        stage = 1;
        log_line("post: FIRST MOVC call obj=%08X handle=%08X",
                 g_mov_collector, g_collector_handle);
    }
    ((col_fn)(void *)(uintptr_t)(exe + (MOVC_VA - EXE_DEFAULT_BASE)))
        ((void *)(uintptr_t)g_mov_collector, 0.0f, 0.0f);
    if (stage == 1) {
        stage = 2;
        log_line("post: MOVC returned OK (first)");
    }
    if (g_run_camc) {
        if (stage < 3) {
            stage = 3;
            log_line("post: FIRST CAMC call obj=%08X",
                     g_mov_collector);
        }
        ((col_fn)(void *)(uintptr_t)(exe + (CAMC_VA - EXE_DEFAULT_BASE)))
            ((void *)(uintptr_t)g_mov_collector, 0.0f, 0.0f);
        if (stage == 3) {
            stage = 4;
            log_line("post: CAMC returned OK (first)");
        }
    }
}


/* ---------------- M2: native X360 pad defaults ----------------
 * Engine binding manager: global 0x14F16FC (0x80-byte object, ctor
 * 0x82E050, vtable 0x1327464). Bind 0x82E140 = thiscall(ecx=mgr,
 * int action, Token by-value 20B, int slot); action elements live in
 * the mgr+0x68 container (stride 0x54, two token slots each at
 * +0x24/+0x38). Pad defaults = 0x82E430 (X360 layout, 33 binds),
 * kb defaults = 0x82E790, full reset = 0x82CC10. The engine's own
 * btnRevert path with the gamepad flag (0x14F18FC = settings+0x74==4)
 * runs exactly: ecx=mgr; 0x82CC10(); 0x82E430(). We replay that once
 * the manager exists and the boot-time profile load has settled. */
#define BIND_VA        0x0082E140u
#define BIND_HEAD      8u
#define MGR_VT_VA      0x01327464u
#define CLEAR_VA       0x0082CC10u
#define PADDEFAULTS_VA 0x0082E430u
/* byte flag = (settings+0x74 == 4); gates the engine's controller mode */
#define PADFLAG_VA     0x014F18FCu

static int g_paddefaults_engine = 0;
static int g_paddefaults_clear = 1;
static int g_paddefaults_delay = 15000;
static int g_log_binds = 1;
static int g_padforce_flag = 0;
static int g_pad_loadfix = 1;
static int g_pad_keep_load = 0;
static int g_pad_boot_mode = 0;
static int g_xenon_bundle = 0;   /* §183: also preload the Xenon movie set */
static int g_xenon_ui = 0;       /* §184: 1=log, 2/3=remap (pf), 4/5=always */
static unsigned g_ui_seen[256];
static DWORD g_ui_tail_wrap;
static DWORD g_ui_tail_fact;
static volatile DWORD g_world_ready_ms;  /* last MusicRegion event (world up) */
static volatile DWORD g_load_start_ms;   /* first popup of the current load */

/* Our own X360 table (decoded from the engine's 0x82E430): 17 button
 * binds (group 1, proven live since diag#9) + 12 axis binds (group 3,
 * added in diag#11): movement halves AXIS0-3 -> acts 0x3/0x2/0x0/0x1
 * (right/left/forward/back), camera AXIS4-7 -> two actions each,
 * exactly the pairs the engine's own defaults function uses. Keyboard
 * binds keep the primary slot, so W/A/S/D remains the fallback. */
struct engine_token {
    unsigned vptr, group, idx, combo, last;
};
struct pad_bind { unsigned char group, idx, combo, action; };
static const struct pad_bind k_pad_binds[] = {
    /* MINIMAL table (diag#31). diag#30 post-mortem: the old X360
     * button set carried {1,6->act5} - act5 is the MOUSE-Y (camera)
     * action, so every stick-up push ALSO yanked the camera,
     * poisoning all direction tests since diag#11; Select/Start
     * (virts 0xE/0xF on this pad) were bound to unknown acts.
     * Button identities are PAD-SPECIFIC (dwOfs order) - a proper
     * per-pad button table is future M2 work. Kept: dpad, the four
     * face buttons (quickbar), the stick-movement quartet. */
    /* diag#69/70: acts 0x50-0x53 proved to do NOTHING in the world
     * (leftover console actions - the binds landed, the user saw
     * nothing). ABXY now go to PROVEN keyboard acts, ids verified
     * from the game's own token dump (tk): 0x7=Space (A4/39),
     * 0x12=key '1' (A4/2), 0x13=key '2' (A4/3), 0x5=use/interact
     * (A2/1=RMB + A4/12=E). Canonical: A=0 B=1 Y=2 X=3.
     * B was on 0x5E (R, weapon) in diag#69 - the bind landed but
     * NOTHING happened (user test), so B moves to act 0x5.
     * RT -> attack: act 0x4 is the game's LMB action (A2/0);
     * canonical 12 (L3) is the carrier - trigger_to_virt digitizes
     * the right trigger into L3's slot, so the physical L3 attacks
     * too. */
    /* §150: X360 console layout. Acts translated from the engine's own
     * X360 table (0x82E430) through the PC action registry (0x82B850
     * names): A=Quickmap 3, B=Quickmap 2, Y=Quickmap 1, X=Quickmap 4
     * (console face order Y/B/A/X = slots 1/2/3/4), LB=Interact/dash,
     * L3=Draw/sheathe weapon, R3=Tactical pause, Start=ESC(0x55),
     * Back=Open map(0xF). Console runs Jump on RT (0x7) — RT is our OS
     * LMB click now, so Jump moves to RB. Dpad quickbar 5-8 stays on
     * the OS '5'-'8' rows (native dpad binds would collide with the
     * stick-movement carrier slots 8..11, MoveVirt). The old L3
     * attack-carrier bind {1,12->act4} is gone: RT is a mouse click. */
    {1, 0, 0, 0x14}, {1, 1, 0, 0x13}, {1, 2, 0, 0x12}, {1, 3, 0, 0x15},
    {1, 6, 0, 0x05}, {1, 7, 0, 0x07}, {1, 12, 0, 0x08},
    /* §155/§156: Start and Back have NO native binds — their acts
     * (0x55 ESC, 0x0F map) open the panel but the panel closes on the
     * real key event, not on the action. Both are delivered as the
     * game's own keys (ESC / M) from the fast row poll. */
    {1, 13, 0, 0x0A},
    /* §151: dpad Quickmap 5-8 (console order U/R/D/L = slots 5/6/7/8).
     * combo=0xFD sentinel: applied ONLY in WASD-movement mode
     * (NativeMove=0) — the dpad canonical slots then carry no movement
     * bind, so a physical dpad press fires exactly one action. */
    {1, 10, 0xFD, 0x16}, {1, 9, 0xFD, 0x17},
    {1, 11, 0xFD, 0x18}, {1, 8, 0xFD, 0x19},
    {3, 2, 0, 0x00}, {3, 3, 0, 0x01}, {3, 0, 0, 0x03}, {3, 1, 0, 0x02},
    /* diag#64/65/66/67: the g1 movement quartet. It was removed in
     * diag#35 because it shadowed the g3 axis pairs - but the X360
     * collector path is provably dead on PC, so the g3 pairs are
     * useless. This quartet makes the game's OWN pad-button path
     * move the hero: our stick_to_virt writes the directions into
     * the pad's virtual button array and these binds map them onto
     * actions 0..3 (forward/back/strafe). The whole path is inside
     * the game - NOT a single OS key event.
     * combo=0xFE is a SENTINEL: the canonical button id for each of
     * the four comes from ini MoveVirt (g_move_virt[action]) instead
     * of this table. diag#66 proved the g1 idx is CANONICAL (the
     * consumer polls va[k_canon_virt[idx]]), so the default
     * MoveVirt=10,11,8,9 binds movement to the canonical D-pad -
     * the physical D-pad walks too, exactly like on the console. */
    {1, 0, 0xFE, 0x00}, {1, 0, 0xFE, 0x01}, {1, 0, 0xFE, 0x02},
    {1, 0, 0xFE, 0x03},
    /* diag#35: the g1 movement quartet ({1,6->0},{1,0->1},{1,4->2},
     * {1,0xC->3}) REMOVED - it overwrote slotB and shadowed the g3
     * axis pairs. gav 0x6EAFF0 (statically decoded): if the act's
     * keyboard level is idle, it reads the slotA/B TOKEN {group@+4,
     * idx@+8}; group 3 + idx<0xE -> GetAxisValue(pad, idx) = the
     * signed-rectified processed axis float. idx space: 0=X+ 1=X-
     * 2=Y+ 3=Y- 4..7=stick2 0xC/0xD=combined triggers. The g3 rows
     * above are the engine's OWN X360 defaults (decoded 0x82E430):
     * act0=W<-Y+ act1=S<-Y- act2=A<-X- act3=D<-X+. Consumer of gav
     * acts 0-3 = MoveCollector::Tick 0xA8B620 (dead on PC - woken
     * by NativeStick). */
};
#define TOKEN_VT_VA 0x01318508u
/* Bind 0x82E140 ends with `ret $0x18`: the callee pops exactly
 * action(4) + token(20) and there is NO 4th stack argument — the
 * slot selector is token.last (-1 auto-shift, 0/1 explicit slot).
 * We pass token.last=1: explicit alternate-slot write, the same
 * pattern the profile loader and the options rebind UI use. */
typedef void (__thiscall *bind_byval_fn)(void *, int, struct engine_token);

extern "C" {
    volatile unsigned g_bind_action, g_bind_group, g_bind_idx,
                      g_bind_combo, g_bind_slot, g_bind_this;
    volatile long g_bind_calls;
}

/* game-thread application helpers and the pump-side hook live above
 * (pump_tick_c applies the binds once g_want_pad_binds is armed) */

extern "C" void __cdecl bind_log_c(void)
{
    /* reset-detection runs before the logging cap: long sessions can
     * exceed 600 logged binds while still needing re-arm after the
     * user hits "reset to defaults" */
    if (g_bind_slot == 0xFFFFFFFFu)
        g_reset_burst++;
    else
        g_reset_burst = 0;
    if (!g_log_binds || g_bind_calls > 600)
        return;
    log_line("bind#%ld: act=0x%X g=%u idx=0x%X combo=0x%X slot=0x%X "
             "mgr=%08X", g_bind_calls, g_bind_action, g_bind_group,
             g_bind_idx, g_bind_combo, g_bind_slot, g_bind_this);
}

static void apply_own_pad_binds(unsigned char *exe, void *mgr)
{
    struct engine_token tok;
    unsigned i, n = sizeof(k_pad_binds) / sizeof(k_pad_binds[0]);

    g_own_binds_running = 1;
    tok.vptr = (unsigned)(uintptr_t)exe + (TOKEN_VT_VA - EXE_DEFAULT_BASE);
    tok.last = 1; /* alternate slot: keep profile keyboard binds primary */
    if (g_paddefaults_engine == 2) {
        /* M4-binds (§134): apply the engine's OWN X360 default table
         * (0x82E430) WITHOUT the 0x82CC10 reset - the reset wipes the
         * profile keyboard binds and killed movement in diag#6. This
         * mode exists to READ the console layout: the options/controls
         * screen then names every action the X360 table binds. */
        typedef void (__thiscall *x360_defaults_fn)(void *);

        ((x360_defaults_fn)(exe + (0x0082E430u - EXE_DEFAULT_BASE)))(mgr);
        log_line("paddefaults: ENGINE X360 table applied (no reset)");
        /* §149: return early — the generic tail line would wrongly
         * claim "%u own pad button binds" for the engine's table */
        g_own_binds_running = 0;
        return;
    } else if (g_paddefaults == 2) {
        /* minimal: ONLY the four movement axis binds (acts 0..3 read
         * by the X360 MoveCollector). The engine profile keeps its own
         * button binds, so nothing of the user's setup is shadowed. */
        static const struct pad_bind min_binds[4] = {
            {3, 2, 0, 0x00}, {3, 3, 0, 0x01}, {3, 0, 0, 0x03}, {3, 1, 0, 0x02}
        };

        for (i = 0; i < 4; i++) {
            const struct pad_bind *b = &min_binds[i];

            tok.group = b->group;
            tok.idx = b->idx;
            tok.combo = b->combo;
            ((bind_byval_fn)(exe + (BIND_VA - EXE_DEFAULT_BASE)))
                (mgr, b->action, tok);
        }
        n = 4;
    } else {
        unsigned applied = 0;

        for (i = 0; i < n; i++) {
            const struct pad_bind *b = &k_pad_binds[i];
            unsigned idx = b->idx, cmb = b->combo;

            if (b->combo == 0xFE) {
                /* movement quartet: slot from ini MoveVirt. §151: in
                 * WASD-movement mode the OS rows own movement and the
                 * dpad canonicals must stay free for Quickmap 5-8. */
                if (!g_native_move)
                    continue;
                idx = (unsigned)g_move_virt[b->action & 3];
                cmb = 0;
            } else if (b->combo == 0xFD) {
                /* dpad Quickmap 5-8: only when movement is NOT on the
                 * native carriers (else dpad would walk AND switch) */
                if (g_native_move)
                    continue;
                cmb = 0;
            }
            tok.group = b->group;
            tok.idx = idx;
            tok.combo = cmb;
            ((bind_byval_fn)(exe + (BIND_VA - EXE_DEFAULT_BASE)))
                (mgr, b->action, tok);
            applied++;
        }
        n = applied;
    }
    g_own_binds_running = 0;
    log_line("paddefaults: %u own pad button binds applied on %s thread "
             "(engine bind calls so far=%ld)", n,
             g_pump_hook_installed ? "game" : "background", g_bind_calls);
}

static int install_bind_hook(void)
{
    /* sub esp,0x14; push edi; mov edi,[esp+0x1C] */
    static const unsigned char expect[8] =
        { 0x83, 0xEC, 0x14, 0x57, 0x8B, 0x7C, 0x24, 0x1C };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (BIND_VA - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;
    unsigned gact = (unsigned)(uintptr_t)&g_bind_action;
    unsigned ggrp = (unsigned)(uintptr_t)&g_bind_group;
    unsigned gidx = (unsigned)(uintptr_t)&g_bind_idx;
    unsigned gcmb = (unsigned)(uintptr_t)&g_bind_combo;
    unsigned gslt = (unsigned)(uintptr_t)&g_bind_slot;
    unsigned gthi = (unsigned)(uintptr_t)&g_bind_this;
    unsigned gcnt = (unsigned)(uintptr_t)&g_bind_calls;

    for (k = 0; k < BIND_HEAD; k++) {
        if (target[k] != expect[k]) {
            log_line("bind hook: prologue mismatch at %p byte %u: "
                     "%02X != %02X", target, k, target[k], expect[k]);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    /* entry stack: [ret][action][tok5 dwords][slot] — capture args,
     * log via cdecl callback, then replay the stolen prologue */
    *c++ = 0x9C;                                    /* pushfd */
    *c++ = 0x50; *c++ = 0x51; *c++ = 0x52;          /* push eax/ecx/edx */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x14;  /* mov eax,[esp+0x14] action */
    *c++ = 0xA3; memcpy(c, &gact, 4); c += 4;
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x1C;  /* group */
    *c++ = 0xA3; memcpy(c, &ggrp, 4); c += 4;
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x20;  /* idx */
    *c++ = 0xA3; memcpy(c, &gidx, 4); c += 4;
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x24;  /* combo */
    *c++ = 0xA3; memcpy(c, &gcmb, 4); c += 4;
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28;  /* slot */
    *c++ = 0xA3; memcpy(c, &gslt, 4); c += 4;
    *c++ = 0x89; *c++ = 0x0D; memcpy(c, &gthi, 4); c += 4; /* mov [gthi],ecx */
    *c++ = 0xFF; *c++ = 0x05; memcpy(c, &gcnt, 4); c += 4; /* inc [gcnt] */
    *c++ = 0xE8;                                    /* call bind_log_c */
    rel = (long)((uintptr_t)&bind_log_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x5A; *c++ = 0x59; *c++ = 0x58;          /* pop edx/ecx/eax */
    *c++ = 0x9D;                                    /* popfd */
    memcpy(c, expect, BIND_HEAD); c += BIND_HEAD;   /* replay prologue */
    *c++ = 0xE9;                                    /* jmp Bind+8 */
    rel = (long)((uintptr_t)(target + BIND_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, BIND_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    for (k = 5; k < BIND_HEAD; k++)
        target[k] = 0x90;
    VirtualProtect(target, BIND_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, BIND_HEAD);
    log_line("bind hook installed: target=%p stub=%p", target, stub);
    return 1;
}

/* §157: direct reset detector. 0x82CC10 (the table clear, called by the
 * 0x82ED10 reset dispatcher — options "reset to defaults" and boot)
 * starts with `sub esp,0x174` (6 bytes, side-effect free): steal it,
 * bump g_reset_seen, replay it and jump back. The paddefaults thread
 * watches the counter and re-applies our binds after the engine's own
 * defaults have been laid down (the earlier burst heuristic never
 * fired — the clear path writes no Bind(-1) traffic). */
extern "C" void __cdecl reset_zap_c(void)
{
    /* §177: runs AT THE LOAD/RESET START, on the calling thread, before
     * the loader continues. Pad mode must be OFF while a save loads
     * (pad-mode loads black-screened); the monitor restores it later. */
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *p = exe + (PADFLAG_VA - EXE_DEFAULT_BASE);
    static long n;

    if (*p && !g_pad_keep_load) {
        *p = 0;
        if (++n <= 30)
            log_line("load: pad flag forced 1->0 (load start)");
    }
}

static int install_reset_hook(void)
{
    static const unsigned char expect[6] =
        { 0x81, 0xEC, 0x74, 0x01, 0x00, 0x00 };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x0082CC10u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k, seen_addr;

    for (k = 0; k < 6; k++) {
        if (target[k] != expect[k]) {
            log_line("reset hook: prologue mismatch at %p byte %u: %02X",
                     target, k, target[k]);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    seen_addr = (unsigned)(uintptr_t)&g_reset_seen;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0xE8;                                  /* call reset_zap_c */
    rel = (long)((uintptr_t)&reset_zap_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0xFF; *c++ = 0x05;                     /* inc dword [seen] */
    memcpy(c, &seen_addr, 4); c += 4;
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, expect, 6); c += 6;                 /* replay sub esp,174h */
    *c++ = 0xE9;                                  /* jmp back */
    rel = (long)((uintptr_t)(target + 6) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    VirtualProtect(target, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    log_line("reset hook installed: target=%p stub=%p", target, stub);
    return 1;
}

/* §158: rumble. The engine's rumble factory (0xFA8820) picks the
 * AkRumbleControllerXInput variant when its selector is 0 and the
 * DirectInput variant otherwise. On PC the selector is non-zero, the DI
 * variant cannot initialise force feedback on an XInput pad (string
 * "Unable to initialise rumble on a direct input device" @ 0xFBB15E)
 * and no rumble happens even though the options screen offers
 * RumbleIntensity/TestRumble. The factory's branch at 0xFA8830 is a
 * 2-byte `jz short` — flip 0x74 to 0xEB and it always builds the
 * XInput variant (XINPUT1_3.dll is statically imported, so
 * XInputSetState/GetCapabilities are live). */
static int install_rumble_patch(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x00FA8830u - EXE_DEFAULT_BASE);
    DWORD old;

    if (target[0] != 0x74) {
        log_line("rumble: expected jz (74) at %p, found %02X — skipped",
                 target, target[0]);
        return 0;
    }
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xEB;               /* jmp short: always the XInput path */
    VirtualProtect(target, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    log_line("rumble: XInput rumble controller forced (jz->jmp at %p)",
             target);
    return 1;
}

/* §180: in "controller mode" (0x14F18FC=1) the engine selects the console
 * loading screen `GUI\Xenon\loadingScreen.swf` (0x1363EF8) at 0xB5232F;
 * the PC data set never renders it, so after a save load the black loading
 * curtain is never lifted (the world itself loads behind it — diag#12).
 * The branch at 0xB5222F is `jz 0xB523E8` -> the PC path. Make it
 * unconditional so the Win32 loading screen is used in pad mode too. */
static int install_loadscreen_fix(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x00B5222Fu - EXE_DEFAULT_BASE);
    static const unsigned char expect[6] = { 0x0F, 0x84, 0xB3, 0x01, 0x00, 0x00 };
    static const unsigned char patch[6]  = { 0xE9, 0xB4, 0x01, 0x00, 0x00, 0x90 };
    DWORD old;

    if (memcmp(target, expect, 6) != 0) {
        log_line("loadfix: unexpected bytes at %p (%02X %02X %02X %02X %02X %02X)"
                 " - skipped", target, target[0], target[1], target[2],
                 target[3], target[4], target[5]);
        return 0;
    }
    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(target, patch, 6);
    VirtualProtect(target, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    log_line("loadfix: pad-mode loading screen forced to Win32 (jz->jmp at %p)",
             target);
    return 1;
}

/* §183: the GUI bundle preloader (0xB523E8..0xB52459) loads 50 movies
 * from the Win32 name table [0x14D0958] when pad mode is off. The console
 * branch of the same function (0xB52387..0xB523E6) loads 10 Xenon movies
 * from [0x14D092C] instead, but only while the pad flag is on — which is
 * exactly when a save load breaks. This patch keeps the Win32 bundle (and
 * with it the PC loading screen) and appends the Xenon set: the loop base
 * is redirected to a 60-entry table (50 Win32 + 10 Xenon) allocated by
 * us, and the loop bound 0xC8 (50) becomes 0xF0 (60). */
static int install_xenon_bundle(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *insn = exe + (0x00B52400u - EXE_DEFAULT_BASE);
    unsigned char *bound = exe + (0x00B52451u - EXE_DEFAULT_BASE);
    unsigned char *tbl;
    DWORD old;
    unsigned int *p;

    /* mov ecx,[edi+disp32] / cmp edi,imm32; the disp32 is rebased by the
     * loader, so only the opcodes and the 0xC8 immediate are stable. */
    if (insn[0] != 0x8B || insn[1] != 0x8F ||
        bound[0] != 0x81 || bound[1] != 0xFF ||
        *(unsigned int *)(bound + 2) != 0xC8) {
        log_line("xenonbundle: pattern mismatch at %p/%p - skipped",
                 insn, bound);
        return 0;
    }
    tbl = (unsigned char *)VirtualAlloc(NULL, 0x100, MEM_COMMIT,
                                        PAGE_READWRITE);
    if (!tbl)
        return 0;
    if (g_xenon_bundle == 2) {
        /* mode 2: swap only GUI\Win32\hud.swf -> GUI\Xenon\hud.swf, so the
         * console HUD can be tested without loading duplicate movies. */
        unsigned int win_hud = (unsigned int)(uintptr_t)
            (exe + (0x01363B24u - EXE_DEFAULT_BASE));
        unsigned int xen_hud = (unsigned int)(uintptr_t)
            (exe + (0x01363818u - EXE_DEFAULT_BASE));
        unsigned int *src = (unsigned int *)(exe +
            (0x014D0958u - EXE_DEFAULT_BASE));
        int i, idx = -1;

        memcpy(tbl, src, 50 * 4);
        p = (unsigned int *)tbl;
        for (i = 0; i < 50; i++) {
            if (p[i] == win_hud) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            log_line("xenonbundle: win32 hud entry not found - skipped");
            return 0;
        }
        p[idx] = xen_hud;
        log_line("xenonbundle: hud swapped at index %d (%08X -> %08X)",
                 idx, win_hud, xen_hud);
    } else {
        memcpy(tbl, exe + (0x014D0958u - EXE_DEFAULT_BASE), 50 * 4);
        memcpy(tbl + 50 * 4, exe + (0x014D092Cu - EXE_DEFAULT_BASE), 10 * 4);
        p = (unsigned int *)tbl;
        log_line("xenonbundle: first=%08X mid=%08X xenon=%08X",
                 p[0], p[49], p[50]);
    }

    if (!VirtualProtect(insn + 2, 4, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    *(unsigned int *)(insn + 2) = (unsigned int)(uintptr_t)tbl;
    VirtualProtect(insn + 2, 4, old, &old);
    if (g_xenon_bundle != 2) {
        if (!VirtualProtect(bound + 2, 4, PAGE_EXECUTE_READWRITE, &old))
            return 0;
        *(unsigned int *)(bound + 2) = 60 * 4;
        VirtualProtect(bound + 2, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), insn, 6);
    FlushInstructionCache(GetCurrentProcess(), bound, 6);
    log_line("xenonbundle: table %p (mode %d)", tbl, g_xenon_bundle);
    return 1;
}

/* §184: console UI via screen-id remapping. FUN_0077d640 is the generic
 * "create screen" wrapper (params: id, unused, flag); it forwards the id
 * to the switch FUN_00a80c90, which instantiates a PC or console screen
 * class per id. Console screens are a parallel id set (e.g. PC
 * menuOverview = 0x4A, Xenon menuOverview = 0x2C), so switching the pad
 * flag alone never reaches them. We hook both functions and, with the
 * pad flag on, rewrite the requested id (wrapper mode) or just the
 * factory argument (factory mode). */
static unsigned ui_remap(unsigned id, int where)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *pf = exe + (PADFLAG_VA - EXE_DEFAULT_BASE);
    static const unsigned short map[][2] = {
        /* §188: the revamp screen carries the whole in-game menu (hero,
         * skills, journal, state, inventory, map) as tabs, so a single
         * pair is enough. Other lazy screens are added only if a test
         * proves they need their own class. */
        { 0x4B, 0x15 },   /* inventory -> inventory_revamp */
    };
    int mode = g_xenon_ui;
    int do_remap = 0;
    unsigned i;

    if (id < 256 && !g_ui_seen[id]) {
        g_ui_seen[id] = 1;
        log_line("ui: create id=0x%X pf=%u site=%s", id, (unsigned)*pf,
                 where ? "factory" : "wrapper");
    }
    if (mode == 2 || mode == 3)
        do_remap = *pf != 0;
    else if (mode >= 4)
        do_remap = 1;
    if (where ? (mode == 2 || mode == 4) : (mode == 3 || mode == 5))
        do_remap = 0;
    if (!do_remap)
        return id;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i][0] == id) {
            log_line("ui: remap 0x%X -> 0x%X (mode %d, %s)", id, map[i][1],
                     mode, where ? "factory" : "wrapper");
            return map[i][1];
        }
    }
    return id;
}

static unsigned (*ui_remap_fn)(unsigned, int) = ui_remap;

/* §184: the stubs are emitted as machine code (GCC has no MS-style asm
 * blocks and x86 GCC has no naked functions). Each stub saves all
 * registers, rewrites the incoming screen id on the caller's stack,
 * restores, replays the stolen prologue bytes and jumps back. */
static unsigned char *make_ui_stub(int where, DWORD tail_slot)
{
    unsigned char *p = (unsigned char *)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *c = p;

    if (!p)
        return NULL;
    *c++ = 0x9C;                                   /* pushfd */
    *c++ = 0x60;                                   /* pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28;
                                                   /* mov eax,[esp+40] */
    *c++ = 0x6A; *c++ = (unsigned char)where;      /* push where */
    *c++ = 0x50;                                   /* push eax (id) */
    *c++ = 0xFF; *c++ = 0x15;                      /* call [ui_remap_fn] */
    *(unsigned int *)c = (unsigned int)(uintptr_t)&ui_remap_fn; c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;         /* add esp,8 */
    *c++ = 0x89; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28;
                                                   /* mov [esp+40],eax */
    *c++ = 0x61;                                   /* popad */
    *c++ = 0x9D;                                   /* popfd */
    if (where) {                                   /* sub esp,0x90 */
        *c++ = 0x81; *c++ = 0xEC;
        *(unsigned int *)c = 0x90; c += 4;
    } else {                                       /* sub esp,8 */
        *c++ = 0x83; *c++ = 0xEC; *c++ = 0x08;
        *c++ = 0x53;                               /* push ebx */
        *c++ = 0x56;                               /* push esi */
    }
    *c++ = 0xFF; *c++ = 0x25;                      /* jmp [tail] */
    *(unsigned int *)c = tail_slot; c += 4;
    return p;
}

static int install_xenon_ui(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *wrap = exe + (0x0077D640u - EXE_DEFAULT_BASE);
    unsigned char *fact = exe + (0x00A80C90u - EXE_DEFAULT_BASE);
    static const unsigned char wexp[5] = { 0x83, 0xEC, 0x08, 0x53, 0x56 };
    static const unsigned char fexp[6] = { 0x81, 0xEC, 0x90, 0x00, 0x00,
                                           0x00 };
    unsigned char *sw, *sf;
    DWORD old;
    long rel;

    if (memcmp(wrap, wexp, 5) != 0 || memcmp(fact, fexp, 6) != 0) {
        log_line("xenonui: prologue mismatch at %p/%p - skipped",
                 wrap, fact);
        return 0;
    }
    g_ui_tail_wrap = (DWORD)(uintptr_t)(wrap + 5);
    g_ui_tail_fact = (DWORD)(uintptr_t)(fact + 6);
    sw = make_ui_stub(0, (DWORD)(uintptr_t)&g_ui_tail_wrap);
    sf = make_ui_stub(1, (DWORD)(uintptr_t)&g_ui_tail_fact);
    if (!sw || !sf) {
        log_line("xenonui: stub allocation failed");
        return 0;
    }

    rel = (long)(sw - (wrap + 5));
    if (rel > 0x7FFFFFFFL || rel < -0x7FFFFFFFL) {
        log_line("xenonui: wrapper stub out of range");
        return 0;
    }
    if (!VirtualProtect(wrap, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    wrap[0] = 0xE9;
    *(long *)(wrap + 1) = rel;
    VirtualProtect(wrap, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), wrap, 5);

    rel = (long)(sf - (fact + 5));
    if (rel > 0x7FFFFFFFL || rel < -0x7FFFFFFFL) {
        log_line("xenonui: factory stub out of range");
        return 0;
    }
    if (!VirtualProtect(fact, 6, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    fact[0] = 0xE9;
    *(long *)(fact + 1) = rel;
    fact[5] = 0x90;
    VirtualProtect(fact, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fact, 6);

    log_line("xenonui: hooks installed (mode %d) wrap=%p fact=%p",
             g_xenon_ui, wrap, fact);
    return 1;
}

/* §181: enable the console ("controller") mode the way the console build
 * does: controller type = 4 BEFORE the engine's control-settings manager
 * derives its flags. The constructor calls FUN_00b38230 at 0xB3A872;
 * the callee reads [this+0x74] and, for 4, sets 0x14F18FC plus the whole
 * console flag block. Intercept that call site, write the type, then run
 * the original call. This is "native" boot mode (not a late flag poke). */
static int install_boot_padmode(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *site = exe + (0x00B3A872u - EXE_DEFAULT_BASE);
    unsigned char *stub;
    DWORD old;
    long rel;

    if (!(site[0] == 0xE8 && site[1] == 0xB9 && site[2] == 0xD9 &&
          site[3] == 0xFF && site[4] == 0xFF)) {
        log_line("bootpad: unexpected call bytes at %p - skipped", site);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    /* mov dword ptr [ecx+0x74], 4 */
    stub[0] = 0xC7; stub[1] = 0x41; stub[2] = 0x74; stub[3] = 0x04;
    stub[4] = 0x00; stub[5] = 0x00; stub[6] = 0x00;
    /* call FUN_00b38230 */
    stub[7] = 0xE8;
    rel = (long)((exe + (0x00B38230u - EXE_DEFAULT_BASE)) - (stub + 12));
    memcpy(stub + 8, &rel, 4);
    /* jmp back after the original call */
    stub[12] = 0xE9;
    rel = (long)((exe + (0x00B3A877u - EXE_DEFAULT_BASE)) - (stub + 17));
    memcpy(stub + 13, &rel, 4);

    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    site[0] = 0xE9;
    rel = (long)(stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log_line("bootpad: controller type 4 forced at ctor call %p", site);
    return 1;
}

/* §158-diag: where does the chain break?
 *  - factory 0xFA8820: was it called, with which userIndex/selector?
 *  - SetVibration 0xFBAD70 (XInput variant): do events/TestRumble call
 *    it, and with which intensities? */
static volatile long g_rumb_factory_calls, g_rumb_set_calls;

static void __cdecl rumb_factory_c(int user_index, int selector)
{
    if (InterlockedIncrement(&g_rumb_factory_calls) <= 3)
        log_line("rumble: factory called userIndex=%d selector=%d",
                 user_index, selector);
}

static void __cdecl rumb_set_c(unsigned l_bits, unsigned r_bits)
{
    long n = InterlockedIncrement(&g_rumb_set_calls);

    if (n <= 40 || (n % 200) == 0) {
        float l, r;

        memcpy(&l, &l_bits, 4);
        memcpy(&r, &r_bits, 4);
        log_line("rumble: SetVibration #%ld l=%.3f r=%.3f", n, l, r);
    }
}

static int hook_steal6(unsigned va, void *cb, const unsigned char *expect,
                       int len, int check_len)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (va - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    int k;

    /* §160-fix: absolute operands carry LOADER RELOCATIONS (the exe
     * runs at a non-preferred base), so only the opcode prefix is
     * comparable; the replay uses the actual in-memory bytes. */
    for (k = 0; k < check_len; k++) {
        if (target[k] != expect[k]) {
            log_line("rumble diag: prologue mismatch at %p byte %u", target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28; /* eax = arg1 */
    *c++ = 0x50;
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x2C; /* eax = arg2 */
    *c++ = 0x50;
    *c++ = 0xE8;
    rel = (long)((uintptr_t)cb - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;        /* add esp,8 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, target, len); c += len;             /* replay actual bytes */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + len) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    for (k = 5; k < len; k++)
        target[k] = 0x90;
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return 1;
}

/* §160: capture the game's RumbleIntensity RTPC. FUN_00f134c0(a1,
 * name, value) is the Wwise parameter setter; the settings screen calls
 * it for "RumbleIntensity" (default 1.0, see _DAT_014cf7a0). Prologue
 * `push ebx; mov ebx,[0x12b76d4]` = 7 replayable bytes. */
static long g_vib_intensity_logged = -1;

static void __cdecl rumb_rtpc_c(const char *name, unsigned value_bits)
{
    float v;
    long ipct;

    if (!name || IsBadReadPtr(name, 20))
        return;
    if (strcmp(name, "RumbleIntensity") != 0)
        return;
    memcpy(&v, &value_bits, 4);
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    g_game_vib = v;
    ipct = (long)(v * 100.0f + 0.5f);
    if (ipct != g_vib_intensity_logged) {
        g_vib_intensity_logged = ipct;
        log_line("rumble: in-game RumbleIntensity = %ld%%", ipct);
    }
}

/* §162: Wwise event-name logger. The engine posts sound events by name
 * through 0xF13340 (same prologue as the RTPC setter: `push ebx; mov
 * ebx,[relocated slot]`). Collecting the vocabulary while the user
 * fights / opens chests lets us bind vibration to real world events. */
static volatile long g_wwise_calls;

static void __cdecl wwise_note_c(const char *name)
{
    static unsigned seen[256];
    static int nseen;
    unsigned h;
    int i;

    if (!name || IsBadReadPtr(name, 4))
        return;
    InterlockedIncrement(&g_wwise_calls);
    if (name[0] == 'o' && strcmp(name, "openPopUp") == 0) {
        unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
        unsigned char *pf = exe + (PADFLAG_VA - EXE_DEFAULT_BASE);

        {
            DWORD now_pop = GetTickCount();

            /* §180: openPopUp fires twice per load (start and the
             * "Загрузка прошла успешно!" message). Only the FIRST popup
             * of a load session starts a new session; otherwise the
             * second one would wipe the MusicRegion marker. */
            if (!g_load_ms || (long)(now_pop - g_load_ms) > 120000) {
                g_load_start_ms = now_pop;
                g_world_ready_ms = 0;
                /* §204: load/transition start feedback */
                vib_load_pulse("openPopUp");
            }
            g_load_ms = now_pop;
        }
        /* §178: the load UI popup fires BEFORE the load init; pad mode
         * must be OFF for the load (pad-mode loads black-screen — this
         * was reproduced with pf=1 and no 0x82CC10 in the path). */
        if (*pf && !g_pad_keep_load) {
            *pf = 0;
            log_line("load: pad flag forced 1->0 at popup");
        }
    }
    /* §180: MusicRegion fires when the loaded world starts its music.
     * Remember it (not for clearing the load window) so the monitor can
     * restore the pad flag as soon as the world is really up. */
    if (strncmp(name, "MusicRegion", 11) == 0)
        g_world_ready_ms = GetTickCount();
    /* §204: area/door transitions — RegionSwap_Start marks the start
     * of the swap (both Swap events fire inside the load window, see
     * §169); only the start drives the pulse. */
    if (strncmp(name, "RegionSwap_Start", 16) == 0)
        vib_load_pulse("RegionSwap_Start");
    /* §169: RegionSwap_End/MusicRegion fire DURING the load, not after
     * it — clearing the window on them made the flag force land inside
     * the load and black-screened the first save load. The window now
     * always runs its full 120 s. */
    h = 2166136261u;
    for (i = 0; i < 64 && name[i]; i++)
        h = (h ^ (unsigned char)name[i]) * 16777619u;
    for (i = 0; i < nseen && i < 256; i++)
        if (seen[i] == h)
            return;
    if (nseen < 256) {
        seen[nseen++] = h;
        log_line("wwise: '%s'", name);
    }
}

static int install_wwise_log(void)
{
    static const unsigned char expect[3] = { 0x53, 0x8B, 0x1D };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x00F13340u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    for (k = 0; k < 3; k++) {
        if (target[k] != expect[k]) {
            log_line("wwise log: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x2C; /* name (arg2) */
    *c++ = 0x50;
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&wwise_note_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x04;        /* add esp,4 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, target, 7); c += 7;                 /* replay actual bytes */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 7) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    target[6] = 0x90;
    VirtualProtect(target, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 7);
    log_line("wwise log hook installed: target=%p stub=%p", target, stub);
    return 1;
}

static int install_rumble_slider_hook(void)
{
    static const unsigned char expect[7] =
        { 0x53, 0x8B, 0x1D, 0xD4, 0x76, 0x2B, 0x01 };
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (0x00F134C0u - EXE_DEFAULT_BASE);
    unsigned char *stub, *c;
    DWORD old;
    long rel;
    unsigned k;

    /* §160-fix: byte 5 holds the relocated IAT address — check the
     * opcodes only, replay the actual bytes. */
    for (k = 0; k < 3; k++) {
        if (target[k] != expect[k]) {
            log_line("rumble slider: prologue mismatch at %p byte %u",
                     target, k);
            return 0;
        }
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT |
                                         MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x2C; /* name */
    *c++ = 0x50;
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x34; /* value */
    *c++ = 0x50;
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&rumb_rtpc_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;        /* add esp,8 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, target, 7); c += 7;                 /* replay actual bytes */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + 7) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    target[6] = 0x90;
    VirtualProtect(target, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 7);
    log_line("rumble slider hook installed: target=%p stub=%p",
             target, stub);
    return 1;
}

static void install_rumble_diag(void)
{
    /* factory: mov ecx,[esp+4]; mov eax,[ecx] */
    static const unsigned char fexp[6] =
        { 0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x01 };
    /* SetVibration: push ecx; fld qword [0x13201d8] */
    static const unsigned char sexp[7] =
        { 0x51, 0xDD, 0x05, 0xD8, 0x01, 0x32, 0x01 };

    if (!hook_steal6(0x00FA8820u, (void *)&rumb_factory_c, fexp, 6, 6))
        log_line("rumble diag: factory hook failed");
    if (!hook_steal6(0x00FBAD70u, (void *)&rumb_set_c, sexp, 7, 3))
        log_line("rumble diag: SetVibration hook failed");
}

typedef void (__thiscall *thiscall0_fn)(void *);

static DWORD WINAPI paddefaults_thread(void *param)
{
    unsigned char *exe;
    void *mgr = NULL;
    DWORD mgr_at = 0;
    int waited = 0, quiet_ms = 0;
    long last_calls = 0;

    (void)param;
    for (;;) {
        exe = (unsigned char *)GetModuleHandleA(NULL);
        mgr = *(void * volatile *)(exe + (MGR_GLOBAL_VA - EXE_DEFAULT_BASE));
        if (mgr)
            break;
        Sleep(250);
        waited += 250;
        if (waited >= 120000) {
            log_line("paddefaults: manager 0x14F16FC never appeared "
                     "after %d ms", waited);
            return 0;
        }
    }
    mgr_at = GetTickCount();
    log_line("paddefaults: manager %p up after %d ms (vt=%08X expect %08X)",
             mgr, waited, *(unsigned *)mgr,
             (unsigned)(uintptr_t)exe + (MGR_VT_VA - EXE_DEFAULT_BASE));
    /* §171: NO boot-time flag force. Forcing 0->1 at ~2.5 s (before the
     * sound/render init) froze the main menu on the splash frame while
     * the UI logic kept running (reproduced twice). The flag is left to
     * the engine at boot; the guarded monitor force below only restores
     * it after a save load parked it, and never before t=60 s. */
    /* arm once the boot/profile bind traffic has quiesced (profile
     * loads within ~2 s of manager creation); hard fallback 45 s */
    for (;;) {
        long now_calls = g_bind_calls;

        Sleep(250);
        if (now_calls == last_calls)
            quiet_ms += 250;
        else
            quiet_ms = 0;
        last_calls = now_calls;
        waited = (int)(GetTickCount() - mgr_at);
        if ((waited >= 8000 && quiet_ms >= 3000) || waited >= 45000)
            break;
    }
    exe = (unsigned char *)GetModuleHandleA(NULL);
    mgr = *(void * volatile *)(exe + (MGR_GLOBAL_VA - EXE_DEFAULT_BASE));
    if (!mgr) {
        log_line("paddefaults: manager gone before apply?!");
        return 0;
    }
    if (g_pump_hook_installed) {
        log_line("paddefaults: armed after %d ms (quiet %d ms), pump "
                 "will apply on the game thread", waited, quiet_ms);
        g_reset_burst = 0;
        g_want_pad_binds = 1;
        /* stay alive: if the user resets defaults in the options UI
         * (btnRevert wipes our tokens), re-apply on the next tick */
        for (;;) {
            static volatile long last_reset_seen;

            Sleep(1000);
            /* §165: controller-mode flag. The options screen shows the
             * gamepad/rumble settings only while this flag is 1; the
             * engine parks it at 0 during SAVE LOADS (forcing it there
             * black-screened the load, §163). Re-arm only when 60 s
             * have passed since the last 0x82CC10 (load or reset). */
            {
                unsigned char *pf = exe + (PADFLAG_VA - EXE_DEFAULT_BASE);
                DWORD now = GetTickCount();
                int load_recent = g_load_ms &&
                                  (long)(now - g_load_ms) < 120000;
                int stable = (DWORD)(now - mgr_at) > 60000 && !load_recent;
                /* §180: restore pad mode right after a load once the
                 * world is demonstrably up: MusicRegion was seen after
                 * the popup, its rate is the in-world 0x14FE one, and
                 * 8 s have passed since that event. */
                int after_load = g_load_start_ms && g_world_ready_ms &&
                                 (long)(g_world_ready_ms - g_load_start_ms) > 0 &&
                                 (long)(now - g_world_ready_ms) > 25000 &&
                                 (long)(now - g_world_ready_ms) < 120000;

                if (g_padforce_flag && !*pf && (stable || after_load)) {
                    *pf = 1;
                    log_line("paddefaults: controller-mode flag forced 0->1 "
                             "(%s)", after_load ? "after load/world" :
                                                 "stable window");
                }
            }
            /* §157: the direct counter (0x82CC10 hook) is the reliable
             * reset signal — the options reset clears the table without
             * the Bind(-1) burst the old heuristic watched for. By the
             * time this thread wakes, the engine's own defaults have
             * been laid down, so our binds land after them. */
            if (g_reset_seen != last_reset_seen) {
                last_reset_seen = g_reset_seen;
                g_reset_seen_ms = GetTickCount();
                if (g_pad_binds_done) {
                    /* NOTE: 0x82CC10 also runs during a SAVE LOAD, where
                     * the engine parks PADFLAG at 0 on purpose. Reviving
                     * the flag there black-screened the load (diag#12
                     * signature, confirmed in the log: openPopUp ->
                     * reset function ran -> flag revived). Never force
                     * the flag here; only re-apply the binds (the
                     * options reset leaves the flag at 1 by itself). */
                    if (g_load_ms &&
                        (long)(GetTickCount() - g_load_ms) < 120000) {
                        /* this clear came from a SAVE LOAD — keep our
                         * hands off the bind table until it settles */
                        log_line("paddefaults: reset during load — "
                                 "deferred");
                        g_defer_reapply = 1;
                        g_pad_binds_done = 0;
                        continue;
                    }
                    log_line("paddefaults: reset function ran "
                             "(seen=%ld) - re-applying", g_reset_seen);
                    g_pad_binds_done = 0;
                    g_want_pad_binds = 1;
                    continue;
                }
            }
            if (g_defer_reapply && g_reset_seen_ms &&
                (long)(GetTickCount() - g_reset_seen_ms) > 15000 &&
                (long)(GetTickCount() - g_load_ms) > 15000) {
                g_defer_reapply = 0;
                log_line("paddefaults: deferred re-apply armed");
                g_want_pad_binds = 1;
            }
            if (g_pad_binds_done && g_reset_burst >= 30) {
                log_line("paddefaults: defaults reset detected "
                         "(burst %ld) - re-applying", g_reset_burst);
                /* §163: never force PADFLAG here — the same clear runs
                 * during save loads where the engine parks it at 0. */
                g_reset_burst = 0;
                g_pad_binds_done = 0;
                g_want_pad_binds = 1;
            }
        }
    } else {
        log_line("paddefaults: no pump hook - applying from background "
                 "thread after %d ms", waited);
        g_pad_binds_done = 1;
        apply_own_pad_binds(exe, mgr);
    }
    return 0;
}


static void *g_pad_dev;            /* live engine PadDevice object */
static unsigned g_pad_dev_vt;
static volatile long g_pad_ctor_hits;

extern "C" void __cdecl pad_ctor_c(unsigned self, unsigned vt_probe)
{
    InterlockedIncrement(&g_pad_ctor_hits);
    if (!IsBadReadPtr((void *)self, 0x60)) {
        g_pad_dev = (void *)self;
        g_pad_dev_vt = *(unsigned *)self;
        log_line("padctor: dev=%p vt=%08X (hit %ld)", self, g_pad_dev_vt,
                 g_pad_ctor_hits);
    }
}

static int install_padctor_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (PADCTOR_VA - EXE_DEFAULT_BASE);
    unsigned char orig[PADCTOR_HEAD];
    unsigned char *stub, *c;
    DWORD old;
    long rel;

    memcpy(orig, target, sizeof(orig));
    /* expected: 8B C1 8A 4C 24 04  (mov eax,ecx; mov cl,[esp+4]) */
    if (!(orig[0] == 0x8B && orig[1] == 0xC1 && orig[2] == 0x8A &&
          orig[3] == 0x4C && orig[4] == 0x24 && orig[5] == 0x04)) {
        log_line("padctor hook: prologue mismatch at %p", target);
        return 0;
    }
    stub = (unsigned char *)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    *c++ = 0x9C; *c++ = 0x60;                     /* pushfd; pushad */
    *c++ = 0x8B; *c++ = 0xD9;                     /* mov ebx, ecx (this) */
    *c++ = 0x51;                                  /* push ecx (vt probe) */
    *c++ = 0x53;                                  /* push ebx */
    *c++ = 0xE8;                                  /* call pad_ctor_c */
    rel = (long)((uintptr_t)&pad_ctor_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;        /* add esp, 8 */
    *c++ = 0x61; *c++ = 0x9D;                     /* popad; popfd */
    memcpy(c, orig, PADCTOR_HEAD); c += PADCTOR_HEAD;
    *c++ = 0xE9;                                  /* jmp back */
    rel = (long)((uintptr_t)(target + PADCTOR_HEAD) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, PADCTOR_HEAD, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    VirtualProtect(target, PADCTOR_HEAD, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, PADCTOR_HEAD);
    log_line("padctor hook installed: target=%p stub=%p", target, stub);
    return 1;
}

/* self-poll disabled: calling GetDeviceState on game-owned DI8 objects
 * from another thread crashed (freed churn objects, non-threadsafe COM) */
static void pad_dev_diag(void)
{
}

static BOOL __stdcall hk_GetKeyboardState(unsigned char *kb)
{
    BOOL r = g_real_getkbstate(kb);
    int i;

    InterlockedIncrement(&g_getkb_calls);
    if (kb) {
        for (i = 0; i < (int)STICK_KEY_COUNT; i++) {
            unsigned char vk = i < (int)KEY_COUNT ? g_keys[i].vk
                                                  : g_stick_keys[i - KEY_COUNT].vk;

            if (g_key_state[i])
                kb[vk] |= 0x80;
        }
    }
    return r;
}

/* called from DllMain: the exe imports Div2Patch.dll statically, so we run
 * before any game code — safe to grab the input-related IAT slots */
static void install_dinput_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    void **slot;
    DWORD old;

    if (!exe)
        return;

    /* primary: PeekMessageA injection channel */
    slot = (void **)(exe + (PEEKMSG_SLOT_VA - EXE_DEFAULT_BASE));
    g_real_peekmsg = (PeekMessageA_fn)*slot;
    if (g_real_peekmsg &&
        VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = (void *)&hk_PeekMessageA;
        VirtualProtect(slot, sizeof(void *), old, &old);
        log_line("input: IAT PeekMessageA -> hook");
    }

    /* primary: cursor-anchoring detector (world vs menu) */
    slot = (void **)(exe + (SETCURSOR_SLOT_VA - EXE_DEFAULT_BASE));
    g_real_setcursor = (SetCursorPos_fn)*slot;
    if (g_real_setcursor &&
        VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = (void *)&hk_SetCursorPos;
        VirtualProtect(slot, sizeof(void *), old, &old);
        log_line("input: IAT SetCursorPos -> hook");
    }

    /* secondary: Win32 keyboard state overlay */
    slot = (void **)(exe + (GETKBSTATE_SLOT_VA - EXE_DEFAULT_BASE));
    g_real_getkbstate = (GetKeyboardState_fn)*slot;
    if (g_real_getkbstate &&
        VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = (void *)&hk_GetKeyboardState;
        VirtualProtect(slot, sizeof(void *), old, &old);
        log_line("input: IAT GetKeyboardState -> hook");
    }

    /* dynamic-import interception: the game may resolve key APIs at
     * runtime via GetProcAddress (not in the import table) — log every
     * user32 lookup and swap in our overlay versions */
    g_user32_mod = GetModuleHandleA("user32.dll");
    g_real_gaks = (GAKS_fn)GetProcAddress(g_user32_mod, "GetAsyncKeyState");
    g_real_gks = (GAKS_fn)GetProcAddress(g_user32_mod, "GetKeyState");
    slot = (void **)(exe + (GETPROCADDR_SLOT_VA - EXE_DEFAULT_BASE));
    g_real_gpa = (GPA_fn)*slot;
    if (g_real_gpa &&
        VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = (void *)&hk_GetProcAddress;
        VirtualProtect(slot, sizeof(void *), old, &old);
        log_line("input: IAT GetProcAddress -> hook (user32=%p)",
                 (void *)g_user32_mod);
    }

    /* secondary: keep the DirectInput chain for rumble/mouse future use */
    slot = (void **)(exe + (DICREATE_SLOT_VA - EXE_DEFAULT_BASE));
    g_real_dicreate = (DirectInput8Create_fn)*slot;
    if (!g_real_dicreate)
        return;
    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = (void *)&hk_DirectInput8Create;
        VirtualProtect(slot, sizeof(void *), old, &old);
        log_line("dinput: IAT DirectInput8Create -> hook");
    }
}

static int build_synth_shim(void)
{
    unsigned char *page;
    unsigned char *c;
    unsigned long addr;
    int i;

    if (!g_trampoline)
        return 0;
    page = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!page)
        return 0;
    c = page;
    *c++ = 0x8B; *c++ = 0x44; *c++ = 0x24; *c++ = 0x04;   /* mov eax,[esp+4] */
    for (i = 0; i < 5; i++) {                             /* push [esp+0x18] */
        *c++ = 0xFF; *c++ = 0x74; *c++ = 0x24; *c++ = 0x18;
    }
    *c++ = 0xFF; *c++ = 0x15;                              /* call [&g_trampoline] */
    addr = (unsigned long)&g_trampoline;
    memcpy(c, &addr, 4); c += 4;
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x14;                /* add esp,0x14 */
    *c++ = 0xC2; *c++ = 0x18; *c++ = 0x00;                /* ret 0x18 */
    g_synth_call = page;
    return 1;
}

extern "C" void __cdecl dispatch_log_c(unsigned self, unsigned mode,
                                       unsigned evt, const unsigned *args)
{
    unsigned d[6] = {0, 0, 0, 0, 0, 0};

    if (!g_disp_fp)
        return;
    if (evt && !IsBadReadPtr((void *)evt, sizeof(d)))
        memcpy(d, (void *)evt, sizeof(d));
    fprintf(g_disp_fp,
            "%lu DISP self=%08X m=%u e=%08X %08X %08X %08X %08X %08X %08X\n",
            GetTickCount(), self, mode, evt,
            d[0], d[1], d[2], d[3], d[4], d[5]);
}

extern "C" void __cdecl pump_tick_c(unsigned type, const unsigned *args);

static int install_pump_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (PUMP_VA - EXE_DEFAULT_BASE);
    unsigned char orig[PUMP_ORIG_BYTES_SIZE];
    unsigned char *stub;
    DWORD old;
    long rel;
    unsigned char *c;

    if (exe == NULL || (uintptr_t)exe < 0x10000)
        return 0;
    memcpy(orig, target, sizeof(orig));
    /* sanity: expected prologue 81 EC C4 01 00 00 56 */
    if (!(orig[0] == 0x81 && orig[1] == 0xEC && orig[2] == 0xC4 &&
          orig[3] == 0x01 && orig[6] == 0x56)) {
        log_line("pump hook: prologue mismatch at %p: %02X %02X %02X %02X %02X %02X %02X",
                 target, orig[0], orig[1], orig[2], orig[3], orig[4],
                 orig[5], orig[6]);
        return 0;
    }

    stub = (unsigned char *)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    /* pushfd; pushad */
    *c++ = 0x9C; *c++ = 0x60;
    /* mov ebx, eax */
    *c++ = 0x8B; *c++ = 0xD8;
    /* lea eax, [esp+0x28]  -> &args[0] (past ret + saved area) */
    *c++ = 0x8D; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28;
    /* push eax; push ebx */
    *c++ = 0x50; *c++ = 0x53;
    /* call rel32 pump_tick_c */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&pump_tick_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    /* add esp, 8; popad; popfd */
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x08;
    *c++ = 0x61; *c++ = 0x9D;
    /* original bytes: sub esp,0x1c4; push esi  — this is the trampoline */
    g_trampoline = c;
    memcpy(c, orig, PUMP_ORIG_BYTES_SIZE); c += PUMP_ORIG_BYTES_SIZE;
    /* jmp rel32 back to target+7 */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + PUMP_ORIG_BYTES_SIZE) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, PUMP_ORIG_BYTES_SIZE, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    /* jmp rel32 stub + 2x nop (7 total) */
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90; target[6] = 0x90;
    VirtualProtect(target, PUMP_ORIG_BYTES_SIZE, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, PUMP_ORIG_BYTES_SIZE);

    log_line("pump hook installed: exe=%p target=%p stub=%p",
             exe, target, stub);
    g_pump_hook_installed = 1;
    return 1;
}

static int install_dispatch_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *target = exe + (DISPATCH_VA - EXE_DEFAULT_BASE);
    unsigned char orig[DISPATCH_ORIG_BYTES_SIZE];
    unsigned char *stub;
    DWORD old;
    long rel;
    unsigned char *c;

    memcpy(orig, target, sizeof(orig));
    /* expected: push ebx; mov ebx,[esp+8]  =  53 8B 5C 24 08 */
    if (!(orig[0] == 0x53 && orig[1] == 0x8B && orig[2] == 0x5C &&
          orig[3] == 0x24 && orig[4] == 0x08)) {
        log_line("dispatch hook: prologue mismatch at %p: %02X %02X %02X %02X %02X",
                 target, orig[0], orig[1], orig[2], orig[3], orig[4]);
        return 0;
    }

    stub = (unsigned char *)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    c = stub;
    /* pushfd; pushad */
    *c++ = 0x9C; *c++ = 0x60;
    /* mov ebx, ecx  (this) */
    *c++ = 0x8B; *c++ = 0xD9;
    /* lea eax, [esp+0x28]  -> &args (mode, evt) */
    *c++ = 0x8D; *c++ = 0x44; *c++ = 0x24; *c++ = 0x28;
    /* mov ecx, [esp+0x2C]  -> evt */
    *c++ = 0x8B; *c++ = 0x4C; *c++ = 0x24; *c++ = 0x2C;
    /* push eax(args); push ecx(evt); push [esp+0x30](mode); push ebx(self) */
    *c++ = 0x50;
    *c++ = 0x51;
    *c++ = 0xFF; *c++ = 0x74; *c++ = 0x24; *c++ = 0x30;
    *c++ = 0x53;
    /* call rel32 dispatch_log_c */
    *c++ = 0xE8;
    rel = (long)((uintptr_t)&dispatch_log_c - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;
    /* add esp,0x10; popad; popfd */
    *c++ = 0x83; *c++ = 0xC4; *c++ = 0x10;
    *c++ = 0x61; *c++ = 0x9D;
    /* original bytes: push ebx; mov ebx,[esp+8] */
    memcpy(c, orig, DISPATCH_ORIG_BYTES_SIZE); c += DISPATCH_ORIG_BYTES_SIZE;
    /* jmp rel32 back */
    *c++ = 0xE9;
    rel = (long)((uintptr_t)(target + DISPATCH_ORIG_BYTES_SIZE) - (uintptr_t)(c + 4));
    memcpy(c, &rel, 4); c += 4;

    if (!VirtualProtect(target, DISPATCH_ORIG_BYTES_SIZE,
                        PAGE_EXECUTE_READWRITE, &old))
        return 0;
    target[0] = 0xE9;
    rel = (long)((uintptr_t)stub - (uintptr_t)(target + 5));
    memcpy(target + 1, &rel, 4);
    VirtualProtect(target, DISPATCH_ORIG_BYTES_SIZE, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, DISPATCH_ORIG_BYTES_SIZE);

    log_line("dispatch hook installed: target=%p stub=%p", target, stub);
    return 1;
}

/* diag#83: log_line is called from 8+ threads (watchdog, state,
 * guard, camera, seq runner, command queue); it was lockless
 * fopen-per-call - interleaved halflines were cosmetic so far, but
 * the command console relies on ordered request/response records. */
static CRITICAL_SECTION g_log_cs;
static volatile long g_log_cs_ready;

static void log_line(const char *fmt, ...)
{
    FILE *f;
    va_list ap;

    if (!g_log_path[0])
        return;
    if (!g_log_cs_ready) {
        /* first call is on DllMain's thread (single-threaded there);
         * the interlocked flag makes later racers wait for init */
        static volatile long initializing;

        if (InterlockedExchange(&initializing, 1)) {
            while (!g_log_cs_ready)
                Sleep(0);
        } else {
            InitializeCriticalSection(&g_log_cs);
            g_log_cs_ready = 1;
        }
    }
    EnterCriticalSection(&g_log_cs);
    f = fopen(g_log_path, "a");
    if (f) {
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void build_paths(void)
{
    char *slash;

    if (!g_self)
        return;
    if (!GetModuleFileNameA(g_self, g_dll_dir, MAX_PATH))
        return;
    slash = strrchr(g_dll_dir, '\\');
    if (slash)
        *slash = 0;
    snprintf(g_log_path, MAX_PATH, "%s\\div2gamepad.log", g_dll_dir);
}

static void read_layout(const char *ini);
static void log_layout(void);

static void read_config(void)
{
    char ini[MAX_PATH];

    snprintf(ini, MAX_PATH, "%s\\%s", g_dll_dir, GAMEPAD_INI);
    g_enable_gamepad = GetPrivateProfileIntA("Gamepad", "Enable", 1, ini);
    g_no_threads = GetPrivateProfileIntA("Gamepad", "NoThreads", 0, ini);
    g_hook_pump = GetPrivateProfileIntA("Gamepad", "HookPump", 0, ini);
    g_hook_dispatch = GetPrivateProfileIntA("Gamepad", "HookDispatch", 0, ini);
    g_synth = GetPrivateProfileIntA("Gamepad", "Synth", 1, ini);
    g_rt_native = GetPrivateProfileIntA("Gamepad", "RTNative", 0, ini);
    g_rumble = GetPrivateProfileIntA("Gamepad", "Rumble", 1, ini);
    g_vibration = GetPrivateProfileIntA("Gamepad", "Vibration", 1, ini);
    g_vib_strength = GetPrivateProfileIntA("Gamepad", "VibStrength", 60, ini);
    g_deadzone = GetPrivateProfileIntA("Gamepad", "Deadzone", 9000, ini);
    g_user_index = GetPrivateProfileIntA("Gamepad", "UserIndex", 0, ini);
    g_diag = GetPrivateProfileIntA("Gamepad", "Diag", 1, ini);
    g_trace = GetPrivateProfileIntA("Gamepad", "Trace", 0, ini);
    g_paddefaults = GetPrivateProfileIntA("Gamepad", "PadDefaults", 1, ini);
    g_paddefaults_engine = GetPrivateProfileIntA("Gamepad", "PadDefaultsEngine",
                                                 0, ini);
    g_paddefaults_clear = GetPrivateProfileIntA("Gamepad", "PadDefaultsClear",
                                                1, ini);
    g_paddefaults_delay = GetPrivateProfileIntA("Gamepad", "PadDefaultsDelayMs",
                                                15000, ini);
    g_log_binds = GetPrivateProfileIntA("Gamepad", "LogBinds", 1, ini);
    g_padforce_flag = GetPrivateProfileIntA("Gamepad", "PadForceFlag", 0, ini);
    g_pad_loadfix = GetPrivateProfileIntA("Gamepad", "PadLoadFix", 1, ini);
    g_pad_keep_load = GetPrivateProfileIntA("Gamepad", "PadKeepOnLoad", 0, ini);
    g_pad_boot_mode = GetPrivateProfileIntA("Gamepad", "PadBootMode", 0, ini);
    g_xenon_bundle = GetPrivateProfileIntA("Gamepad", "XenonBundle", 0, ini);
    g_vib_on_damage = GetPrivateProfileIntA("Gamepad", "VibOnDamage", 0, ini);
    g_vib_damage_str = GetPrivateProfileIntA("Gamepad", "VibDamageStrength",
                                             0, ini);
    g_vib_load = GetPrivateProfileIntA("Gamepad", "VibLoad", 1, ini);
    g_vib_load_start = GetPrivateProfileIntA("Gamepad", "VibLoadStart", 0,
                                             ini);

    g_xenon_ui = GetPrivateProfileIntA("Gamepad", "XenonUi", 0, ini);
    g_native_move = GetPrivateProfileIntA("Gamepad", "NativeMove", 1, ini);
    g_padmode_force = GetPrivateProfileIntA("Gamepad", "PadMode", 1, ini);
    g_force_menu = GetPrivateProfileIntA("Gamepad", "ForceMenu", 1, ini);
    g_test_sticky = GetPrivateProfileIntA("Gamepad", "TestSticky", 0, ini);
    g_dbgcmd = GetPrivateProfileIntA("Gamepad", "DbgCmd", 0, ini);
    g_calib_fix = GetPrivateProfileIntA("Gamepad", "CalibFix", 1, ini);
    g_run_collectors = GetPrivateProfileIntA("Gamepad", "RunCollectors", 0,
                                             ini);
    g_run_camc = GetPrivateProfileIntA("Gamepad", "RunCamC", 0, ini);
    g_native_stick = GetPrivateProfileIntA("Gamepad", "NativeStick", 0, ini);
    g_stick_virt = GetPrivateProfileIntA("Gamepad", "StickVirt", 1, ini);
    g_stick_dead = GetPrivateProfileIntA("Gamepad", "StickDead", 35, ini);
    g_stick_src = GetPrivateProfileIntA("Gamepad", "StickSrc", 0, ini);
    g_sticky_fix = GetPrivateProfileIntA("Gamepad", "StickyFix", 1, ini);
    g_river_fix = GetPrivateProfileIntA("Gamepad", "RiverFix", 1, ini);
    {
        char mv[40];
        const char *s;
        int k, vals[4] = { 0, 1, 2, 3 };

        /* MoveVirt=U,D,L,R - the four CANONICAL X360 button ids for
         * the movement directions (default 10,11,8,9 = D-pad
         * U/D/L/R). Parsed by hand: sscanf would statically link
         * mingw's __mingw_sformat (+28 KB of .text). */
        GetPrivateProfileStringA("Gamepad", "MoveVirt", "10,11,8,9", mv,
                                 sizeof(mv), ini);
        s = mv;
        for (k = 0; k < 4; k++) {
            int base = 10, v = 0;

            while (*s == ' ' || *s == '\t')
                s++;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                base = 16;
                s += 2;
            }
            while (*s) {
                int d = -1;

                if (*s >= '0' && *s <= '9')
                    d = *s - '0';
                else if (base == 16 && *s >= 'a' && *s <= 'f')
                    d = *s - 'a' + 10;
                else if (base == 16 && *s >= 'A' && *s <= 'F')
                    d = *s - 'A' + 10;
                if (d < 0)
                    break;
                v = v * base + d;
                s++;
            }
            vals[k] = v;
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s == ',' || *s == ';')
                s++;
        }
        for (k = 0; k < 4; k++)
            g_move_virt[k] = vals[k];
    }
    g_native_gate = GetPrivateProfileIntA("Gamepad", "NativeGate", 0, ini);
    g_tick_drive = GetPrivateProfileIntA("Gamepad", "NativeTickDrive", 0,
                                         ini);
    g_move_tick = GetPrivateProfileIntA("Gamepad", "MoveTick", 0, ini);
    g_axis_inject = GetPrivateProfileIntA("Gamepad", "AxisInject", 0, ini);
    g_axis_dead = GetPrivateProfileIntA("Gamepad", "AxisDead", 10, ini);
    g_axis_flip = GetPrivateProfileIntA("Gamepad", "AxisFlip", 0, ini);
    g_move_cam = GetPrivateProfileIntA("Gamepad", "MoveCam", 0, ini);
    g_mt_probe = GetPrivateProfileIntA("Gamepad", "MTProbe", 0, ini);
    g_guard_auto = GetPrivateProfileIntA("Gamepad", "GuardAuto", 0, ini);
    log_line("cfg: synth=%d paddef=%d padmode=%d calib=%d "
             "runcol=%d runcamc=%d nstick=%d svirt=%d ngate=%d "
             "ntickdrv=%d mtick=%d ainj=%d adead=%d aflip=%d mcam=%d "
             "mtp=%d ssrc=%d gauto=%d", g_synth,
             g_paddefaults, g_padmode_force, g_calib_fix,
             g_run_collectors, g_run_camc, g_native_stick, g_stick_virt,
             g_native_gate, g_tick_drive, g_move_tick, g_axis_inject,
             g_axis_dead, g_axis_flip, g_move_cam, g_mt_probe, g_stick_src,
             g_guard_auto);
    log_line("cfg2: mvirt=%d,%d,%d,%d", g_move_virt[0], g_move_virt[1],
             g_move_virt[2], g_move_virt[3]);
    g_delivery = GetPrivateProfileIntA("Gamepad", "Delivery", 1, ini);
    g_rstick_cam = GetPrivateProfileIntA("Gamepad", "RStickCam", 1, ini);
    g_cam_speed = GetPrivateProfileIntA("Gamepad", "CamSpeed", 600, ini);
    g_cam_world_speed = GetPrivateProfileIntA("Gamepad", "CamWorldSpeed",
                                              800, ini);
    g_cam_invert_y = GetPrivateProfileIntA("Gamepad", "CamInvertY", 0, ini);
    g_zero_pad_axes = GetPrivateProfileIntA("Gamepad", "ZeroPadAxes", 1, ini);
    g_dump_input = GetPrivateProfileIntA("Gamepad", "DumpInput", 0, ini);
    g_cam_probe = GetPrivateProfileIntA("Gamepad", "CamProbe", 0, ini);
    g_scheme_off = GetPrivateProfileIntA("Gamepad", "PadSchemeOff", 0, ini);
    g_cam_hold = GetPrivateProfileIntA("Gamepad", "CamHold", 1, ini);
    g_trig_thresh = GetPrivateProfileIntA("Gamepad", "TriggerThreshold",
                                          60, ini);
    read_layout(ini);
    {
        unsigned i;

        g_r3_idx = -1;
        for (i = 0; i < KEY_COUNT; i++)
            if (g_keys[i].btn == XI_RTHUMB) {
                g_r3_idx = (int)i;
                break;
            }
    }
    {
        int alt = GetPrivateProfileIntA("Layout", "R3Double", 0, ini);

        g_r3_alt_vk = (unsigned char)(alt ? alt : 0);
        g_r3_alt_dik = (unsigned char)(alt ? MapVirtualKeyA((UINT)alt, 0) : 0);
    }
    log_layout();
}

/* [Layout] per-button overrides: value = VK code (dec or 0x-hex),
 * or LMB / RMB, or NONE (disable). Names match g_keys rows. */
static void read_layout(const char *ini)
{
    static const struct { const char *name; int idx; } map[] = {
        { "A", 0 },  { "B", 1 },   { "Start", 2 }, { "Back", 3 },
        { "X", 4 },  { "Y", 5 },   { "LB", 6 },    { "RB", 7 },
        { "R3", 8 }, { "DpadUp", 9 }, { "DpadDown", 10 },
        { "DpadLeft", 11 }, { "DpadRight", 12 }, { "RT", 13 }, { "LT", 14 },
    };
    char buf[32];
    unsigned i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        struct synth_key *k = &g_keys[map[i].idx];

        buf[0] = 0;
        GetPrivateProfileStringA("Layout", map[i].name, "", buf,
                                 sizeof(buf), ini);
        if (!buf[0])
            continue;
        if (!_stricmp(buf, "NONE")) {
            k->flags = PADF_OFF;
            continue;
        }
        if (!_stricmp(buf, "LMB") || !_stricmp(buf, "RMB")) {
            k->flags = !_stricmp(buf, "LMB") ? PADF_MOUSE_L : PADF_MOUSE_R;
            k->vk = 0;
            k->dik = 0;
            continue;
        }
        {
            char *end = NULL;
            unsigned long vk = strtoul(buf, &end, 0);

            if (end && end != buf && vk && vk <= 0xFF) {
                k->vk = (unsigned char)vk;
                k->dik = (unsigned char)MapVirtualKeyA((UINT)vk, 0);
                k->flags = 0;
            } else {
                log_line("layout: bad value %s=%s (ignored)", map[i].name, buf);
            }
        }
    }
}

static void log_layout(void)
{
    static const char *names[] = { "A", "B", "Start", "Back", "X", "Y",
                                   "LB", "RB", "R3", "DpadU", "DpadD",
                                   "DpadL", "DpadR", "RT", "LT" };
    char line[512];
    int n = 0, i;

    for (i = 0; i < (int)KEY_COUNT; i++) {
        const struct synth_key *k = &g_keys[i];

        if (k->flags & PADF_OFF)
            n += snprintf(line + n, sizeof(line) - n, "%s=off ", names[i]);
        else if (k->flags & PADF_MOUSE_L)
            n += snprintf(line + n, sizeof(line) - n, "%s=LMB ", names[i]);
        else if (k->flags & PADF_MOUSE_R)
            n += snprintf(line + n, sizeof(line) - n, "%s=RMB ", names[i]);
        else if (k->m_vk && k->m_vk != k->vk)
            n += snprintf(line + n, sizeof(line) - n, "%s=VK%02X/VK%02X ",
                          names[i], k->vk, k->m_vk);
        else
            n += snprintf(line + n, sizeof(line) - n, "%s=VK%02X ",
                          names[i], k->vk);
    }
    log_line("layout: %s(trig_thr=%ld r3dbl=%sVK%02X)", line, g_trig_thresh,
             g_r3_alt_vk ? "" : "off/", g_r3_alt_vk);
}

/* smooth camera: dedicated ~125 Hz thread. The engine tick is only
 * ~9 Hz in-world (0x14FE), which made the camera chunky (one ~65px
 * jump per 110ms). This thread touches OS APIs only (XInputGetState
 * + mouse_event) — no game objects, so it is thread-safe. */
/* ---- diag#71 feeder REMOVED (§73): it missed short presses entirely
 * (240 ms idle gate) so Start still did not close the menu, and a
 * second writer of the pad state is a race we do not need. The menu
 * needs a different route anyway: the game's UI polls only
 * Start/Back/LB/RB per frame, the D-pad is barely polled. */

/* diag#73b: 1 Hz full-state line. The 5 s watchdog missed the sticky
 * episodes entirely (both sessions had clean dumps). Once per second
 * we log: the pad's virtual array (first byte of the 16 used slots),
 * the last POV angle, the engine's analog left-stick pair and the
 * LEVEL byte (+0x4C) of action entries 0..3 at [0x14F16FC] — the
 * byte whose value 1 means "the action is latched down". If the hero
 * runs while virt=0000000000000000 and the LEVEL of a movement act
 * stays 01, that is the smoking gun: the engine's action state is
 * stuck, not our array. */
static DWORD WINAPI state_thread(void *param)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);

    (void)param;
    for (;;) {
        char sb[60];
        int sl = 0, a2;
        unsigned char *pp = (unsigned char *)(uintptr_t)g_pad_obj;
        unsigned va = pp ? *(unsigned *)(pp + 0x30) : 0;
        unsigned mgr;

        Sleep(1000);
        /* §204: the whole thread is periodic diagnostics (STATE/AV/
         * MVDUMP + d95) — behind Diag so the release log stays small.
         * log_line opens/CLOSES the file per line, so this spam was
         * also a per-second disk hit. */
        if (!g_diag)
            continue;
        d95_poll();
        if (va > 0x10000 && !IsBadReadPtr((void *)va, 0x140))
            for (a2 = 0; a2 < 16; a2++)
                sl += snprintf(sb + sl, sizeof(sb) - sl, "%X",
                               *(unsigned char *)(va + a2 * 0x10));
        sb[sl] = 0;
        mgr = *(unsigned *)(exe + (0x014F16FCu - EXE_DEFAULT_BASE));
        {
            /* diag#74/76: raw left-stick pair, the quartet's dynamic
             * entry bytes ([x28][x29][x3C][x3D][x4C][x4D] per act)
             * and the avatar's movement delta in one line. d = sum
             * |float diff| over the snap block: 0 = standing, big =
             * walking. If the hero walks with a clean array AND a
             * clean quartet, the speed is latched in the locomotion;
             * a stuck byte inside an entry names the exact latch. */
            XINPUT_STATE_T xst;
            long xi = g_xinput_get ? (long)g_xinput_get(g_user_index, &xst)
                                   : -1;
            char eb[240];
            int el = 0;
            unsigned ent = 0;
            static float ppos[8];
            static int have_pos;
            int walk = -1;

            eb[0] = 0;
            if (mgr > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)mgr, 0x100)) {
                unsigned arr = *(unsigned *)(mgr + 0x74);

                if (arr > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)arr, 4 * 0x54)) {
                    for (a2 = 0; a2 < 4; a2++) {
                        unsigned e2 = arr + (unsigned)a2 * 0x54;

                        el += snprintf(eb + el, sizeof(eb) - el,
                                       "%02X%02X%02X%02X%02X%02X ",
                                       *(unsigned char *)(e2 + 0x28),
                                       *(unsigned char *)(e2 + 0x29),
                                       *(unsigned char *)(e2 + 0x3C),
                                       *(unsigned char *)(e2 + 0x3D),
                                       *(unsigned char *)(e2 + 0x4C),
                                       *(unsigned char *)(e2 + 0x4D));
                    }
                    eb[el ? el - 1 : 0] = 0;
                }
            }
            {
                /* diag#78: the avatar = the resolver site with the most
                 * calls (the camera's, per A48S). The collector chain
                 * resolved a different entity — its node never moved
                 * (pos=0 while walking), so it is dropped. */
                struct res_site_s *best = NULL;
                int si;

                for (si = 0; si < RES_MAXSITE; si++) {
                    struct res_site_s *sp = &g_res_sites[si];

                    if (sp->site && sp->calls &&
                        (!best || sp->calls > best->calls))
                        best = sp;
                }
                if (best && best->ent) {
                    float cur[8];

                    if (res_snap(best->ent, cur)) {
                        double d = 0.0;
                        int i2;

                        ent = best->ent;
                        for (i2 = 0; i2 < 8; i2++) {
                            double dd = (double)cur[i2] -
                                (double)ppos[i2];

                            d += dd < 0 ? -dd : dd;
                        }
                        if (have_pos)
                            walk = (int)(d * 1000.0);
                        memcpy(ppos, cur, sizeof(cur));
                        have_pos = 1;
                    }
                }
            }
            {
                /* diag#78: the move controller's STORED direction —
                 * nonzero while every input is clean = the latch */
                unsigned mctrl = g_vc_ctrl;
                float mv0 = 0.0f, mv1 = 0.0f, mv2 = 0.0f;

                if (mctrl > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)mctrl, 0x44)) {
                    mv0 = *(float *)(uintptr_t)(mctrl + 0x38);
                    mv1 = *(float *)(uintptr_t)(mctrl + 0x3C);
                    mv2 = *(float *)(uintptr_t)(mctrl + 0x40);
                }
                if (mgr > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)mgr, 0x100)) {
                    if (xi == 0)
                        log_line("STATE: virt=%s pov=%X act=%s xi=%d,%d "
                                 "r%02X b%04X pf=%u pos=%d e=%08X "
                                 "mv=%08X:%.3f,%.3f,%.3f",
                                 sb, g_last_pov, eb,
                                 (int)(xst.Gamepad.sThumbLX / 327),
                                 (int)(xst.Gamepad.sThumbLY / 327),
                                 xst.Gamepad.bRightTrigger,
                                 xst.Gamepad.wButtons & 0xFFFF,
                                 (unsigned)*(unsigned char *)(exe +
                                     (PADFLAG_VA - EXE_DEFAULT_BASE)),
                                 walk, ent, mctrl, mv0, mv1, mv2);
                    else
                        log_line("STATE: virt=%s pov=%X act=%s xi=off "
                                 "pf=%u pos=%d e=%08X mv=%08X:%.3f,%.3f,%.3f",
                                 sb, g_last_pov, eb,
                                 (unsigned)*(unsigned char *)(exe +
                                     (PADFLAG_VA - EXE_DEFAULT_BASE)),
                                 walk, ent, mctrl,
                                 mv0, mv1, mv2);
                } else {
                    log_line("STATE: virt=%s pov=%X acts=none", sb,
                             g_last_pov);
                }
            }
            /* diag#79: the move controller's object, 20 dwords at 1 Hz —
             * settles what +0x38..+0x4C are and which fields change
             * with the walk */
            {
                unsigned mc = g_vc_ctrl;
                char db[260];
                int dl = 0, i3;

                if (mc > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)mc, 0x50)) {
                    for (i3 = 0; i3 < 0x50; i3 += 4)
                        dl += snprintf(db + dl, sizeof(db) - dl, "%08X ",
                                       *(unsigned *)(uintptr_t)(mc + i3));
                    db[dl ? dl - 1 : 0] = 0;
                    log_line("MVDUMP: %s", db);
                }
            }
            /* diag#85: the analog channel array [pad+0x38] (32-byte
             * entries). The pump's second loop commits field0->field8
             * and field4->fieldC every frame; gav resolves act values
             * through these channels (axq 0x733B90). A nonzero
             * now-pair with no further DI8 events = the sticky
             * direction candidate. */
            {
                unsigned char *pp = (unsigned char *)(uintptr_t)g_pad_obj;
                unsigned cnt2, arr2;

                if (pp && !IsBadReadPtr(pp, 0x40)) {
                    cnt2 = *(unsigned *)(pp + 0x34);
                    arr2 = *(unsigned *)(pp + 0x38);
                    if (cnt2 > 8)
                        cnt2 = 8;
                    if (arr2 > 0x10000 &&
                        !IsBadReadPtr((void *)(uintptr_t)arr2, cnt2 * 0x20)) {
                        char ab2[260];
                        int al = 0, ai;

                        for (ai = 0; ai < (int)cnt2; ai++) {
                            float *e = (float *)(uintptr_t)(arr2 + ai * 0x20);

                            al += snprintf(ab2 + al, sizeof(ab2) - al,
                                           "%d:%.1f/%.1f p%.1f/%.1f ",
                                           ai, e[0], e[1], e[2], e[3]);
                            if (al > (int)sizeof(ab2) - 60)
                                break;
                        }
                        log_line("AV: %s", ab2);
                    }
                }
            }
        }
    }
    return 0;
}

static DWORD WINAPI camera_thread(void *param)
{
    double frac_x = 0.0, frac_y = 0.0;
    DWORD last_ms = 0;

    (void)param;
    timeBeginPeriod(1);
    for (;;) {
        XINPUT_STATE_T st;
        DWORD now_ms, dt, fg_pid = 0;
        HWND fg;
        unsigned long idx;
        long rx, ry;
        double dx = 0.0, dy = 0.0;

        Sleep(8);
        if (g_vibration)
            vib_read_setting();          /* §164: slider from file */
        if (g_vib_until_ms &&
            (long)(GetTickCount() - g_vib_until_ms) >= 0) {
            unsigned short z[2] = { 0, 0 };

            g_vib_until_ms = 0;
            if (g_xinput_set)
                g_xinput_set(g_user_index, z);
        }
        cam_probe_tick();   /* internally gated by CamProbe */
        pad_scheme_tick();  /* internally gated by PadSchemeOff */
        camhold_tick();     /* internally gated by CamHold */
        if (!g_xinput_get)
            continue;
        if (!g_xinput_seen_ok) {
            /* diag#70: detect here too - the pump callback may be
             * slow or absent, and the camera/menu detector must not
             * depend on the synthesis shim */
            XINPUT_STATE_T probe;

            xinput_detect(&probe);
        }
        if (!g_xinput_seen_ok)
            continue;
        fg = GetForegroundWindow();
        if (fg)
            GetWindowThreadProcessId(fg, &fg_pid);
        if (!fg || fg_pid != GetCurrentProcessId())
            continue;
        idx = g_user_index;
        if (g_xinput_get(idx, &st) != 0)
            continue;
        /* §149: RT->LMB / LT->RMB clicks live HERE, not in the engine
         * pump tick (~9 Hz is too slow and menus need them most).
         * Focus was verified above — we never forge clicks into
         * another app's window. */
        mousebtn_poll(&st);
        /* §154/§155: dpad Quickmap rows + Start (ESC) */
        fastrow_poll(&st);
        if (!g_rstick_cam)
            continue;
        now_ms = GetTickCount();
        dt = last_ms ? now_ms - last_ms : 0;
        last_ms = now_ms;
        if (!dt || dt >= 500)
            continue;
        rx = st.Gamepad.sThumbRX;
        ry = st.Gamepad.sThumbRY;
        {
            /* mode detector v2: engine re-anchors the cursor via
             * SetCursorPos in the world; in menus it stays quiet.
             * No signal -> menu speed (safe fallback). The old
             * 0x14FE-rate idea was wrong: it is ~9/s everywhere. */
            static int last_mode = -1;
            static long prev_setcur;
            static DWORD rate_win;
            static int menu_mode = 1;
            /* bind-layout mode: needs 2 agreeing windows before switching
             * (the raw detector flaps in some menus; dpad target depends
             * on this, camera speed uses the raw mode as before) */
            static int bind_mode = 1, bind_pend = -1;
            long speed, dz = g_deadzone;
            double range = (double)(32768 - dz);

            if (!rate_win)
                rate_win = now_ms;
            else if (now_ms - rate_win >= 400) {
                long rate = (g_setcur_calls - prev_setcur) * 1000 /
                            (long)(now_ms - rate_win);

                menu_mode = rate < 10;
                prev_setcur = g_setcur_calls;
                rate_win = now_ms;
                if (menu_mode != bind_mode) {
                    if (bind_pend == menu_mode) {
                        bind_mode = menu_mode;
                        g_in_menu = menu_mode;
                        if (g_diag)
                            log_line("bind: mode=%s (confirmed)",
                                     menu_mode ? "menu" : "world");
                    }
                    bind_pend = menu_mode;
                } else {
                    bind_pend = -1;
                }
            }
            speed = menu_mode ? g_cam_speed : g_cam_world_speed;
            if (menu_mode != last_mode) {
                last_mode = menu_mode;
                if (g_diag)
                    log_line("cam: mode=%s setcur_rate_thr",
                             menu_mode ? "menu" : "world");
            }
            if (abs(rx) > dz)
                dx = (double)(rx - (rx > 0 ? dz : -dz)) / range *
                     (double)speed * (double)dt / 1000.0;
            if (abs(ry) > dz)
                dy = (double)(ry - (ry > 0 ? dz : -dz)) / range *
                     (double)speed * (double)dt / 1000.0;
        }
        dy = -dy; /* stick up -> mouse up -> look up */
        if (g_cam_invert_y)
            dy = -dy;
        frac_x += dx;
        frac_y += dy;
        {
            long ix = (long)frac_x;
            long iy = (long)frac_y;

            if (ix || iy) {
                mouse_event(MOUSEEVENTF_MOVE, (DWORD)ix, (DWORD)iy, 0, 0);
                frac_x -= ix;
                frac_y -= iy;
                InterlockedIncrement(&g_cam_moves);
                if (g_cam_moves == 1)
                    log_line("cam: first move dx=%ld dy=%ld", ix, iy);
            }
        }
    }
}

/* independent 5-second heartbeat: proves whether our cadence sources
 * (pump 0x14FE ticks / pad GetDeviceState polls) keep running in-world */
/* diag#80: self-test — reproduce the sticky trigger WITHOUT a pad:
 * the exact write pattern of stick_to_virt (the engine's own
 * setters), from a dedicated thread. Waits for the in-world state
 * (the 0x14FE pump tick rate), holds the up+left diagonal (the
 * pattern the user reports as always-sticky), releases it and lets
 * the 1 Hz STATE/MVDUMP instruments capture the aftermath. */
/* diag#81c: a wireless pad left idle radio-sleeps; XInput still
 * reports the donglе connected but the engine's DI8 boot
 * enumeration finds nothing -> no PadDevice, no pad object, the
 * whole input pump stays dormant (14FE+0, no evh). A short rumble
 * every 25 s wakes it and keeps it enumerable. Only armed for
 * TestSticky sessions so it never buzzes during normal play. */
/* ---------------- diag#83: file-queue debug console ----------------
 * The dbgcmd\ directory next to the exe is a transport-agnostic
 * command channel: a local folder on this PC, an SMB share to the
 * future test rig, a hypervisor shared folder, even a USB stick.
 * The operator (or the automation agent) drops NNN.cmd files
 * (written as NNN.tmp then RENAMED - atomic publish); this thread
 * executes them in enumeration order, replies into NNN.rsp plus
 * the main log, then deletes the .cmd. Replaces the TestSticky
 * thread: the sticky scenario is now a seq file driven at will,
 * without rebuilds or restarts. */
typedef void (__thiscall *virtset_fn)(void *, int, int);
typedef void (__thiscall *virtclr_fn)(void *, int);

static volatile long g_seq_busy;

static int parse_slots(char *s, unsigned char *out, int max)
{
    int n = 0;
    char *tok;

    for (tok = strtok(s, ","); tok && n < max; tok = strtok(NULL, ",")) {
        char *end;
        unsigned long v = strtoul(tok, &end, 16);

        if (end == tok || v > 0xFF)
            return n;
        out[n++] = (unsigned char)v;
    }
    return n;
}

/* the exact write pattern stick_to_virt uses: the engine's own
 * setter + the changed-hint bit at pad+0x158 */
static void cmd_virt_write(unsigned char *p, int idx, int on)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);

    if (on)
        ((virtset_fn)(exe + (0x00D12430u - EXE_DEFAULT_BASE)))(p, idx, 0xFF);
    else
        ((virtclr_fn)(exe + (0x00D124B0u - EXE_DEFAULT_BASE)))(p, idx);
    *(unsigned *)(p + 0x158) |= 1u << (idx & 31);
}

static int pad_for_writes(unsigned char **p_out)
{
    unsigned char *p = (unsigned char *)(uintptr_t)g_pad_obj;
    unsigned va;

    if (!p || IsBadReadPtr(p, 0x160))
        return 0;
    va = *(unsigned *)(p + 0x30);
    if (va < 0x10000 || IsBadReadPtr((void *)(uintptr_t)va, 0x140))
        return 0;
    *p_out = p;
    return 1;
}

/* one console command; reply lines go to the .rsp stream (caller
 * adds the newline) and everything notable also hits the main log.
 * Runs on the console thread: pulse/seq block the queue for their
 * duration - documented and fine for a debug tool. */
static int cmd_exec(FILE *rsp, char *line)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
    char verb[16], *rest, *e;
    int i;

    for (i = 0; line[i] && line[i] != ' ' && line[i] != '\t' && i < 15; i++)
        verb[i] = line[i];
    verb[i] = 0;
    rest = line + i;
    while (*rest == ' ' || *rest == '\t')
        rest++;
    e = rest + strlen(rest);
    while (e > rest && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;

    if (!verb[0]) {
        fprintf(rsp, "ERR empty line");
        return 1;
    }
    if (!_stricmp(verb, "ping")) {
        fprintf(rsp, "pong pid=%lu tick=%lu", GetCurrentProcessId(),
                GetTickCount());
        return 0;
    }
    if (!_stricmp(verb, "state")) {
        unsigned char *p = (unsigned char *)(uintptr_t)g_pad_obj;
        unsigned mgr = *(unsigned *)(exe +
                            (0x014F16FCu - EXE_DEFAULT_BASE));

        fprintf(rsp, "pad=%08X", (unsigned)(uintptr_t)p);
        if (p && !IsBadReadPtr(p, 0x160)) {
            unsigned va = *(unsigned *)(p + 0x30);

            if (va > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)va, 0x140)) {
                char sb[40];

                sb[0] = 0;
                for (i = 0; i < 16; i++)
                    snprintf(sb + i * 2, 3, "%02X",
                             *(unsigned char *)(va + i * 0x10));
                fprintf(rsp, " slots=%s", sb);
            }
        } else {
            fprintf(rsp, " slots=? (pad asleep)");
        }
        if (mgr > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)mgr, 0x100)) {
            unsigned arr = *(unsigned *)(mgr + 0x74);

            if (arr > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)arr, 4 * 0x54)) {
                fprintf(rsp, " acts=");
                for (i = 0; i < 4; i++)
                    fprintf(rsp, "%02X",
                            *(unsigned char *)(arr + i * 0x54 + 0x4C));
            }
        }
        {
            unsigned mc = g_vc_ctrl;

            if (mc > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)mc, 0x50))
                fprintf(rsp, " mv=%08X:%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",
                        mc, *(float *)(uintptr_t)(mc + 0x38),
                        *(float *)(uintptr_t)(mc + 0x3C),
                        *(float *)(uintptr_t)(mc + 0x40),
                        *(float *)(uintptr_t)(mc + 0x44),
                        *(float *)(uintptr_t)(mc + 0x48),
                        *(float *)(uintptr_t)(mc + 0x4C));
        }
        fprintf(rsp, " 14FE=%ld xi=%d guard_cap=%ld fire=%ld(+%ld) "
                "pump84=%ld",
                g_pump_hist[0x14FE], g_xinput_seen_ok, g_guard_captures,
                g_fire_calls, g_fire_dropped, g_pump84_calls);
        return 0;
    }
    if (!_stricmp(verb, "schemeoff")) {
        g_scheme_off = atoi(rest) ? 1 : 0;
        fprintf(rsp, "schemeoff=%d writes=%ld", g_scheme_off,
                g_scheme_writes);
        return 0;
    }
    if (!_stricmp(verb, "camhold")) {
        g_cam_hold = atoi(rest);
        if (g_cam_hold < 0)
            g_cam_hold = 0;
        fprintf(rsp, "camhold=%d flag=%ld skips=%ld", g_cam_hold,
                g_camhold_skip_flag, g_camhold_skips);
        return 0;
    }
    if (!_stricmp(verb, "camresets")) {
        fprintf(rsp, "camresets yaw=%ld pitch=%ld logs=%ld",
                g_camreset_hits[1], g_camreset_hits[2], g_camreset_logs);
        return 0;
    }
    if (!_stricmp(verb, "camprobe")) {
        g_cam_probe = atoi(rest) ? 1 : 0;
        fprintf(rsp, "camprobe=%d", g_cam_probe);
        return 0;
    }
    if (!_stricmp(verb, "camstate")) {
        InterlockedExchange(&g_cam_probe_now, 1);
        fprintf(rsp, "camstate queued");
        return 0;
    }
    if (!_stricmp(verb, "hold") || !_stricmp(verb, "release")) {
        unsigned char sl[8];
        unsigned char *p;
        int n, on = !_stricmp(verb, "hold");
        char tmp[80];

        snprintf(tmp, sizeof(tmp), "%s", rest);
        n = parse_slots(tmp, sl, 8);
        if (!n) {
            fprintf(rsp, "ERR usage: %s 2,0 (hex slots)", verb);
            return 1;
        }
        if (!pad_for_writes(&p)) {
            fprintf(rsp, "ERR pad not ready (g_pad_obj=%08X)",
                    (unsigned)g_pad_obj);
            return 1;
        }
        for (i = 0; i < n; i++)
            cmd_virt_write(p, sl[i], on);
        fprintf(rsp, "%s %d slot(s) ok pad=%08X", verb, n,
                (unsigned)(uintptr_t)p);
        return 0;
    }
    if (!_stricmp(verb, "pulse")) {
        unsigned char sl[8];
        unsigned char *p;
        char *sp = strchr(rest, ' ');
        int n, ms;
        char tmp[80];

        if (!sp) {
            fprintf(rsp, "ERR usage: pulse 2,0 1500");
            return 1;
        }
        ms = atoi(sp + 1);
        *sp = 0;
        snprintf(tmp, sizeof(tmp), "%s", rest);
        n = parse_slots(tmp, sl, 8);
        if (!n || ms <= 0 || ms > 60000) {
            fprintf(rsp, "ERR usage: pulse 2,0 1500 (ms 1..60000)");
            return 1;
        }
        if (!pad_for_writes(&p)) {
            fprintf(rsp, "ERR pad not ready (g_pad_obj=%08X)",
                    (unsigned)g_pad_obj);
            return 1;
        }
        for (i = 0; i < n; i++)
            cmd_virt_write(p, sl[i], 1);
        log_line("CMD: pulse hold %d ms slots=%s", ms, rest);
        Sleep(ms);
        for (i = 0; i < n; i++)
            cmd_virt_write(p, sl[i], 0);
        fprintf(rsp, "pulse done %d ms", ms);
        return 0;
    }
    if (!_stricmp(verb, "lmb") || !_stricmp(verb, "rmb")) {
        /* §149: test hook for the OS click path — the same apply_key
         * the camera-thread poll uses. The poll auto-releases ~8 ms
         * later, so one call = one real click at the cursor. */
        unsigned want_btn = verb[0] == 'l' ? XI_TRIG_R : XI_TRIG_L;
        int on = atoi(rest) != 0;
        int j;

        for (j = 0; j < (int)KEY_COUNT; j++)
            if (g_keys[j].btn == want_btn) {
                if (!(g_keys[j].flags & (PADF_MOUSE_L | PADF_MOUSE_R))) {
                    fprintf(rsp, "ERR %s row is not a mouse row "
                            "(Layout remapped?)", verb);
                    return 1;
                }
                apply_key(j, on);
                log_line("CMD: %s %d (state=%d)", verb, on,
                         g_key_state[j]);
                fprintf(rsp, "%s %d ok", verb, on);
                return 0;
            }
        fprintf(rsp, "ERR no g_keys row for %s", verb);
        return 1;
    }
    if (!_stricmp(verb, "xenonui")) {
        /* §184: live mode switch for the console-id remap hooks:
         * 0 off, 1 log, 2 wrapper+pf, 3 factory+pf, 4 wrapper always,
         * 5 factory always */
        g_xenon_ui = atoi(rest);
        log_line("CMD: xenonui=%d", g_xenon_ui);
        fprintf(rsp, "xenonui=%d", g_xenon_ui);
        return 0;
    }
    if (!_stricmp(verb, "padflag")) {
        /* §166: read/write the controller-mode byte for bisects */
        unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
        unsigned char *p = exe + (PADFLAG_VA - EXE_DEFAULT_BASE);
        int v;

        if (!rest[0]) {
            fprintf(rsp, "padflag=%u", (unsigned)*p);
            return 0;
        }
        v = atoi(rest) ? 1 : 0;
        *p = (unsigned char)v;
        log_line("CMD: padflag=%d", v);
        fprintf(rsp, "padflag=%d", v);
        return 0;
    }
    if (!_stricmp(verb, "vibtest")) {
        /* §175: fire our own vibration pulse chain and report the
         * computed strength (slider x VibStrength) */
        vib_pulse(0);
        fprintf(rsp, "vib pulse game=%.2f strength=%ld",
                (double)g_game_vib, g_vib_strength);
        return 0;
    }
    if (!_stricmp(verb, "findf")) {
        /* §162: locate a float in the exe image (settings scan) */
        float want = (float)atof(rest);
        unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
        unsigned char *pe = base + *(unsigned *)(base + 0x3C);
        unsigned size = *(unsigned *)(pe + 0x50);
        unsigned bits, r, hit = 0;

        memcpy(&bits, &want, 4);
        fprintf(rsp, "findf %.4f over %u bytes", want, size);
        for (r = 0; r + 4 <= size; r += 4) {
            unsigned *p = (unsigned *)(base + r);

            if (IsBadReadPtr(p, 4))
                continue;
            if (*p == bits) {
                log_line("findf: %.4f at %08X", want,
                         (unsigned)(uintptr_t)p);
                if (++hit >= 24)
                    break;
            }
        }
        fprintf(rsp, "findf done, %u hits", hit);
        return 0;
    }
    if (!_stricmp(verb, "rumble")) {
        /* §158-diag: drive the pad motor directly (bypasses the game) */
        int ms = atoi(rest);
        unsigned short vib[2];

        if (!g_xinput_set) {
            fprintf(rsp, "ERR XInputSetState not resolved");
            return 1;
        }
        if (ms <= 0 || ms > 5000)
            ms = 500;
        vib[0] = vib[1] = 0xFFFF;
        g_xinput_set(g_user_index, vib);
        log_line("CMD: rumble %d ms (direct XInputSetState)", ms);
        Sleep(ms);
        vib[0] = vib[1] = 0;
        g_xinput_set(g_user_index, vib);
        fprintf(rsp, "rumble %d ms done", ms);
        return 0;
    }
    if (!_stricmp(verb, "armguard")) {
        InterlockedExchange(&g_guard_force, 1);
        fprintf(rsp, "guard force-armed SLOTS page only (safe mode, "
                     "fires within 250 ms)");
        return 0;
    }
    if (!_stricmp(verb, "armguard2")) {
        InterlockedExchange(&g_guard_force, 2);
        fprintf(rsp, "guard force-armed BOTH pages - WARNING: this "
                     "killed the game twice (diag#86a/86b)");
        return 0;
    }
    if (!_stricmp(verb, "guardstat")) {
        int acc = 0, j;

        for (j = 0; j < GUARD_MAXEIP; j++)
            if (g_guard_eip[j])
                acc++;
        fprintf(rsp, "guard captures=%ld traps=%ld accessors=%d",
                g_guard_captures, g_guard_traps, acc);
        return 0;
    }
    if (!_stricmp(verb, "rd")) {
        unsigned a, n_dw = 8;
        char *sp = strchr(rest, ' ');

        if (sp) {
            n_dw = strtoul(sp + 1, NULL, 0);
            *sp = 0;
        }
        a = strtoul(rest, NULL, 16);
        if (!a || n_dw < 1 || n_dw > 32) {
            fprintf(rsp, "ERR usage: rd <va> [1..32 dwords]");
            return 1;
        }
        /* input is a default-base exe VA (0x400000..0x1551000) like
         * every VA in the notes; anything else = raw pointer */
        if (a >= 0x400000u && a < 0x1551000u)
            a = (unsigned)(uintptr_t)(exe + (a - 0x400000u));
        if (IsBadReadPtr((void *)(uintptr_t)a, n_dw * 4)) {
            fprintf(rsp, "ERR unreadable %08X", a);
            return 1;
        }
        /* 4 dwords per line, address of the group first */
        {
            char buf[64];
            int len = 0;

            for (i = 0; i < (int)n_dw; i++) {
                len += snprintf(buf + len, sizeof(buf) - len, "%08X ",
                                *(unsigned *)(uintptr_t)(a + i * 4));
                if ((i % 4) == 3 || i == (int)n_dw - 1) {
                    fprintf(rsp, "\n   %08X: %s", a + (i & ~3) * 4, buf);
                    len = 0;
                }
            }
        }
        return 0;
    }
    if (!_stricmp(verb, "seq")) {
        char path[MAX_PATH];
        char sline[512];
        FILE *sf;
        int ln = 0, rc = 0;

        if (!rest[0]) {
            fprintf(rsp, "ERR usage: seq <file-in-dbgcmd>");
            return 1;
        }
        if (g_seq_busy) {
            fprintf(rsp, "ERR seq already running");
            return 1;
        }
        snprintf(path, sizeof(path), "dbgcmd\\%s", rest);
        sf = fopen(path, "r");
        if (!sf) {
            fprintf(rsp, "ERR cannot open %s", path);
            return 1;
        }
        InterlockedExchange(&g_seq_busy, 1);
        while (fgets(sline, sizeof(sline), sf)) {
            char *s = sline, *nl;

            ln++;
            if ((nl = strchr(s, '\n')))
                *nl = 0;
            while (*s == ' ' || *s == '\t')
                s++;
            e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
                *--e = 0;
            if (!*s || *s == '#' || *s == ';')
                continue;
            if (!_strnicmp(s, "sleep ", 6)) {
                int ms = atoi(s + 6);

                if (ms > 0 && ms < 60000)
                    Sleep(ms);
                continue;
            }
            if (!_strnicmp(s, "mark ", 5)) {
                log_line("SEQ:%d MARK %s", ln, s + 5);
                fprintf(rsp, "[%s]", s + 5);
                fputc('\n', rsp);
                continue;
            }
            log_line("SEQ:%d: %s", ln, s);
            cmd_exec(rsp, s);
            fputc('\n', rsp);
        }
        fclose(sf);
        InterlockedExchange(&g_seq_busy, 0);
        fprintf(rsp, "seq %s done (%d lines)", rest, ln);
        return 0;
    }
    fprintf(rsp, "ERR unknown verb '%s' (ping state hold release pulse "
                 "lmb rmb "
                 "armguard armguard2 guardstat rd seq)", verb);
    return 1;
}

static DWORD WINAPI dbgcmd_thread(void *param)
{
    WIN32_FIND_DATAA fd;

    (void)param;
    CreateDirectoryA("dbgcmd", NULL);
    log_line("dbgc: console up - drop NNN.cmd files into dbgcmd\\ "
             "(poll 150 ms)");
    for (;;) {
        HANDLE h;

        Sleep(150);
        h = FindFirstFileA("dbgcmd\\*.cmd", &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            char cmd_path[MAX_PATH], rsp_path[MAX_PATH];
            char tmp_path[MAX_PATH];
            size_t base_len;
            FILE *cf, *rf;
            char line[512];

            /* only *.cmd matched; writers publish atomically by
             * renaming from .tmp, so the file is complete */
            snprintf(cmd_path, MAX_PATH, "dbgcmd\\%s", fd.cFileName);
            base_len = strlen(fd.cFileName) - 4;   /* strip ".cmd" */
            snprintf(tmp_path, MAX_PATH, "dbgcmd\\%.*s.rsp.tmp",
                     (int)base_len, fd.cFileName);
            snprintf(rsp_path, MAX_PATH, "dbgcmd\\%.*s.rsp",
                     (int)base_len, fd.cFileName);
            cf = fopen(cmd_path, "r");
            if (!cf)
                continue;
            rf = fopen(tmp_path, "w");
            if (!rf) {
                fclose(cf);
                continue;
            }
            log_line("dbgc: exec %s", fd.cFileName);
            while (fgets(line, sizeof(line), cf)) {
                char *s = line, *nl, *e2;

                if ((nl = strchr(s, '\n')))
                    *nl = 0;
                while (*s == ' ' || *s == '\t')
                    s++;
                e2 = s + strlen(s);
                while (e2 > s &&
                       (e2[-1] == ' ' || e2[-1] == '\t' || e2[-1] == '\r'))
                    *--e2 = 0;
                if (!*s || *s == '#' || *s == ';')
                    continue;
                log_line("CMD: %s", s);
                cmd_exec(rf, s);
                fputc('\n', rf);
            }
            fclose(cf);
            fclose(rf);
            MoveFileExA(tmp_path, rsp_path, MOVEFILE_REPLACE_EXISTING);
            DeleteFileA(cmd_path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static DWORD WINAPI watchdog_thread(void *param)
{
    long pt = 0, pp = 0, pk = 0, ps = 0, pq = 0, pj = 0, psc = 0;
    long ph_tick = 0, ph_kd = 0, ph_ku = 0, ph_o = 0;
    long pa = 0, pg[3] = { 0, 0, 0 };
    long pgdd = 0, pgds = 0, pevh = 0;
    long g_ispb_prev = 0;
    static long pofs[0x15];
    unsigned gdd_printed_ret = 0, gds_printed_ret = 0;
    unsigned gdd_printed_stk0 = 0, gds_printed_stk0 = 0;

    (void)param;
    for (;;) {
        long t, polls, kb, syn, n_tick, n_kd, n_ku, n_all, n_o, i;

        Sleep(5000);
        /* §204: watchdog is pure periodic diagnostics (ACT/wd/T46/A48/
         * ARR2/GATE45/xi/evh dumps) — behind Diag. */
        if (!g_diag)
            continue;
        dump_pad_format();
        dump_input_chain();
        /* stir detectors: does EITHER side of the split see input?
         * hold a button/stick during the test to catch the snapshot */
        if (g_chain_dumped) {
            static unsigned pq, pv;
            unsigned q = 0, v = 0, i2;

            if (g_qvec_begin > 0x10000 &&
                !IsBadReadPtr((void *)g_qvec_begin, 0xA0))
                for (i2 = 0; i2 < 0xA0; i2 += 4)
                    q += *(unsigned *)(g_qvec_begin + i2);
            if (q != pq) {
                log_line("qvec CHANGED: sum=%08X (prev %08X)", q, pq);
                pq = q;
            }
            if (g_pad_obj &&
                !IsBadReadPtr((void *)(uintptr_t)g_pad_obj, 0x140)) {
                unsigned char *pp = (unsigned char *)(uintptr_t)g_pad_obj;
                unsigned va = *(unsigned *)(pp + 0x30);

                if (va > 0x10000 && !IsBadReadPtr((void *)va, 0x140))
                    for (i2 = 0; i2 < 0x140; i2 += 4)
                        v += *(unsigned *)(va + i2);
            }
            if (v != pv) {
                log_line("padvirt CHANGED: sum=%08X (prev %08X)", v, pv);
                pv = v;
            }
        }
        /* diag#73: the FULL state line every window - the virtual
         * array (first byte of each of the 16 used entries), the
         * last POV angle and the engine's per-action flags for
         * acts 0..3. During a sticky episode this shows WHICH
         * internal state stayed latched: the array, the POV, the
         * analog pairs or the action state. */
        {
            char sb[120];
            int sl = 0;
            unsigned char *exe2 = (unsigned char *)GetModuleHandleA(NULL);
            unsigned char *pp2 = (unsigned char *)(uintptr_t)g_pad_obj;
            unsigned va2 = pp2 ? *(unsigned *)(pp2 + 0x30) : 0;
            unsigned pd = g_paddev_obj;
            int a2;

            if (va2 > 0x10000 && !IsBadReadPtr((void *)va2, 0x140))
                for (a2 = 0; a2 < 16; a2++)
                    sl += snprintf(sb + sl, sizeof(sb) - sl, "%X",
                                   *(unsigned char *)(va2 + a2 * 0x10));
            sb[sl] = 0;
            if (pd > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)pd, 0x50)) {
                float lx = *(float *)(pd + 0x28);
                float ly = *(float *)(pd + 0x2C);

                log_line("STATE: virt=%s pov=%X lx=%d ly=%d", sb,
                         g_last_pov, (int)(lx * 100.0f),
                         (int)(ly * 100.0f));
            } else {
                log_line("STATE: virt=%s pov=%X lx=? ly=?", sb, g_last_pov);
            }
            /* the three candidate action managers; the action entry is
             * inline at [mgr+0x74]+act*0x54: flags@+0x28, flags@+0x3C,
             * keyboard level @+0x4C */
            {
                static const unsigned mgs[3] = { 0x014F16FCu, 0x014F1704u,
                                                 0x014F13D8u };
                int m;

                for (m = 0; m < 3; m++) {
                    unsigned mgr = *(unsigned *)(exe2 + (mgs[m] -
                                                         EXE_DEFAULT_BASE));
                    unsigned arr;
                    char ab[160];
                    int al = 0;

                    if (mgr <= 0x10000 ||
                        IsBadReadPtr((void *)(uintptr_t)mgr, 0x100))
                        continue;
                    arr = *(unsigned *)(mgr + 0x74);
                    if (arr <= 0x10000 ||
                        IsBadReadPtr((void *)(uintptr_t)arr, 4 * 0x54))
                        continue;
                    for (a2 = 0; a2 < 4; a2++) {
                        unsigned e2 = arr + (unsigned)a2 * 0x54;

                        al += snprintf(ab + al, sizeof(ab) - al,
                                       "%02X%02X%02X%02X ",
                                       *(unsigned char *)(e2 + 0x28),
                                       *(unsigned char *)(e2 + 0x29),
                                       *(unsigned char *)(e2 + 0x3C),
                                       *(unsigned char *)(e2 + 0x4C));
                    }
                    ab[al > 0 ? al - 1 : 0] = 0;
                    log_line("ACT%d mgr=%08X: %s", m, mgr, ab);
                }
            }
        }
        t = g_tick_count;
        polls = g_pad_polls;
        kb = g_getkb_calls;
        syn = g_synth_events;

        if (g_gdd_calls > 0 &&
            ((unsigned)g_gdd_ret != gdd_printed_ret ||
             g_gdd_stk[0] != gdd_printed_stk0)) {
            unsigned base = (unsigned)GetModuleHandleA(NULL);

            gdd_printed_ret = (unsigned)g_gdd_ret;
            gdd_printed_stk0 = g_gdd_stk[0];
            log_line("gdd: calls=%ld ret=%08X exeVA=%08X this=%08X "
                     "ecx=%08X edx=%08X stk: %08X %08X %08X %08X %08X "
                     "%08X %08X %08X",
                     g_gdd_calls, (unsigned)g_gdd_ret,
                     (unsigned)g_gdd_ret - base + 0x400000,
                     g_gdd_stk[0], g_gdd_ecx, g_gdd_edx,
                     g_gdd_stk[0], g_gdd_stk[1], g_gdd_stk[2], g_gdd_stk[3],
                     g_gdd_stk[4], g_gdd_stk[5], g_gdd_stk[6], g_gdd_stk[7]);
        }

        if (g_gds_calls > 0 &&
            ((unsigned)g_gds_ret != gds_printed_ret ||
             g_gds_stk[0] != gds_printed_stk0)) {
            unsigned base = (unsigned)GetModuleHandleA(NULL);

            gds_printed_ret = (unsigned)g_gds_ret;
            gds_printed_stk0 = g_gds_stk[0];
            log_line("gds: calls=%ld ret=%08X exeVA=%08X this=%08X "
                     "ecx=%08X edx=%08X stk: %08X %08X %08X %08X %08X "
                     "%08X %08X %08X",
                     g_gds_calls, (unsigned)g_gds_ret,
                     (unsigned)g_gds_ret - base + 0x400000,
                     g_gds_stk[0], g_gds_ecx, g_gds_edx,
                     g_gds_stk[0], g_gds_stk[1], g_gds_stk[2], g_gds_stk[3],
                     g_gds_stk[4], g_gds_stk[5], g_gds_stk[6], g_gds_stk[7]);
        }

        n_tick = g_pump_hist[0x14FE];
        n_kd = g_pump_hist[0x100];
        n_ku = g_pump_hist[0x101];
        n_all = 0;
        for (i = 0; i < PUMP_HIST_SIZE; i++)
            n_all += g_pump_hist[i];
        n_o = n_all - n_tick - n_kd - n_ku;
        log_line("wd: t=%ld(+%ld) padpolls=%ld(+%ld) gdd=%ld(+%ld) gds=%ld(+%ld) "
                 "peek=%ld(+%ld) "
                 "inj=%ld(+%ld) sc=%ld(+%ld) kb=%ld(+%ld) synth=%ld(+%ld) "
                 "pump5s: 14FE+%ld K+%ld U+%ld o+%ld xi=%d "
                 "tok: an=%ld gn=%ld opeq=%ld parse=%ld",
                 t, t - pt, polls, polls - pp,
                 g_gdd_calls, g_gdd_calls - pgdd,
                 g_gds_calls, g_gds_calls - pgds,
                 g_peek_calls, g_peek_calls - pq,
                 g_wm_injected, g_wm_injected - pj,
                 g_setcur_calls, g_setcur_calls - psc,
                 kb, kb - pk, syn, syn - ps,
                 n_tick - ph_tick, n_kd - ph_kd, n_ku - ph_ku,
                 n_o - ph_o, g_xinput_seen_ok,
                 g_an_calls - pa, g_tfn_calls[0] - pg[0],
                 g_tfn_calls[1] - pg[1], g_tfn_calls[2] - pg[2]);
        /* axis evidence: per-dwOfs histogram of the pad event handler
         * (0..7 buttons, 8 POV, 9..0x14 half-axes) */
        if (g_evh_calls != pevh) {
            char buf[512];
            int len = 0;

            for (i = 0; i < 0x15; i++)
                len += snprintf(buf + len, sizeof(buf) - len, "%ld ",
                                g_ofs_hist[i] - pofs[i]);
            buf[len > 0 ? len - 1 : 0] = 0;
            log_line("evh: calls=%ld(+%ld) dwOfs deltas: %s "
                     "[ispb %ld(+%ld) hits %ld]", g_evh_calls,
                     g_evh_calls - pevh, buf,
                     g_ispb_calls, g_ispb_calls - g_ispb_prev,
                     g_ispb_hits);
            len = 0;
            for (i = 0; i < 8; i++)
                if (g_ch_seen[i])
                    len += snprintf(buf + len, sizeof(buf) - len,
                                    "%u:[%ld..%ld] ", (unsigned)i,
                                    g_ch_smin[i], g_ch_smax[i]);
            buf[len > 0 ? len - 1 : 0] = 0;
            log_line("ch signed ranges: %s", buf);
            len = 0;
            for (i = 0; i < 0x14; i++)
                if (g_ispb_idx[i])
                    len += snprintf(buf + len, sizeof(buf) - len,
                                    "%X:%ld/%ld ", (unsigned)i,
                                    g_ispb_idx[i], g_ispb_true[i]);
            buf[len > 0 ? len - 1 : 0] = 0;
            log_line("ispb idx: called/true: %s", buf);
            g_ispb_prev = g_ispb_calls;
            len = 0;
            for (i = 0; i < 0x15; i++)
                if (g_dw_seen[i])
                    len += snprintf(buf + len, sizeof(buf) - len,
                                    "%02X:%X-%X ", (unsigned)i,
                                    g_dw_min[i], g_dw_max[i]);
            buf[len > 0 ? len - 1 : 0] = 0;
            log_line("evh dwData ranges: %s", buf);
            if (g_pad_obj &&
                !IsBadReadPtr((void *)(uintptr_t)g_pad_obj, 0x160)) {
                unsigned char *pad = (unsigned char *)(uintptr_t)g_pad_obj;
                unsigned chp = *(unsigned *)(pad + 0x38);

                if (chp > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)chp, 6 * 0x20)) {
                    unsigned k;

                    len = 0;
                    for (k = 0; k < 6; k++)
                        len += snprintf(buf + len, sizeof(buf) - len,
                                        "%u:%X/%X ", k,
                                        *(unsigned *)(chp + k * 0x20),
                                        *(unsigned *)(chp + k * 0x20 + 4));
                    buf[len > 0 ? len - 1 : 0] = 0;
                    log_line("padch raw/proc: %s", buf);
                }
            }
            pevh = g_evh_calls;
            for (i = 0; i < 0x15; i++)
                pofs[i] = g_ofs_hist[i];
            log_line("stk: writes=%ld want=%02X lx=%d ly=%d src=%c",
                     g_stk_writes, (unsigned)g_synth_virt47,
                     g_stk_lx, g_stk_ly, g_stk_src);
            /* diag#65: the physical XInput state - the ground truth
             * for "which stick is which". Printed next to the DI8
             * channel dump so one session maps channels to sticks. */
            if (g_xinput_get && g_xinput_seen_ok) {
                XINPUT_STATE_T xst;

                if (g_xinput_get(g_user_index, &xst) == 0)
                    log_line("xi: LX=%d LY=%d RX=%d RY=%d LT=%d RT=%d "
                             "btn=%04X",
                             xst.Gamepad.sThumbLX, xst.Gamepad.sThumbLY,
                             xst.Gamepad.sThumbRX, xst.Gamepad.sThumbRY,
                             xst.Gamepad.bLeftTrigger,
                             xst.Gamepad.bRightTrigger,
                             xst.Gamepad.wButtons);
            }
            /* diag#28: virt entry state (cur, +1, prev, +3) -
             * shows whether the engine latched previous over our
             * write (cur==prev) or the edge is visible (cur!=prev).
             * diag#65: the movement dirs now live in the D-pad
             * slots 0..3 (the physical D-pad uses them as well). */
            {
                unsigned char *pp = (unsigned char *)(uintptr_t)g_pad_obj;
                unsigned va = pp ? *(unsigned *)(pp + 0x30) : 0;

                if (va > 0x10000 && !IsBadReadPtr((void *)va, 0xE0)) {
                    static const unsigned char vk[4] = { 0, 1, 2, 3 };
                    unsigned k2, any = 0;
                    char vbuf[160];
                    int vlen = 0;

                    for (k2 = 0; k2 < 4; k2++) {
                        unsigned char *e =
                            (unsigned char *)va + vk[k2] * 0x10;

                        if (e[0] || e[1] || e[2] || e[3])
                            any = 1;
                        vlen += snprintf(vbuf + vlen, sizeof(vbuf) - vlen,
                                         "%X:%X/%X/%X/%X ", vk[k2],
                                         e[0], e[1], e[2], e[3]);
                    }
                    if (any) {
                        vbuf[vlen > 0 ? vlen - 1 : 0] = 0;
                        log_line("virt47 cur/?/prev/?: %s", vbuf);
                    }
                }
            }
        }
        if (g_gav_calls != g_gav_prev || g_axq_calls != g_axq_prev ||
            g_movc_calls != g_movc_prev || g_camc_calls != g_camc_prev) {
            char abuf[512];
            int alen = 0;
            int pfx = 0, pfy = 0;
            unsigned char *pp = (unsigned char *)(uintptr_t)g_pad_obj;

            /* processed axis floats the native chain feeds on:
             * GetAxisValue 0x733B90 reads pad+0x28(LX)/+0x2C(LY),
             * normalized by gav -> +/-1.0; CalibFix keeps them fed */
            if (pp && !IsBadReadPtr(pp, 0x38)) {
                pfx = (int)(*(float *)(pp + 0x28) * 100.0f);
                pfy = (int)(*(float *)(pp + 0x2C) * 100.0f);
            }

            for (i = 0; i < 0x61; i++)
                if (g_gav_act[i] || g_gav_pact[i])
                    alen += snprintf(abuf + alen, sizeof(abuf) - alen,
                                     "%X:%ld ", (unsigned)i,
                                     g_gav_act[i] - g_gav_pact[i]);
            abuf[alen > 0 ? alen - 1 : 0] = 0;
            {
                char hb[256];
                int hn = 0;

                for (i = 0; i < 0x10; i++)
                    hn += snprintf(hb + hn, sizeof(hb) - hn,
                                   "%u:%ld@%08X ", i, g_axq_idx[i],
                                   g_axq_this[i]);
                hb[hn > 0 ? hn - 1 : 0] = 0;
                log_line("axq idx: %s paddev=%08X mode=%u", hb,
                         g_paddev_obj,
                         g_paddev_obj ? *(unsigned char *)(uintptr_t)
                         (g_paddev_obj + 0x4C) : 0);
            }
            log_line("vc: ctrl=%08X prev=%08X calls=%ld vec=%.3f,%.3f,%.3f",
                     g_vc_ctrl, g_vc_ctrl2, g_vc_calls,
                     g_vc_vec[0], g_vc_vec[1], g_vc_vec[2]);
            log_line("q2: gav=%ld(+%ld) movc=%ld(+%ld) camc=%ld(+%ld) "
                     "axq=%ld(+%ld) pf=%d,%d gav acts: %s",
                     g_gav_calls, g_gav_calls - g_gav_prev,
                     g_movc_calls, g_movc_calls - g_movc_prev,
                     g_camc_calls, g_camc_calls - g_camc_prev,
                     g_axq_calls, g_axq_calls - g_axq_prev, pfx, pfy,
                     abuf);
            g_gav_prev = g_gav_calls;
            g_movc_prev = g_movc_calls;
            g_camc_prev = g_camc_calls;
            g_axq_prev = g_axq_calls;
            for (i = 0; i < 0x61; i++)
                g_gav_pact[i] = g_gav_act[i];
            for (i = 0; i < 0x10; i++)
                g_axq_previ[i] = g_axq_idx[i];
        }
        /* diag#45: gate-reset breadcrumbs. Tracks the platform gate
         * bytes [0x14F13C0]+0x52/+0x53, the mirror-check carrier
         * [0x15483E8]+0x52, wrapper creation [0x14F1704] (expected
         * CREATED after GATEPATCH), its bundle MoveCollector handle
         * (+0x14 -> +0x10 -> [+4]; non-FF-ish = the ORIGINAL attach
         * flow is alive), the PC alternate manager [0x14F13D8]
         * (expected ABSENT with the patch) and [0x14F17F4]. A line
         * prints on ANY change; the first 30 s it is periodic so
         * absence of changes is also visible. */
        {
            static unsigned pw, palt, pmini, pf52, pf53, po52;
            static long n45;
            unsigned char *e = (unsigned char *)GetModuleHandleA(NULL);
            unsigned wrap = *(unsigned *)(e + (0x014F1704u -
                                               EXE_DEFAULT_BASE));
            unsigned alt = *(unsigned *)(e + (0x014F13D8u -
                                              EXE_DEFAULT_BASE));
            unsigned mini = *(unsigned *)(e + (0x014F17F4u -
                                               EXE_DEFAULT_BASE));
            unsigned oth = *(unsigned *)(e + (0x015483E8u -
                                              EXE_DEFAULT_BASE));
            unsigned gc = *(unsigned *)(e + (0x014F13C0u -
                                             EXE_DEFAULT_BASE));
            unsigned f52, f53, o52, bund, mch;
            int ok, oook;

            f52 = f53 = o52 = 0xFF;
            bund = mch = 0;
            ok = gc > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)gc,
                                               0x100);
            oook = oth > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)oth,
                                                  0x60);
            if (ok) {
                f52 = *(unsigned char *)(uintptr_t)(gc + 0x52);
                f53 = *(unsigned char *)(uintptr_t)(gc + 0x53);
            }
            if (oook)
                o52 = *(unsigned char *)(uintptr_t)(oth + 0x52);
            if (wrap > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)wrap, 0x18)) {
                bund = *(unsigned *)(uintptr_t)(wrap + 0x14);
                if (bund > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)bund, 0x14)) {
                    mch = *(unsigned *)(uintptr_t)(bund + 0x10);
                    if (mch > 0x10000 &&
                        !IsBadReadPtr((void *)(uintptr_t)mch, 8))
                        mch = *(unsigned *)(uintptr_t)(mch + 4);
                    else
                        mch = 0;
                } else
                    bund = 0;
            }
            n45++;
            if (wrap != pw || alt != palt || mini != pmini ||
                f52 != pf52 || f53 != pf53 || o52 != po52 || n45 <= 6) {
                if (wrap && !pw)
                    log_line("GATE45: wrapper CREATED %08X (collector "
                             "had %08X)", wrap, g_collector_handle);
                if (alt && !palt)
                    log_line("GATE45: ALT manager [0x14F13D8] CREATED "
                             "%08X", alt);
                log_line("GATE45: f52=%02X f53=%02X oth52=%02X "
                         "wrap=%08X alt=%08X mini=%08X bund=%08X "
                         "mch=%08X", f52, f53, o52, wrap, alt, mini,
                         bund, mch);
                pw = wrap;
                palt = alt;
                pmini = mini;
                pf52 = f52;
                pf53 = f53;
                po52 = o52;
            }
        }
        /* diag#46: drive-chain breadcrumbs. The input tuple
         * [0x14F13E4] (0x10 bytes: [0]=root-lit, [+4] = pad slot
         * (0 until attach), [+8] = inputMgr 0x14, [+0xC] = tickCtx
         * 0x34 per ctor 0x6EA920), the profile singleton
         * [0x14F13CC] and tickCtx health (ctx[0] root, ctx+0x18 the
         * CRITICAL_SECTION counter at 0x14+4). A line on ANY change
         * or during the first 30 s; q2 prints movc/camc growth. */
        {
            static unsigned pe4, ptp0, ptp4, ptp8, ptpc, pcc, pct0,
                            pct18;
            static long n46;
            unsigned char *e = (unsigned char *)GetModuleHandleA(NULL);
            unsigned tp = *(unsigned *)(e + (0x014F13E4u -
                                             EXE_DEFAULT_BASE));
            unsigned cc = *(unsigned *)(e + (0x014F13CCu -
                                             EXE_DEFAULT_BASE));
            unsigned p0 = 0, p4 = 0, p8 = 0, pc = 0;
            unsigned ct0 = 0, ct18 = 0;

            if (tp > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)tp, 0x10)) {
                p0 = *(unsigned *)(uintptr_t)tp;
                p4 = *(unsigned *)(uintptr_t)(tp + 0x4);
                p8 = *(unsigned *)(uintptr_t)(tp + 0x8);
                pc = *(unsigned *)(uintptr_t)(tp + 0xC);
                if (pc > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)pc, 0x20)) {
                    ct0 = *(unsigned *)(uintptr_t)pc;
                    ct18 = *(unsigned *)(uintptr_t)(pc + 0x18);
                }
            }
            n46++;
            if (tp != pe4 || p0 != ptp0 || p4 != ptp4 || p8 != ptp8 ||
                pc != ptpc || cc != pcc || ct0 != pct0 ||
                ct18 != pct18 || n46 <= 6)
                log_line("T46: tuple=%08X [0]=%08X [4]=%08X [8]=%08X "
                         "[C]=%08X ctx0=%08X ctx18=%08X cc=%08X "
                         "tdrv=%ld", tp, p0, p4, p8, pc, ct0, ct18,
                         cc, g_tdrive_calls);
        }
        /* diag#47: apply-space census. The movement apply resolves
         * its entity through manager [0x14F13C4]: [mgr+0x30] = entity
         * pointer array, [mgr+0x3C] = count; the handle IS the index
         * in that array. Print the count and the first entries so we
         * can match a live entity (camera/avatar) to its handle. */
        {
            static unsigned pmgr, pcnt, pfirst;
            static long n47;
            unsigned char *e = (unsigned char *)GetModuleHandleA(NULL);
            unsigned mgr = *(unsigned *)(e + (0x014F13C4u -
                                             EXE_DEFAULT_BASE));
            unsigned cnt = 0, arr = 0, f0 = 0, f1 = 0, f2 = 0;

            if (mgr > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)mgr, 0x40)) {
                arr = *(unsigned *)(uintptr_t)(mgr + 0x30);
                cnt = *(unsigned *)(uintptr_t)(mgr + 0x3C);
                if (cnt > 0 && cnt <= 0x100000 && arr > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)arr, cnt * 4)) {
                    f0 = *(unsigned *)(uintptr_t)arr;
                    f1 = *(unsigned *)(uintptr_t)(arr + 4);
                    f2 = *(unsigned *)(uintptr_t)(arr + 8);
                } else
                    cnt = 0;
            }
            n47++;
            if (mgr != pmgr || cnt != pcnt || f0 != pfirst || n47 <= 6)
                log_line("ARR2: mgr=%08X cnt=%u arr=%08X e0=%08X e1=%08X "
                         "e2=%08X applycalls=%ld apph=%08X appent=%08X",
                         mgr, cnt, arr, f0, f1, f2, g_apply_calls,
                         g_apply_h, g_apply_ent);
            pmgr = mgr;
            pcnt = cnt;
            pfirst = f0;
        }
        /* diag#48: the avatar census.  Three independent signals:
         *   (a) per manager: the hottest handle the engine asks for
         *       (the camera resolves the avatar every frame);
         *   (b) per call site: which code resolves it, and how often;
         *   (c) per entity: the float block around its position, to see
         *       which entity MOVES while the player walks.
         * Signal (c) is the decisive one: the avatar is the entity the
         * player moves.  We also print the collector's own handle,
         * which the engine attaches by itself (diag#47: mch=00000293).
         */
        {
            static long n48;
            int mi, h, si;
            unsigned char *e48 = (unsigned char *)GetModuleHandleA(NULL);
            unsigned wrap48 = *(unsigned *)(e48 + (0x014F1704u -
                                                  EXE_DEFAULT_BASE));
            unsigned col48 = 0, mch48 = 0;

            if (wrap48 > 0x10000 &&
                !IsBadReadPtr((void *)(uintptr_t)wrap48, 0x18)) {
                unsigned bund = *(unsigned *)(uintptr_t)(wrap48 + 0x14);

                if (bund > 0x10000 &&
                    !IsBadReadPtr((void *)(uintptr_t)bund, 0x18)) {
                    col48 = *(unsigned *)(uintptr_t)(bund + 0x10);
                    if (col48 > 0x10000 &&
                        !IsBadReadPtr((void *)(uintptr_t)col48, 8))
                        mch48 = *(unsigned *)(uintptr_t)(col48 + 4);
                }
            }
            n48++;
            for (mi = 0; mi < RES_MAXMGR; mi++) {
                struct res_mgr_s *s = &g_res_mgrs[mi];
                unsigned top_h = 0, top_c = 0, top_e = 0;
                unsigned mv_h = 0, mv_n = 0, mv_e = 0;
                float mv_d = 0.0f;
                char tb[200];
                int tl = 0;

                if (!s->mgr || !s->hcnt)
                    continue;
                for (h = 0; h < RES_MAXH; h++) {
                    float snap[RES_SNAPF], d = 0.0f;
                    int k;

                    if (s->hcnt[h] > top_c) {
                        top_c = s->hcnt[h];
                        top_h = (unsigned)h;
                        top_e = s->hent[h];
                    }
                    if (!s->hcnt[h] || !s->hent[h])
                        continue;
                    if (!res_snap(s->hent[h], snap))
                        continue;
                    if (s->hseen[h]) {
                        for (k = 0; k < RES_SNAPF; k++) {
                            float dd = snap[k] - s->hsnap[h * RES_SNAPF + k];

                            if (dd < 0.0f)
                                dd = -dd;
                            if (dd > d)
                                d = dd;
                        }
                        if (d > 0.001f) {
                            mv_n++;
                            if (d > mv_d) {
                                mv_d = d;
                                mv_h = (unsigned)h;
                                mv_e = s->hent[h];
                            }
                        }
                    }
                    memcpy(&s->hsnap[h * RES_SNAPF], snap,
                           sizeof(float) * RES_SNAPF);
                    s->hseen[h] = 1;
                }
                tl += snprintf(tb + tl, sizeof(tb) - tl,
                               "M%08X:c%ld n%u top=h%X(n%u e%08X) "
                               "moved=%u(h%X e%08X d%.3f)",
                               s->mgr, s->calls, s->cnt, top_h, top_c,
                               top_e, mv_n, mv_h, mv_e, mv_d);
                if (tl > 0 && (n48 <= 6 || mv_n || (n48 % 12) == 0))
                    log_line("A48: col=%08X mch=%08X %s", col48, mch48, tb);
            }
            /* call sites: the camera's resolver call stands out with a
             * per-frame rate and always names the same handle */
            {
                char tb[400];
                int tl = 0, printed = 0;

                for (si = 0; si < RES_MAXSITE; si++) {
                    struct res_site_s *sp = &g_res_sites[si];
                    int best = -1;
                    long bc = 0;
                    int j;

                    if (!sp->site)
                        continue;
                    for (j = 0; j < RES_MAXSITE; j++)
                        if (g_res_sites[j].site && g_res_sites[j].calls > bc) {
                            bc = g_res_sites[j].calls;
                            best = j;
                        }
                    if (best != si)
                        continue;
                    if (tl + 60 >= (int)sizeof(tb))
                        break;
                    tl += snprintf(tb + tl, sizeof(tb) - tl,
                                   "[%08X n%ld m%08X h%X e%08X]",
                                   sp->site, sp->calls, sp->mgr,
                                   sp->handle, sp->ent);
                    printed++;
                    if (printed >= 8)
                        break;
                }
                if (tl > 0 && (n48 <= 6 || (n48 % 12) == 0))
                    log_line("A48S: %s", tb);
            }
        }
        find_collectors((unsigned char *)GetModuleHandleA(NULL));
        paddev_diag((unsigned char *)GetModuleHandleA(NULL));
        pad_calib_fix();
        tk_dump((unsigned char *)GetModuleHandleA(NULL));
        pt = t; pp = polls; pk = kb; ps = syn; pgdd = g_gdd_calls;
        pgds = g_gds_calls;
        pq = g_peek_calls; pj = g_wm_injected; psc = g_setcur_calls;
        ph_tick = n_tick; ph_kd = n_kd; ph_ku = n_ku; ph_o = n_o;
        pa = g_an_calls; pg[0] = g_tfn_calls[0]; pg[1] = g_tfn_calls[1];
        pg[2] = g_tfn_calls[2];
    }
}

/* diag#45: reset the hard-coded PC platform gate on the master input
 * singleton. Divinity2's init writes mov byte [esi+0x52],1 at static
 * VA 0x6A6C53 (only writer found in .text). With [0x14F13C0]+0x52==0
 * (the X360 value) the per-frame master vmethod 0x69E2F0 (reached via
 * secondary vtable 0x1314B00, so no direct call sites) runs the
 * ensure-singletons chain 0x6A8950 every frame, which then passes the
 * creation gate at 0x6A9B17 and self-assembles the console wrapper
 * [0x14F1704] (alloc 0x18 -> ctor 0x834070, whose ctor builds the
 * bundle 0x9F1A20 at +0x14 itself) and skips the PC alternate manager
 * [0x14F13D8]. Same byte also switches the vmethod's +0x4A (=+0x52 of
 * base) PC/X360 wiring branches per frame. NativeGate=g flips the
 * imm8 0x01 -> 0x00 in the init writer. */
static void gate_patch(void)
{
    unsigned char *e = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *site = e + (0x006A6C53u - EXE_DEFAULT_BASE);
    static const unsigned char ref[4] = { 0xC6, 0x46, 0x52, 0x01 };
    DWORD old;
    int i;

    for (i = 0; i < 4; i++)
        if (site[i] != ref[i]) {
            log_line("GATEPATCH: pattern %02X%02X%02X%02X != ref "
                     "C6 46 52 01 - SKIP", site[0], site[1], site[2],
                     site[3]);
            return;
        }
    if (!VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &old)) {
        log_line("GATEPATCH: VirtualProtect failed GLE=%lu",
                 GetLastError());
        return;
    }
    site[3] = 0x00;
    VirtualProtect(site, 4, old, &old);
    log_line("GATEPATCH: applied - init will now write 0 into "
             "[0x14F13C0]+0x52 (original byte was 01)");
}

extern "C" __declspec(dllexport) void __stdcall InitPatch(void)
{
    char base_path[MAX_PATH];
    void (__stdcall *base_init)(void);

    log_line("[%lu] InitPatch entry (self=%p exe=%p)", GetCurrentProcessId(),
             g_self, GetModuleHandleA(NULL));
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, veh_handler);

    snprintf(base_path, MAX_PATH, "%s\\%s", g_dll_dir, BASE_DLL_NAME);
    g_base = LoadLibraryA(base_path);
    if (!g_base) {
        log_line("ERROR: LoadLibraryA(\"%s\") failed, GLE=%lu",
                 base_path, GetLastError());
        return;
    }
    base_init = (void (__stdcall *)(void))GetProcAddress(g_base, "InitPatch");
    if (!base_init) {
        log_line("ERROR: Div2PatchBase.dll has no InitPatch export");
        return;
    }
    base_init();
    log_line("base InitPatch forwarded OK");

    if (g_native_gate)
        gate_patch();

    if (g_enable_gamepad && !g_no_threads)
        CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    AddVectoredExceptionHandler(1, guard_veh);
    if (g_enable_gamepad && !g_no_threads)
        CreateThread(NULL, 0, guard_armer_thread, NULL, 0, NULL);
    if (g_enable_gamepad && !g_no_threads)
        CreateThread(NULL, 0, state_thread, NULL, 0, NULL);
    if (g_enable_gamepad && g_dbgcmd && !g_no_threads)
        CreateThread(NULL, 0, dbgcmd_thread, NULL, 0, NULL);
    if (g_enable_gamepad && g_rstick_cam && !g_no_threads)
        CreateThread(NULL, 0, camera_thread, NULL, 0, NULL);

    if (g_enable_gamepad) {
        if (g_hook_pump) {
            char pump_log[MAX_PATH];

            snprintf(pump_log, MAX_PATH, "%s\\div2gamepad_pump.log",
                     g_dll_dir);
            g_pump_fp = fopen(pump_log, "w");
            if (g_pump_fp)
                setvbuf(g_pump_fp, NULL, _IOFBF, 64 * 1024);
        }
        /* pump hook: synthesis cadence (0x14FE ticks) AND the game-thread
         * application of pad defaults. diag#11 lesson: with Synth=0 the
         * condition was false and the binds silently degraded to a
         * background-thread apply (race, actions dead). */
        if (g_hook_pump || g_synth || g_paddefaults) {
            if (install_pump_hook()) {
                load_xinput();
                if (g_synth) {
                    if (build_synth_shim())
                        log_line("synthesis ready: shim=%p tramp=%p "
                                 "dz=%ld user=%lu", g_synth_call,
                                 g_trampoline, g_deadzone, g_user_index);
                    else
                        log_line("synthesis shim FAILED");
                }
            } else {
                log_line("pump hook FAILED (see above)");
            }
        }
        install_padctor_hook();
        install_appendname_hook();
        install_tokenfn_hooks();
        install_evh_hook();
        install_tick_hook();
        install_pup_hook();
        if (g_vib_on_damage)
            install_vibd_hook();
        if (g_move_tick && !g_move_cam)
            patch_collector_camera((unsigned char *)GetModuleHandleA(NULL));
        install_e170_hook();
        install_apply_hook();
        install_vc_hook();
        install_vc3_hook();
        install_motor_hooks();
        install_m6_hook();
        install_resolve_hook();
        install_axq_hook();
        {
            static const unsigned char movc_head[9] = {
                0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04
            };
            static const unsigned char camc_head[9] = {
                0x83, 0xEC, 0x18, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x04
            };

            install_inc_hook("movc", 0x00A8B620u, movc_head, 9,
                             &g_movc_calls);
            install_inc_hook("camc", 0x00A8B780u, camc_head, 9,
                             &g_camc_calls);
        }
        install_gav_hook();
        install_firelog_hook();
        install_ispb_hook();
        install_vset_watch();
        install_vclr_watch();
        install_fire84_watch();
        install_pump84_watch();
        install_b270_watch();
        install_camhold_hook();                     /* §148: real camera fix */
        install_camreset_watch(0x0090FA40u, 9, 1);  /* yaw offset = 0 */
        install_camreset_watch(0x0090FA50u, 9, 2);  /* pitch offset = 0 */
        install_d96a_watch();
        install_d96b_watch();
        install_d97a_watch();
        install_d97e_watch();
        install_d98a_watch();
        install_d98b_watch();
        if (g_log_binds)
            install_bind_hook();
        install_reset_hook();       /* §157: options-reset detector */
        if (g_pad_loadfix)
            install_loadscreen_fix(); /* §180: Win32 load screen in pad mode */
        if (g_pad_boot_mode)
            install_boot_padmode();   /* §181: native console mode at boot */
        if (g_xenon_bundle)
            install_xenon_bundle();   /* §183: + 10 Xenon GUI movies */
        if (g_xenon_ui)
            install_xenon_ui();       /* §184: console screen-id remap */
        if (g_rumble) {
            install_rumble_patch(); /* §158: XInput rumble */
            install_rumble_diag();  /* §158-diag: chain probes */
        }
        if (g_vibration)
            install_rumble_slider_hook(); /* §160: in-game slider */
        install_wwise_log();              /* §162: event vocabulary */
        if (g_paddefaults)
            CreateThread(NULL, 0, paddefaults_thread, NULL, 0, NULL);
        if (g_hook_dispatch) {
            char disp_log[MAX_PATH];

            snprintf(disp_log, MAX_PATH, "%s\\div2gamepad_disp.log",
                     g_dll_dir);
            g_disp_fp = fopen(disp_log, "w");
            if (g_disp_fp)
                setvbuf(g_disp_fp, NULL, _IOFBF, 64 * 1024);
            if (!install_dispatch_hook())
                log_line("dispatch hook FAILED (see above)");
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hinst;
        DisableThreadLibraryCalls(hinst);
        build_paths();
        read_config();
        log_line("[%lu] DllMain attach, dir=%s gamepad=%d",
                 GetCurrentProcessId(), g_dll_dir, g_enable_gamepad);
        if (g_enable_gamepad && g_synth)
            install_dinput_hook();
    }
    return TRUE;
}
