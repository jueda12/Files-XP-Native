#pragma once

#include <cwctype>
#include <string>
#include <string_view>

namespace filesxp::core
{
    inline constexpr std::size_t maxFilenameLength = 255;

    [[nodiscard]] inline bool validWindowsFilename(std::wstring_view value,
        bool allowDot = true) noexcept
    {
        if (value.empty() || value.size() > maxFilenameLength ||
            value.back() == L' ' || value.back() == L'.')
        {
            return false;
        }
        for (wchar_t character : value)
        {
            if (character < 32 || character == L'<' || character == L'>' || character == L':' ||
                character == L'"' || character == L'/' || character == L'\\' || character == L'|' ||
                character == L'?' || character == L'*' || (!allowDot && character == L'.'))
            {
                return false;
            }
        }

        const std::size_t dot = value.find(L'.');
        const std::wstring_view stem = value.substr(0, dot);
        const auto reserved = [stem](std::wstring_view value) noexcept
        {
            if (stem.size() != value.size()) return false;
            for (std::size_t index = 0; index < value.size(); ++index)
            {
                if (static_cast<wchar_t>(std::towupper(stem[index])) != value[index]) return false;
            }
            return true;
        };
        if (reserved(L"CON") || reserved(L"PRN") || reserved(L"AUX") || reserved(L"NUL"))
        {
            return false;
        }
        if (stem.size() == 4 && stem[3] >= L'1' && stem[3] <= L'9' &&
            ((std::towupper(stem[0]) == L'C' && std::towupper(stem[1]) == L'O' &&
                 std::towupper(stem[2]) == L'M') ||
             (std::towupper(stem[0]) == L'L' && std::towupper(stem[1]) == L'P' &&
                 std::towupper(stem[2]) == L'T')))
        {
            return false;
        }
        return true;
    }

    [[nodiscard]] inline std::wstring bulkRenameTarget(std::wstring_view base,
        std::wstring_view originalName, bool folder)
    {
        if (!validWindowsFilename(base, false))
        {
            return {};
        }
        std::wstring result(base);
        if (!folder)
        {
            const std::size_t dot = originalName.find_last_of(L'.');
            if (dot != std::wstring_view::npos && dot != 0 && dot + 1 < originalName.size())
            {
                result.append(originalName.substr(dot));
            }
        }
        return result.size() <= maxFilenameLength ? result : std::wstring{};
    }

    [[nodiscard]] inline bool validAlternateStreamName(std::wstring_view value) noexcept
    {
        if (value.empty() || value.size() > maxFilenameLength)
        {
            return false;
        }
        if (value.size() == 5 && value[0] == L'$' && std::towupper(value[1]) == L'D' &&
            std::towupper(value[2]) == L'A' && std::towupper(value[3]) == L'T' &&
            std::towupper(value[4]) == L'A')
        {
            return false;
        }
        for (wchar_t character : value)
        {
            if (character < 32 || character == L':' || character == L'/' ||
                character == L'\\' || character == L'*' || character == L'?' ||
                character == L'"' || character == L'<' || character == L'>' || character == L'|')
            {
                return false;
            }
        }
        return value.back() != L' ' && value.back() != L'.';
    }
}
