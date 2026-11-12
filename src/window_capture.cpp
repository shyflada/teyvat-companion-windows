#include "teyvat/window_capture.hpp"
#include <algorithm>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace teyvat {
std::optional<WindowSource> inspect_window(quintptr id) {
#ifdef Q_OS_WIN
    const auto window = reinterpret_cast<HWND>(id); RECT rect{}; DWORD process = 0; wchar_t title[256]{};
    if (!IsWindow(window) || !GetClientRect(window, &rect) || !GetWindowThreadProcessId(window, &process)
        || !GetWindowTextW(window, title, 256)) { return {}; }
    return WindowSource{id, process, QString::fromWCharArray(title), QSize(rect.right - rect.left, rect.bottom - rect.top)};
#else
    (void)id; return {};
#endif
}
std::optional<WindowSource> find_game_window() {
#ifdef Q_OS_WIN
    for (const auto* title : {L"Genshin Impact", L"原神"}) {
        if (const auto window = FindWindowW(nullptr, title)) { if (auto source = inspect_window(reinterpret_cast<quintptr>(window))) { return source; } }
    }
#endif
    return {};
}
bool source_active(const WindowSource& source) {
#ifdef Q_OS_WIN
    const auto current = inspect_window(source.handle);
    return current && current->process == source.process && current->title == source.title
        && GetForegroundWindow() == reinterpret_cast<HWND>(source.handle) && !IsIconic(reinterpret_cast<HWND>(source.handle));
#else
    (void)source; return false;
#endif
}
CapturedRegions capture_regions(const WindowSource& source, QSize calibrated_size, const QVector<QRect>& regions, quintptr own_overlay) {
    if (regions.isEmpty() || regions.size() > 2 || calibrated_size.width() < 1 || calibrated_size.height() < 1
        || calibrated_size.width() > 7680 || calibrated_size.height() > 4320) { throw std::invalid_argument("Invalid capture dimensions."); }
    for (const auto& area : regions) {
        if (area.width() < 8 || area.height() < 8 || area.width() > 640 || area.height() > 640
            || !QRect(QPoint(), calibrated_size).contains(area)) { throw std::invalid_argument("Capture regions must fit calibrated client bounds and be at most 640 pixels/side."); }
    }
    CapturedRegions result;
#ifdef Q_OS_WIN
    const auto current = inspect_window(source.handle);
    if (!current || current->process != source.process || current->title != source.title || current->client_size != calibrated_size) { result.reason = CaptureReason::Changed; return result; }
    if (!source_active(source)) { result.reason = CaptureReason::Inactive; return result; }
    const auto window = reinterpret_cast<HWND>(source.handle); POINT origin{};
    if (!ClientToScreen(window, &origin)) { return result; }
    const QRect desktop(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    for (const auto& area : regions) {
        const auto physical = area.translated(origin.x, origin.y);
        if (!desktop.contains(physical)) { result.reason = CaptureReason::Occluded; return result; }
        RECT overlay{};
        if (own_overlay && IsWindowVisible(reinterpret_cast<HWND>(own_overlay)) && GetWindowRect(reinterpret_cast<HWND>(own_overlay), &overlay)
            && physical.intersects(QRect(overlay.left, overlay.top, overlay.right - overlay.left, overlay.bottom - overlay.top))) {
            result.reason = CaptureReason::Occluded; return result;
        }
        for (const auto point : {physical.topLeft(), physical.topRight(), physical.bottomLeft(), physical.bottomRight(), physical.center()}) {
            const POINT p{point.x(), point.y()};
            if (GetAncestor(WindowFromPoint(p), GA_ROOT) != window) { result.reason = CaptureReason::Occluded; return result; }
        }
    }
    for (const auto& area : regions) {
        HDC screen = GetDC(nullptr);
        if (!screen) { result.images.clear(); return result; }
        HDC memory = CreateCompatibleDC(screen); BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = area.width(); info.bmiHeader.biHeight = -area.height();
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr; HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
        const auto previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
        const bool copied = bitmap && previous && BitBlt(memory, 0, 0, area.width(), area.height(), screen, origin.x + area.x(), origin.y + area.y(), SRCCOPY | CAPTUREBLT);
        if (copied) { GdiFlush(); }
        if (copied) { result.images.append(QImage(static_cast<const uchar*>(pixels), area.width(), area.height(), area.width() * 4, QImage::Format_RGB32).copy()); }
        if (previous) { SelectObject(memory, previous); } if (bitmap) { DeleteObject(bitmap); } if (memory) { DeleteDC(memory); } ReleaseDC(nullptr, screen);
        if (!copied) { result.images.clear(); return result; }
    }
    const auto after = inspect_window(source.handle); POINT after_origin{};
    if (!source_active(source) || !after || after->client_size != calibrated_size || !ClientToScreen(window, &after_origin)
        || origin.x != after_origin.x || origin.y != after_origin.y) { result.reason = CaptureReason::Changed; result.images.clear(); return result; }
    result.reason = CaptureReason::Ready;
#else
    (void)source; (void)own_overlay;
#endif
    return result;
}
} // namespace teyvat
