#pragma once

#include <windows.h>

namespace filesxp::app
{
    inline constexpr UINT archiveCompleteMessage = WM_APP + 10;
    inline constexpr UINT archiveProgressMessage = WM_APP + 11;
    [[nodiscard]] int runArchiveWorker(int argumentCount, wchar_t** arguments) noexcept;
}
