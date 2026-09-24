#include "window.h"

#include "config.h"
#include "log.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static HWND    g_hwnd      = NULL;
static WNDPROC g_origProc  = NULL;
static BOOL    g_unicode   = TRUE;
static LONG    g_enforcing = 0;   // guards re-entry via WM_SIZE/WM_WINDOWPOSCHANGING
static HANDLE  g_watchdog  = NULL;
static HANDLE  g_stopEvent = NULL;
static HHOOK   g_kbHook    = NULL;

static LRESULT CALLBACK BorderlessWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// DWM hints (cosmetic)
// ---------------------------------------------------------------------------

typedef HRESULT(WINAPI* PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

// Spelled out to avoid an SDK version dependency.
#define DBL_DWMWA_TRANSITIONS_FORCEDISABLED  3
#define DBL_DWMWA_EXCLUDED_FROM_PEEK         12

static void ApplyDwmHints(HWND hwnd)
{
    HMODULE dwmapi = GetModuleHandleA("dwmapi.dll");
    if (!dwmapi) dwmapi = LoadLibraryA("dwmapi.dll");
    if (!dwmapi) return;

    PFN_DwmSetWindowAttribute fn =
        (PFN_DwmSetWindowAttribute)GetProcAddress(dwmapi, "DwmSetWindowAttribute");
    if (!fn) return;

    // Suppress the fullscreen-entry fade animation.
    BOOL val = TRUE;
    fn(hwnd, DBL_DWMWA_TRANSITIONS_FORCEDISABLED, &val, sizeof(val));

    // Keep Aero Peek working normally when the taskbar is in use.
    val = FALSE;
    fn(hwnd, DBL_DWMWA_EXCLUDED_FROM_PEEK, &val, sizeof(val));

    LOGI("applied DWM hints to window %x", (unsigned)(UINT_PTR)hwnd);
}

// ---------------------------------------------------------------------------
// Shell hotkey recovery (plan.md 4.3)
// ---------------------------------------------------------------------------

// A WS_POPUP window that exactly covers a monitor causes Windows to suppress
// shell hotkeys for it, even in windowed mode. A WH_KEYBOARD_LL hook fires
// before that suppression is applied, so we can intercept the affected keys
// and handle them ourselves.
//
// Restored: plain Win tap (Start menu via SC_TASKLIST), Alt+F4 (SC_CLOSE).
// Alt+Tab is unaffected and needs no intervention.
//
// Win+key combinations are swallowed. Forwarding them to the shell requires
// undocumented message IDs that differ between Windows versions, so the only
// safe option is to discard them -- leaving them through would produce
// phantom-key side effects inside the game.

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION)
        return CallNextHookEx(g_kbHook, code, wParam, lParam);

    KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;

    // Skip keystrokes we injected ourselves (avoids infinite loop).
    if (kb->flags & LLKHF_INJECTED)
        return CallNextHookEx(g_kbHook, code, wParam, lParam);

    const BOOL keyUp     = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    const BOOL gameFocus = (GetForegroundWindow() == g_hwnd);

    if (!gameFocus)
        return CallNextHookEx(g_kbHook, code, wParam, lParam);

    // Alt+F4: close the game gracefully. Handle on key-down only.
    if (kb->vkCode == VK_F4 && !keyUp &&
        (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        if (g_hwnd && IsWindow(g_hwnd))
            PostMessageW(g_hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
        return 1;
    }

    // Win key: track held state and trigger shell actions directly.
    // We never inject Win key events — instead we call shell APIs directly
    // to avoid Windows thinking Win is stuck held.
    static BOOL s_winHeld   = FALSE;
    static BOOL s_comboUsed = FALSE;

    if (kb->vkCode == VK_LWIN || kb->vkCode == VK_RWIN) {
        if (!keyUp) {
            s_winHeld   = TRUE;
            s_comboUsed = FALSE;
        } else {
            if (s_winHeld && !s_comboUsed) {
                // Plain Win tap: toggle Start menu.
                // WM_SYSCOMMAND SC_TASKLIST is the documented way to do this.
                HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
                if (tray) PostMessageW(tray, WM_SYSCOMMAND, SC_TASKLIST, 0);
            }
            s_winHeld   = FALSE;
            s_comboUsed = FALSE;
        }
        return 1;  // always swallow Win key from game
    }

    // Any Win+key combo other than plain Win tap: swallow silently.
    // We don't attempt to forward combos to the shell — the undocumented
    // command IDs are version-dependent and unreliable.
    if (s_winHeld && !keyUp) {
        s_comboUsed = TRUE;
        return 1;
    }

    return CallNextHookEx(g_kbHook, code, wParam, lParam);
}

static void InstallKeyboardHook()
{
    if (g_kbHook) return;
    // NULL module handle + 0 thread ID = system-wide low-level hook.
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (g_kbHook) LOGI("installed low-level keyboard hook");
    else          LOGE("SetWindowsHookEx(WH_KEYBOARD_LL) failed (%u)", GetLastError());
}

static void RemoveKeyboardHook()
{
    if (!g_kbHook) return;
    UnhookWindowsHookEx(g_kbHook);
    g_kbHook = NULL;
    LOGI("removed low-level keyboard hook");
}

// ---------------------------------------------------------------------------
// Style / geometry helpers
// ---------------------------------------------------------------------------

// Decoration bits only. WS_MINIMIZE/WS_MAXIMIZE are state, not decoration, and
// are deliberately left alone so we never lie to the game about its own state.
static const DWORD kDecoration =
    WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

static const DWORD kExDecoration =
    WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE;

static DWORD SanitizeStyle(DWORD style)
{
    style &= ~kDecoration;
    style |= WS_POPUP | WS_CLIPSIBLINGS;
    return style;
}

static DWORD SanitizeExStyle(DWORD ex)
{
    ex &= ~kExDecoration;
    if (Cfg().topmost) ex |= WS_EX_TOPMOST;
    else               ex &= ~WS_EX_TOPMOST;
    return ex;
}

struct MonitorSearch {
    int   want;    // 1-based index
    int   seen;
    RECT  rect;
    BOOL  found;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM param)
{
    MonitorSearch* s = (MonitorSearch*)param;
    s->seen++;
    if (s->seen != s->want) return TRUE;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(mon, &mi)) {
        s->rect  = mi.rcMonitor;
        s->found = TRUE;
    }
    return FALSE;
}

// Full-screen rect of the monitor selected by TargetMonitorIndex.
static BOOL GetMonitorRect(HWND hwnd, RECT* out)
{
    const Config& cfg = Cfg();

    if (cfg.targetMonitorIndex > 0) {
        MonitorSearch s;
        s.want  = cfg.targetMonitorIndex;
        s.seen  = 0;
        s.found = FALSE;
        SetRectEmpty(&s.rect);
        EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&s);
        if (s.found) {
            *out = s.rect;
            return TRUE;
        }
        LOGE("TargetMonitorIndex %d not found, falling back to the window's monitor",
             cfg.targetMonitorIndex);
    }

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(mon, &mi)) return FALSE;
    *out = mi.rcMonitor;
    return TRUE;
}

// Final window rect: the monitor rect, or a centred rect when the user pinned
// an explicit resolution.
static BOOL GetTargetRect(HWND hwnd, RECT* out)
{
    RECT mon;
    if (!GetMonitorRect(hwnd, &mon)) return FALSE;

    const Config& cfg = Cfg();
    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;

    int w = cfg.resolutionWidth  > 0 ? cfg.resolutionWidth  : monW;
    int h = cfg.resolutionHeight > 0 ? cfg.resolutionHeight : monH;
    if (w < 64) w = 64;
    if (h < 64) h = 64;

    out->left   = mon.left + (monW - w) / 2;
    out->top    = mon.top + (monH - h) / 2;
    out->right  = out->left + w;
    out->bottom = out->top + h;
    return TRUE;
}

BOOL Win_GetTargetSize(HWND hwnd, int* width, int* height)
{
    RECT rc;
    if (!GetTargetRect(hwnd ? hwnd : g_hwnd, &rc)) return FALSE;
    if (width)  *width  = rc.right - rc.left;
    if (height) *height = rc.bottom - rc.top;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Enforcement
// ---------------------------------------------------------------------------

void Win_Enforce(BOOL async)
{
    HWND hwnd = g_hwnd;
    if (!hwnd || !IsWindow(hwnd)) return;
    if (!Cfg().enabled) return;

    // Our own SetWindowLong/SetWindowPos below re-enter this WndProc; the guard
    // keeps that from recursing.
    if (InterlockedCompareExchange(&g_enforcing, 1, 0) != 0) return;

    RECT target;
    if (GetTargetRect(hwnd, &target)) {
        const DWORD style   = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
        const DWORD exStyle = (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE);
        const DWORD wantStyle   = SanitizeStyle(style);
        const DWORD wantExStyle = SanitizeExStyle(exStyle);

        BOOL restyled = FALSE;
        if (style != wantStyle) {
            SetWindowLongA(hwnd, GWL_STYLE, (LONG)wantStyle);
            restyled = TRUE;
            LOGD("style %x -> %x", style, wantStyle);
        }
        if (exStyle != wantExStyle) {
            SetWindowLongA(hwnd, GWL_EXSTYLE, (LONG)wantExStyle);
            restyled = TRUE;
            LOGD("exstyle %x -> %x", exStyle, wantExStyle);
        }

        RECT current;
        GetWindowRect(hwnd, &current);
        if (restyled || !EqualRect(&current, &target)) {
            UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS;
            if (restyled) flags |= SWP_FRAMECHANGED;
            if (async)    flags |= SWP_ASYNCWINDOWPOS;

            HWND after = Cfg().topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
            SetWindowPos(hwnd, after, target.left, target.top, target.right - target.left,
                         target.bottom - target.top, flags);
            LOGD("enforced rect %d,%d %dx%d", target.left, target.top,
                 target.right - target.left, target.bottom - target.top);
        }
    }

    InterlockedExchange(&g_enforcing, 0);
}

// ---------------------------------------------------------------------------
// Window discovery (plan.md 4.3.1)
// ---------------------------------------------------------------------------

struct WindowSearch {
    DWORD pid;
    HWND  best;
};

static BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM param)
{
    WindowSearch* s = (WindowSearch*)param;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;   // skip tool/owned windows

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    if (rc.right - rc.left < 64 || rc.bottom - rc.top < 64) return TRUE;

    s->best = hwnd;
    return FALSE;
}

HWND Win_FindGameWindow()
{
    WindowSearch s;
    s.pid  = GetCurrentProcessId();
    s.best = NULL;
    EnumWindows(FindWindowProc, (LPARAM)&s);
    return s.best;
}

HWND Win_GetWindow() { return g_hwnd; }

// ---------------------------------------------------------------------------
// Watchdog (plan.md 4.6)
// ---------------------------------------------------------------------------

static DWORD WINAPI WatchdogProc(LPVOID)
{
    const DWORD period = Cfg().enforceIntervalMs;
    LOGI("watchdog started (%u ms)", period);

    for (;;) {
        if (WaitForSingleObject(g_stopEvent, period) == WAIT_OBJECT_0) break;

        HWND hwnd = g_hwnd;
        if (!hwnd || !IsWindow(hwnd)) {
            // The game tore its window down and (probably) built a new one.
            HWND replacement = Win_FindGameWindow();
            if (replacement && replacement != hwnd) {
                LOGI("window recreated, re-attaching to %x", (unsigned)(UINT_PTR)replacement);
                g_hwnd     = NULL;
                g_origProc = NULL;
                Win_Attach(replacement);
            }
            continue;
        }

        // Something else may have replaced our subclass; put it back.
        WNDPROC installed = g_unicode
                                ? (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
                                : (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
        if (Cfg().enableWndProcHook && installed != BorderlessWndProc) {
            LOGI("subclass lost, reinstalling");
            g_origProc = NULL;
            Win_Attach(hwnd);
            continue;
        }

        Win_Enforce(TRUE);
    }

    LOGI("watchdog stopped");
    return 0;
}

static void StartWatchdog()
{
    if (g_watchdog || Cfg().enforceIntervalMs == 0) return;
    if (!g_stopEvent) g_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_watchdog = CreateThread(NULL, 0, WatchdogProc, NULL, 0, NULL);
    if (!g_watchdog) LOGE("CreateThread for watchdog failed (%u)", GetLastError());
}

static void StopWatchdog()
{
    if (!g_watchdog) return;
    if (g_stopEvent) SetEvent(g_stopEvent);
    WaitForSingleObject(g_watchdog, 2000);
    CloseHandle(g_watchdog);
    g_watchdog = NULL;
}

// ---------------------------------------------------------------------------
// Subclass
// ---------------------------------------------------------------------------

static LRESULT CallOriginal(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_origProc) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return g_unicode ? CallWindowProcW(g_origProc, hwnd, msg, wParam, lParam)
                     : CallWindowProcA(g_origProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK BorderlessWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (hwnd != g_hwnd || !Cfg().enabled) return CallOriginal(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_NCCALCSIZE:
        // The client area is the whole window rect. This is what actually makes
        // the window borderless, and it survives the game re-adding WS_CAPTION
        // behind our back.
        return 0;

    case WM_STYLECHANGING: {
        // Intercepts the game's SetWindowLong(GWL_STYLE) at the source, so its
        // style is never applied and then visibly corrected.
        STYLESTRUCT* ss = (STYLESTRUCT*)lParam;
        if (ss) {
            if (wParam == (WPARAM)GWL_STYLE)        ss->styleNew = SanitizeStyle(ss->styleNew);
            else if (wParam == (WPARAM)GWL_EXSTYLE) ss->styleNew = SanitizeExStyle(ss->styleNew);
        }
        break;
    }

    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS* wp = (WINDOWPOS*)lParam;
        RECT target;
        if (wp && !(wp->flags & SWP_HIDEWINDOW) && GetTargetRect(hwnd, &target)) {
            wp->x  = target.left;
            wp->y  = target.top;
            wp->cx = target.right - target.left;
            wp->cy = target.bottom - target.top;
            wp->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
        }
        break;
    }

    case WM_GETMINMAXINFO: {
        LRESULT     result = CallOriginal(hwnd, msg, wParam, lParam);
        MINMAXINFO* mmi    = (MINMAXINFO*)lParam;
        RECT        target;
        if (mmi && GetTargetRect(hwnd, &target)) {
            const int w = target.right - target.left;
            const int h = target.bottom - target.top;
            mmi->ptMinTrackSize.x = 1;
            mmi->ptMinTrackSize.y = 1;
            mmi->ptMaxTrackSize.x = w > mmi->ptMaxTrackSize.x ? w : mmi->ptMaxTrackSize.x;
            mmi->ptMaxTrackSize.y = h > mmi->ptMaxTrackSize.y ? h : mmi->ptMaxTrackSize.y;
            mmi->ptMaxSize.x      = w;
            mmi->ptMaxSize.y      = h;
            mmi->ptMaxPosition.x  = target.left;
            mmi->ptMaxPosition.y  = target.top;
        }
        return result;
    }

    case WM_SYSCOMMAND:
        // plan.md lines 30/38: the documented flicker/minimise symptom. Forcing
        // Windowed=TRUE removes D3D9's own auto-minimise, this covers the game
        // asking for it explicitly.
        if (Cfg().blockMinimizeOnFocusLoss && (wParam & 0xFFF0) == SC_MINIMIZE) {
            LOGD("swallowed SC_MINIMIZE");
            return 0;
        }
        break;

    case WM_SIZE:
    case WM_MOVE:
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_SETFOCUS:
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED: {
        LRESULT result = CallOriginal(hwnd, msg, wParam, lParam);
        Win_Enforce(FALSE);
        return result;
    }

    case WM_NCDESTROY: {
        LOGI("window %x destroyed", (unsigned)(UINT_PTR)hwnd);
        LRESULT result = CallOriginal(hwnd, msg, wParam, lParam);
        if (g_origProc) {
            if (g_unicode) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
            else           SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
        }
        g_hwnd     = NULL;
        g_origProc = NULL;
        return result;
    }

    default:
        break;
    }

    return CallOriginal(hwnd, msg, wParam, lParam);
}

void Win_Attach(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) {
        LOGE("Win_Attach called with an invalid window");
        return;
    }
    if (!Cfg().enabled) return;

    if (g_hwnd == hwnd && g_origProc) {
        Win_Enforce(FALSE);
        return;
    }

    g_hwnd    = hwnd;
    g_unicode = IsWindowUnicode(hwnd);

    char cls[128] = "";
    GetClassNameA(hwnd, cls, sizeof(cls));
    LOGI("attaching to window %x (class '%s', %s)", (unsigned)(UINT_PTR)hwnd, cls,
         g_unicode ? "unicode" : "ansi");

    if (Cfg().enableWndProcHook) {
        WNDPROC previous = g_unicode
                               ? (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc)
                               : (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc);
        if (!previous) {
            LOGE("SetWindowLongPtr(GWLP_WNDPROC) failed (%u)", GetLastError());
        } else if (previous != BorderlessWndProc) {
            g_origProc = previous;
        }
    } else {
        LOGI("EnableWndProcHook=0: geometry is maintained by the watchdog only");
    }

    ApplyDwmHints(hwnd);
    InstallKeyboardHook();
    Win_Enforce(FALSE);
    StartWatchdog();
}

void Win_Detach()
{
    RemoveKeyboardHook();
    StopWatchdog();

    HWND hwnd = g_hwnd;
    if (hwnd && IsWindow(hwnd) && g_origProc) {
        if (g_unicode) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
        else           SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
    }
    g_hwnd     = NULL;
    g_origProc = NULL;

    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
    }
}
