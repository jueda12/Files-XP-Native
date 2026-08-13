#include "bulk_rename_worker.h"
#include "archive_worker.h"
#include "file_operation_progress_sink.h"
#include "../core/bulk_rename_request.h"
#include "../core/filename_policy.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <new>
#include <string>

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
            core::BulkRenameRequest& request) noexcept
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
                    if (size >= 20 && size <= core::maxBulkRenameRequestBytes &&
                        size <= memory.RegionSize)
                    {
                        try
                        {
                            valid = core::decodeBulkRenameRequest(data, size, request);
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

        [[nodiscard]] HRESULT cancellationStatus(HANDLE cancel) noexcept
        {
            return cancel != nullptr && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0 ?
                HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
        }

        [[nodiscard]] HRESULT runRename(const core::BulkRenameRequest& request,
            HANDLE cancel, HWND window, std::uint32_t generation,
            std::size_t& renamed) noexcept
        {
            std::wstring baseName;
            try
            {
                baseName = fromUtf16(request.baseName);
            }
            catch (...)
            {
                return E_OUTOFMEMORY;
            }
            if (!core::validWindowsFilename(baseName, false)) return E_INVALIDARG;
            IFileOperation* operation{};
            auto* sink = new (std::nothrow) FileOperationProgressSink(
                cancel, window, archiveProgressMessage, generation);
            HRESULT status = sink == nullptr ? E_OUTOFMEMORY : S_OK;
            DWORD cookie{};
            bool advised{};
            if (SUCCEEDED(status)) status = CoCreateInstance(CLSID_FileOperation, nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&operation));
            if (SUCCEEDED(status)) status = operation->SetOwnerWindow(window);
            if (SUCCEEDED(status)) status = operation->SetOperationFlags(
                FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION);
            if (SUCCEEDED(status))
            {
                status = operation->Advise(sink, &cookie);
                advised = SUCCEEDED(status);
            }
            for (const auto& encodedPath : request.paths)
            {
                if (SUCCEEDED(status)) status = cancellationStatus(cancel);
                if (FAILED(status)) break;
                std::wstring path;
                try
                {
                    path = fromUtf16(encodedPath);
                }
                catch (...)
                {
                    status = E_OUTOFMEMORY;
                    break;
                }
                if (path.empty() || path.size() >= 32767 || PathIsRelativeW(path.c_str()))
                {
                    status = E_INVALIDARG;
                    break;
                }
                const DWORD attributes = GetFileAttributesW(path.c_str());
                const DWORD attributeFailure = attributes == INVALID_FILE_ATTRIBUTES ?
                    GetLastError() : ERROR_SUCCESS;
                const wchar_t* originalName = PathFindFileNameW(path.c_str());
                std::wstring target;
                try
                {
                    if (attributes != INVALID_FILE_ATTRIBUTES && originalName != nullptr &&
                        *originalName != L'\0')
                        target = core::bulkRenameTarget(baseName, originalName,
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
                }
                catch (...)
                {
                    status = E_OUTOFMEMORY;
                    break;
                }
                if (target.empty())
                {
                    status = attributes == INVALID_FILE_ATTRIBUTES ?
                        HRESULT_FROM_WIN32(attributeFailure) : E_INVALIDARG;
                    break;
                }
                IShellItem* item{};
                status = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
                if (SUCCEEDED(status)) status = operation->RenameItem(item, target.c_str(), nullptr);
                if (item != nullptr) item->Release();
            }
            if (SUCCEEDED(status)) status = operation->PerformOperations();
            BOOL aborted{};
            if (SUCCEEDED(status) && operation->GetAnyOperationsAborted(&aborted) == S_OK && aborted)
                status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if (SUCCEEDED(status)) status = cancellationStatus(cancel);
            if (SUCCEEDED(status) && sink != nullptr && sink->failures() != 0)
                status = HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY);
            if (operation != nullptr && advised) operation->Unadvise(cookie);
            if (operation != nullptr) operation->Release();
            if (sink != nullptr) sink->Release();
            if (SUCCEEDED(status)) renamed = request.paths.size();
            return status;
        }

        [[nodiscard]] DWORD win32Status(HRESULT status) noexcept
        {
            if (SUCCEEDED(status)) return ERROR_SUCCESS;
            if (HRESULT_FACILITY(status) == FACILITY_WIN32) return HRESULT_CODE(status);
            return ERROR_GEN_FAILURE;
        }

        void writeSummary(HANDLE output, std::size_t renamed) noexcept
        {
            char summary[96]{};
            const int length = std::snprintf(summary, sizeof(summary),
                "Renamed %zu item(s).\r\n", renamed);
            if (length > 0 && static_cast<std::size_t>(length) < sizeof(summary))
            {
                DWORD written{};
                WriteFile(output, summary, static_cast<DWORD>(length), &written, nullptr);
            }
        }
    }

    int runBulkRenameWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 7 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long generation{};
        if (!parseUnsigned(arguments[2], windowValue) || !parseUnsigned(arguments[3], generation) ||
            generation > UINT_MAX || arguments[4] == nullptr || arguments[5] == nullptr ||
            arguments[6] == nullptr || std::wcslen(arguments[4]) >= 32767 ||
            std::wcslen(arguments[5]) >= 260 || std::wcslen(arguments[6]) >= 260)
            return 3;
        const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, arguments[5]);
        HANDLE output = CreateFileW(arguments[4], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        HRESULT status = cancel == nullptr ? HRESULT_FROM_WIN32(GetLastError()) : S_OK;
        if (SUCCEEDED(status) && output == INVALID_HANDLE_VALUE)
            status = HRESULT_FROM_WIN32(GetLastError());
        core::BulkRenameRequest request;
        if (SUCCEEDED(status) && !loadRequest(arguments[6], request))
            status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        std::size_t renamed{};
        if (SUCCEEDED(status)) status = runRename(request, cancel, window,
            static_cast<std::uint32_t>(generation), renamed);
        if (SUCCEEDED(status)) writeSummary(output, renamed);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (cancel != nullptr) CloseHandle(cancel);
        const DWORD result = win32Status(status);
        if (!PostMessageW(window, archiveCompleteMessage,
                static_cast<WPARAM>(generation), result))
            DeleteFileW(arguments[4]);
        return result == ERROR_SUCCESS ? 0 : 4;
    }
}
