#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace filesxp::core
{
    inline constexpr std::size_t maxShelfItems = 4096;

    [[nodiscard]] inline bool validShelfOrder(std::span<const std::uint32_t> order,
        std::uint32_t sourceCount, std::size_t maximumItems)
    {
        if (order.empty() || maximumItems == 0 || maximumItems > maxShelfItems ||
            order.size() > maximumItems || sourceCount == 0 || order.size() > sourceCount)
            return false;
        std::array<std::uint32_t, maxShelfItems> sorted{};
        std::copy(order.begin(), order.end(), sorted.begin());
        const auto end = sorted.begin() + static_cast<std::ptrdiff_t>(order.size());
        if (std::any_of(sorted.begin(), end, [sourceCount](std::uint32_t index)
                { return index >= sourceCount; }))
            return false;
        std::sort(sorted.begin(), end);
        return std::adjacent_find(sorted.begin(), end) == end;
    }
}
