#include "ModeOverlay.h"
#include <memory>
#include <vector>
// GDI+ wants min/max as macros and Windows.h is included without NOMINMAX
// elsewhere; the headers bring their own if these are missing.
#include <objidl.h>
#include <gdiplus.h>

namespace {

// An icon's colour plane with its alpha kept. Gdiplus::Bitmap::FromHICON
// drops the alpha channel, which turns the controller's anti-aliased edge
// into a black fringe.
std::unique_ptr<Gdiplus::Bitmap> IconBitmap(HICON icon) {
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return nullptr;
    std::unique_ptr<Gdiplus::Bitmap> out;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmBitsPixel == 32) {
        const int w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;  // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        HDC screen = GetDC(nullptr);
        const bool ok = GetDIBits(screen, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h;
        ReleaseDC(nullptr, screen);
        if (ok) {
            out = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppARGB);
            Gdiplus::BitmapData data{};
            Gdiplus::Rect all(0, 0, w, h);
            if (out->LockBits(&all, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data)
                    == Gdiplus::Ok) {
                for (int y = 0; y < h; ++y)
                    memcpy(static_cast<BYTE*>(data.Scan0) + static_cast<size_t>(y) * data.Stride,
                           px.data() + static_cast<size_t>(y) * w, static_cast<size_t>(w) * 4);
                out->UnlockBits(&data);
            }
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return out;
}

void RoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    const float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

}  // namespace

ModeOverlay::~ModeOverlay() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    if (m_memDC) {
        SelectObject(m_memDC, m_oldBitmap);
        DeleteDC(m_memDC);
    }
    if (m_bitmap) DeleteObject(m_bitmap);
    if (m_gdiplusToken) Gdiplus::GdiplusShutdown(m_gdiplusToken);
}

LRESULT CALLBACK ModeOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ModeOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_TIMER && wp == IDT_FRAME && self) {
        self->Tick();
        return 0;
    }
    // Belt and braces with WS_EX_NOACTIVATE: never take focus from a game.
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ModeOverlay::EnsureWindow(HINSTANCE hInst) {
    if (m_hwnd) return true;
    if (!m_gdiplusToken) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
            m_gdiplusToken = 0;
            return false;
        }
    }
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);  // OK if already registered

    // TOOLWINDOW keeps it off the taskbar and Alt+Tab; TRANSPARENT passes
    // every click through to whatever is underneath.
    m_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST
                                 | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             CLASS_NAME, L"", WS_POPUP, 0, 0, 1, 1,
                             nullptr, nullptr, hInst, nullptr);
    if (!m_hwnd) return false;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

void ModeOverlay::Show(HINSTANCE hInst, int iconId, COLORREF accent, const std::wstring& title,
                       const std::wstring& detail, const std::wstring& reason,
                       HMONITOR monitor) {
    if (!EnsureWindow(hInst)) return;
    Render(hInst, iconId, accent, title, detail, reason, monitor);

    // Re-asserted on every show: a launcher's own always-on-top window (Big
    // Box's startup screen) may have gone up since, and the newest topmost
    // window sits above the older ones.
    SetWindowPos(m_hwnd, HWND_TOPMOST, m_pos.x, m_pos.y, m_size.cx, m_size.cy,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // A notice already fully up just takes the new content and starts its
    // time over; anything else fades in afresh.
    if (m_phase == Phase::Holding) {
        m_phaseStart = GetTickCount64();
        SetAlpha(255);
    } else {
        m_phase      = Phase::FadingIn;
        m_phaseStart = GetTickCount64();
        SetAlpha(0);
    }
    SetTimer(m_hwnd, IDT_FRAME, FRAME_MS, nullptr);
}

void ModeOverlay::Render(HINSTANCE hInst, int iconId, COLORREF accent,
                         const std::wstring& title, const std::wstring& detail,
                         const std::wstring& reason, HMONITOR monitor) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);

    // The DPI of the monitor it is about to sit on: move there first, then
    // ask, since the answer belongs to where the window is.
    SetWindowPos(m_hwnd, nullptr, mi.rcWork.left, mi.rcWork.top, 1, 1,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    const float scale = static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f;
    m_size = { static_cast<LONG>(WIDTH * scale), static_cast<LONG>(HEIGHT * scale) };
    const int margin = static_cast<int>(MARGIN * scale);
    m_pos  = { mi.rcWork.right - m_size.cx - margin, mi.rcWork.top + margin };

    // A fresh top-down 32-bit DIB per show: the size follows the monitor.
    if (m_memDC) {
        SelectObject(m_memDC, m_oldBitmap);
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    if (m_bitmap) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = m_size.cx;
    bi.bmiHeader.biHeight      = -m_size.cy;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    m_memDC  = CreateCompatibleDC(screen);
    m_bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!m_bitmap || !bits) return;
    m_oldBitmap = SelectObject(m_memDC, m_bitmap);

    // Drawn straight into the DIB as premultiplied ARGB, which is the format
    // UpdateLayeredWindow composites.
    Gdiplus::Bitmap canvas(m_size.cx, m_size.cy, m_size.cx * 4, PixelFormat32bppPARGB,
                           static_cast<BYTE*>(bits));
    Gdiplus::Graphics g(&canvas);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float w = static_cast<float>(m_size.cx), h = static_cast<float>(m_size.cy);
    const float radius = 14 * scale;
    const Gdiplus::Color tint(255, GetRValue(accent), GetGValue(accent), GetBValue(accent));

    // Panel: near-black slate, edged in the mode's colour.
    {
        Gdiplus::GraphicsPath panel;
        RoundedRect(panel, 1, 1, w - 2, h - 2, radius);
        Gdiplus::SolidBrush fill(Gdiplus::Color(238, 17, 23, 31));
        g.FillPath(&fill, &panel);
        Gdiplus::Pen edge(Gdiplus::Color(150, tint.GetR(), tint.GetG(), tint.GetB()),
                          1.5f * scale);
        g.DrawPath(&edge, &panel);
    }

    // The coloured controller, on a soft disc of its own colour.
    const float pad   = 16 * scale;
    const float iconD = h - pad * 2;
    {
        Gdiplus::SolidBrush halo(Gdiplus::Color(40, tint.GetR(), tint.GetG(), tint.GetB()));
        g.FillEllipse(&halo, pad, pad, iconD, iconD);
        // The largest frame, scaled down here, looks far better than the
        // 32 or 48 pixel ones scaled up.
        HICON icon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
                                                   256, 256, LR_DEFAULTCOLOR));
        if (icon) {
            if (auto bmp = IconBitmap(icon)) {
                const float inset = iconD * 0.16f;
                g.DrawImage(bmp.get(), pad + inset, pad + inset,
                            iconD - inset * 2, iconD - inset * 2);
            }
            DestroyIcon(icon);
        }
    }

    // Three lines of text: the mode, what it means, and why it changed.
    const float textX = pad * 2 + iconD - 4 * scale;
    const float textW = w - textX - pad;
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font titleFont(&family, 17 * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font bodyFont(&family, 13 * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font smallFont(&family, 12 * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    Gdiplus::SolidBrush white(Gdiplus::Color(255, 244, 247, 250));
    Gdiplus::SolidBrush body(Gdiplus::Color(255, 184, 194, 204));
    Gdiplus::SolidBrush dim(Gdiplus::Color(255, 122, 136, 150));
    const float lineTop = h / 2 - 34 * scale;
    g.DrawString(title.c_str(), -1, &titleFont,
                 Gdiplus::RectF(textX, lineTop, textW, 24 * scale), &fmt, &white);
    g.DrawString(detail.c_str(), -1, &bodyFont,
                 Gdiplus::RectF(textX, lineTop + 25 * scale, textW, 20 * scale), &fmt, &body);
    g.DrawString(reason.c_str(), -1, &smallFont,
                 Gdiplus::RectF(textX, lineTop + 46 * scale, textW, 20 * scale), &fmt, &dim);
    g.Flush(Gdiplus::FlushIntentionSync);
}

void ModeOverlay::SetAlpha(BYTE alpha) {
    if (!m_hwnd || !m_memDC) return;
    POINT src{ 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(m_hwnd, nullptr, &m_pos, &m_size, m_memDC, &src, 0, &blend, ULW_ALPHA);
}

void ModeOverlay::Tick() {
    const ULONGLONG elapsed = GetTickCount64() - m_phaseStart;
    auto ease = [](double t) { return 1 - (1 - t) * (1 - t); };  // ease-out
    switch (m_phase) {
    case Phase::FadingIn:
        if (elapsed >= FADE_IN_MS) {
            m_phase      = Phase::Holding;
            m_phaseStart = GetTickCount64();
            SetAlpha(255);
        } else {
            SetAlpha(static_cast<BYTE>(255 * ease(static_cast<double>(elapsed) / FADE_IN_MS)));
        }
        break;
    case Phase::Holding:
        if (elapsed >= HOLD_MS) {
            m_phase      = Phase::FadingOut;
            m_phaseStart = GetTickCount64();
        }
        break;
    case Phase::FadingOut:
        if (elapsed >= FADE_OUT_MS) {
            m_phase = Phase::Hidden;
            KillTimer(m_hwnd, IDT_FRAME);
            ShowWindow(m_hwnd, SW_HIDE);
        } else {
            SetAlpha(static_cast<BYTE>(
                255 * (1 - ease(static_cast<double>(elapsed) / FADE_OUT_MS))));
        }
        break;
    case Phase::Hidden:
        KillTimer(m_hwnd, IDT_FRAME);
        break;
    }
}
