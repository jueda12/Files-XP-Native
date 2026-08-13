#pragma once

#include <cstdint>

namespace filesxp::core
{
    inline constexpr std::uint32_t maxTagColor = 7;

    [[nodiscard]] constexpr bool validTagColor(std::uint32_t color) noexcept
    {
        return color <= maxTagColor;
    }
}
