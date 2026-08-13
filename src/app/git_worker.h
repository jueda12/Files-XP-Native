#pragma once

#include <windows.h>

namespace filesxp::app
{
    inline constexpr UINT gitCompleteMessage = WM_APP + 9;
    [[nodiscard]] int runGitWorker(int argumentCount, wchar_t** arguments) noexcept;
}
