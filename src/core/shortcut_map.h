#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace filesxp::core
{
    inline constexpr std::uint32_t shortcutControl = 1U << 16;
    inline constexpr std::uint32_t shortcutShift = 1U << 17;
    inline constexpr std::uint32_t shortcutAlt = 1U << 18;
    inline constexpr std::uint32_t shortcutKeyMask = 0xffffU;
    inline constexpr std::size_t shortcutCount = 6;
    inline constexpr std::array<std::uint32_t, shortcutCount> defaultShortcuts{
        shortcutControl | 'T', shortcutControl | 'W', shortcutControl | 'L',
        shortcutControl | 'F', shortcutControl | shortcutShift | 'P',
        shortcutControl | shortcutShift | 'N'};
    inline constexpr std::array<std::uint32_t, 20> reservedShortcuts{
        shortcutControl | shortcutShift | 'T', shortcutControl | shortcutShift | 0x21,
        shortcutControl | shortcutShift | 0x22, shortcutControl | 'N', shortcutControl | 0x09,
        shortcutControl | shortcutShift | 0x09,
        shortcutAlt | 0x25, shortcutAlt | 0x27, shortcutAlt | 0x26, 0x75, 0x74, 0x71,
        shortcutShift | 0x2e, 0x2e, shortcutControl | 'C', shortcutControl | 'X',
        shortcutControl | 'V', shortcutControl | 'A', shortcutControl | 'Z', shortcutControl | 'Y'};

    [[nodiscard]] constexpr bool validShortcut(std::uint32_t chord) noexcept
    {
        const std::uint32_t key = chord & shortcutKeyMask;
        const std::uint32_t modifiers = chord & ~shortcutKeyMask;
        if ((modifiers & ~(shortcutControl | shortcutShift | shortcutAlt)) != 0 ||
            key < 8 || key > 0xfe)
        {
            return false;
        }
        const bool functionKey = key >= 0x70 && key <= 0x87;
        return functionKey || (modifiers & (shortcutControl | shortcutAlt)) != 0;
    }

    [[nodiscard]] constexpr bool validShortcutMap(
        const std::array<std::uint32_t, shortcutCount>& shortcuts) noexcept
    {
        for (std::size_t index = 0; index < shortcuts.size(); ++index)
        {
            if (!validShortcut(shortcuts[index])) return false;
            for (std::uint32_t reserved : reservedShortcuts)
            {
                if (shortcuts[index] == reserved) return false;
            }
            for (std::size_t other = index + 1; other < shortcuts.size(); ++other)
            {
                if (shortcuts[index] == shortcuts[other]) return false;
            }
        }
        return true;
    }
}
