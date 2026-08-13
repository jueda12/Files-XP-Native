#include "shell_artifact_worker.h"
#include "archive_worker.h"
#include "../core/filename_policy.h"
#include "../core/shell_artifact_request.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
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
            core::ShellArtifactRequest& request) noexcept
        {
            HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
            if (mapping == nullptr) return false;
            void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            bool valid{};
            if (view != nullptr)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(view, &memory, sizeof(memory)) == sizeof(memory) &&
                    memory.RegionSize >= 40)
                {
                    const auto* data = static_cast<const std::uint8_t*>(view);
                    const std::uint32_t size = static_cast<std::uint32_t>(data[8]) |
                        (static_cast<std::uint32_t>(data[9]) << 8) |
                        (static_cast<std::uint32_t>(data[10]) << 16) |
                        (static_cast<std::uint32_t>(data[11]) << 24);
                    if (size >= 40 && size <= core::maxShellArtifactRequestBytes &&
                        size <= memory.RegionSize)
                    {
                        try
                        {
                            valid = core::decodeShellArtifactRequest(data, size, request);
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

        [[nodiscard]] HRESULT createShortcut(const core::ShellArtifactRequest& request,
            HANDLE cancel) noexcept
        {
            std::wstring folder;
            std::wstring name;
            std::wstring target;
            std::wstring arguments;
            std::wstring workingDirectory;
            std::wstring icon;
            try
            {
                folder = fromUtf16(request.destinationFolder);
                name = fromUtf16(request.name);
                target = fromUtf16(request.target);
                arguments = fromUtf16(request.arguments);
                workingDirectory = fromUtf16(request.workingDirectory);
                icon = fromUtf16(request.icon);
            }
            catch (...)
            {
                return E_OUTOFMEMORY;
            }
            if (!core::validWindowsFilename(name) || folder.empty() ||
                PathIsRelativeW(folder.c_str()) || target.empty())
                return E_INVALIDARG;
            const DWORD attributes = GetFileAttributesW(folder.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                return HRESULT_FROM_WIN32(attributes == INVALID_FILE_ATTRIBUTES ?
                    GetLastError() : ERROR_DIRECTORY);
            std::wstring destination;
            try
            {
                destination = folder;
                if (destination.back() != L'\\') destination.push_back(L'\\');
                destination += name;
            }
            catch (...)
            {
                return E_OUTOFMEMORY;
            }
            if (destination.size() >= 32767) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES)
                return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
            HRESULT status = cancellationStatus(cancel);
            IShellLinkW* link{};
            if (SUCCEEDED(status)) status = CoCreateInstance(CLSID_ShellLink, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
            if (SUCCEEDED(status)) status = link->SetPath(target.c_str());
            if (SUCCEEDED(status)) status = link->SetArguments(arguments.c_str());
            if (SUCCEEDED(status) && !workingDirectory.empty())
                status = link->SetWorkingDirectory(workingDirectory.c_str());
            if (SUCCEEDED(status) && !icon.empty()) status = link->SetIconLocation(icon.c_str(), 0);
            IPersistFile* persistence{};
            if (SUCCEEDED(status)) status = link->QueryInterface(IID_PPV_ARGS(&persistence));
            if (SUCCEEDED(status)) status = cancellationStatus(cancel);
            if (SUCCEEDED(status)) status = persistence->Save(destination.c_str(), TRUE);
            if (persistence != nullptr) persistence->Release();
            if (link != nullptr) link->Release();
            if (SUCCEEDED(status)) SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW,
                destination.c_str(), nullptr);
            return status;
        }

        [[nodiscard]] HRESULT createLibrary(const core::ShellArtifactRequest& request,
            HANDLE cancel) noexcept
        {
            std::wstring name;
            try
            {
                name = fromUtf16(request.name);
            }
            catch (...)
            {
                return E_OUTOFMEMORY;
            }
            if (!core::validWindowsFilename(name)) return E_INVALIDARG;
            HRESULT status = cancellationStatus(cancel);
            IShellLibrary* library{};
            if (SUCCEEDED(status)) status = SHCreateLibrary(IID_PPV_ARGS(&library));
            IShellItem* saved{};
            if (SUCCEEDED(status)) status = cancellationStatus(cancel);
            if (SUCCEEDED(status)) status = library->SaveInKnownFolder(FOLDERID_Libraries,
                name.c_str(), LSF_MAKEUNIQUENAME, &saved);
            if (saved != nullptr) saved->Release();
            if (library != nullptr) library->Release();
            return status;
        }

        [[nodiscard]] DWORD win32Status(HRESULT status) noexcept
        {
            if (SUCCEEDED(status)) return ERROR_SUCCESS;
            if (HRESULT_FACILITY(status) == FACILITY_WIN32) return HRESULT_CODE(status);
            return ERROR_GEN_FAILURE;
        }

        void writeSummary(HANDLE output) noexcept
        {
            constexpr char summary[] = "Created the Shell artifact.\r\n";
            DWORD written{};
            WriteFile(output, summary, static_cast<DWORD>(sizeof(summary) - 1), &written, nullptr);
        }
    }

    int runShellArtifactWorker(int argumentCount, wchar_t** arguments) noexcept
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
        core::ShellArtifactRequest request;
        if (SUCCEEDED(status) && !loadRequest(arguments[6], request))
            status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (SUCCEEDED(status))
        {
            switch (request.operation)
            {
            case core::ShellArtifactOperation::createShortcut:
                status = createShortcut(request, cancel);
                break;
            case core::ShellArtifactOperation::createLibrary:
                status = createLibrary(request, cancel);
                break;
            }
        }
        if (SUCCEEDED(status)) writeSummary(output);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (cancel != nullptr) CloseHandle(cancel);
        const DWORD result = win32Status(status);
        if (!PostMessageW(window, archiveCompleteMessage,
                static_cast<WPARAM>(generation), result))
            DeleteFileW(arguments[4]);
        return result == ERROR_SUCCESS ? 0 : 4;
    }
}
