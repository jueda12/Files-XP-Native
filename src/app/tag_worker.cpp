#include "tag_worker.h"
#include "archive_worker.h"
#include "../core/tag_codec.h"
#include "../core/tag_identity.h"
#include "../core/tag_request.h"
#include "../core/tag_result_codec.h"

#include <shobjidl.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace filesxp::app
{
    namespace
    {
        [[nodiscard]] bool parseUnsigned(const wchar_t* value,
            unsigned long long& parsed) noexcept
        {
            if (value == nullptr || *value == L'\0') return false;
            errno = 0;
            wchar_t* end{};
            parsed = std::wcstoull(value, &end, 10);
            return errno == 0 && end != value && *end == L'\0';
        }

        [[nodiscard]] std::wstring fromUtf16(const std::u16string& value)
        {
            static_assert(sizeof(wchar_t) == sizeof(char16_t));
            return std::wstring(reinterpret_cast<const wchar_t*>(value.data()), value.size());
        }

        [[nodiscard]] bool loadTagRequest(const wchar_t* mappingName,
            core::TagRequest& request) noexcept
        {
            HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
            if (mapping == nullptr) return false;
            void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            bool valid{};
            if (view != nullptr)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(view, &memory, sizeof(memory)) == sizeof(memory) &&
                    memory.RegionSize >= 20)
                {
                    const auto* data = static_cast<const std::uint8_t*>(view);
                    const std::uint32_t size = static_cast<std::uint32_t>(data[8]) |
                        (static_cast<std::uint32_t>(data[9]) << 8) |
                        (static_cast<std::uint32_t>(data[10]) << 16) |
                        (static_cast<std::uint32_t>(data[11]) << 24);
                    if (size >= 20 && size <= core::maxTagRequestBytes &&
                        size <= memory.RegionSize)
                    {
                        try
                        {
                            valid = core::decodeTagRequest(data, size, request);
                        }
                        catch (...)
                        {
                            valid = false;
                        }
                        SecureZeroMemory(view, size);
                    }
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(mapping);
            return valid;
        }

        [[nodiscard]] DWORD cancellationStatus(HANDLE cancel) noexcept
        {
            return cancel != nullptr && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0 ?
                ERROR_CANCELLED : ERROR_SUCCESS;
        }

        [[nodiscard]] HRESULT setKeywords(const wchar_t* path,
            const std::vector<std::wstring>& tags) noexcept
        {
            IPropertyStore* store{};
            HRESULT status = SHGetPropertyStoreFromParsingName(path, nullptr, GPS_READWRITE,
                IID_PPV_ARGS(&store));
            if (FAILED(status))
            {
                return status;
            }
            std::vector<const wchar_t*> values;
            try
            {
                values.reserve(tags.size());
                for (const auto& tag : tags) values.push_back(tag.c_str());
            }
            catch (...)
            {
                store->Release();
                return E_OUTOFMEMORY;
            }
            PROPVARIANT value{};
            status = InitPropVariantFromStringVector(values.empty() ? nullptr : values.data(),
                static_cast<ULONG>(values.size()), &value);
            if (SUCCEEDED(status))
            {
                status = store->SetValue(PKEY_Keywords, value);
            }
            if (SUCCEEDED(status))
            {
                status = store->Commit();
            }
            PropVariantClear(&value);
            store->Release();
            return status;
        }

        [[nodiscard]] std::wstring sidecarKey(const wchar_t* path) noexcept
        {
            HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            BY_HANDLE_FILE_INFORMATION information{};
            if (file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file, &information))
            {
                CloseHandle(file);
                wchar_t key[40]{};
                swprintf_s(key, L"V%08lX-F%08lX%08lX", information.dwVolumeSerialNumber,
                    information.nFileIndexHigh, information.nFileIndexLow);
                try { return std::wstring(key); }
                catch (...) { return {}; }
            }
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            std::uint64_t hash = 1469598103934665603ULL;
            for (const wchar_t* value = path; *value != L'\0'; ++value)
            {
                hash ^= static_cast<std::uint64_t>(std::towupper(*value));
                hash *= 1099511628211ULL;
            }
            wchar_t key[24]{};
            swprintf_s(key, L"P%016llX", static_cast<unsigned long long>(hash));
            try { return std::wstring(key); }
            catch (...) { return {}; }
        }

        [[nodiscard]] bool saveSidecar(const wchar_t* path,
            const std::vector<std::wstring>& tags) noexcept
        {
            HKEY key{};
            try
            {
                const std::wstring identity = sidecarKey(path);
                if (identity.empty()) return false;
                const std::wstring keyPath = L"Software\\FilesXPNative\\Tags\\" + identity;
                if (tags.empty())
                {
                    const LSTATUS result = RegDeleteTreeW(HKEY_CURRENT_USER, keyPath.c_str());
                    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
                }
                if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
                    return false;
                std::vector<wchar_t> encoded;
                for (const auto& tag : tags)
                {
                    encoded.insert(encoded.end(), tag.begin(), tag.end());
                    encoded.push_back(L'\0');
                }
                encoded.push_back(L'\0');
                const LSTATUS pathResult = RegSetValueExW(key, L"Path", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(path),
                    static_cast<DWORD>((std::wcslen(path) + 1) * sizeof(wchar_t)));
                const LSTATUS tagsResult = RegSetValueExW(key, L"Tags", 0, REG_MULTI_SZ,
                    reinterpret_cast<const BYTE*>(encoded.data()),
                    static_cast<DWORD>(encoded.size() * sizeof(wchar_t)));
                RegCloseKey(key);
                key = nullptr;
                return pathResult == ERROR_SUCCESS && tagsResult == ERROR_SUCCESS;
            }
            catch (...)
            {
                if (key != nullptr) RegCloseKey(key);
                return false;
            }
        }

        [[nodiscard]] bool readStringValue(HKEY key, const wchar_t* name,
            DWORD expectedType, std::vector<wchar_t>& value) noexcept
        {
            DWORD type{};
            DWORD bytes{};
            LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
            if (status != ERROR_SUCCESS || type != expectedType || bytes < sizeof(wchar_t) ||
                bytes > 128 * 1024 || bytes % sizeof(wchar_t) != 0)
            {
                return false;
            }
            try
            {
                value.assign(bytes / sizeof(wchar_t), L'\0');
            }
            catch (...)
            {
                value.clear();
                return false;
            }
            status = RegQueryValueExW(key, name, nullptr, &type,
                reinterpret_cast<BYTE*>(value.data()), &bytes);
            return status == ERROR_SUCCESS && type == expectedType && !value.empty() &&
                value.back() == L'\0';
        }

        [[nodiscard]] std::wstring resolveTagIdentity(std::wstring_view keyName) noexcept
        {
            core::TagFileIdentity identity;
            if (!core::parseTagFileIdentity(keyName, identity)) return {};
            std::wstring path;
            try { path.assign(32768, L'\0'); }
            catch (...) { return {}; }
            const DWORD drives = GetLogicalDrives();
            for (int index = 0; index < 26; ++index)
            {
                if ((drives & (1U << index)) == 0) continue;
                wchar_t root[]{static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
                const UINT driveType = GetDriveTypeW(root);
                if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE &&
                    driveType != DRIVE_RAMDISK)
                    continue;
                DWORD serial{};
                if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) ||
                    serial != identity.volumeSerial)
                    continue;
                wchar_t volumePath[]{L'\\', L'\\', L'.', L'\\', root[0], L':', L'\0'};
                HANDLE volume = CreateFileW(volumePath, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, 0, nullptr);
                if (volume == INVALID_HANDLE_VALUE) continue;
                FILE_ID_DESCRIPTOR descriptor{};
                descriptor.dwSize = sizeof(descriptor);
                descriptor.Type = FileIdType;
                descriptor.FileId.QuadPart = static_cast<LONGLONG>(identity.fileId);
                HANDLE item = OpenFileById(volume, &descriptor, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    FILE_FLAG_BACKUP_SEMANTICS);
                CloseHandle(volume);
                if (item == INVALID_HANDLE_VALUE) continue;
                const DWORD length = GetFinalPathNameByHandleW(item, path.data(),
                    static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                CloseHandle(item);
                if (length == 0 || length >= path.size()) continue;
                try
                {
                    path.resize(length);
                    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
                        path = L"\\\\" + path.substr(8);
                    else if (path.rfind(L"\\\\?\\", 0) == 0)
                        path.erase(0, 4);
                }
                catch (...)
                {
                    return {};
                }
                return path;
            }
            return {};
        }

        [[nodiscard]] bool sidecarMatches(HKEY key, std::wstring_view keyName,
            std::wstring_view expected,
            std::wstring& path) noexcept
        {
            try
            {
                std::vector<wchar_t> rawPath;
                std::vector<wchar_t> rawTags;
                if (!readStringValue(key, L"Path", REG_SZ, rawPath) ||
                    !readStringValue(key, L"Tags", REG_MULTI_SZ, rawTags) ||
                    rawPath.size() < 2 || rawPath[rawPath.size() - 2] == L'\0')
                    return false;
                path.assign(rawPath.data());
                if (path.empty() || path.size() >= 32767) return false;
                core::TagFileIdentity ignoredIdentity;
                const bool hasIdentity = core::parseTagFileIdentity(keyName, ignoredIdentity);
                const bool currentMatches = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
                    (!hasIdentity || core::sameTag(sidecarKey(path.c_str()), keyName));
                if (!currentMatches)
                {
                    path = resolveTagIdentity(keyName);
                    if (path.empty()) return false;
                    RegSetValueExW(key, L"Path", 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(path.c_str()),
                        static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
                }
                std::vector<std::wstring> tags;
                std::size_t offset{};
                while (offset + 1 < rawTags.size() && rawTags[offset] != L'\0')
                {
                    const std::size_t length = std::wcslen(rawTags.data() + offset);
                    if (length == 0 || offset + length >= rawTags.size()) return false;
                    tags.emplace_back(rawTags.data() + offset, length);
                    if (tags.size() > core::maxTags) return false;
                    offset += length + 1;
                }
                return core::containsTag(tags, expected);
            }
            catch (...)
            {
                path.clear();
                return false;
            }
        }

        void writeTagSummary(HANDLE output, std::size_t succeeded,
            std::size_t failed) noexcept
        {
            if (output == INVALID_HANDLE_VALUE) return;
            char summary[128]{};
            const int length = std::snprintf(summary, sizeof(summary),
                "Updated %zu item(s); %zu failed.\r\n", succeeded, failed);
            if (length > 0 && static_cast<std::size_t>(length) < sizeof(summary))
            {
                DWORD written{};
                WriteFile(output, summary, static_cast<DWORD>(length), &written, nullptr);
            }
        }
    }

    int runTagSetWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 7 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long generation{};
        if (!parseUnsigned(arguments[2], windowValue) || !parseUnsigned(arguments[3], generation) ||
            generation > UINT_MAX || arguments[4] == nullptr || arguments[5] == nullptr ||
            arguments[6] == nullptr || std::wcslen(arguments[4]) >= 32767 ||
            std::wcslen(arguments[5]) >= 260 || std::wcslen(arguments[6]) >= 260)
            return 3;
        const HWND notificationWindow =
            reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, arguments[5]);
        DWORD status = cancel == nullptr ? GetLastError() : ERROR_SUCCESS;
        HANDLE output = INVALID_HANDLE_VALUE;
        if (status == ERROR_SUCCESS)
        {
            output = CreateFileW(arguments[4], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (output == INVALID_HANDLE_VALUE) status = GetLastError();
        }
        core::TagRequest request;
        if (status == ERROR_SUCCESS && !loadTagRequest(arguments[6], request))
            status = ERROR_INVALID_DATA;
        std::vector<std::wstring> tags;
        if (status == ERROR_SUCCESS)
        {
            try
            {
                tags.reserve(request.tags.size());
                for (const auto& tag : request.tags) tags.push_back(fromUtf16(tag));
            }
            catch (...)
            {
                status = ERROR_NOT_ENOUGH_MEMORY;
            }
        }
        std::size_t succeeded{};
        std::size_t failed{};
        for (std::size_t index = 0; status == ERROR_SUCCESS && index < request.paths.size(); ++index)
        {
            status = cancellationStatus(cancel);
            if (status != ERROR_SUCCESS) break;
            std::wstring path;
            try
            {
                path = fromUtf16(request.paths[index]);
            }
            catch (...)
            {
                status = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
            const bool sidecarSaved = saveSidecar(path.c_str(), tags);
            const HRESULT propertyStatus = setKeywords(path.c_str(), tags);
            if (sidecarSaved || SUCCEEDED(propertyStatus)) ++succeeded;
            else ++failed;
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path.c_str(), nullptr);
            const LPARAM percent = static_cast<LPARAM>(
                (static_cast<unsigned long long>(index + 1) * 100ULL) / request.paths.size());
            PostMessageW(notificationWindow, archiveProgressMessage,
                static_cast<WPARAM>(generation), percent);
        }
        if (status == ERROR_SUCCESS) status = cancellationStatus(cancel);
        if (status == ERROR_SUCCESS && failed != 0) status = ERROR_PARTIAL_COPY;
        writeTagSummary(output, succeeded, failed);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (cancel != nullptr) CloseHandle(cancel);
        if (!PostMessageW(notificationWindow, archiveCompleteMessage,
                static_cast<WPARAM>(generation), static_cast<LPARAM>(status)))
            DeleteFileW(arguments[4]);
        return status == ERROR_SUCCESS ? 0 : 4;
    }

    int runTagSearchWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 7 || arguments == nullptr || arguments[4] == nullptr ||
            arguments[5] == nullptr || arguments[6] == nullptr ||
            std::wcslen(arguments[5]) >= 32767 || std::wcslen(arguments[6]) >= 260)
        {
            return 2;
        }
        unsigned long long windowValue{};
        unsigned long long workerToken{};
        if (!parseUnsigned(arguments[2], windowValue) ||
            !parseUnsigned(arguments[3], workerToken) || workerToken == 0 ||
            workerToken > UINTPTR_MAX)
        {
            return 3;
        }
        const HWND notificationWindow =
            reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        if (notificationWindow == nullptr) return 3;
        std::vector<std::wstring> normalized;
        DWORD failure{};
        try
        {
            if (!core::normalizeTags(arguments[4], normalized) || normalized.size() != 1)
                failure = ERROR_INVALID_DATA;
        }
        catch (...)
        {
            failure = ERROR_NOT_ENOUGH_MEMORY;
        }
        if (failure != ERROR_SUCCESS)
        {
            PostMessageW(notificationWindow, tagSearchCompleteMessage,
                static_cast<WPARAM>(workerToken), static_cast<LPARAM>(failure));
            return 4;
        }
        HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, arguments[6]);
        if (cancel == nullptr)
        {
            failure = GetLastError();
            PostMessageW(notificationWindow, tagSearchCompleteMessage,
                static_cast<WPARAM>(workerToken), static_cast<LPARAM>(failure));
            return 4;
        }
        std::vector<std::wstring> paths;
        HKEY root{};
        LSTATUS registryStatus = RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\FilesXPNative\\Tags", 0, KEY_ENUMERATE_SUB_KEYS, &root);
        if (registryStatus == ERROR_FILE_NOT_FOUND)
        {
            registryStatus = ERROR_SUCCESS;
        }
        for (DWORD index = 0; registryStatus == ERROR_SUCCESS && root != nullptr &&
                paths.size() < core::maxTagResults; ++index)
        {
            if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
            {
                registryStatus = ERROR_CANCELLED;
                break;
            }
            wchar_t childName[256]{};
            DWORD length = static_cast<DWORD>(std::size(childName));
            const LSTATUS enumStatus = RegEnumKeyExW(root, index, childName, &length,
                nullptr, nullptr, nullptr, nullptr);
            if (enumStatus == ERROR_NO_MORE_ITEMS) break;
            if (enumStatus != ERROR_SUCCESS)
            {
                registryStatus = enumStatus;
                break;
            }
            HKEY child{};
            if (RegOpenKeyExW(root, childName, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &child) == ERROR_SUCCESS)
            {
                std::wstring path;
                if (sidecarMatches(child, childName, normalized.front(), path))
                {
                    try { paths.push_back(std::move(path)); }
                    catch (...) { registryStatus = ERROR_NOT_ENOUGH_MEMORY; }
                }
                RegCloseKey(child);
            }
        }
        if (root != nullptr) RegCloseKey(root);
        CloseHandle(cancel);

        failure = registryStatus;
        std::vector<wchar_t> encoded;
        if (failure == ERROR_SUCCESS)
        {
            try { encoded = core::encodeTagResults(paths); }
            catch (...) { failure = ERROR_NOT_ENOUGH_MEMORY; }
        }
        if (failure == ERROR_SUCCESS && encoded.empty()) failure = ERROR_BUFFER_OVERFLOW;
        HANDLE file = INVALID_HANDLE_VALUE;
        if (failure == ERROR_SUCCESS)
        {
            file = CreateFileW(arguments[5], GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (file == INVALID_HANDLE_VALUE) failure = GetLastError();
        }
        if (failure == ERROR_SUCCESS)
        {
            const DWORD bytes = static_cast<DWORD>(encoded.size() * sizeof(wchar_t));
            DWORD written{};
            if (!WriteFile(file, encoded.data(), bytes, &written, nullptr) || written != bytes)
                failure = GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT : GetLastError();
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!PostMessageW(notificationWindow, tagSearchCompleteMessage,
                static_cast<WPARAM>(workerToken), static_cast<LPARAM>(failure)))
            DeleteFileW(arguments[5]);
        return failure == ERROR_SUCCESS ? 0 : 4;
    }
}
