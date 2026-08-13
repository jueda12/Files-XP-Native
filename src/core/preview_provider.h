#pragma once

#include <cstdint>

namespace filesxp::core
{
    enum class PreviewProvider : std::uint32_t
    {
        automatic,
        windows,
        quickLook,
        seer,
        powerToys
    };

    [[nodiscard]] constexpr bool validPreviewProvider(std::uint32_t value) noexcept
    {
        return value <= static_cast<std::uint32_t>(PreviewProvider::powerToys);
    }
}
