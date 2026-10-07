#pragma once
#include <Windows.h>
#include <functional>
#include <string>
#include "ProcessIdentity.h"

// Reports which application the user has switched to — event-driven, with a
// slow look at the stacking order as a backstop (see FrontWindow and OnPoll).
//
// Built on SetWinEventHook(EVENT_SYSTEM_FOREGROUND), which is the only
// push-based answer to "what is the user doing now" available to a process
// that never runs elevated: the event-driven alternatives for watching
// processes start (Win32_ProcessStartTrace, an ETW kernel session) all
// require administrator rights, and the unelevated WMI route polls
// internally. Registering costs one hook and no thread; the callback only
// runs when focus actually changes, which is a human-scale rate.
//
// Focus is also the better question than "did a game launch". A launcher
// that starts a game and stays resident — Ubisoft Connect, the EA app —
// keeps its process alive long after the game exits, so "is it running"
// answers yes for far too long. What is in front does not have that problem,
// and it means alt-tabbing to a browser hands the desktop its own settings
// back.
class ForegroundWatcher {
public:
    using ChangedFn = std::function<void(const ForegroundIdentity&)>;

    ~ForegroundWatcher() { Stop(); }

    // Begins reporting. The callback runs on the calling thread, which must
    // be the one pumping messages — out-of-context hooks are delivered
    // through the message queue, and the debounce below is a window timer.
    bool Start(ChangedFn onChange);
    void Stop();

    // Resolve the application in front right now, for callers that need to
    // catch up rather than wait for the next switch (taking the controller
    // back, say, when the user has been sitting in a game the whole time).
    static ForegroundIdentity Current();

    // The window that is in front as the user sees it. Usually the one with
    // focus — but a launcher can keep focus on a splash or startup window
    // while the game it launched covers the screen, and a game can take focus
    // while a launcher's always-on-top intro still covers it. Either way what
    // is on screen is the honest answer, so a different application's window
    // that fills the focused window's monitor, and is what shows at its
    // centre, wins over focus.
    static HWND FrontWindow();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK HookProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild,
                                  DWORD thread, DWORD time);
    void OnDebounceElapsed();
    // Windows changing places raises no foreground event, so the stacking is
    // also looked at on a slow timer; a change goes through the same debounce.
    void OnPoll();

    HWINEVENTHOOK m_hook   = nullptr;
    HWND          m_hwnd   = nullptr;  // message-only, owns the debounce timer
    ChangedFn     m_onChange;
    ForegroundIdentity m_last;
    HWND          m_lastFront = nullptr;  // what the poll last saw

    static ForegroundWatcher* s_instance;

    static constexpr wchar_t CLASS_NAME[] = L"SteamlessForegroundWatcher";
    static constexpr UINT_PTR IDT_DEBOUNCE = 1;
    // Focus churns while an application starts — splash screens, launcher
    // windows, the game's own window replacing its loader — and every one of
    // those is a foreground change. Settling first keeps a burst from being
    // read as several different applications, which matters because a
    // profile switch can rebuild the virtual controller.
    static constexpr UINT DEBOUNCE_MS = 400;
    static constexpr UINT_PTR IDT_POLL = 2;
    // Cheap — one hit test and a rectangle — and only acted on when the
    // answer moves, so this is about how late a covered focus is noticed.
    static constexpr UINT POLL_MS = 750;
    // How much of the monitor a window must cover to count as being in front
    // of the focused one. High enough that a dialog or a corner overlay never
    // does; low enough to forgive a borderless game a few pixels short.
    static constexpr int COVER_PERCENT = 90;
};
