#pragma once

#include "archive_request.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxTagRequestBytes = 4U * 1024U * 1024U;
    inline constexpr std::size_t maxTagRequestItems = 256;
    inline constexpr std::size_t maxTagRequestTags = 16;
    inline constexpr std::size_t maxTagRequestTagCharacters = 64;

    struct TagRequest final
    {
        std::vector<std::u16string> tags;
        std::vector<std::u16string> paths;
    };

    namespace tag_request_detail
    {
        inline constexpr std::uint32_t magic = 0x54525846U; // FXRT
        inline constexpr std::uint32_t version = 1;

        [[nodiscard]] inline bool validTag(std::u16string_view value) noexcept
        {
            if (value.empty() || value.size() > maxTagRequestTagCharacters ||
                !archive_request_detail::wellFormedUtf16(value))
                return false;
            for (char16_t character : value)
            {
                if (character < u' ' || character == u'"' || character == u';') return false;
            }
            return true;
        }

        [[nodiscard]] inline bool validPath(std::u16string_view value) noexcept
        {
            return !value.empty() && value.size() < 32767 &&
                value.find(u'\0') == std::u16string_view::npos &&
                archive_request_detail::wellFormedUtf16(value);
        }
    }

    [[nodiscard]] inline std::vector<std::uint8_t> encodeTagRequest(const TagRequest& request)
    {
        using namespace tag_request_detail;
        if (request.tags.size() > maxTagRequestTags || request.paths.empty() ||
            request.paths.size() > maxTagRequestItems)
            return {};
        std::vector<std::uint8_t> bytes;
        bytes.reserve(1024);
        archive_request_detail::appendU32(bytes, magic);
        archive_request_detail::appendU32(bytes, version);
        archive_request_detail::appendU32(bytes, 0);
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.tags.size()));
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.paths.size()));
        for (const auto& tag : request.tags)
        {
            if (!validTag(tag) || !archive_request_detail::appendString(bytes, tag) ||
                bytes.size() > maxTagRequestBytes)
                return {};
        }
        for (const auto& path : request.paths)
        {
            if (!validPath(path) || !archive_request_detail::appendString(bytes, path) ||
                bytes.size() > maxTagRequestBytes)
                return {};
        }
        archive_request_detail::writeU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
        return bytes;
    }

    [[nodiscard]] inline bool decodeTagRequest(const std::uint8_t* data, std::size_t size,
        TagRequest& request)
    {
        using namespace tag_request_detail;
        if (data == nullptr || size < 20 || size > maxTagRequestBytes) return false;
        const std::uint8_t* current = data;
        const std::uint8_t* const end = data + size;
        std::uint32_t storedMagic{};
        std::uint32_t storedVersion{};
        std::uint32_t storedSize{};
        std::uint32_t tagCount{};
        std::uint32_t pathCount{};
        if (!archive_request_detail::readU32(current, end, storedMagic) ||
            !archive_request_detail::readU32(current, end, storedVersion) ||
            !archive_request_detail::readU32(current, end, storedSize) ||
            !archive_request_detail::readU32(current, end, tagCount) ||
            !archive_request_detail::readU32(current, end, pathCount) ||
            storedMagic != magic || storedVersion != version || storedSize != size ||
            tagCount > maxTagRequestTags || pathCount == 0 || pathCount > maxTagRequestItems)
            return false;
        TagRequest decoded;
        decoded.tags.reserve(tagCount);
        decoded.paths.reserve(pathCount);
        for (std::uint32_t index = 0; index < tagCount; ++index)
        {
            std::u16string tag;
            if (!archive_request_detail::readString(current, end, tag) || !validTag(tag))
                return false;
            decoded.tags.push_back(std::move(tag));
        }
        for (std::uint32_t index = 0; index < pathCount; ++index)
        {
            std::u16string path;
            if (!archive_request_detail::readString(current, end, path) || !validPath(path))
                return false;
            decoded.paths.push_back(std::move(path));
        }
        if (current != end) return false;
        request = std::move(decoded);
        return true;
    }
}
