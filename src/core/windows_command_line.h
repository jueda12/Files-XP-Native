#pragma once

#include <string>
#include <string_view>

namespace filesxp::core
{
    [[nodiscard]] inline std::wstring quoteWindowsArgument(std::wstring_view argument)
    {
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        {
            return std::wstring(argument);
        }

        std::wstring result(1, L'"');
        std::size_t backslashes{};
        for (wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'"');
            }
            else
            {
                result.append(backslashes, L'\\');
                result.push_back(character);
            }
            backslashes = 0;
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }
}
