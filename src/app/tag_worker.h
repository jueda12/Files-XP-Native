#pragma once

#include <windows.h>

namespace filesxp::app
{
    inline constexpr UINT tagSearchCompleteMessage = WM_APP + 6;

    [[nodiscard]] int runTagSetWorker(int argumentCount, wchar_t** arguments) noexcept;
    [[nodiscard]] int runTagSearchWorker(int argumentCount, wchar_t** arguments) noexcept;
}
