#pragma once

#include <windows.h>

namespace filesxp::app
{
    inline constexpr UINT ftpCompleteMessage = WM_APP + 17;
    [[nodiscard]] int runFtpWorker(int argumentCount, wchar_t** arguments) noexcept;
}
