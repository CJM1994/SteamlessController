#pragma once
#include <Windows.h>
#include <string>

// The on-screen notice raised when the controller switches mode.
//
// A window of our own rather than a Windows notification, for two reasons
// that both bit: Do Not Disturb turns on by itself whenever a game or any
// full-screen app is running — which is exactly when mode switches happen —
// and swallows notifications without a trace; and a notification cannot be
// made to look like anything but a notification.
//
// Always on top, click-through and never activated, so it cannot take focus
// from a game or be mistaken for the application in front (see
// ForegroundWatcher::FrontWindow, which also skips our own windows). Drawn
// over borderless and windowed games; nothing outside a game process can draw
// over one in exclusive fullscreen.
class ModeOverlay {
public:
    ModeOverlay() = default;
    ~ModeOverlay();
    ModeOverlay(const ModeOverlay&) = delete;
    ModeOverlay& operator=(const ModeOverlay&) = delete;

    // Shows the notice, or replaces the one showing and starts its time over.
    // iconId is a resource icon in hInst; accent tints the panel's edge.
    // Placed at the top right of `monitor`.
    void Show(HINSTANCE hInst, int iconId, COLORREF accent, const std::wstring& title,
              const std::wstring& detail, const std::wstring& reason, HMONITOR monitor);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool EnsureWindow(HINSTANCE hInst);
    // Draws the panel once into a premultiplied 32-bit bitmap and hands it to
    // the layered window; fading only changes the constant alpha after that.
    void Render(HINSTANCE hInst, int iconId, COLORREF accent, const std::wstring& title,
                const std::wstring& detail, const std::wstring& reason, HMONITOR monitor);
    void SetAlpha(BYTE alpha);
    void Tick();

    HWND      m_hwnd  = nullptr;
    ULONG_PTR m_gdiplusToken = 0;
    // Where the bitmap went last, so fading can re-present it without
    // drawing it again.
    HDC       m_memDC = nullptr;
    HBITMAP   m_bitmap = nullptr;
    HGDIOBJ   m_oldBitmap = nullptr;
    POINT     m_pos{};
    SIZE      m_size{};

    enum class Phase { Hidden, FadingIn, Holding, FadingOut };
    Phase     m_phase = Phase::Hidden;
    ULONGLONG m_phaseStart = 0;

    static constexpr wchar_t CLASS_NAME[] = L"SteamlessModeOverlay";
    static constexpr UINT_PTR IDT_FRAME = 1;
    static constexpr UINT FRAME_MS    = 15;
    static constexpr UINT FADE_IN_MS  = 160;
    static constexpr UINT HOLD_MS     = 2400;
    static constexpr UINT FADE_OUT_MS = 380;
    // Design sizes at 96 DPI; scaled to the monitor shown on.
    static constexpr int WIDTH  = 360;
    static constexpr int HEIGHT = 96;
    static constexpr int MARGIN = 24;
};
