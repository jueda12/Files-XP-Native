#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace filesxp::core
{
    inline constexpr std::size_t maxClipboardPathCharacters = 2U * 1024U * 1024U;

    [[nodiscard]] inline bool appendClipboardPath(std::wstring& output,
        std::wstring_view path, bool quoted,
        std::size_t maximumCharacters = maxClipboardPathCharacters)
    {
        if (path.empty() || path.find(L'\0') != std::wstring_view::npos ||
            output.size() > maximumCharacters)
            return false;
        const std::size_t overhead = (output.empty() ? 0U : 2U) + (quoted ? 2U : 0U);
        if (overhead > maximumCharacters - output.size() ||
            path.size() > maximumCharacters - output.size() - overhead)
            return false;
        if (!output.empty()) output += L"\r\n";
        if (quoted) output.push_back(L'"');
        output.append(path);
        if (quoted) output.push_back(L'"');
        return true;
    }
}
