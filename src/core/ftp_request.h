#pragma once

#include "natural_order.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxFtpRequestBytes = 96 * 1024;
    inline constexpr std::size_t maxFtpUrlCharacters = 4096;
    inline constexpr std::size_t maxFtpUsernameCharacters = 256;
    inline constexpr std::size_t maxFtpPasswordCharacters = 1024;
    inline constexpr std::size_t maxFtpRemoteNameCharacters = 1024;
    inline constexpr std::size_t maxFtpLocalPathCharacters = 32767;
    inline constexpr std::size_t maxFtpListingItems = 100000;

    enum class FtpNameListStep : std::uint8_t
    {
        item,
        skipped,
        done
    };

    enum class FtpOperation : std::uint8_t
    {
        list,
        download,
        upload,
        makeDirectory,
        deleteFile,
        deleteDirectory,
        count
    };

    struct FtpRequest final
    {
        FtpOperation operation{FtpOperation::list};
        bool requireTls{true};
        std::u16string url;
        std::u16string username;
        std::u16string password;
        std::u16string localPath;
        std::u16string remoteName;
    };

    template <typename Character>
    [[nodiscard]] inline bool validFtpRemoteName(
        std::basic_string_view<Character> name) noexcept
    {
        if (name.empty() || name.size() > maxFtpRemoteNameCharacters ||
            (name.size() == 1 && name[0] == static_cast<Character>('.')) ||
            (name.size() == 2 && name[0] == static_cast<Character>('.') &&
                name[1] == static_cast<Character>('.')))
            return false;
        return std::none_of(name.begin(), name.end(), [](Character value)
        {
            return value == 0 || value < 0x20 || value == 0x7f ||
                value == static_cast<Character>('/') ||
                value == static_cast<Character>('\\');
        });
    }

    [[nodiscard]] inline bool validFtpDirectoryUrl(std::u16string_view url) noexcept
    {
        constexpr std::u16string_view ftp = u"ftp://";
        constexpr std::u16string_view ftps = u"ftps://";
        const std::size_t scheme = url.starts_with(ftp) ? ftp.size() :
            url.starts_with(ftps) ? ftps.size() : 0;
        if (scheme == 0 || url.size() <= scheme || url.size() > maxFtpUrlCharacters ||
            url.back() != u'/')
            return false;
        const std::size_t authorityEnd = url.find(u'/', scheme);
        if (authorityEnd == scheme || authorityEnd == std::u16string_view::npos)
            return false;
        const std::u16string_view authority = url.substr(scheme, authorityEnd - scheme);
        if (authority.find(u'@') != std::u16string_view::npos ||
            authority.find(u'%') != std::u16string_view::npos)
            return false;
        for (std::size_t index = 0; index < url.size(); ++index)
        {
            const char16_t value = url[index];
            if (value <= 0x20 || value == 0x7f || value == u'\\' ||
                value == u'?' || value == u'#')
                return false;
            if (value == u'%' && (index + 2 >= url.size() ||
                    !((url[index + 1] >= u'0' && url[index + 1] <= u'9') ||
                      (url[index + 1] >= u'a' && url[index + 1] <= u'f') ||
                      (url[index + 1] >= u'A' && url[index + 1] <= u'F')) ||
                    !((url[index + 2] >= u'0' && url[index + 2] <= u'9') ||
                      (url[index + 2] >= u'a' && url[index + 2] <= u'f') ||
                      (url[index + 2] >= u'A' && url[index + 2] <= u'F'))))
                return false;
        }
        return true;
    }

    [[nodiscard]] inline std::u16string parentFtpDirectoryUrl(std::u16string_view url)
    {
        if (!validFtpDirectoryUrl(url)) return {};
        const std::size_t scheme = url.starts_with(u"ftps://") ? 7U : 6U;
        const std::size_t rootSlash = url.find(u'/', scheme);
        if (rootSlash == std::u16string_view::npos || rootSlash + 1 == url.size())
            return std::u16string(url);
        const std::size_t parentSlash = url.rfind(u'/', url.size() - 2);
        if (parentSlash == std::u16string_view::npos || parentSlash < rootSlash)
            return {};
        return std::u16string(url.substr(0, parentSlash + 1));
    }

    [[nodiscard]] inline bool validFtpRequest(const FtpRequest& request) noexcept
    {
        if (request.operation >= FtpOperation::count || !validFtpDirectoryUrl(request.url) ||
            request.username.size() > maxFtpUsernameCharacters ||
            request.password.size() > maxFtpPasswordCharacters ||
            request.localPath.size() > maxFtpLocalPathCharacters ||
            std::any_of(request.username.begin(), request.username.end(), [](char16_t value)
                { return value == 0 || value < 0x20 || value == 0x7f || value == u':'; }) ||
            std::any_of(request.password.begin(), request.password.end(), [](char16_t value)
                { return value == 0 || value < 0x20 || value == 0x7f; }) ||
            std::any_of(request.localPath.begin(), request.localPath.end(), [](char16_t value)
                { return value == 0; }) ||
            (request.username.empty() && !request.password.empty()))
            return false;
        const bool hasName = validFtpRemoteName(std::u16string_view(request.remoteName));
        switch (request.operation)
        {
        case FtpOperation::list:
            return request.localPath.empty() && request.remoteName.empty();
        case FtpOperation::download:
        case FtpOperation::upload:
            return !request.localPath.empty() && hasName;
        case FtpOperation::makeDirectory:
        case FtpOperation::deleteFile:
        case FtpOperation::deleteDirectory:
            return request.localPath.empty() && hasName;
        case FtpOperation::count:
            return false;
        }
        return false;
    }

    [[nodiscard]] inline std::string quoteCurlConfigValue(std::string_view value)
    {
        std::string quoted;
        try
        {
            quoted.reserve(value.size() + 2);
            quoted.push_back('"');
            for (const unsigned char character : value)
            {
                switch (character)
                {
                case '\\': quoted += "\\\\"; break;
                case '"': quoted += "\\\""; break;
                case '\t': quoted += "\\t"; break;
                case '\n': quoted += "\\n"; break;
                case '\r': quoted += "\\r"; break;
                case '\v': quoted += "\\v"; break;
                default:
                    if (character == 0 || character < 0x20 || character == 0x7f)
                    {
                        std::fill(quoted.begin(), quoted.end(), '\0');
                        quoted.clear();
                        return quoted;
                    }
                    quoted.push_back(static_cast<char>(character));
                    break;
                }
            }
            quoted.push_back('"');
        }
        catch (...)
        {
            std::fill(quoted.begin(), quoted.end(), '\0');
            quoted.clear();
        }
        return quoted;
    }

    [[nodiscard]] inline std::string percentEncodeFtpSegment(std::string_view value)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        if (value.empty()) return encoded;
        try
        {
            encoded.reserve(value.size() * 3);
            for (const unsigned char character : value)
            {
                const bool unreserved = (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '-' ||
                    character == '.' || character == '_' || character == '~';
                if (unreserved)
                    encoded.push_back(static_cast<char>(character));
                else
                {
                    encoded.push_back('%');
                    encoded.push_back(digits[character >> 4]);
                    encoded.push_back(digits[character & 0x0f]);
                }
            }
        }
        catch (...)
        {
            encoded.clear();
        }
        return encoded;
    }

    namespace ftp_detail
    {
        inline void append32(std::vector<std::uint8_t>& output, std::uint32_t value)
        {
            for (unsigned shift = 0; shift < 32; shift += 8)
                output.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        [[nodiscard]] inline bool read32(const std::uint8_t*& cursor,
            const std::uint8_t* end, std::uint32_t& value) noexcept
        {
            if (static_cast<std::size_t>(end - cursor) < 4) return false;
            value = static_cast<std::uint32_t>(cursor[0]) |
                (static_cast<std::uint32_t>(cursor[1]) << 8) |
                (static_cast<std::uint32_t>(cursor[2]) << 16) |
                (static_cast<std::uint32_t>(cursor[3]) << 24);
            cursor += 4;
            return true;
        }

        inline void appendText(std::vector<std::uint8_t>& output, std::u16string_view value)
        {
            append32(output, static_cast<std::uint32_t>(value.size()));
            if (!value.empty())
            {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
                output.insert(output.end(), bytes, bytes + value.size() * sizeof(char16_t));
            }
        }

        [[nodiscard]] inline bool readText(const std::uint8_t*& cursor,
            const std::uint8_t* end, std::u16string& value) noexcept
        {
            std::uint32_t characters{};
            if (!read32(cursor, end, characters) ||
                characters > static_cast<std::uint32_t>((end - cursor) / sizeof(char16_t)))
                return false;
            value.resize(characters);
            if (characters != 0)
                std::memcpy(value.data(), cursor, characters * sizeof(char16_t));
            cursor += characters * sizeof(char16_t);
            return true;
        }
    }

    [[nodiscard]] inline std::vector<std::uint8_t> encodeFtpRequest(const FtpRequest& request)
    {
        if (!validFtpRequest(request)) return {};
        std::vector<std::uint8_t> output;
        try
        {
            output.reserve(32 + (request.url.size() + request.username.size() +
                request.password.size() + request.localPath.size() +
                request.remoteName.size()) * sizeof(char16_t));
            output.insert(output.end(), {'F', 'X', 'F', '1'});
            output.push_back(static_cast<std::uint8_t>(request.operation));
            output.push_back(request.requireTls ? 1 : 0);
            output.push_back(0);
            output.push_back(0);
            ftp_detail::append32(output, 0);
            ftp_detail::appendText(output, request.url);
            ftp_detail::appendText(output, request.username);
            ftp_detail::appendText(output, request.password);
            ftp_detail::appendText(output, request.localPath);
            ftp_detail::appendText(output, request.remoteName);
            if (output.size() > maxFtpRequestBytes)
            {
                std::fill(output.begin(), output.end(), std::uint8_t{0});
                return {};
            }
            const auto size = static_cast<std::uint32_t>(output.size());
            for (unsigned index = 0; index < 4; ++index)
                output[8 + index] = static_cast<std::uint8_t>(size >> (index * 8));
            return output;
        }
        catch (...)
        {
            std::fill(output.begin(), output.end(), std::uint8_t{0});
            return {};
        }
    }

    [[nodiscard]] inline bool decodeFtpRequest(const std::uint8_t* data, std::size_t size,
        FtpRequest& request) noexcept
    {
        if (data == nullptr || size < 32 || size > maxFtpRequestBytes ||
            std::memcmp(data, "FXF1", 4) != 0 || data[6] != 0 || data[7] != 0)
            return false;
        const std::uint8_t* cursor = data + 8;
        const std::uint8_t* const end = data + size;
        std::uint32_t encodedSize{};
        if (!ftp_detail::read32(cursor, end, encodedSize) || encodedSize != size ||
            data[4] >= static_cast<std::uint8_t>(FtpOperation::count) || data[5] > 1)
            return false;
        FtpRequest decoded;
        decoded.operation = static_cast<FtpOperation>(data[4]);
        decoded.requireTls = data[5] != 0;
        try
        {
            if (!ftp_detail::readText(cursor, end, decoded.url) ||
                !ftp_detail::readText(cursor, end, decoded.username) ||
                !ftp_detail::readText(cursor, end, decoded.password) ||
                !ftp_detail::readText(cursor, end, decoded.localPath) ||
                !ftp_detail::readText(cursor, end, decoded.remoteName) || cursor != end ||
                !validFtpRequest(decoded))
            {
                std::fill(decoded.password.begin(), decoded.password.end(), u'\0');
                return false;
            }
            std::fill(request.password.begin(), request.password.end(), u'\0');
            request.password.clear();
            request = std::move(decoded);
            return true;
        }
        catch (...)
        {
            std::fill(decoded.password.begin(), decoded.password.end(), u'\0');
            return false;
        }
    }

    class FtpNameListCursor final
    {
    public:
        [[nodiscard]] bool start(std::wstring_view listing) noexcept
        {
            cancel();
            listing_ = listing;
            active_ = !listing.empty();
            return active_;
        }

        void cancel() noexcept
        {
            listing_ = {};
            next_ = 0;
            inspected_ = 0;
            accepted_ = 0;
            active_ = false;
        }

        [[nodiscard]] FtpNameListStep next(std::wstring_view& name) noexcept
        {
            name = {};
            if (!active_) return FtpNameListStep::done;
            if (next_ >= listing_.size() || inspected_ >= maxFtpListingItems)
            {
                active_ = false;
                return FtpNameListStep::done;
            }
            const std::size_t start = next_;
            std::size_t end = listing_.find(L'\n', start);
            if (end == std::wstring_view::npos)
            {
                end = listing_.size();
                next_ = listing_.size();
            }
            else
            {
                next_ = end + 1;
            }
            ++inspected_;
            if (next_ >= listing_.size() || inspected_ >= maxFtpListingItems)
                active_ = false;
            name = listing_.substr(start, end - start);
            if (!name.empty() && name.back() == L'\r') name.remove_suffix(1);
            if (!validFtpRemoteName(name)) return FtpNameListStep::skipped;
            ++accepted_;
            return FtpNameListStep::item;
        }

        [[nodiscard]] bool active() const noexcept { return active_; }
        [[nodiscard]] std::size_t inspected() const noexcept { return inspected_; }
        [[nodiscard]] std::size_t accepted() const noexcept { return accepted_; }

    private:
        std::wstring_view listing_;
        std::size_t next_{};
        std::size_t inspected_{};
        std::size_t accepted_{};
        bool active_{};
    };

    [[nodiscard]] inline std::vector<std::wstring> parseFtpNameListImpl(
        std::wstring_view listing, bool sort)
    {
        std::vector<std::wstring> names;
        try
        {
            FtpNameListCursor cursor;
            (void)cursor.start(listing);
            for (;;)
            {
                std::wstring_view name;
                const FtpNameListStep step = cursor.next(name);
                if (step == FtpNameListStep::done) break;
                if (step == FtpNameListStep::item) names.emplace_back(name);
            }
            if (sort) std::sort(names.begin(), names.end(), NaturalLess{});
        }
        catch (...)
        {
            names.clear();
        }
        return names;
    }

    [[nodiscard]] inline std::vector<std::wstring> parseFtpNameList(
        std::wstring_view listing)
    {
        return parseFtpNameListImpl(listing, true);
    }

    [[nodiscard]] inline std::vector<std::wstring> parsePresortedFtpNameList(
        std::wstring_view listing)
    {
        return parseFtpNameListImpl(listing, false);
    }
}
