#include "git_worker.h"
#include "../core/git_policy.h"
#include "../core/windows_command_line.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <array>
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

        [[nodiscard]] std::vector<std::wstring> argumentsForOperation(core::GitOperation operation)
        {
            std::vector<std::wstring> result;
            for (const wchar_t* argument : core::gitArguments(operation)) result.emplace_back(argument);
            return result;
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

        [[nodiscard]] bool isRegularFile(const std::wstring& path) noexcept
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        [[nodiscard]] std::wstring moduleDirectory()
        {
            std::wstring path(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size()) return {};
            path.resize(length);
            const std::size_t separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
        }

        [[nodiscard]] std::wstring findGitExecutable()
        {
            const std::wstring applicationDirectory = moduleDirectory();
            if (!applicationDirectory.empty())
            {
                const std::wstring bundled = applicationDirectory + L"\\git.exe";
                if (isRegularFile(bundled)) return bundled;
            }
            const std::array<std::wstring, 3> roots{
                environmentPath(L"ProgramW6432"), environmentPath(L"ProgramFiles"),
                environmentPath(L"ProgramFiles(x86)")};
            for (const auto& root : roots)
            {
                if (root.empty()) continue;
                for (const wchar_t* relative : {L"\\Git\\cmd\\git.exe", L"\\Git\\bin\\git.exe"})
                {
                    const std::wstring candidate = root + relative;
                    if (isRegularFile(candidate)) return candidate;
                }
            }
            return {};
        }

        struct CaptureContext final
        {
            HANDLE input{};
            HANDLE output{};
            DWORD stored{};
        };

        DWORD WINAPI captureOutput(void* raw) noexcept
        {
            constexpr DWORD maxBytes = 4U * 1024U * 1024U;
            constexpr char truncated[] = "\r\n[output truncated at 4 MiB]\r\n";
            constexpr DWORD payloadLimit = maxBytes - static_cast<DWORD>(sizeof(truncated) - 1);
            auto* context = static_cast<CaptureContext*>(raw);
            if (context == nullptr || context->input == nullptr || context->output == nullptr)
                return ERROR_INVALID_PARAMETER;
            char buffer[16384]{};
            DWORD stored = context->stored;
            bool didTruncate = stored > payloadLimit;
            while (true)
            {
                DWORD read{};
                if (!ReadFile(context->input, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr))
                {
                    const DWORD failure = GetLastError();
                    if (failure == ERROR_BROKEN_PIPE) break;
                    return failure;
                }
                if (read == 0) break;
                const DWORD available = stored < payloadLimit ? payloadLimit - stored : 0;
                const DWORD keep = read < available ? read : available;
                if (keep != 0)
                {
                    DWORD written{};
                    if (!WriteFile(context->output, buffer, keep, &written, nullptr) || written != keep)
                        return GetLastError();
                    stored += keep;
                }
                if (keep != read && !didTruncate)
                {
                    DWORD written{};
                    constexpr DWORD markerBytes = static_cast<DWORD>(sizeof(truncated) - 1);
                    if (!WriteFile(context->output, truncated, markerBytes,
                            &written, nullptr) || written != markerBytes)
                        return GetLastError();
                    didTruncate = true;
                }
            }
            return ERROR_SUCCESS;
        }

        [[nodiscard]] DWORD runGit(const wchar_t* git, const wchar_t* directory,
            const std::vector<std::wstring>& arguments, HANDLE output, HANDLE cancel) noexcept
        {
            std::wstring commandLine = core::quoteWindowsArgument(git);
            for (const auto& argument : arguments)
            {
                commandLine.push_back(L' ');
                commandLine += core::quoteWindowsArgument(argument);
            }
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.bInheritHandle = TRUE;
            HANDLE captureRead{};
            HANDLE captureWrite{};
            if (!CreatePipe(&captureRead, &captureWrite, &security, 0)) return GetLastError();
            if (!SetHandleInformation(captureRead, HANDLE_FLAG_INHERIT, 0))
            {
                const DWORD failure = GetLastError();
                CloseHandle(captureWrite);
                CloseHandle(captureRead);
                return failure;
            }
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = captureWrite;
            startup.hStdError = captureWrite;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(git, commandLine.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW, nullptr, directory, &startup, &process))
            {
                const DWORD failure = GetLastError();
                CloseHandle(captureWrite);
                CloseHandle(captureRead);
                return failure;
            }
            CloseHandle(captureWrite);
            LARGE_INTEGER initialSize{};
            const bool measured = GetFileSizeEx(output, &initialSize) != FALSE;
            if (!measured || initialSize.QuadPart < 0 || initialSize.QuadPart > 4LL * 1024LL * 1024LL)
            {
                const DWORD failure = measured ? ERROR_FILE_TOO_LARGE : GetLastError();
                TerminateProcess(process.hProcess, failure);
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(captureRead);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                return failure;
            }
            CaptureContext capture{captureRead, output, static_cast<DWORD>(initialSize.QuadPart)};
            HANDLE captureThread = CreateThread(nullptr, 0, &captureOutput, &capture, 0, nullptr);
            if (captureThread == nullptr)
            {
                const DWORD failure = GetLastError();
                TerminateProcess(process.hProcess, failure);
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(captureRead);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                return failure;
            }

            const HANDLE waits[]{process.hProcess, cancel};
            const DWORD waitResult = WaitForMultipleObjects(cancel != nullptr ? 2 : 1,
                waits, FALSE, INFINITE);
            DWORD result{};
            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                TerminateProcess(process.hProcess, ERROR_CANCELLED);
                WaitForSingleObject(process.hProcess, 5000);
                result = ERROR_CANCELLED;
            }
            else if (waitResult != WAIT_OBJECT_0)
            {
                result = GetLastError();
            }
            else
            {
                DWORD exitCode{};
                result = GetExitCodeProcess(process.hProcess, &exitCode) ? exitCode : GetLastError();
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            if (WaitForSingleObject(captureThread, 5000) != WAIT_OBJECT_0)
            {
                CancelSynchronousIo(captureThread);
                WaitForSingleObject(captureThread, INFINITE);
                if (result == ERROR_SUCCESS) result = ERROR_TIMEOUT;
            }
            DWORD captureResult{};
            if (GetExitCodeThread(captureThread, &captureResult) &&
                captureResult != ERROR_SUCCESS && result == ERROR_SUCCESS)
                result = captureResult;
            CloseHandle(captureThread);
            CloseHandle(captureRead);
            return result;
        }

        [[nodiscard]] bool appendStatusMarker(HANDLE output) noexcept
        {
            constexpr char marker[] = "\r\n[FilesXPNative-GitStatus]\r\n";
            DWORD written{};
            constexpr DWORD markerBytes = static_cast<DWORD>(sizeof(marker) - 1);
            if (!WriteFile(output, marker, markerBytes, &written, nullptr)) return false;
            if (written == markerBytes) return true;
            SetLastError(ERROR_WRITE_FAULT);
            return false;
        }
    }

    int runGitWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 8 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long generation{};
        unsigned long long verb{};
        if (!parseUnsigned(arguments[2], windowValue) ||
            !parseUnsigned(arguments[3], generation) || generation > UINT_MAX ||
            !parseUnsigned(arguments[7], verb) ||
                verb >= static_cast<unsigned long long>(core::GitOperation::count) ||
            arguments[4] == nullptr || arguments[5] == nullptr || arguments[6] == nullptr ||
            std::wcslen(arguments[4]) >= 32767 ||
            std::wcslen(arguments[5]) >= 260 || std::wcslen(arguments[6]) >= 32767)
            return 3;
        const DWORD attributes = GetFileAttributesW(arguments[6]);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return 3;
        const std::wstring git = findGitExecutable();
        HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, arguments[5]);
        const DWORD cancelFailure = cancel == nullptr ? GetLastError() : ERROR_SUCCESS;
        HANDLE output = CreateFileW(arguments[4], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        DWORD result = output == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (result == ERROR_SUCCESS && cancel == nullptr) result = cancelFailure;
        if (result == ERROR_SUCCESS && git.empty()) result = ERROR_FILE_NOT_FOUND;
        if (result == ERROR_SUCCESS)
        {
            const auto operation = static_cast<core::GitOperation>(verb);
            if (operation == core::GitOperation::sync)
            {
                result = runGit(git.c_str(), arguments[6],
                    argumentsForOperation(core::GitOperation::pull), output, cancel);
                if (result == ERROR_SUCCESS)
                    result = runGit(git.c_str(), arguments[6],
                        argumentsForOperation(core::GitOperation::push), output, cancel);
            }
            else
            {
                result = runGit(git.c_str(), arguments[6], argumentsForOperation(operation), output, cancel);
            }
            if (result == ERROR_SUCCESS && operation != core::GitOperation::status)
            {
                if (!appendStatusMarker(output))
                    result = GetLastError();
                else
                    result = runGit(git.c_str(), arguments[6],
                        argumentsForOperation(core::GitOperation::status), output, cancel);
            }
        }
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (cancel != nullptr) CloseHandle(cancel);
        const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        if (!PostMessageW(window, gitCompleteMessage, static_cast<WPARAM>(generation), result))
            DeleteFileW(arguments[4]);
        return result == ERROR_SUCCESS ? 0 : 4;
    }
}
