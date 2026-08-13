#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr std::size_t maxArchiveRequestBytes = 4U * 1024U * 1024U;
    inline constexpr std::size_t maxArchiveItems = 256;
    inline constexpr std::size_t maxArchivePasswordCharacters = 1024;

    enum class ArchiveOperation : std::uint32_t
    {
        create7z,
        createZip,
        createTar,
        extract,
        count
    };

    enum class ArchiveCollision : std::uint32_t
    {
        rename,
        overwrite,
        skip,
        count
    };

    struct ArchiveRequest final
    {
        ArchiveOperation operation{ArchiveOperation::create7z};
        ArchiveCollision collision{ArchiveCollision::rename};
        std::u16string workingDirectory;
        std::u16string target;
        std::u16string password;
        std::vector<std::u16string> paths;
    };

    namespace archive_request_detail
    {
        inline constexpr std::uint32_t magic = 0x41525846U; // FXRA
        inline constexpr std::uint32_t version = 1;

        [[nodiscard]] inline bool wellFormedUtf16(std::u16string_view value) noexcept
        {
            for (std::size_t index = 0; index < value.size(); ++index)
            {
                const char16_t character = value[index];
                if (character >= 0xd800 && character <= 0xdbff)
                {
                    if (++index >= value.size() || value[index] < 0xdc00 || value[index] > 0xdfff)
                        return false;
                }
                else if (character >= 0xdc00 && character <= 0xdfff)
                    return false;
            }
            return true;
        }

        inline void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
        {
            bytes.push_back(static_cast<std::uint8_t>(value));
            bytes.push_back(static_cast<std::uint8_t>(value >> 8));
            bytes.push_back(static_cast<std::uint8_t>(value >> 16));
            bytes.push_back(static_cast<std::uint8_t>(value >> 24));
        }

        inline void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
            std::uint32_t value) noexcept
        {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
        }

        inline bool appendString(std::vector<std::uint8_t>& bytes, std::u16string_view value)
        {
            if (value.size() > 32767) return false;
            appendU32(bytes, static_cast<std::uint32_t>(value.size()));
            for (char16_t character : value)
            {
                bytes.push_back(static_cast<std::uint8_t>(character));
                bytes.push_back(static_cast<std::uint8_t>(character >> 8));
            }
            return bytes.size() <= maxArchiveRequestBytes;
        }

        inline bool readU32(const std::uint8_t*& current, const std::uint8_t* end,
            std::uint32_t& value) noexcept
        {
            if (end - current < 4) return false;
            value = static_cast<std::uint32_t>(current[0]) |
                (static_cast<std::uint32_t>(current[1]) << 8) |
                (static_cast<std::uint32_t>(current[2]) << 16) |
                (static_cast<std::uint32_t>(current[3]) << 24);
            current += 4;
            return true;
        }

        inline bool readString(const std::uint8_t*& current, const std::uint8_t* end,
            std::u16string& value)
        {
            std::uint32_t count{};
            if (!readU32(current, end, count) || count > 32767 ||
                static_cast<std::size_t>(end - current) < static_cast<std::size_t>(count) * 2)
                return false;
            value.clear();
            value.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index)
            {
                const char16_t character = static_cast<char16_t>(current[0] |
                    static_cast<std::uint16_t>(current[1]) << 8);
                if (character == u'\0') return false;
                value.push_back(character);
                current += 2;
            }
            return true;
        }
    }

    [[nodiscard]] inline bool validArchivePassword(ArchiveOperation operation,
        std::u16string_view password) noexcept
    {
        if (password.size() > maxArchivePasswordCharacters ||
            password.find_first_of(u"\r\n\0", 0, 3) != std::u16string_view::npos ||
            !archive_request_detail::wellFormedUtf16(password))
            return false;
        if (operation == ArchiveOperation::createTar) return password.empty();
        if (operation == ArchiveOperation::createZip)
        {
            for (char16_t character : password)
            {
                if (character < 0x20 || character > 0x7e) return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline std::vector<std::uint8_t> encodeArchiveRequest(
        const ArchiveRequest& request)
    {
        const auto invalidLine = [](std::u16string_view value)
        {
            return value.find_first_of(u"\r\n\0", 0, 3) != std::u16string_view::npos ||
                !archive_request_detail::wellFormedUtf16(value);
        };
        if (request.operation >= ArchiveOperation::count ||
            request.collision >= ArchiveCollision::count || request.workingDirectory.empty() ||
            request.target.empty() || request.paths.empty() || request.paths.size() > maxArchiveItems ||
            invalidLine(request.workingDirectory) || invalidLine(request.target) ||
            !validArchivePassword(request.operation, request.password))
            return {};
        std::vector<std::uint8_t> bytes;
        bytes.reserve(1024);
        archive_request_detail::appendU32(bytes, archive_request_detail::magic);
        archive_request_detail::appendU32(bytes, archive_request_detail::version);
        archive_request_detail::appendU32(bytes, 0);
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.operation));
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.collision));
        archive_request_detail::appendU32(bytes, static_cast<std::uint32_t>(request.paths.size()));
        if (!archive_request_detail::appendString(bytes, request.workingDirectory) ||
            !archive_request_detail::appendString(bytes, request.target) ||
            !archive_request_detail::appendString(bytes, request.password))
            return {};
        for (const auto& path : request.paths)
        {
            if (path.empty() || invalidLine(path) ||
                !archive_request_detail::appendString(bytes, path)) return {};
        }
        if (bytes.size() > maxArchiveRequestBytes) return {};
        archive_request_detail::writeU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
        return bytes;
    }

    [[nodiscard]] inline bool decodeArchiveRequest(const std::uint8_t* data, std::size_t size,
        ArchiveRequest& request)
    {
        using namespace archive_request_detail;
        const auto invalidLine = [](std::u16string_view value)
        {
            return value.find_first_of(u"\r\n\0", 0, 3) != std::u16string_view::npos ||
                !archive_request_detail::wellFormedUtf16(value);
        };
        if (data == nullptr || size < 24 || size > maxArchiveRequestBytes) return false;
        const std::uint8_t* current = data;
        const std::uint8_t* const end = data + size;
        std::uint32_t storedMagic{};
        std::uint32_t storedVersion{};
        std::uint32_t storedSize{};
        std::uint32_t operation{};
        std::uint32_t collision{};
        std::uint32_t count{};
        if (!readU32(current, end, storedMagic) || !readU32(current, end, storedVersion) ||
            !readU32(current, end, storedSize) || !readU32(current, end, operation) ||
            !readU32(current, end, collision) || !readU32(current, end, count) ||
            storedMagic != magic || storedVersion != version || storedSize != size ||
            operation >= static_cast<std::uint32_t>(ArchiveOperation::count) ||
            collision >= static_cast<std::uint32_t>(ArchiveCollision::count) ||
            count == 0 || count > maxArchiveItems)
            return false;
        ArchiveRequest decoded;
        decoded.operation = static_cast<ArchiveOperation>(operation);
        decoded.collision = static_cast<ArchiveCollision>(collision);
        if (!readString(current, end, decoded.workingDirectory) || decoded.workingDirectory.empty() ||
            !readString(current, end, decoded.target) || decoded.target.empty() ||
            !readString(current, end, decoded.password) ||
            invalidLine(decoded.workingDirectory) || invalidLine(decoded.target) ||
            !validArchivePassword(decoded.operation, decoded.password))
            return false;
        decoded.paths.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            std::u16string path;
            if (!readString(current, end, path) || path.empty() || invalidLine(path)) return false;
            decoded.paths.push_back(std::move(path));
        }
        if (current != end) return false;
        request = std::move(decoded);
        return true;
    }
}
