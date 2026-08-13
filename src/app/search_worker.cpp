#include "search_worker.h"
#include "tag_worker.h"
#include "../core/search_request.h"
#include "../core/tag_result_codec.h"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <string_view>
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

        [[nodiscard]] bool loadRequest(const wchar_t* mappingName,
            core::SearchRequest& request) noexcept
        {
            HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
            if (mapping == nullptr) return false;
            void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            bool valid{};
            if (view != nullptr)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(view, &memory, sizeof(memory)) == sizeof(memory) &&
                    memory.RegionSize >= 24)
                {
                    const auto* data = static_cast<const std::uint8_t*>(view);
                    const std::uint32_t size = static_cast<std::uint32_t>(data[8]) |
                        (static_cast<std::uint32_t>(data[9]) << 8) |
                        (static_cast<std::uint32_t>(data[10]) << 16) |
                        (static_cast<std::uint32_t>(data[11]) << 24);
                    if (size >= 24 && size <= core::maxSearchRequestBytes &&
                        size <= memory.RegionSize)
                    {
                        try
                        {
                            valid = core::decodeSearchRequest(data, size, request);
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

        [[nodiscard]] bool containsOrdinalIgnoreCase(std::wstring_view value,
            std::wstring_view query) noexcept
        {
            if (query.empty() || query.size() > value.size() ||
                value.size() > static_cast<std::size_t>(INT_MAX) ||
                query.size() > static_cast<std::size_t>(INT_MAX))
                return false;
            const int queryLength = static_cast<int>(query.size());
            for (std::size_t offset = 0; offset + query.size() <= value.size(); ++offset)
            {
                if (CompareStringOrdinal(value.data() + offset, queryLength,
                        query.data(), queryLength, TRUE) == CSTR_EQUAL)
                    return true;
            }
            return false;
        }

        struct SearchState final
        {
            std::wstring_view query;
            HANDLE cancel{};
            bool includeHidden{};
            bool truncated{};
            std::size_t resultCharacters{};
            std::vector<std::wstring> results;
        };

        [[nodiscard]] DWORD searchDirectory(const std::wstring& directory,
            std::size_t depth, SearchState& state)
        {
            DWORD status = cancellationStatus(state.cancel);
            if (status != ERROR_SUCCESS || state.truncated) return status;
            if (depth > core::maxSearchDepth) return ERROR_SUCCESS;
            if (directory.size() > 32764) return ERROR_SUCCESS;
            const std::wstring pattern = directory + L"\\*";
            WIN32_FIND_DATAW data{};
            HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (search == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
                search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                    FindExSearchNameMatch, nullptr, 0);
            if (search == INVALID_HANDLE_VALUE)
            {
                const DWORD failure = GetLastError();
                return failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND ||
                    failure == ERROR_ACCESS_DENIED ? ERROR_SUCCESS : failure;
            }
            do
            {
                status = cancellationStatus(state.cancel);
                if (status != ERROR_SUCCESS) break;
                if ((data.cFileName[0] == L'.' && data.cFileName[1] == L'\0') ||
                    (data.cFileName[0] == L'.' && data.cFileName[1] == L'.' &&
                        data.cFileName[2] == L'\0'))
                    continue;
                const bool hidden = (data.dwFileAttributes &
                    (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
                if (!state.includeHidden && hidden) continue;
                const std::size_t nameLength = std::wcslen(data.cFileName);
                if (directory.size() + nameLength + 2 >= 32767) continue;
                const std::wstring path = directory + L"\\" + data.cFileName;
                if (containsOrdinalIgnoreCase(
                        std::wstring_view(data.cFileName, nameLength), state.query))
                {
                    if (state.results.size() >= core::maxTagResults ||
                        path.size() + 1 > core::maxTagResultCharacters -
                            std::min(core::maxTagResultCharacters, state.resultCharacters))
                    {
                        state.truncated = true;
                        break;
                    }
                    state.resultCharacters += path.size() + 1;
                    state.results.push_back(path);
                }
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                    (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
                {
                    status = searchDirectory(path, depth + 1, state);
                    if (status != ERROR_SUCCESS || state.truncated) break;
                }
            } while (FindNextFileW(search, &data));
            if (status == ERROR_SUCCESS && !state.truncated)
            {
                const DWORD failure = GetLastError();
                if (failure != ERROR_NO_MORE_FILES) status = failure;
            }
            FindClose(search);
            return status;
        }

        [[nodiscard]] DWORD writeResults(const wchar_t* path,
            const std::vector<std::wstring>& results) noexcept
        {
            try
            {
                const std::vector<wchar_t> encoded = core::encodeTagResults(results);
                if (encoded.empty()) return ERROR_BUFFER_OVERFLOW;
                HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (file == INVALID_HANDLE_VALUE) return GetLastError();
                const DWORD bytes = static_cast<DWORD>(encoded.size() * sizeof(wchar_t));
                DWORD written{};
                const bool saved = WriteFile(file, encoded.data(), bytes, &written, nullptr) != FALSE &&
                    written == bytes;
                const DWORD failure = saved ? ERROR_SUCCESS :
                    (GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT : GetLastError());
                CloseHandle(file);
                return failure;
            }
            catch (...)
            {
                return ERROR_NOT_ENOUGH_MEMORY;
            }
        }
    }

    int runFallbackSearchWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 7 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long workerToken{};
        if (!parseUnsigned(arguments[2], windowValue) || !parseUnsigned(arguments[3], workerToken) ||
            workerToken == 0 || workerToken > UINTPTR_MAX || arguments[4] == nullptr || arguments[5] == nullptr ||
            arguments[6] == nullptr || std::wcslen(arguments[4]) >= 32767 ||
            std::wcslen(arguments[5]) >= 260 || std::wcslen(arguments[6]) >= 260)
            return 3;
        const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, arguments[5]);
        DWORD status = cancel == nullptr ? GetLastError() : ERROR_SUCCESS;
        core::SearchRequest request;
        if (status == ERROR_SUCCESS && !loadRequest(arguments[6], request))
            status = ERROR_INVALID_DATA;
        SearchState state{};
        try
        {
            const std::wstring root = fromUtf16(request.root);
            const std::wstring query = fromUtf16(request.query);
            state.query = query;
            state.cancel = cancel;
            state.includeHidden = request.includeHidden;
            const DWORD attributes = status == ERROR_SUCCESS ? GetFileAttributesW(root.c_str()) :
                INVALID_FILE_ATTRIBUTES;
            if (status == ERROR_SUCCESS && (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0))
                status = ERROR_PATH_NOT_FOUND;
            if (status == ERROR_SUCCESS) status = searchDirectory(root, 0, state);
            if (status == ERROR_SUCCESS) status = writeResults(arguments[4], state.results);
        }
        catch (...)
        {
            status = ERROR_NOT_ENOUGH_MEMORY;
        }
        if (cancel != nullptr) CloseHandle(cancel);
        if (!PostMessageW(window, tagSearchCompleteMessage,
                static_cast<WPARAM>(workerToken), static_cast<LPARAM>(status)))
            DeleteFileW(arguments[4]);
        return status == ERROR_SUCCESS ? 0 : 4;
    }
}
