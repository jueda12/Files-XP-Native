#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxLocalePackBytes = 256U * 1024U;
    inline constexpr std::size_t maxLocaleEntryCharacters = 512;

    struct LocaleOverride final
    {
        std::size_t index{};
        std::u16string value;
    };

    [[nodiscard]] inline bool parseLocalePack(std::u16string_view source,
        std::size_t stringCount, std::vector<LocaleOverride>& result)
    {
        if (source.empty() || source.size() > maxLocalePackBytes ||
            stringCount == 0)
            return false;
        std::vector<bool> seen(stringCount, false);
        std::vector<LocaleOverride> parsed;
        bool header{};
        std::size_t position{};
        while (position <= source.size())
        {
            const std::size_t newline = source.find(u'\n', position);
            const std::size_t end = newline == std::u16string_view::npos ? source.size() : newline;
            std::u16string_view line = source.substr(position, end - position);
            if (!line.empty() && line.back() == u'\r') line.remove_suffix(1);
            position = newline == std::u16string_view::npos ? source.size() + 1 : newline + 1;
            std::size_t first{};
            while (first < line.size() && (line[first] == u' ' || line[first] == u'\t')) ++first;
            if (first == line.size() || line[first] == u'#' || line[first] == u';') continue;
            line.remove_prefix(first);
            if (!header)
            {
                if (line != u"FXL1") return false;
                header = true;
                continue;
            }
            const std::size_t equals = line.find(u'=');
            if (equals == std::u16string_view::npos || equals == 0) return false;
            std::u16string_view key = line.substr(0, equals);
            while (!key.empty() && (key.back() == u' ' || key.back() == u'\t')) key.remove_suffix(1);
            if (key.empty()) return false;
            std::size_t index{};
            for (char16_t character : key)
            {
                if (character < u'0' || character > u'9') return false;
                const std::size_t digit = static_cast<std::size_t>(character - u'0');
                if (index > (std::numeric_limits<std::size_t>::max() - digit) / 10) return false;
                index = index * 10 + digit;
            }
            if (index >= stringCount || seen[index]) return false;
            std::u16string value;
            const std::u16string_view encoded = line.substr(equals + 1);
            value.reserve(encoded.size());
            for (std::size_t offset = 0; offset < encoded.size(); ++offset)
            {
                char16_t character = encoded[offset];
                if (character == u'\\')
                {
                    if (++offset >= encoded.size()) return false;
                    character = encoded[offset];
                    if (character == u'n')
                    {
                        value += u"\r\n";
                        continue;
                    }
                    if (character == u't') character = u'\t';
                    else if (character != u'\\' && character != u'=') return false;
                }
                else if (character < 32 && character != u'\t')
                    return false;
                value.push_back(character);
                if (value.size() > maxLocaleEntryCharacters) return false;
            }
            if (value.empty()) return false;
            seen[index] = true;
            parsed.push_back(LocaleOverride{index, std::move(value)});
        }
        if (!header) return false;
        result = std::move(parsed);
        return true;
    }
}
