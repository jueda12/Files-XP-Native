#pragma once

#include "../core/settings_codec.h"

namespace filesxp::app
{
    class SettingsStore final
    {
    public:
        [[nodiscard]] static core::AppSettings load() noexcept;
        [[nodiscard]] static bool save(const core::AppSettings& settings) noexcept;
        [[nodiscard]] static bool reset() noexcept;
    };
}
