#pragma once

#include "archive_request.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxSearchRequestBytes = 128U * 1024U;
    inline constexpr std::size_t maxSearchQueryCharacters = 512;
    inline constexpr std::size_t maxSearchDepth = 256;

    struct SearchRequest final
    {
        std::u16string root;
        std::u16string query;
        bool includeHidden{true};
    };

    namespace search_request_detail
    {
        inline constexpr std::uint32_t magic = 0x53525846U; // FXRS
        inline constexpr std::uint32_t version = 1;

        [[nodiscard]] inline bool validRoot(std::u16string_view value) noexcept
        {
            return !value.empty() && value.size() < 32767 &&
                value.find(u'\0') == std::u16string_view::npos &&
                archive_request_detail::wellFormedUtf16(value);
        }

        [[nodiscard]] inline bool validQuery(std::u16string_view value) noexcept
        {
            if (value.empty() || value.size() > maxSearchQueryCharacters ||
                !archive_request_detail::wellFormedUtf16(value))
                return false;
            for (char16_t character : value)
            {
                if (character < u' ' || character == 0x7f) return false;
            }
            return true;
        }
    }

    [[nodiscard]] inline std::vector<std::uint8_t> encodeSearchRequest(
        const SearchRequest& request)
    {
        using namespace search_request_detail;
        if (!validRoot(request.root) || !validQuery(request.query)) return {};
        std::vector<std::uint8_t> bytes;
        bytes.reserve(64 + (request.root.size() + request.query.size()) * sizeof(char16_t));
        archive_request_detail::appendU32(bytes, magic);
        archive_request_detail::appendU32(bytes, version);
        archive_request_detail::appendU32(bytes, 0);
        archive_request_detail::appendU32(bytes, request.includeHidden ? 1U : 0U);
        if (!archive_request_detail::appendString(bytes, request.root) ||
            !archive_request_detail::appendString(bytes, request.query) ||
            bytes.size() > maxSearchRequestBytes)
            return {};
        archive_request_detail::writeU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
        return bytes;
    }

    [[nodiscard]] inline bool decodeSearchRequest(const std::uint8_t* data,
        std::size_t size, SearchRequest& request)
    {
        using namespace search_request_detail;
        if (data == nullptr || size < 24 || size > maxSearchRequestBytes) return false;
        const std::uint8_t* current = data;
        const std::uint8_t* const end = data + size;
        std::uint32_t storedMagic{};
        std::uint32_t storedVersion{};
        std::uint32_t storedSize{};
        std::uint32_t flags{};
        SearchRequest decoded;
        if (!archive_request_detail::readU32(current, end, storedMagic) ||
            !archive_request_detail::readU32(current, end, storedVersion) ||
            !archive_request_detail::readU32(current, end, storedSize) ||
            !archive_request_detail::readU32(current, end, flags) || storedMagic != magic ||
            storedVersion != version || storedSize != size || flags > 1 ||
            !archive_request_detail::readString(current, end, decoded.root) ||
            !archive_request_detail::readString(current, end, decoded.query) || current != end ||
            !validRoot(decoded.root) || !validQuery(decoded.query))
            return false;
        decoded.includeHidden = flags != 0;
        request = std::move(decoded);
        return true;
    }
}
