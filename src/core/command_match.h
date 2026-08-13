#pragma once

#include <cwctype>
#include <limits>
#include <string_view>

namespace filesxp::core
{
    inline constexpr int noCommandMatch = std::numeric_limits<int>::max();

    [[nodiscard]] inline int commandMatchScore(std::wstring_view label,
        std::wstring_view query) noexcept
    {
        if (query.empty())
        {
            return 0;
        }
        std::size_t queryIndex{};
        int score{};
        int gap{};
        for (wchar_t character : label)
        {
            if (queryIndex < query.size() && std::towlower(character) == std::towlower(query[queryIndex]))
            {
                score += gap;
                gap = 0;
                ++queryIndex;
                if (queryIndex == query.size())
                {
                    return score + static_cast<int>(label.size() - query.size());
                }
            }
            else
            {
                ++gap;
            }
        }
        return noCommandMatch;
    }
}
