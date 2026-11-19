// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>

namespace NativeDpi {
constexpr UINT Base = 96;
constexpr UINT Changed = 0x02e0;
constexpr UINT AfterParent = 0x02e3;

inline UINT System() {
    HDC screen = GetDC(nullptr);
    if (!screen) return Base;
    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? UINT(dpi) : Base;
}

inline UINT Window(HWND window) {
    using Query = UINT (WINAPI*)(HWND);
    static const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = query && window ? query(window) : 0;
    return dpi ? dpi : System();
}

inline int Scale(int value, UINT dpi, UINT from = Base) {
    return MulDiv(value, dpi, from);
}

// Keep the exact source measurement until the user changes it. This also
// preserves sizes first measured on a fractional-scale monitor.
class Measurement {
public:
    int Resize(int pixels, UINT from, UINT to) {
        if (!sourceDpi_ || Scale(source_, from, sourceDpi_) != pixels) {
            source_ = pixels;
            sourceDpi_ = from;
        }
        return Scale(source_, to, sourceDpi_);
    }

private:
    int source_ = 0;
    UINT sourceDpi_ = 0;
};
}
