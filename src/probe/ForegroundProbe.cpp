// Console diagnostic: prints the foreground application as the user switches
// between windows, which is the signal per-game profiles are resolved from.
// Exists because the interesting failures here are silent — a packaged app
// resolving to ApplicationFrameHost.exe instead of itself looks like nothing
// happening at all.
//
// With --zorder it instead samples, twice a second, what each of three
// different questions says is "in front": the window with focus, the window
// actually showing at the centre of each monitor, and the top of the z-order.
// They normally agree. A launcher that keeps focus on a splash screen while a
// game draws over it is the case where they do not, and this is what tells
// which of them is lying, and how.
#include "app/ForegroundWatcher.h"
#include <Windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void Print(const wchar_t* tag, const ForegroundIdentity& id) {
    wprintf(L"%-8ls exe=[%ls]\n         aumid=[%ls]\n", tag,
            id.exePath.empty() ? L"(none)" : id.exePath.c_str(),
            id.aumid.empty()   ? L"(none)" : id.aumid.c_str());
    fflush(stdout);
}

// ---- --zorder ------------------------------------------------------------

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n) - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

static std::string ExeOf(HWND hwnd, DWORD& pid) {
    pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return "?";
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring exe = QueryFullProcessImageNameW(proc, 0, path, &size) ? path : L"?";
    CloseHandle(proc);
    const size_t slash = exe.find_last_of(L'\\');
    return Narrow(slash == std::wstring::npos ? exe : exe.substr(slash + 1));
}

// Everything about one top-level window that could make it look in front
// without being so, or the reverse, on one line.
static std::string Describe(HWND hwnd) {
    if (!hwnd) return "(none)";
    DWORD pid = 0;
    const std::string exe = ExeOf(hwnd, pid);

    wchar_t buf[128] = {};
    GetWindowTextW(hwnd, buf, 128);
    std::string title = Narrow(buf);
    if (title.size() > 40) title.resize(40);
    GetClassNameW(hwnd, buf, 128);
    const std::string cls = Narrow(buf);

    RECT r{};
    GetWindowRect(hwnd, &r);
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    std::string flags;
    if (!IsWindowVisible(hwnd))       flags += " HIDDEN";
    if (IsIconic(hwnd))               flags += " MINIMIZED";
    if (cloaked)                      flags += " CLOAKED";
    if (ex & WS_EX_TOPMOST)           flags += " TOPMOST";
    if (ex & WS_EX_TOOLWINDOW)        flags += " TOOL";
    if (ex & WS_EX_NOACTIVATE)        flags += " NOACTIVATE";
    if (ex & WS_EX_TRANSPARENT)       flags += " CLICKTHROUGH";
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD how = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &how) && (how & LWA_ALPHA))
            flags += " LAYERED(alpha=" + std::to_string(alpha) + ")";
        else
            flags += " LAYERED";
    }
    if (GetWindow(hwnd, GW_OWNER))    flags += " OWNED";

    char line[512];
    snprintf(line, sizeof(line), "%s pid=%lu hwnd=%p \"%s\" class=%s rect=%ld,%ld %ldx%ld%s",
             exe.c_str(), pid, static_cast<void*>(hwnd), title.c_str(), cls.c_str(),
             r.left, r.top, r.right - r.left, r.bottom - r.top, flags.c_str());
    return line;
}

static BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(mon);
    return TRUE;
}

struct TopWindows { std::vector<HWND> list; size_t max = 6; };
static BOOL CALLBACK CollectTop(HWND hwnd, LPARAM lp) {
    auto* top = reinterpret_cast<TopWindows*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT r{};
    GetWindowRect(hwnd, &r);
    if (r.right - r.left < 50 || r.bottom - r.top < 50) return TRUE;  // slivers and dots
    top->list.push_back(hwnd);
    return top->list.size() < top->max;
}

static std::string Snapshot() {
    std::string out = "  FOCUS   " + Describe(GetForegroundWindow()) + "\n"
                    + "  FRONT   " + Describe(ForegroundWatcher::FrontWindow())
                    + "   <- what the app uses\n";

    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    for (size_t i = 0; i < monitors.size(); ++i) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitors[i], &mi);
        const POINT centre{ (mi.rcMonitor.left + mi.rcMonitor.right) / 2,
                            (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2 };
        // WindowFromPoint skips click-through and hidden windows, which is
        // exactly the "what would a click land on" answer wanted here.
        HWND at = GetAncestor(WindowFromPoint(centre), GA_ROOT);
        out += "  CENTRE" + std::to_string(i) + " " + Describe(at) + "\n";
    }

    TopWindows top;
    EnumWindows(CollectTop, reinterpret_cast<LPARAM>(&top));  // top of z-order first
    for (size_t i = 0; i < top.list.size(); ++i)
        out += "  Z" + std::to_string(i) + "      " + Describe(top.list[i]) + "\n";
    return out;
}

static int ZOrder(int seconds) {
    wchar_t local[MAX_PATH];
    std::wstring logPath;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        logPath = std::wstring(local) + L"\\SteamlessController\\zorder.log";
    FILE* log = nullptr;
    if (!logPath.empty()) _wfopen_s(&log, logPath.c_str(), L"w");

    printf("Sampling for %d seconds. Reproduce the problem now.\n", seconds);
    if (log) printf("Also writing to %s\n", Narrow(logPath).c_str());
    fflush(stdout);

    std::string last;
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000) {
        const std::string now = Snapshot();
        // Only on a change, so a minute of output is a handful of blocks
        // rather than a hundred identical ones.
        if (now != last) {
            last = now;
            SYSTEMTIME st;
            GetLocalTime(&st);
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            fputs(stamp, stdout);
            fputs(now.c_str(), stdout);
            fflush(stdout);
            if (log) { fputs(stamp, log); fputs(now.c_str(), log); fflush(log); }
        }
        Sleep(500);
    }
    if (log) fclose(log);
    printf("Done.\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--zorder") == 0)
        return ZOrder(argc > 2 ? atoi(argv[2]) : 60);

    const int seconds = argc > 1 ? atoi(argv[1]) : 25;

    Print(L"INITIAL", ForegroundWatcher::Current());
    wprintf(L"Watching for %d seconds — switch windows now.\n", seconds);
    fflush(stdout);

    ForegroundWatcher watcher;
    if (!watcher.Start([](const ForegroundIdentity& id) { Print(L"CHANGED", id); })) {
        wprintf(L"Failed to install the foreground hook.\n");
        return 1;
    }

    // The hook is out-of-context, so its callback arrives through this
    // thread's message queue — without a pump nothing is ever delivered.
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    return 0;
}
