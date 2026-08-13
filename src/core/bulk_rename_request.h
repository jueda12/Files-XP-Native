#pragma once

#include "archive_request.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxBulkRenameRequestBytes = 4U * 1024U * 1024U;
    inline constexpr std::size_t maxBulkRenameItems = 256;
    inline constexpr std::size_t maxBulkRenameBaseCharacters = 255;

    struct BulkRenameRequest final
    {
        std::u16string baseName;
        std::vector<std::u16string> paths;
    };

    namespace bulk_rename_request_detail
    {
        inline constexpr std::uint32_t magic = 0x52425846U; // FXBR
        inline constexpr std::uint32_t version = 1;

        [[nodiscard]] inline bool validString(std::u16string_view value) noexcept
        {
            return !value.empty() && value.size() < 32767 &&
                value.find(u'\0') == std::u16string_view::npos &&
                archive_request_detail::wellFormedUtf16(value);
        }
    }

    [[nodiscard]] inline std::vector<std::uint8_t> encodeBulkRenameRequest(
        const BulkRenameRequest& request)
    {
        using namespace bulk_rename_request_detail;
        if (!validString(request.baseName) || request.baseName.size() > maxBulkRenameBaseCharacters ||
            request.paths.size() < 2 || request.paths.size() > maxBulkRenameItems)
            return {};
        std::vector<std::uint8_t> bytes;
        bytes.reserve(1024);
        archive_request_detail::appendU32(bytes, magic);
        archive_request_detail::appendU32(bytes, version);
        archive_request_detail::appendU32(bytes, 0);
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.paths.size()));
        if (!archive_request_detail::appendString(bytes, request.baseName)) return {};
        for (const auto& path : request.paths)
        {
            if (!validString(path) || !archive_request_detail::appendString(bytes, path) ||
                bytes.size() > maxBulkRenameRequestBytes)
                return {};
        }
        archive_request_detail::writeU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
        return bytes;
    }

    [[nodiscard]] inline bool decodeBulkRenameRequest(const std::uint8_t* data,
        std::size_t size, BulkRenameRequest& request)
    {
        using namespace bulk_rename_request_detail;
        if (data == nullptr || size < 20 || size > maxBulkRenameRequestBytes) return false;
        const std::uint8_t* current = data;
        const std::uint8_t* const end = data + size;
        std::uint32_t storedMagic{};
        std::uint32_t storedVersion{};
        std::uint32_t storedSize{};
        std::uint32_t count{};
        if (!archive_request_detail::readU32(current, end, storedMagic) ||
            !archive_request_detail::readU32(current, end, storedVersion) ||
            !archive_request_detail::readU32(current, end, storedSize) ||
            !archive_request_detail::readU32(current, end, count) || storedMagic != magic ||
            storedVersion != version || storedSize != size || count < 2 || count > maxBulkRenameItems)
            return false;
        BulkRenameRequest decoded;
        if (!archive_request_detail::readString(current, end, decoded.baseName) ||
            !validString(decoded.baseName) || decoded.baseName.size() > maxBulkRenameBaseCharacters)
            return false;
        decoded.paths.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            std::u16string path;
            if (!archive_request_detail::readString(current, end, path) || !validString(path))
                return false;
            decoded.paths.push_back(std::move(path));
        }
        if (current != end) return false;
        request = std::move(decoded);
        return true;
    }
}
