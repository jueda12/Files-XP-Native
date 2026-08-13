#pragma once

#include "settings_codec.h"

#include <cstdint>

namespace filesxp::core
{
    [[nodiscard]] constexpr Locale resolveLocale(Locale requested, std::uint16_t languageId) noexcept
    {
        if (requested != Locale::system)
        {
            return requested;
        }
        constexpr std::uint16_t chinese = 0x04;
        const std::uint16_t primary = languageId & 0x03ff;
        if (primary != chinese)
        {
            return Locale::english;
        }
        const std::uint16_t sublanguage = languageId >> 10;
        return sublanguage == 1 || sublanguage == 3 || sublanguage == 5
            ? Locale::traditionalChinese : Locale::simplifiedChinese;
    }
}
