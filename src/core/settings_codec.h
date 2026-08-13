#pragma once

#include "shortcut_map.h"
#include "preview_provider.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace filesxp::core
{
    enum class Locale : std::uint32_t
    {
        system,
        english,
        traditionalChinese,
        simplifiedChinese
    };

    enum SettingFlag : std::uint32_t
    {
        restoreSession = 1U << 0,
        showPlaces = 1U << 1,
        compactToolbar = 1U << 2,
        confirmPermanentDelete = 1U << 3,
        enableGit = 1U << 4,
        enableArchives = 1U << 5,
        enableQuickPreview = 1U << 6
    };

    inline constexpr std::uint32_t knownSettingFlags = restoreSession | showPlaces |
        compactToolbar | confirmPermanentDelete | enableGit | enableArchives | enableQuickPreview;
    inline constexpr std::uint32_t defaultSettingFlags = restoreSession | showPlaces |
        confirmPermanentDelete | enableGit | enableArchives | enableQuickPreview;
    inline constexpr std::uint32_t knownToolbarButtons = 0x3fU;

    struct AppSettings final
    {
        Locale locale{Locale::system};
        std::uint32_t flags{defaultSettingFlags};
        std::uint32_t defaultView{};
        std::uint32_t toolbarButtons{knownToolbarButtons};
        std::array<std::uint32_t, shortcutCount> shortcuts{defaultShortcuts};
        PreviewProvider previewProvider{PreviewProvider::automatic};
        std::wstring startLocation;

        [[nodiscard]] bool enabled(SettingFlag flag) const noexcept
        {
            return (flags & static_cast<std::uint32_t>(flag)) != 0;
        }

        void set(SettingFlag flag, bool value) noexcept
        {
            if (value)
            {
                flags |= static_cast<std::uint32_t>(flag);
            }
            else
            {
                flags &= ~static_cast<std::uint32_t>(flag);
            }
        }
    };

    class SettingsCodec final
    {
    public:
        static constexpr std::size_t maxStartLocationLength = 32768;
        static constexpr std::size_t maxEncodedCharacters = 65536;
        static constexpr std::uint32_t maxView = 5;

        [[nodiscard]] static std::vector<wchar_t> encode(const AppSettings& settings)
        {
            if (static_cast<std::uint32_t>(settings.locale) >
                    static_cast<std::uint32_t>(Locale::simplifiedChinese) ||
                (settings.flags & ~knownSettingFlags) != 0 || settings.defaultView > maxView ||
                (settings.toolbarButtons & ~knownToolbarButtons) != 0 ||
                !validShortcutMap(settings.shortcuts) ||
                !validPreviewProvider(static_cast<std::uint32_t>(settings.previewProvider)) ||
                settings.startLocation.size() > maxStartLocationLength)
            {
                return {};
            }
            std::vector<wchar_t> result;
            result.reserve(128 + settings.startLocation.size());
            append(result, L"FXS4");
            append(result, std::to_wstring(static_cast<std::uint32_t>(settings.locale)));
            append(result, std::to_wstring(settings.flags));
            append(result, std::to_wstring(settings.defaultView));
            append(result, std::to_wstring(settings.toolbarButtons));
            append(result, encodeShortcuts(settings.shortcuts));
            append(result, std::to_wstring(static_cast<std::uint32_t>(settings.previewProvider)));
            append(result, settings.startLocation.empty() ? L"-" : settings.startLocation);
            result.push_back(L'\0');
            return result.size() <= maxEncodedCharacters ? result : std::vector<wchar_t>{};
        }

        [[nodiscard]] static bool decode(const wchar_t* data, std::size_t characters,
            AppSettings& settings)
        {
            settings = {};
            if (data == nullptr || characters < 6 || characters > maxEncodedCharacters ||
                data[characters - 1] != L'\0' || data[characters - 2] != L'\0')
            {
                return false;
            }
            std::vector<std::wstring_view> fields;
            std::size_t offset{};
            while (offset + 1 < characters && data[offset] != L'\0')
            {
                std::size_t end = offset;
                while (end < characters && data[end] != L'\0')
                {
                    ++end;
                }
                if (end >= characters)
                {
                    return false;
                }
                fields.emplace_back(data + offset, end - offset);
                offset = end + 1;
            }
            const bool versionOne = fields.size() == 5 && fields[0] == L"FXS1";
            const bool versionTwo = fields.size() == 6 && fields[0] == L"FXS2";
            const bool versionThree = fields.size() == 7 && fields[0] == L"FXS3";
            const bool versionFour = fields.size() == 8 && fields[0] == L"FXS4";
            if ((!versionOne && !versionTwo && !versionThree && !versionFour) ||
                fields.back().size() > maxStartLocationLength)
            {
                return false;
            }
            std::uint32_t locale{};
            std::uint32_t flags{};
            std::uint32_t view{};
            std::uint32_t toolbar = knownToolbarButtons;
            std::array<std::uint32_t, shortcutCount> shortcuts{defaultShortcuts};
            std::uint32_t previewProvider{};
            if (!parse(fields[1], locale) || !parse(fields[2], flags) || !parse(fields[3], view) ||
                (versionTwo && !parse(fields[4], toolbar)) ||
                (versionThree && (!parse(fields[4], toolbar) || !parseShortcuts(fields[5], shortcuts))) ||
                (versionFour && (!parse(fields[4], toolbar) || !parseShortcuts(fields[5], shortcuts) ||
                    !parse(fields[6], previewProvider))) ||
                locale > static_cast<std::uint32_t>(Locale::simplifiedChinese) ||
                (flags & ~knownSettingFlags) != 0 || view > maxView ||
                (toolbar & ~knownToolbarButtons) != 0 || !validPreviewProvider(previewProvider))
            {
                return false;
            }
            settings.locale = static_cast<Locale>(locale);
            settings.flags = flags;
            settings.defaultView = view;
            settings.toolbarButtons = toolbar;
            settings.shortcuts = shortcuts;
            settings.previewProvider = static_cast<PreviewProvider>(previewProvider);
            const std::wstring_view location = fields[versionFour ? 7 :
                (versionThree ? 6 : (versionTwo ? 5 : 4))];
            if (location != L"-") settings.startLocation.assign(location);
            return true;
        }

    private:
        [[nodiscard]] static std::wstring encodeShortcuts(
            const std::array<std::uint32_t, shortcutCount>& shortcuts)
        {
            std::wstring result;
            for (std::uint32_t shortcut : shortcuts)
            {
                if (!result.empty()) result.push_back(L',');
                result += std::to_wstring(shortcut);
            }
            return result;
        }

        [[nodiscard]] static bool parseShortcuts(std::wstring_view value,
            std::array<std::uint32_t, shortcutCount>& shortcuts) noexcept
        {
            std::size_t start{};
            for (std::size_t index = 0; index < shortcuts.size(); ++index)
            {
                const std::size_t delimiter = value.find(L',', start);
                if ((index + 1 < shortcuts.size() && delimiter == std::wstring_view::npos) ||
                    (index + 1 == shortcuts.size() && delimiter != std::wstring_view::npos))
                {
                    return false;
                }
                const std::size_t end = delimiter == std::wstring_view::npos ? value.size() : delimiter;
                if (!parse(value.substr(start, end - start), shortcuts[index])) return false;
                start = end + 1;
            }
            return validShortcutMap(shortcuts);
        }

        static void append(std::vector<wchar_t>& destination, std::wstring_view value)
        {
            destination.insert(destination.end(), value.begin(), value.end());
            destination.push_back(L'\0');
        }

        [[nodiscard]] static bool parse(std::wstring_view value, std::uint32_t& result) noexcept
        {
            if (value.empty())
            {
                return false;
            }
            std::uint32_t parsed{};
            for (wchar_t character : value)
            {
                if (character < L'0' || character > L'9')
                {
                    return false;
                }
                const std::uint32_t digit = static_cast<std::uint32_t>(character - L'0');
                if (parsed > (std::numeric_limits<std::uint32_t>::max() - digit) / 10)
                {
                    return false;
                }
                parsed = parsed * 10 + digit;
            }
            result = parsed;
            return true;
        }
    };
}
