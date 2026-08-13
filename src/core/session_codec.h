#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace filesxp::core
{
    struct SessionSnapshot final
    {
        std::size_t activeIndex{};
        std::vector<std::wstring> locations;
    };

    class SessionCodec final
    {
    public:
        static constexpr std::size_t maxTabs = 64;
        static constexpr std::size_t maxLocationLength = 32768;
        static constexpr std::size_t maxEncodedCharacters = 1024 * 1024;

        [[nodiscard]] static std::vector<wchar_t> encode(const SessionSnapshot& snapshot)
        {
            if (snapshot.locations.empty() || snapshot.locations.size() > maxTabs ||
                snapshot.activeIndex >= snapshot.locations.size())
            {
                return {};
            }
            std::vector<wchar_t> result;
            result.reserve(64 + snapshot.locations.size() * 64);
            append(result, L"FXP1");
            append(result, std::to_wstring(snapshot.activeIndex));
            for (const auto& location : snapshot.locations)
            {
                if (location.empty() || location.size() > maxLocationLength ||
                    result.size() + location.size() + 2 > maxEncodedCharacters)
                {
                    return {};
                }
                append(result, location);
            }
            result.push_back(L'\0');
            return result;
        }

        [[nodiscard]] static bool decode(const wchar_t* data, std::size_t characters,
            SessionSnapshot& snapshot)
        {
            snapshot = {};
            if (data == nullptr || characters < 3 || characters > maxEncodedCharacters ||
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
            if (fields.size() < 3 || fields[0] != L"FXP1" || fields.size() - 2 > maxTabs)
            {
                return false;
            }

            std::size_t active{};
            if (!parseIndex(fields[1], active) || active >= fields.size() - 2)
            {
                return false;
            }
            snapshot.locations.reserve(fields.size() - 2);
            for (std::size_t index = 2; index < fields.size(); ++index)
            {
                if (fields[index].empty() || fields[index].size() > maxLocationLength)
                {
                    snapshot = {};
                    return false;
                }
                snapshot.locations.emplace_back(fields[index]);
            }
            snapshot.activeIndex = active;
            return true;
        }

    private:
        static void append(std::vector<wchar_t>& destination, std::wstring_view value)
        {
            destination.insert(destination.end(), value.begin(), value.end());
            destination.push_back(L'\0');
        }

        [[nodiscard]] static bool parseIndex(std::wstring_view value, std::size_t& result) noexcept
        {
            if (value.empty())
            {
                return false;
            }
            std::size_t parsed{};
            for (wchar_t character : value)
            {
                if (character < L'0' || character > L'9')
                {
                    return false;
                }
                const std::size_t digit = static_cast<std::size_t>(character - L'0');
                if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10)
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
