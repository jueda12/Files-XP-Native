#include "preview_worker.h"
#include "../core/preview_policy.h"
#include "../core/preview_provider.h"
#include "../core/windows_command_line.h"

#include <sddl.h>
#include <shellapi.h>
#include <cerrno>
#include <climits>
#include <array>
#include <cstdint>
#include <cstring>
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

        [[nodiscard]] bool decode(const std::vector<std::uint8_t>& bytes,
            std::wstring& text)
        {
            text.clear();
            if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe)
            {
                const std::size_t characters = (bytes.size() - 2) / 2;
                text.resize(characters);
                std::memcpy(text.data(), bytes.data() + 2, characters * sizeof(wchar_t));
            }
            else if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff)
            {
                const std::size_t characters = (bytes.size() - 2) / 2;
                text.resize(characters);
                for (std::size_t index = 0; index < characters; ++index)
                {
                    text[index] = static_cast<wchar_t>((bytes[2 + index * 2] << 8) |
                        bytes[3 + index * 2]);
                }
            }
            else
            {
                const std::size_t offset = bytes.size() >= 3 && bytes[0] == 0xef &&
                    bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
                const int length = static_cast<int>(bytes.size() - offset);
                UINT codePage = CP_UTF8;
                DWORD flags = MB_ERR_INVALID_CHARS;
                int required = MultiByteToWideChar(codePage, flags,
                    reinterpret_cast<const char*>(bytes.data() + offset), length, nullptr, 0);
                if (required == 0)
                {
                    codePage = CP_ACP;
                    flags = 0;
                    required = MultiByteToWideChar(codePage, flags,
                        reinterpret_cast<const char*>(bytes.data() + offset), length, nullptr, 0);
                }
                if (required < 0 || static_cast<std::size_t>(required) > core::maxTextPreviewCharacters)
                    return false;
                text.resize(static_cast<std::size_t>(required));
                if (required != 0 && MultiByteToWideChar(codePage, flags,
                        reinterpret_cast<const char*>(bytes.data() + offset), length,
                        text.data(), required) == 0)
                    return false;
            }
            for (wchar_t& character : text)
            {
                if (character == L'\0') character = 0xfffd;
            }
            return true;
        }

        [[nodiscard]] bool regularFile(const std::wstring& path) noexcept
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        [[nodiscard]] std::wstring environmentPath(const wchar_t* name)
        {
            std::wstring value(32768, L'\0');
            const DWORD length = GetEnvironmentVariableW(name, value.data(),
                static_cast<DWORD>(value.size()));
            if (length == 0 || length >= value.size()) return {};
            value.resize(length);
            return value;
        }

        [[nodiscard]] std::wstring findPowerToysPeek()
        {
            const std::array<std::wstring, 3> candidates{
                environmentPath(L"ProgramFiles") + L"\\PowerToys\\WinUI3Apps\\PowerToys.Peek.UI.exe",
                environmentPath(L"ProgramW6432") + L"\\PowerToys\\WinUI3Apps\\PowerToys.Peek.UI.exe",
                environmentPath(L"LOCALAPPDATA") + L"\\PowerToys\\WinUI3Apps\\PowerToys.Peek.UI.exe"};
            for (const auto& path : candidates)
                if (regularFile(path)) return path;
            return {};
        }

        [[nodiscard]] bool quickLook(const wchar_t* path, bool toggle)
        {
            HANDLE token{};
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
            DWORD bytes{};
            GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
            std::vector<std::uint8_t> buffer(bytes);
            if (bytes == 0 || !GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes))
            {
                CloseHandle(token);
                return false;
            }
            CloseHandle(token);
            PWSTR sid{};
            if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
                return false;
            const std::wstring pipe = L"\\\\.\\pipe\\QuickLook.App.Pipe." + std::wstring(sid);
            LocalFree(sid);
            if (!WaitNamedPipeW(pipe.c_str(), 500)) return false;
            HANDLE handle = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            const std::wstring message = std::wstring(toggle ?
                L"QuickLook.App.PipeMessages.Toggle|" : L"QuickLook.App.PipeMessages.Switch|") +
                path + L"\r\n";
            const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(required > 0 ? static_cast<std::size_t>(required) : 0, '\0');
            if (required > 0) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                message.data(), static_cast<int>(message.size()), utf8.data(), required, nullptr, nullptr);
            DWORD written{};
            const bool success = required > 0 && WriteFile(handle, utf8.data(),
                static_cast<DWORD>(utf8.size()), &written, nullptr) &&
                written == static_cast<DWORD>(utf8.size());
            CloseHandle(handle);
            return success;
        }

        [[nodiscard]] bool seer(const wchar_t* path) noexcept
        {
            const HWND window = FindWindowW(L"SeerWindowClass", nullptr);
            if (window == nullptr) return false;
            COPYDATASTRUCT data{};
            data.dwData = 5000;
            data.cbData = static_cast<DWORD>((std::wcslen(path) + 1) * sizeof(wchar_t));
            data.lpData = const_cast<wchar_t*>(path);
            DWORD_PTR result{};
            return SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) != 0;
        }

        [[nodiscard]] bool powerToys(const wchar_t* path)
        {
            const std::wstring executable = findPowerToysPeek();
            if (executable.empty()) return false;
            std::wstring commandLine = core::quoteWindowsArgument(executable) + L" " +
                core::quoteWindowsArgument(path);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            const BOOL created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr,
                nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
            if (created)
            {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
            return created != FALSE;
        }
    }

    int runTextPreviewWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 6 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long generation{};
        if (!parseUnsigned(arguments[2], windowValue) || !parseUnsigned(arguments[3], generation) ||
            generation > UINT_MAX || arguments[4] == nullptr || arguments[5] == nullptr)
            return 3;
        const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        // ponytail: A worker boundary must turn allocation failures into a completion message;
        // otherwise the frame can remain permanently stuck in its loading state.
        HANDLE input = INVALID_HANDLE_VALUE;
        HANDLE output = INVALID_HANDLE_VALUE;
        DWORD failure = ERROR_SUCCESS;
        try
        {
            input = CreateFileW(arguments[4], GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            failure = input == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            LARGE_INTEGER size{};
            bool truncated{};
            if (failure == ERROR_SUCCESS && !GetFileSizeEx(input, &size)) failure = GetLastError();
            if (failure == ERROR_SUCCESS && size.QuadPart < 0) failure = ERROR_INVALID_DATA;
            std::vector<std::uint8_t> bytes;
            if (failure == ERROR_SUCCESS)
            {
                const auto requested = static_cast<DWORD>(std::min<LONGLONG>(
                    size.QuadPart, static_cast<LONGLONG>(core::maxTextPreviewBytes)));
                truncated = size.QuadPart > static_cast<LONGLONG>(core::maxTextPreviewBytes);
                bytes.resize(requested);
                DWORD read{};
                if (requested != 0 && (!ReadFile(input, bytes.data(), requested, &read, nullptr) ||
                    read != requested)) failure = GetLastError();
            }
            if (input != INVALID_HANDLE_VALUE)
            {
                CloseHandle(input);
                input = INVALID_HANDLE_VALUE;
            }
            std::wstring text;
            if (failure == ERROR_SUCCESS && !decode(bytes, text))
                failure = ERROR_NO_UNICODE_TRANSLATION;
            if (failure == ERROR_SUCCESS && truncated) text += core::textPreviewTruncationNotice;

            if (failure == ERROR_SUCCESS)
            {
                output = CreateFileW(arguments[5], GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
                if (output == INVALID_HANDLE_VALUE) failure = GetLastError();
            }
            if (failure == ERROR_SUCCESS)
            {
                text.push_back(L'\0');
                const DWORD bytesToWrite = static_cast<DWORD>(text.size() * sizeof(wchar_t));
                DWORD written{};
                if (!WriteFile(output, text.data(), bytesToWrite, &written, nullptr) ||
                    written != bytesToWrite) failure = GetLastError();
            }
        }
        catch (const std::bad_alloc&)
        {
            failure = ERROR_NOT_ENOUGH_MEMORY;
        }
        catch (...)
        {
            failure = ERROR_UNHANDLED_EXCEPTION;
        }
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (!PostMessageW(window, textPreviewCompleteMessage,
                static_cast<WPARAM>(generation), static_cast<LPARAM>(failure)) &&
            failure == ERROR_SUCCESS)
            DeleteFileW(arguments[5]);
        return failure == ERROR_SUCCESS ? 0 : 4;
    }

    int runPreviewPopupWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 6 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long providerValue{};
        unsigned long long actionValue{};
        if (!parseUnsigned(arguments[2], windowValue) ||
            !parseUnsigned(arguments[3], providerValue) ||
            !parseUnsigned(arguments[4], actionValue) || actionValue > 1 ||
            !core::validPreviewProvider(static_cast<std::uint32_t>(providerValue)) ||
            arguments[5] == nullptr || std::wcslen(arguments[5]) >= 32767)
            return 3;
        const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        bool success{};
        core::PreviewProvider actual = core::PreviewProvider::automatic;
        try
        {
            const auto provider = static_cast<core::PreviewProvider>(providerValue);
            const bool toggle = actionValue == 0;
            actual = provider;
            if (provider == core::PreviewProvider::automatic)
            {
                if (quickLook(arguments[5], toggle)) actual = core::PreviewProvider::quickLook;
                else if (seer(arguments[5])) actual = core::PreviewProvider::seer;
                else if (toggle && powerToys(arguments[5])) actual = core::PreviewProvider::powerToys;
                else actual = core::PreviewProvider::automatic;
                success = actual != core::PreviewProvider::automatic;
            }
            else if (provider == core::PreviewProvider::quickLook)
                success = quickLook(arguments[5], toggle);
            else if (provider == core::PreviewProvider::seer)
                success = seer(arguments[5]);
            else if (provider == core::PreviewProvider::powerToys)
                success = toggle && powerToys(arguments[5]);
        }
        catch (...)
        {
            success = false;
            actual = core::PreviewProvider::automatic;
        }
        PostMessageW(window, previewPopupCompleteMessage,
            success ? static_cast<WPARAM>(actual) + 1 : 0, actionValue);
        return success ? 0 : 4;
    }
}
