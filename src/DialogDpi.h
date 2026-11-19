// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "NativeDpi.h"
#include <algorithm>

namespace NativeDpi {
inline UINT Metric(int index, UINT dpi) {
    using Query = int (WINAPI*)(int, UINT);
    static const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    return query ? query(index, dpi) : GetSystemMetrics(index);
}

inline void KeepDialogControlFont(HWND control) {
    using SetBehavior = BOOL (WINAPI*)(HWND, DWORD, DWORD);
    static const auto setBehavior = reinterpret_cast<SetBehavior>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetDialogControlDpiChangeBehavior"));
    constexpr DWORD disableFontUpdate = 1;
    if (setBehavior) setBehavior(control, disableFontUpdate, disableFontUpdate);
}

inline void CenterPopup(HWND popup, HWND anchor) {
    if (!anchor || !IsWindow(anchor)) return;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    RECT relative = monitor.rcWork;
    if (!IsIconic(anchor)) GetWindowRect(anchor, &relative);
    auto center = [&] {
        RECT current{};
        if (!GetWindowRect(popup, &current)) return;
        const LONG width = current.right-current.left, height = current.bottom-current.top;
        const LONG x = std::clamp(relative.left+(relative.right-relative.left-width)/2,
            monitor.rcWork.left, (std::max)(monitor.rcWork.left, monitor.rcWork.right-width));
        const LONG y = std::clamp(relative.top+(relative.bottom-relative.top-height)/2,
            monitor.rcWork.top, (std::max)(monitor.rcWork.top, monitor.rcWork.bottom-height));
        SetWindowPos(popup, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const auto before = Window(popup);
    center();
    // A move across monitors can synchronously resize the dialog template.
    if (Window(popup) != before) center();
}
}
