#include "shell_operation_worker.h"
#include "archive_worker.h"
#include "file_operation_progress_sink.h"
#include "../core/filename_policy.h"
#include "../core/shell_operation_request.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <new>
#include <algorithm>
#include <iterator>
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

        [[nodiscard]] bool loadRequest(const wchar_t* mappingName,
            core::ShellOperationRequest& request) noexcept
        {
            HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
            if (mapping == nullptr) return false;
            void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            bool valid{};
            if (view != nullptr)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(view, &memory, sizeof(memory)) == sizeof(memory) &&
                    memory.RegionSize >= 32)
                {
                    const auto* data = static_cast<const std::uint8_t*>(view);
                    const std::uint32_t size = static_cast<std::uint32_t>(data[8]) |
                        (static_cast<std::uint32_t>(data[9]) << 8) |
                        (static_cast<std::uint32_t>(data[10]) << 16) |
                        (static_cast<std::uint32_t>(data[11]) << 24);
                    if (size >= 32 && size <= core::maxShellOperationRequestBytes &&
                        size <= memory.RegionSize)
                    {
                        try
                        {
                            valid = core::decodeShellOperationRequest(data, size, request);
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

        [[nodiscard]] DWORD operationFlags(const core::ShellOperationRequest& request) noexcept
        {
            switch (request.operation)
            {
            case core::ShellOperation::createFolder:
                return FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
            case core::ShellOperation::createFile:
            case core::ShellOperation::deleteRecycle:
            case core::ShellOperation::copy:
            case core::ShellOperation::move:
                return FOF_ALLOWUNDO;
            case core::ShellOperation::deletePermanent:
                return request.confirmPermanent ? FOF_WANTNUKEWARNING : FOF_NOCONFIRMATION;
            case core::ShellOperation::emptyRecycleBin:
            case core::ShellOperation::restoreRecycleBin:
                return 0;
            }
            return 0;
        }

        [[nodiscard]] HRESULT invokeUndelete(IShellFolder* folder,
            PITEMID_CHILD* items, UINT count, HWND window) noexcept
        {
            if (folder == nullptr || items == nullptr || count == 0) return E_INVALIDARG;
            IContextMenu* contextMenu{};
            HRESULT status = folder->GetUIObjectOf(window, count,
                const_cast<PCUITEMID_CHILD_ARRAY>(items), IID_IContextMenu, nullptr,
                reinterpret_cast<void**>(&contextMenu));
            HMENU menu = SUCCEEDED(status) ? CreatePopupMenu() : nullptr;
            if (SUCCEEDED(status) && menu == nullptr)
                status = HRESULT_FROM_WIN32(GetLastError());
            constexpr UINT firstCommand = 1;
            constexpr UINT lastCommand = 0x7fff;
            if (SUCCEEDED(status))
                status = contextMenu->QueryContextMenu(menu, 0, firstCommand,
                    lastCommand, CMF_NORMAL | CMF_OPTIMIZEFORINVOKE);
            UINT command{};
            if (SUCCEEDED(status))
            {
                const int menuItems = GetMenuItemCount(menu);
                for (int index = 0; index < menuItems; ++index)
                {
                    const UINT candidate = GetMenuItemID(menu, index);
                    if (candidate < firstCommand || candidate > lastCommand) continue;
                    char canonical[64]{};
                    if (SUCCEEDED(contextMenu->GetCommandString(candidate - firstCommand,
                            GCS_VERBA, nullptr, canonical,
                            static_cast<UINT>(std::size(canonical)))) &&
                        lstrcmpiA(canonical, "undelete") == 0)
                    {
                        command = candidate;
                        break;
                    }
                }
                if (command == 0) status = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            }
            if (SUCCEEDED(status))
            {
                CMINVOKECOMMANDINFOEX invoke{};
                invoke.cbSize = sizeof(invoke);
                invoke.fMask = CMIC_MASK_UNICODE;
                invoke.hwnd = window;
                invoke.lpVerb = MAKEINTRESOURCEA(command - firstCommand);
                invoke.lpVerbW = MAKEINTRESOURCEW(command - firstCommand);
                invoke.nShow = SW_SHOWNORMAL;
                status = contextMenu->InvokeCommand(
                    reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
            }
            if (menu != nullptr) DestroyMenu(menu);
            if (contextMenu != nullptr) contextMenu->Release();
            return status;
        }

        [[nodiscard]] HRESULT restoreRecycleBin(HANDLE cancel, HWND window,
            std::uint32_t generation, std::size_t& affected) noexcept
        {
            constexpr std::size_t maximumItems = 100000;
            constexpr std::size_t batchItems = 128;
            IShellItem* recycleItem{};
            IShellFolder* recycleFolder{};
            IEnumIDList* enumerator{};
            HRESULT status = cancellationStatus(cancel);
            if (SUCCEEDED(status)) status = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder,
                KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&recycleItem));
            if (SUCCEEDED(status)) status = recycleItem->BindToHandler(nullptr, BHID_SFObject,
                IID_PPV_ARGS(&recycleFolder));
            if (SUCCEEDED(status)) status = recycleFolder->EnumObjects(window,
                SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &enumerator);
            std::vector<PITEMID_CHILD> items;
            if (SUCCEEDED(status))
            {
                try { items.reserve(4096); }
                catch (...) { status = E_OUTOFMEMORY; }
            }
            while (SUCCEEDED(status))
            {
                status = cancellationStatus(cancel);
                if (FAILED(status)) break;
                PITEMID_CHILD item{};
                ULONG fetched{};
                const HRESULT next = enumerator->Next(1, &item, &fetched);
                if (next == S_FALSE || fetched == 0)
                {
                    status = S_OK;
                    break;
                }
                if (FAILED(next))
                {
                    status = next;
                    break;
                }
                if (items.size() >= maximumItems)
                {
                    CoTaskMemFree(item);
                    status = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
                    break;
                }
                try { items.push_back(item); }
                catch (...)
                {
                    CoTaskMemFree(item);
                    status = E_OUTOFMEMORY;
                }
            }
            if (enumerator != nullptr) enumerator->Release();
            if (recycleItem != nullptr) recycleItem->Release();
            for (std::size_t offset = 0; SUCCEEDED(status) && offset < items.size();
                    offset += batchItems)
            {
                status = cancellationStatus(cancel);
                const UINT count = static_cast<UINT>(
                    std::min(batchItems, items.size() - offset));
                if (SUCCEEDED(status)) status = invokeUndelete(recycleFolder,
                    items.data() + offset, count, window);
                if (SUCCEEDED(status))
                {
                    affected += count;
                    const LPARAM percent = static_cast<LPARAM>(
                        (affected * 100ULL) / items.size());
                    PostMessageW(window, archiveProgressMessage, generation, percent);
                }
            }
            for (PITEMID_CHILD item : items) CoTaskMemFree(item);
            if (recycleFolder != nullptr) recycleFolder->Release();
            return status;
        }

        [[nodiscard]] HRESULT runOperation(const core::ShellOperationRequest& request,
            HANDLE cancel, HWND window, std::uint32_t generation,
            std::size_t& affected) noexcept
        {
            if (request.operation == core::ShellOperation::emptyRecycleBin)
            {
                HRESULT status = cancellationStatus(cancel);
                if (SUCCEEDED(status)) status = SHEmptyRecycleBinW(window, nullptr, 0);
                if (SUCCEEDED(status)) affected = 1;
                return status;
            }
            if (request.operation == core::ShellOperation::restoreRecycleBin)
                return restoreRecycleBin(cancel, window, generation, affected);
            auto* sink = new (std::nothrow) FileOperationProgressSink(
                cancel, window, archiveProgressMessage, generation);
            IFileOperation* operation{};
            DWORD cookie{};
            bool advised{};
            HRESULT status = sink == nullptr ? E_OUTOFMEMORY : S_OK;
            if (SUCCEEDED(status)) status = CoCreateInstance(CLSID_FileOperation, nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&operation));
            if (SUCCEEDED(status)) status = operation->SetOwnerWindow(window);
            if (SUCCEEDED(status)) status = operation->SetOperationFlags(operationFlags(request));
            if (SUCCEEDED(status))
            {
                status = operation->Advise(sink, &cookie);
                advised = SUCCEEDED(status);
            }

            IShellItem* destination{};
            if (SUCCEEDED(status) && !request.destination.empty())
            {
                try
                {
                    const std::wstring name = fromUtf16(request.destination);
                    status = SHCreateItemFromParsingName(name.c_str(), nullptr,
                        IID_PPV_ARGS(&destination));
                }
                catch (...)
                {
                    status = E_OUTOFMEMORY;
                }
            }

            if (SUCCEEDED(status) && (request.operation == core::ShellOperation::createFolder ||
                    request.operation == core::ShellOperation::createFile))
            {
                try
                {
                    const std::wstring name = fromUtf16(request.newName);
                    if (!core::validWindowsFilename(name))
                        status = E_INVALIDARG;
                    else
                        status = operation->NewItem(destination,
                            request.operation == core::ShellOperation::createFolder ?
                                FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                            name.c_str(), nullptr, nullptr);
                }
                catch (...)
                {
                    status = E_OUTOFMEMORY;
                }
            }

            for (const auto& encodedItem : request.items)
            {
                if (SUCCEEDED(status)) status = cancellationStatus(cancel);
                if (FAILED(status)) break;
                IShellItem* item{};
                try
                {
                    const std::wstring parsingName = fromUtf16(encodedItem);
                    status = SHCreateItemFromParsingName(parsingName.c_str(), nullptr,
                        IID_PPV_ARGS(&item));
                }
                catch (...)
                {
                    status = E_OUTOFMEMORY;
                }
                if (SUCCEEDED(status))
                {
                    switch (request.operation)
                    {
                    case core::ShellOperation::deleteRecycle:
                    case core::ShellOperation::deletePermanent:
                        status = operation->DeleteItem(item, nullptr);
                        break;
                    case core::ShellOperation::copy:
                        status = operation->CopyItem(item, destination, nullptr, nullptr);
                        break;
                    case core::ShellOperation::move:
                        status = operation->MoveItem(item, destination, nullptr, nullptr);
                        break;
                    default:
                        status = E_INVALIDARG;
                        break;
                    }
                }
                if (item != nullptr) item->Release();
            }

            if (SUCCEEDED(status)) status = operation->PerformOperations();
            BOOL aborted{};
            if (SUCCEEDED(status) && operation->GetAnyOperationsAborted(&aborted) == S_OK && aborted)
                status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if (SUCCEEDED(status)) status = cancellationStatus(cancel);
            if (SUCCEEDED(status) && sink->failures() != 0)
                status = HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY);
            if (operation != nullptr && advised) operation->Unadvise(cookie);
            if (destination != nullptr) destination->Release();
            if (operation != nullptr) operation->Release();
            if (sink != nullptr) sink->Release();
            if (SUCCEEDED(status)) affected = request.items.empty() ? 1 : request.items.size();
            return status;
        }

        [[nodiscard]] DWORD win32Status(HRESULT status) noexcept
        {
            if (SUCCEEDED(status)) return ERROR_SUCCESS;
            if (HRESULT_FACILITY(status) == FACILITY_WIN32) return HRESULT_CODE(status);
            return ERROR_GEN_FAILURE;
        }

        void writeSummary(HANDLE output, std::size_t affected) noexcept
        {
            char summary[96]{};
            const int length = std::snprintf(summary, sizeof(summary),
                "Completed %zu item(s).\r\n", affected);
            if (length > 0 && static_cast<std::size_t>(length) < sizeof(summary))
            {
                DWORD written{};
                WriteFile(output, summary, static_cast<DWORD>(length), &written, nullptr);
            }
        }
    }

    int runShellOperationWorker(int argumentCount, wchar_t** arguments) noexcept
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
        core::ShellOperationRequest request;
        if (SUCCEEDED(status) && !loadRequest(arguments[6], request))
            status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        std::size_t affected{};
        if (SUCCEEDED(status)) status = runOperation(request, cancel, window,
            static_cast<std::uint32_t>(generation), affected);
        if (SUCCEEDED(status)) writeSummary(output, affected);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (cancel != nullptr) CloseHandle(cancel);
        const DWORD result = win32Status(status);
        if (!PostMessageW(window, archiveCompleteMessage,
                static_cast<WPARAM>(generation), result))
            DeleteFileW(arguments[4]);
        return result == ERROR_SUCCESS ? 0 : 4;
    }
}
