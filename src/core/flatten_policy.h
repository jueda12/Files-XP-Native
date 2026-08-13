#pragma once

#include <cstddef>

namespace filesxp::core
{
    inline constexpr std::size_t maxFlattenItems = 100000;
    inline constexpr std::size_t maxFlattenDepth = 256;

    enum class FlattenAction
    {
        ignore,
        move,
        descend
    };

    [[nodiscard]] constexpr FlattenAction flattenAction(bool insideSubfolder,
        bool directory, bool reparsePoint, std::size_t depth) noexcept
    {
        if (directory && !reparsePoint && depth < maxFlattenDepth)
        {
            return FlattenAction::descend;
        }
        return insideSubfolder ? FlattenAction::move : FlattenAction::ignore;
    }
}
