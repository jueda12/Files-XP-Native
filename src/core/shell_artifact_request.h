#pragma once

#include "archive_request.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxShellArtifactRequestBytes = 4U * 1024U * 1024U;

    enum class ShellArtifactOperation : std::uint32_t
    {
        createShortcut = 1,
        createLibrary = 2
    };

    struct ShellArtifactRequest final
    {
        ShellArtifactOperation operation{ShellArtifactOperation::createShortcut};
        std::u16string destinationFolder;
        std::u16string name;
        std::u16string target;
        std::u16string arguments;
        std::u16string workingDirectory;
        std::u16string icon;
    };

    namespace shell_artifact_request_detail
    {
        inline constexpr std::uint32_t magic = 0x54415846U; // FXAT
        inline constexpr std::uint32_t version = 1;

        [[nodiscard]] inline bool validString(std::u16string_view value,
            bool allowEmpty) noexcept
        {
            return (allowEmpty || !value.empty()) && value.size() < 32767 &&
                value.find(u'\0') == std::u16string_view::npos &&
                archive_request_detail::wellFormedUtf16(value);
        }

        [[nodiscard]] inline bool validShape(const ShellArtifactRequest& request) noexcept
        {
            if (!validString(request.name, false) || request.name.size() > 255) return false;
            switch (request.operation)
            {
            case ShellArtifactOperation::createShortcut:
                return validString(request.destinationFolder, false) &&
                    validString(request.target, false) && validString(request.arguments, true) &&
                    validString(request.workingDirectory, true) && validString(request.icon, true);
            case ShellArtifactOperation::createLibrary:
                return request.destinationFolder.empty() && request.target.empty() &&
                    request.arguments.empty() && request.workingDirectory.empty() &&
                    request.icon.empty();
            }
            return false;
        }
    }

    [[nodiscard]] inline std::vector<std::uint8_t> encodeShellArtifactRequest(
        const ShellArtifactRequest& request)
    {
        using namespace shell_artifact_request_detail;
        if (!validShape(request)) return {};
        std::vector<std::uint8_t> bytes;
        bytes.reserve(1024);
        archive_request_detail::appendU32(bytes, magic);
        archive_request_detail::appendU32(bytes, version);
        archive_request_detail::appendU32(bytes, 0);
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.operation));
        for (const auto* value : {&request.destinationFolder, &request.name, &request.target,
                 &request.arguments, &request.workingDirectory, &request.icon})
        {
            if (!archive_request_detail::appendString(bytes, *value) ||
                bytes.size() > maxShellArtifactRequestBytes)
                return {};
        }
        archive_request_detail::writeU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
        return bytes;
    }

    [[nodiscard]] inline bool decodeShellArtifactRequest(const std::uint8_t* data,
        std::size_t size, ShellArtifactRequest& request)
    {
        using namespace shell_artifact_request_detail;
        if (data == nullptr || size < 40 || size > maxShellArtifactRequestBytes) return false;
        const std::uint8_t* current = data;
        const std::uint8_t* const end = data + size;
        std::uint32_t storedMagic{};
        std::uint32_t storedVersion{};
        std::uint32_t storedSize{};
        std::uint32_t operation{};
        ShellArtifactRequest decoded;
        if (!archive_request_detail::readU32(current, end, storedMagic) ||
            !archive_request_detail::readU32(current, end, storedVersion) ||
            !archive_request_detail::readU32(current, end, storedSize) ||
            !archive_request_detail::readU32(current, end, operation) || storedMagic != magic ||
            storedVersion != version || storedSize != size)
            return false;
        decoded.operation = static_cast<ShellArtifactOperation>(operation);
        for (auto* value : {&decoded.destinationFolder, &decoded.name, &decoded.target,
                 &decoded.arguments, &decoded.workingDirectory, &decoded.icon})
        {
            if (!archive_request_detail::readString(current, end, *value) ||
                !validString(*value, true))
                return false;
        }
        if (current != end || !validShape(decoded)) return false;
        request = std::move(decoded);
        return true;
    }
}
