#pragma once

#include <cstddef>

namespace filesxp::core
{
    inline constexpr std::size_t maxCopyDataPathCharacters = 32768;

    [[nodiscard]] inline bool validCopyDataPath(const wchar_t* value,
        std::size_t characters) noexcept
    {
        if (value == nullptr || characters < 2 ||
            characters > maxCopyDataPathCharacters || value[characters - 1] != L'\0')
            return false;
        for (std::size_t index = 0; index + 1 < characters; ++index)
        {
            if (value[index] == L'\0') return false;
        }
        return true;
    }
}
