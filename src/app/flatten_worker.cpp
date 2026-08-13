#include "flatten_worker.h"
#include "archive_worker.h"
#include "file_operation_progress_sink.h"
#include "../core/flatten_policy.h"

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

        [[nodiscard]] HRESULT cancellationStatus(HANDLE cancel) noexcept
        {
            return cancel != nullptr && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0 ?
                HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
        }

        [[nodiscard]] HRESULT enumerateFolder(const std::wstring& folder, bool insideSubfolder,
            std::size_t depth, std::vector<std::wstring>& moveItems,
            std::vector<std::wstring>& folders, HANDLE cancel)
        {
            HRESULT status = cancellationStatus(cancel);
            if (FAILED(status)) return status;
            if (folder.size() + 3 >= 32767) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            std::wstring pattern = folder;
            if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
            pattern.push_back(L'*');
            WIN32_FIND_DATAW data{};
            HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (search == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
                search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                    FindExSearchNameMatch, nullptr, 0);
            if (search == INVALID_HANDLE_VALUE)
            {
                const DWORD failure = GetLastError();
                return failure == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(failure);
            }
            do
            {
                status = cancellationStatus(cancel);
                if (FAILED(status)) break;
                if (std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0)
                    continue;
                std::wstring child = folder;
                if (!child.empty() && child.back() != L'\\') child.push_back(L'\\');
                child += data.cFileName;
                if (child.size() >= 32767)
                {
                    status = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
                    break;
                }
                const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                const core::FlattenAction action = core::flattenAction(
                    insideSubfolder, directory, reparse, depth);
                if (action == core::FlattenAction::descend)
                {
                    status = enumerateFolder(child, true, depth + 1, moveItems, folders, cancel);
                    if (SUCCEEDED(status)) folders.push_back(std::move(child));
                }
                else if (action == core::FlattenAction::move)
                {
                    moveItems.push_back(std::move(child));
                }
                if (moveItems.size() + folders.size() > core::maxFlattenItems)
                    status = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
            }
            while (SUCCEEDED(status) && FindNextFileW(search, &data));
            if (SUCCEEDED(status))
            {
                const DWORD failure = GetLastError();
                if (failure != ERROR_NO_MORE_FILES) status = HRESULT_FROM_WIN32(failure);
            }
            FindClose(search);
            return status;
        }

        [[nodiscard]] HRESULT runFlatten(const std::wstring& rootPath, HANDLE cancel,
            HWND window, std::uint32_t generation, std::size_t& moved,
            std::size_t& removed) noexcept
        {
            const DWORD attributes = GetFileAttributesW(rootPath.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return E_INVALIDARG;
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return HRESULT_FROM_WIN32(ERROR_CANT_ACCESS_FILE);
            std::vector<std::wstring> moveItems;
            std::vector<std::wstring> folders;
            HRESULT status{};
            try
            {
                status = enumerateFolder(rootPath, false, 0, moveItems, folders, cancel);
            }
            catch (...)
            {
                return E_OUTOFMEMORY;
            }
            if (FAILED(status) || (moveItems.empty() && folders.empty())) return status;

            IShellItem* root{};
            IFileOperation* operation{};
            FileOperationProgressSink* sink = new (std::nothrow) FileOperationProgressSink(
                cancel, window, archiveProgressMessage, generation);
            DWORD cookie{};
            bool advised{};
            if (sink == nullptr) status = E_OUTOFMEMORY;
            if (SUCCEEDED(status)) status = SHCreateItemFromParsingName(rootPath.c_str(), nullptr,
                IID_PPV_ARGS(&root));
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
            for (const auto& path : moveItems)
            {
                if (FAILED(status)) break;
                IShellItem* item{};
                status = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
                if (SUCCEEDED(status)) status = operation->MoveItem(item, root, nullptr, nullptr);
                if (item != nullptr) item->Release();
            }
            for (const auto& path : folders)
            {
                if (FAILED(status)) break;
                IShellItem* item{};
                status = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
                if (SUCCEEDED(status)) status = operation->DeleteItem(item, nullptr);
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
            if (root != nullptr) root->Release();
            if (sink != nullptr) sink->Release();
            if (SUCCEEDED(status))
            {
                moved = moveItems.size();
                removed = folders.size();
            }
            return status;
        }

        [[nodiscard]] DWORD win32Status(HRESULT status) noexcept
        {
            if (SUCCEEDED(status)) return ERROR_SUCCESS;
            if (HRESULT_FACILITY(status) == FACILITY_WIN32) return HRESULT_CODE(status);
            return ERROR_GEN_FAILURE;
        }

        void writeSummary(HANDLE output, std::size_t moved, std::size_t removed) noexcept
        {
            char summary[160]{};
            const int length = std::snprintf(summary, sizeof(summary),
                "Moved %zu item(s); removed %zu empty folder(s).\r\n", moved, removed);
            if (length > 0 && static_cast<std::size_t>(length) < sizeof(summary))
            {
                DWORD written{};
                WriteFile(output, summary, static_cast<DWORD>(length), &written, nullptr);
            }
        }
    }

    int runFlattenWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 7 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long generation{};
        if (!parseUnsigned(arguments[2], windowValue) || !parseUnsigned(arguments[3], generation) ||
            generation > UINT_MAX || arguments[4] == nullptr || arguments[5] == nullptr ||
            arguments[6] == nullptr || std::wcslen(arguments[4]) >= 32767 ||
            std::wcslen(arguments[5]) >= 260 || std::wcslen(arguments[6]) >= 32767 ||
            PathIsRelativeW(arguments[6]))
            return 3;
        const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, arguments[5]);
        HANDLE output = CreateFileW(arguments[4], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        HRESULT status = cancel == nullptr ? HRESULT_FROM_WIN32(GetLastError()) : S_OK;
        if (SUCCEEDED(status) && output == INVALID_HANDLE_VALUE)
            status = HRESULT_FROM_WIN32(GetLastError());
        std::size_t moved{};
        std::size_t removed{};
        if (SUCCEEDED(status))
        {
            try
            {
                status = runFlatten(std::wstring(arguments[6]), cancel, window,
                    static_cast<std::uint32_t>(generation), moved, removed);
            }
            catch (...)
            {
                status = E_OUTOFMEMORY;
            }
        }
        if (SUCCEEDED(status)) writeSummary(output, moved, removed);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (cancel != nullptr) CloseHandle(cancel);
        const DWORD result = win32Status(status);
        if (!PostMessageW(window, archiveCompleteMessage,
                static_cast<WPARAM>(generation), result))
            DeleteFileW(arguments[4]);
        return result == ERROR_SUCCESS ? 0 : 4;
    }
}
