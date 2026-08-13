#pragma once

#include <windows.h>

namespace filesxp::app
{
    inline constexpr UINT textPreviewCompleteMessage = WM_APP + 7;
    inline constexpr UINT previewPopupCompleteMessage = WM_APP + 8;

    [[nodiscard]] int runTextPreviewWorker(int argumentCount, wchar_t** arguments) noexcept;
    [[nodiscard]] int runPreviewPopupWorker(int argumentCount, wchar_t** arguments) noexcept;
}
