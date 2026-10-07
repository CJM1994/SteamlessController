#include "ForegroundWatcher.h"

ForegroundWatcher* ForegroundWatcher::s_instance = nullptr;

// The whole of resolving a window to an application now lives in
// ProcessIdentity, shared with the remap window's running-app picker.
ForegroundIdentity ForegroundWatcher::Current() {
    return ProcessIdentity::ForWindow(FrontWindow());
}

// Windows that fill the screen without being anything the user is doing.
static bool IsShellBackground(HWND hwnd) {
    if (hwnd == GetShellWindow() || hwnd == GetDesktopWindow()) return true;
    wchar_t cls[32] = {};
    GetClassNameW(hwnd, cls, 32);
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0
        || wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

HWND ForegroundWatcher::FrontWindow() {
    HWND focus = GetForegroundWindow();
    if (!focus) return focus;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(focus, MONITOR_DEFAULTTONEAREST), &mi)) return focus;
    const RECT& screen = mi.rcMonitor;

    // What a click at the centre would land on. WindowFromPoint already skips
    // hidden and click-through windows, which is what keeps game overlays —
    // FPS counters, chat overlays — from ever being taken for the game.
    const POINT centre{ (screen.left + screen.right) / 2, (screen.top + screen.bottom) / 2 };
    HWND shown = GetAncestor(WindowFromPoint(centre), GA_ROOT);
    if (!shown || shown == focus || IsShellBackground(shown)) return focus;

    DWORD focusPid = 0, shownPid = 0;
    GetWindowThreadProcessId(focus, &focusPid);
    GetWindowThreadProcessId(shown, &shownPid);
    // Another window of the same application is the same answer, and one of
    // ours (the mode overlay, the settings window) is never the user's game.
    if (shownPid == focusPid || shownPid == GetCurrentProcessId()) return focus;

    RECT r{}, overlap{};
    GetWindowRect(shown, &r);
    if (!IntersectRect(&overlap, &r, &screen)) return focus;
    const long long covered = static_cast<long long>(overlap.right - overlap.left)
                            * (overlap.bottom - overlap.top);
    const long long area    = static_cast<long long>(screen.right - screen.left)
                            * (screen.bottom - screen.top);
    return covered * 100 >= area * COVER_PERCENT ? shown : focus;
}

void CALLBACK ForegroundWatcher::HookProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                          LONG idObject, LONG, DWORD, DWORD) {
    if (!s_instance || !s_instance->m_hwnd) return;
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || !hwnd) return;

    // Deliberately does not resolve anything here. The window that raised the
    // event is often not the one focus settles on, so the timer re-reads the
    // foreground when it expires and a burst collapses into one answer.
    SetTimer(s_instance->m_hwnd, IDT_DEBOUNCE, DEBOUNCE_MS, nullptr);
}

LRESULT CALLBACK ForegroundWatcher::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == IDT_DEBOUNCE && s_instance) {
        KillTimer(hwnd, IDT_DEBOUNCE);
        s_instance->OnDebounceElapsed();
        return 0;
    }
    if (msg == WM_TIMER && wp == IDT_POLL && s_instance) {
        s_instance->OnPoll();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ForegroundWatcher::OnPoll() {
    const HWND front = FrontWindow();
    if (front == m_lastFront) return;
    m_lastFront = front;
    // Through the debounce, like a foreground event: a window shuffle mid-
    // launch settles before anything is read from it.
    SetTimer(m_hwnd, IDT_DEBOUNCE, DEBOUNCE_MS, nullptr);
}

void ForegroundWatcher::OnDebounceElapsed() {
    const ForegroundIdentity now = Current();
    // An unidentifiable foreground is not a change worth reporting: it says
    // nothing about what the user switched to, and treating it as "no game"
    // would drop a profile every time focus passed through something we
    // cannot open.
    if (now.Empty() || now == m_last) return;
    m_last = now;
    if (m_onChange) m_onChange(now);
}

bool ForegroundWatcher::Start(ChangedFn onChange) {
    Stop();
    m_onChange = std::move(onChange);
    s_instance = this;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);  // OK if already registered

    // Message-only: it exists purely to own the debounce timer, and must
    // never appear anywhere a user could see it.
    m_hwnd = CreateWindowExW(0, CLASS_NAME, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!m_hwnd) {
        s_instance = nullptr;
        return false;
    }

    // SKIPOWNPROCESS: our own tray menu and settings window taking focus is
    // not the user switching applications, and would otherwise clear whatever
    // profile is running the moment they opened the menu.
    m_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                             nullptr, HookProc, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!m_hook) {
        DestroyWindow(m_hwnd);
        m_hwnd     = nullptr;
        s_instance = nullptr;
        return false;
    }

    m_last      = Current();
    m_lastFront = FrontWindow();
    SetTimer(m_hwnd, IDT_POLL, POLL_MS, nullptr);
    return true;
}

void ForegroundWatcher::Stop() {
    if (m_hook) {
        UnhookWinEvent(m_hook);
        m_hook = nullptr;
    }
    if (m_hwnd) {
        KillTimer(m_hwnd, IDT_DEBOUNCE);
        KillTimer(m_hwnd, IDT_POLL);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (s_instance == this) s_instance = nullptr;
    m_onChange = nullptr;
    m_last      = {};
    m_lastFront = nullptr;
}
