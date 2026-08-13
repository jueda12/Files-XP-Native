#pragma once

#include "archive_request.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxFolderSelectionRequestBytes = 4U * 1024U * 1024U;
    inline constexpr std::size_t maxFolderSelectionItems = 256;

    struct FolderSelectionRequest final
    {
        std::u16string workingDirectory;
        std::u16string folderName;
        std::vector<std::u16string> paths;
    };

    namespace folder_selection_request_detail
    {
        inline constexpr std::uint32_t magic = 0x53465846U; // FXFS
        inline constexpr std::uint32_t version = 1;

        [[nodiscard]] inline bool validString(std::u16string_view value) noexcept
        {
            return !value.empty() && value.size() < 32767 &&
                value.find(u'\0') == std::u16string_view::npos &&
                archive_request_detail::wellFormedUtf16(value);
        }
    }

    [[nodiscard]] inline std::vector<std::uint8_t> encodeFolderSelectionRequest(
        const FolderSelectionRequest& request)
    {
        using namespace folder_selection_request_detail;
        if (!validString(request.workingDirectory) || !validString(request.folderName) ||
            request.folderName.size() > 255 || request.paths.empty() ||
            request.paths.size() > maxFolderSelectionItems)
            return {};
        std::vector<std::uint8_t> bytes;
        bytes.reserve(1024);
        archive_request_detail::appendU32(bytes, magic);
        archive_request_detail::appendU32(bytes, version);
        archive_request_detail::appendU32(bytes, 0);
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.paths.size()));
        if (!archive_request_detail::appendString(bytes, request.workingDirectory) ||
            !archive_request_detail::appendString(bytes, request.folderName))
            return {};
        for (const auto& path : request.paths)
        {
            if (!validString(path) || !archive_request_detail::appendString(bytes, path) ||
                bytes.size() > maxFolderSelectionRequestBytes)
                return {};
        }
        archive_request_detail::writeU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
        return bytes;
    }

    [[nodiscard]] inline bool decodeFolderSelectionRequest(const std::uint8_t* data,
        std::size_t size, FolderSelectionRequest& request)
    {
        using namespace folder_selection_request_detail;
        if (data == nullptr || size < 24 || size > maxFolderSelectionRequestBytes) return false;
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
            storedVersion != version || storedSize != size || count == 0 ||
            count > maxFolderSelectionItems)
            return false;
        FolderSelectionRequest decoded;
        if (!archive_request_detail::readString(current, end, decoded.workingDirectory) ||
            !validString(decoded.workingDirectory) ||
            !archive_request_detail::readString(current, end, decoded.folderName) ||
            !validString(decoded.folderName) || decoded.folderName.size() > 255)
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
