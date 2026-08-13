#include "xp_theme.h"

#include <commctrl.h>
#include <iterator>
#include <string>

namespace filesxp::app::xp
{
    bool isHighContrast() noexcept
    {
        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
            (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }

    HFONT createUiFont(int dpi)
    {
        LOGFONTW font{};
        font.lfHeight = -MulDiv(9, dpi, 72);
        font.lfWeight = FW_NORMAL;
        font.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(font.lfFaceName, L"Tahoma");
        return CreateFontIndirectW(&font);
    }

    void fillVerticalGradient(HDC dc, const RECT& bounds, COLORREF top, COLORREF bottom)
    {
        TRIVERTEX vertices[2]{};
        vertices[0] = {bounds.left, bounds.top,
            static_cast<COLOR16>(GetRValue(top) << 8),
            static_cast<COLOR16>(GetGValue(top) << 8),
            static_cast<COLOR16>(GetBValue(top) << 8), 0xFF00};
        vertices[1] = {bounds.right, bounds.bottom,
            static_cast<COLOR16>(GetRValue(bottom) << 8),
            static_cast<COLOR16>(GetGValue(bottom) << 8),
            static_cast<COLOR16>(GetBValue(bottom) << 8), 0xFF00};
        GRADIENT_RECT gradient{0, 1};
        GradientFill(dc, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);
    }

    void drawButton(const DRAWITEMSTRUCT& item, bool greenButton)
    {
        RECT bounds = item.rcItem;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        if (isHighContrast())
        {
            const int background = pressed ? COLOR_HIGHLIGHT : COLOR_BTNFACE;
            FillRect(item.hDC, &bounds, GetSysColorBrush(background));
            FrameRect(item.hDC, &bounds, GetSysColorBrush(COLOR_WINDOWTEXT));
            wchar_t text[64]{};
            GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, GetSysColor(disabled ? COLOR_GRAYTEXT :
                (pressed ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT)));
            DrawTextW(item.hDC, text, -1, &bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if ((item.itemState & ODS_FOCUS) != 0)
            {
                InflateRect(&bounds, -3, -3);
                DrawFocusRect(item.hDC, &bounds);
            }
            return;
        }
        const auto top = greenButton ? RGB(134, 202, 103) : blueLight;
        const auto bottom = greenButton ? green : blue;
        fillVerticalGradient(item.hDC, bounds, pressed ? bottom : top, pressed ? top : bottom);

        const auto oldPen = SelectObject(item.hDC, GetStockObject(DC_PEN));
        const auto oldBrush = SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
        SetDCPenColor(item.hDC, greenButton ? RGB(33, 110, 30) : blueDark);
        Rectangle(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom);
        SelectObject(item.hDC, oldBrush);
        SelectObject(item.hDC, oldPen);

        wchar_t text[64]{};
        GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, disabled ? RGB(190, 190, 190) : RGB(255, 255, 255));
        if (pressed)
        {
            OffsetRect(&bounds, 1, 1);
        }
        DrawTextW(item.hDC, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if ((item.itemState & ODS_FOCUS) != 0)
        {
            InflateRect(&bounds, -3, -3);
            DrawFocusRect(item.hDC, &bounds);
        }
    }

    void drawPlacesItem(const DRAWITEMSTRUCT& item)
    {
        if (item.itemID == static_cast<UINT>(-1))
        {
            return;
        }

        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        const bool highContrast = isHighContrast();
        if (highContrast)
        {
            FillRect(item.hDC, &item.rcItem,
                GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        }
        else
        {
            HBRUSH background = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
            SetDCBrushColor(item.hDC, selected ? selection : paneBlue);
            FillRect(item.hDC, &item.rcItem, background);
        }

        wchar_t text[MAX_PATH]{};
        SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(text));
        RECT textBounds = item.rcItem;
        textBounds.left += 12;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, highContrast
            ? GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)
            : (selected ? RGB(255, 255, 255) : blueDark));
        DrawTextW(item.hDC, text, -1, &textBounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if ((item.itemState & ODS_FOCUS) != 0)
        {
            DrawFocusRect(item.hDC, &item.rcItem);
        }
    }
}
