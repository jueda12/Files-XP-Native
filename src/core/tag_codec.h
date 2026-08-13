#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxTags = 16;
    inline constexpr std::size_t maxTagLength = 64;

    [[nodiscard]] inline bool normalizeTags(std::wstring_view input,
        std::vector<std::wstring>& tags)
    {
        tags.clear();
        std::size_t start{};
        while (start <= input.size())
        {
            const std::size_t delimiter = input.find(L';', start);
            const std::size_t end = delimiter == std::wstring_view::npos ? input.size() : delimiter;
            std::wstring_view value = input.substr(start, end - start);
            while (!value.empty() && std::iswspace(value.front()) != 0) value.remove_prefix(1);
            while (!value.empty() && std::iswspace(value.back()) != 0) value.remove_suffix(1);
            if (!value.empty())
            {
                if (value.size() > maxTagLength || std::any_of(value.begin(), value.end(), [](wchar_t character)
                    {
                        return character < L' ' || character == L'"';
                    }))
                {
                    tags.clear();
                    return false;
                }
                const bool duplicate = std::any_of(tags.begin(), tags.end(), [value](const std::wstring& existing)
                {
                    return existing.size() == value.size() && std::equal(existing.begin(), existing.end(),
                        value.begin(), [](wchar_t left, wchar_t right)
                        {
                            return std::towlower(left) == std::towlower(right);
                        });
                });
                if (!duplicate)
                {
                    tags.emplace_back(value);
                    if (tags.size() > maxTags)
                    {
                        tags.clear();
                        return false;
                    }
                }
            }
            if (delimiter == std::wstring_view::npos)
            {
                break;
            }
            start = delimiter + 1;
        }
        return true;
    }

    [[nodiscard]] inline std::wstring joinTags(const std::vector<std::wstring>& tags)
    {
        std::wstring result;
        for (const auto& tag : tags)
        {
            if (!result.empty()) result += L"; ";
            result += tag;
        }
        return result;
    }
}
