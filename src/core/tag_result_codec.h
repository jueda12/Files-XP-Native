#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxTagResults = 100000;
    inline constexpr std::size_t maxTagResultCharacters = 16 * 1024 * 1024;
    inline constexpr wchar_t tagResultMagicA = 0x4658; // FX
    inline constexpr wchar_t tagResultMagicB = 0x5254; // TR
    inline constexpr wchar_t tagResultVersion = 1;
    inline constexpr std::size_t tagResultHeaderCharacters = 5;

    enum class TagResultStep
    {
        item,
        complete,
        invalid
    };

    class TagResultCursor final
    {
    public:
        [[nodiscard]] bool start(const wchar_t* data, std::size_t characters) noexcept
        {
            cancel();
            if (data == nullptr || characters < tagResultHeaderCharacters + 1 ||
                characters > maxTagResultCharacters || data[0] != tagResultMagicA ||
                data[1] != tagResultMagicB || data[2] != tagResultVersion ||
                data[characters - 1] != L'\0')
                return false;
            const std::uint32_t countLow = static_cast<std::uint32_t>(data[3]);
            const std::uint32_t countHigh = static_cast<std::uint32_t>(data[4]);
            if (countLow > 0xffffU || countHigh > 0xffffU) return false;
            const std::uint32_t declaredCount = countLow | (countHigh << 16);
            if (declaredCount > maxTagResults) return false;
            data_ = data;
            characters_ = characters;
            offset_ = tagResultHeaderCharacters;
            expectedCount_ = declaredCount;
            active_ = true;
            return true;
        }

        void cancel() noexcept
        {
            data_ = nullptr;
            characters_ = 0;
            offset_ = 0;
            count_ = 0;
            expectedCount_ = 0;
            active_ = false;
        }

        [[nodiscard]] TagResultStep next(std::wstring_view& path) noexcept
        {
            path = {};
            if (!active_ || data_ == nullptr) return TagResultStep::complete;
            const std::size_t sentinel = characters_ - 1;
            if (count_ == expectedCount_)
            {
                const bool validEnd = offset_ == sentinel && data_[offset_] == L'\0';
                active_ = false;
                return validEnd ? TagResultStep::complete : TagResultStep::invalid;
            }
            if (count_ >= maxTagResults || offset_ >= sentinel || data_[offset_] == L'\0')
            {
                active_ = false;
                return TagResultStep::invalid;
            }
            std::size_t end = offset_;
            while (end < sentinel && data_[end] != L'\0' && end - offset_ < 32767) ++end;
            if (end >= sentinel || data_[end] != L'\0' || end == offset_ || end - offset_ >= 32767)
            {
                active_ = false;
                return TagResultStep::invalid;
            }
            path = std::wstring_view(data_ + offset_, end - offset_);
            offset_ = end + 1;
            ++count_;
            return TagResultStep::item;
        }

        [[nodiscard]] bool active() const noexcept { return active_; }
        [[nodiscard]] std::size_t count() const noexcept { return count_; }
        [[nodiscard]] std::size_t expectedCount() const noexcept { return expectedCount_; }

    private:
        const wchar_t* data_{};
        std::size_t characters_{};
        std::size_t offset_{};
        std::size_t count_{};
        std::size_t expectedCount_{};
        bool active_{};
    };

    [[nodiscard]] inline bool sameTag(std::wstring_view left, std::wstring_view right) noexcept
    {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
            [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    }

    [[nodiscard]] inline bool containsTag(const std::vector<std::wstring>& tags,
        std::wstring_view expected) noexcept
    {
        return std::any_of(tags.begin(), tags.end(), [expected](const std::wstring& tag)
        {
            return sameTag(tag, expected);
        });
    }

    [[nodiscard]] inline std::vector<wchar_t> encodeTagResults(
        const std::vector<std::wstring>& paths)
    {
        if (paths.size() > maxTagResults)
        {
            return {};
        }
        std::vector<wchar_t> result;
        result.reserve(tagResultHeaderCharacters + 1);
        result.push_back(tagResultMagicA);
        result.push_back(tagResultMagicB);
        result.push_back(tagResultVersion);
        const std::uint32_t count = static_cast<std::uint32_t>(paths.size());
        result.push_back(static_cast<wchar_t>(count & 0xffffU));
        result.push_back(static_cast<wchar_t>((count >> 16) & 0xffffU));
        for (const auto& path : paths)
        {
            if (path.empty() || path.size() >= 32767 ||
                result.size() + path.size() + 2 > maxTagResultCharacters)
            {
                return {};
            }
            result.insert(result.end(), path.begin(), path.end());
            result.push_back(L'\0');
        }
        result.push_back(L'\0');
        return result;
    }

    [[nodiscard]] inline bool decodeTagResults(const wchar_t* data, std::size_t characters,
        std::vector<std::wstring>& paths)
    {
        paths.clear();
        TagResultCursor cursor;
        if (!cursor.start(data, characters)) return false;
        while (true)
        {
            std::wstring_view path;
            const TagResultStep step = cursor.next(path);
            if (step == TagResultStep::complete) return true;
            if (step == TagResultStep::invalid)
            {
                paths.clear();
                return false;
            }
            paths.emplace_back(path);
        }
    }
}
