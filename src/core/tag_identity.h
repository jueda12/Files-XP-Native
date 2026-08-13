#pragma once

#include <cstdint>
#include <string_view>

namespace filesxp::core
{
    struct TagFileIdentity final
    {
        std::uint32_t volumeSerial{};
        std::uint64_t fileId{};
    };

    [[nodiscard]] constexpr int hexDigit(wchar_t value) noexcept
    {
        return value >= L'0' && value <= L'9' ? value - L'0' :
            value >= L'A' && value <= L'F' ? value - L'A' + 10 :
            value >= L'a' && value <= L'f' ? value - L'a' + 10 : -1;
    }

    [[nodiscard]] constexpr bool parseTagFileIdentity(std::wstring_view key,
        TagFileIdentity& identity) noexcept
    {
        identity = {};
        if (key.size() != 27 || key[0] != L'V' || key[9] != L'-' || key[10] != L'F')
            return false;
        std::uint32_t volume{};
        std::uint64_t file{};
        for (std::size_t index = 1; index < 9; ++index)
        {
            const int digit = hexDigit(key[index]);
            if (digit < 0) return false;
            volume = (volume << 4) | static_cast<std::uint32_t>(digit);
        }
        for (std::size_t index = 11; index < 27; ++index)
        {
            const int digit = hexDigit(key[index]);
            if (digit < 0) return false;
            file = (file << 4) | static_cast<std::uint64_t>(digit);
        }
        identity = TagFileIdentity{volume, file};
        return true;
    }
}
