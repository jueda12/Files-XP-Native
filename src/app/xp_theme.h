#pragma once

#include <windows.h>

namespace filesxp::app::xp
{
    inline constexpr COLORREF blueDark = RGB(0, 51, 153);
    inline constexpr COLORREF blue = RGB(36, 91, 216);
    inline constexpr COLORREF blueLight = RGB(111, 151, 255);
    inline constexpr COLORREF paneBlue = RGB(214, 223, 247);
    inline constexpr COLORREF paneHeader = RGB(49, 106, 197);
    inline constexpr COLORREF toolbarBeige = RGB(239, 237, 222);
    inline constexpr COLORREF border = RGB(125, 139, 173);
    inline constexpr COLORREF selection = RGB(49, 106, 197);
    inline constexpr COLORREF green = RGB(61, 149, 48);

    [[nodiscard]] HFONT createUiFont(int dpi);
    [[nodiscard]] bool isHighContrast() noexcept;
    void drawButton(const DRAWITEMSTRUCT& item, bool greenButton = false);
    void drawPlacesItem(const DRAWITEMSTRUCT& item);
    void fillVerticalGradient(HDC dc, const RECT& bounds, COLORREF top, COLORREF bottom);
}

