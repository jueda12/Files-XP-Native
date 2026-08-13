#pragma once

#include "archive_request.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxShellOperationRequestBytes = 4U * 1024U * 1024U;
    inline constexpr std::size_t maxShellOperationItems = 4096;

    enum class ShellOperation : std::uint32_t
    {
        createFolder = 1,
        createFile = 2,
        deleteRecycle = 3,
        deletePermanent = 4,
        copy = 5,
        move = 6,
        emptyRecycleBin = 7,
        restoreRecycleBin = 8
    };

    struct ShellOperationRequest final
    {
        ShellOperation operation{ShellOperation::copy};
        bool confirmPermanent{true};
        std::u16string destination;
        std::u16string newName;
        std::vector<std::u16string> items;
    };

    namespace shell_operation_request_detail
    {
        inline constexpr std::uint32_t magic = 0x504f5846U; // FXOP
        inline constexpr std::uint32_t version = 1;

        [[nodiscard]] inline bool validString(std::u16string_view value) noexcept
        {
            return !value.empty() && value.size() < 32767 &&
                value.find(u'\0') == std::u16string_view::npos &&
                archive_request_detail::wellFormedUtf16(value);
        }

        [[nodiscard]] inline bool validShape(const ShellOperationRequest& request) noexcept
        {
            switch (request.operation)
            {
            case ShellOperation::createFolder:
            case ShellOperation::createFile:
                return validString(request.destination) && validString(request.newName) &&
                    request.newName.size() <= 255 && request.items.empty();
            case ShellOperation::deleteRecycle:
            case ShellOperation::deletePermanent:
                return request.destination.empty() && request.newName.empty() &&
                    !request.items.empty() && request.items.size() <= maxShellOperationItems;
            case ShellOperation::copy:
            case ShellOperation::move:
                return validString(request.destination) && request.newName.empty() &&
                    !request.items.empty() && request.items.size() <= maxShellOperationItems;
            case ShellOperation::emptyRecycleBin:
            case ShellOperation::restoreRecycleBin:
                return request.destination.empty() && request.newName.empty() &&
                    request.items.empty();
            }
            return false;
        }
    }

    [[nodiscard]] inline std::vector<std::uint8_t> encodeShellOperationRequest(
        const ShellOperationRequest& request)
    {
        using namespace shell_operation_request_detail;
        if (!validShape(request)) return {};
        std::vector<std::uint8_t> bytes;
        bytes.reserve(1024);
        archive_request_detail::appendU32(bytes, magic);
        archive_request_detail::appendU32(bytes, version);
        archive_request_detail::appendU32(bytes, 0);
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.operation));
        archive_request_detail::appendU32(bytes, request.confirmPermanent ? 1U : 0U);
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.items.size()));
        if (!archive_request_detail::appendString(bytes, request.destination) ||
            !archive_request_detail::appendString(bytes, request.newName))
            return {};
        for (const auto& item : request.items)
        {
            if (!validString(item) || !archive_request_detail::appendString(bytes, item) ||
                bytes.size() > maxShellOperationRequestBytes)
                return {};
        }
        archive_request_detail::writeU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
        return bytes;
    }

    [[nodiscard]] inline bool decodeShellOperationRequest(const std::uint8_t* data,
        std::size_t size, ShellOperationRequest& request)
    {
        using namespace shell_operation_request_detail;
        if (data == nullptr || size < 32 || size > maxShellOperationRequestBytes) return false;
        const std::uint8_t* current = data;
        const std::uint8_t* const end = data + size;
        std::uint32_t storedMagic{};
        std::uint32_t storedVersion{};
        std::uint32_t storedSize{};
        std::uint32_t operation{};
        std::uint32_t flags{};
        std::uint32_t count{};
        if (!archive_request_detail::readU32(current, end, storedMagic) ||
            !archive_request_detail::readU32(current, end, storedVersion) ||
            !archive_request_detail::readU32(current, end, storedSize) ||
            !archive_request_detail::readU32(current, end, operation) ||
            !archive_request_detail::readU32(current, end, flags) ||
            !archive_request_detail::readU32(current, end, count) || storedMagic != magic ||
            storedVersion != version || storedSize != size || flags > 1 ||
            count > maxShellOperationItems)
            return false;
        ShellOperationRequest decoded;
        decoded.operation = static_cast<ShellOperation>(operation);
        decoded.confirmPermanent = flags != 0;
        if (!archive_request_detail::readString(current, end, decoded.destination) ||
            !archive_request_detail::wellFormedUtf16(decoded.destination) ||
            !archive_request_detail::readString(current, end, decoded.newName) ||
            !archive_request_detail::wellFormedUtf16(decoded.newName))
            return false;
        decoded.items.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            std::u16string item;
            if (!archive_request_detail::readString(current, end, item) || !validString(item))
                return false;
            decoded.items.push_back(std::move(item));
        }
        if (current != end || !validShape(decoded)) return false;
        request = std::move(decoded);
        return true;
    }
}
