#include "ftp_worker.h"
#include "../core/ftp_request.h"
#include "../core/windows_command_line.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <objbase.h>
#include <shlwapi.h>
#include <string>
#include <string_view>
#include <vector>

namespace filesxp::app
{
    namespace
    {
        constexpr DWORD maxCaptureBytes = 4U * 1024U * 1024U;
        // ponytail: Result capture is deliberately capped so a hostile server cannot grow
        // worker memory or the result file without bound.

        struct CaptureContext final
        {
            HANDLE input{};
            HANDLE output{};
        };

        struct SensitiveBytes final
        {
            std::string value;
            ~SensitiveBytes()
            {
                if (!value.empty()) SecureZeroMemory(value.data(), value.size());
            }
            SensitiveBytes() = default;
            SensitiveBytes(const SensitiveBytes&) = delete;
            SensitiveBytes& operator=(const SensitiveBytes&) = delete;
        };

        struct TemporaryFile final
        {
            std::wstring path;
            ~TemporaryFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
            TemporaryFile() = default;
            TemporaryFile(const TemporaryFile&) = delete;
            TemporaryFile& operator=(const TemporaryFile&) = delete;
        };

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

        [[nodiscard]] std::string toUtf8(std::u16string_view value)
        {
            static_assert(sizeof(wchar_t) == sizeof(char16_t));
            if (value.empty()) return {};
            const auto* wide = reinterpret_cast<const wchar_t*>(value.data());
            const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                wide, static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (required <= 0) return {};
            std::string result(static_cast<std::size_t>(required), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                    static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) != required)
                result.clear();
            return result;
        }

        [[nodiscard]] bool appendConfigValue(std::string& config, std::string_view option,
            std::string_view value)
        {
            SensitiveBytes quoted;
            quoted.value = core::quoteCurlConfigValue(value);
            if (quoted.value.empty()) return false;
            try
            {
                config.append(option);
                config.append(" = ");
                config.append(quoted.value);
                config.push_back('\n');
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] bool loadRequest(const wchar_t* mappingName,
            core::FtpRequest& request) noexcept
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
                    if (size >= 32 && size <= core::maxFtpRequestBytes && size <= memory.RegionSize)
                    {
                        valid = core::decodeFtpRequest(data, size, request);
                        SecureZeroMemory(view, size);
                    }
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(mapping);
            return valid;
        }

        [[nodiscard]] std::wstring systemCurlPath()
        {
            std::wstring directory(32768, L'\0');
            const UINT length = GetSystemDirectoryW(directory.data(),
                static_cast<UINT>(directory.size()));
            if (length == 0 || length >= directory.size()) return {};
            directory.resize(length);
            std::wstring path = directory + L"\\curl.exe";
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                return {};
            return path;
        }

        [[nodiscard]] std::wstring parentDirectory(const std::wstring& path)
        {
            const std::size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash == 0) return {};
            if (slash == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3);
            return path.substr(0, slash);
        }

        [[nodiscard]] std::wstring uniqueTemporaryPath(const std::wstring& directory)
        {
            GUID identifier{};
            if (FAILED(CoCreateGuid(&identifier))) return {};
            wchar_t text[40]{};
            if (StringFromGUID2(identifier, text, static_cast<int>(std::size(text))) == 0)
                return {};
            std::wstring result = directory;
            if (!result.empty() && result.back() != L'\\' && result.back() != L'/')
                result.push_back(L'\\');
            result += L".FilesXPNative-FTP-";
            result += text;
            result += L".tmp";
            return result;
        }

        [[nodiscard]] std::wstring collisionCandidate(const std::wstring& target,
            unsigned suffix)
        {
            if (suffix == 1) return target;
            const std::size_t slash = target.find_last_of(L"\\/");
            const std::size_t dot = target.find_last_of(L'.');
            const bool hasExtension = dot != std::wstring::npos &&
                (slash == std::wstring::npos || dot > slash + 1);
            const std::wstring base = hasExtension ? target.substr(0, dot) : target;
            const std::wstring extension = hasExtension ? target.substr(dot) : std::wstring{};
            return base + L" (" + std::to_wstring(suffix) + L")" + extension;
        }

        [[nodiscard]] DWORD commitDownload(TemporaryFile& temporary,
            const std::wstring& target, std::wstring& savedPath) noexcept
        {
            for (unsigned suffix = 1; suffix <= 9999; ++suffix)
            {
                std::wstring candidate = collisionCandidate(target, suffix);
                const DWORD attributes = GetFileAttributesW(candidate.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES) continue;
                if (MoveFileExW(temporary.path.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH))
                {
                    temporary.path.clear();
                    savedPath = std::move(candidate);
                    return ERROR_SUCCESS;
                }
                const DWORD failure = GetLastError();
                if (failure != ERROR_ALREADY_EXISTS && failure != ERROR_FILE_EXISTS)
                    return failure;
            }
            return ERROR_FILE_EXISTS;
        }

        [[nodiscard]] DWORD validateLocalPath(const core::FtpRequest& request,
            TemporaryFile& temporary) noexcept
        {
            if (request.operation != core::FtpOperation::download &&
                request.operation != core::FtpOperation::upload)
                return ERROR_SUCCESS;
            const std::wstring path = fromUtf16(request.localPath);
            if (path.empty() || PathIsRelativeW(path.c_str())) return ERROR_BAD_PATHNAME;
            if (request.operation == core::FtpOperation::upload)
            {
                const DWORD attributes = GetFileAttributesW(path.c_str());
                return attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
            }
            const std::wstring directory = parentDirectory(path);
            const DWORD attributes = directory.empty() ? INVALID_FILE_ATTRIBUTES :
                GetFileAttributesW(directory.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                return ERROR_PATH_NOT_FOUND;
            temporary.path = uniqueTemporaryPath(directory);
            if (temporary.path.empty()) return ERROR_INVALID_DATA;
            HANDLE reserved = CreateFileW(temporary.path.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (reserved == INVALID_HANDLE_VALUE) return GetLastError();
            CloseHandle(reserved);
            return ERROR_SUCCESS;
        }

        [[nodiscard]] DWORD buildConfig(core::FtpRequest& request,
            const TemporaryFile& temporary, SensitiveBytes& config) noexcept
        {
            try
            {
                config.value = "silent\nshow-error\nfail\nftp-pasv\nconnect-timeout = 15\n"
                    "proto = \"=ftp,ftps\"\n";
                const std::string baseUrl = toUtf8(request.url);
                if (baseUrl.empty()) return ERROR_NO_UNICODE_TRANSLATION;
                std::string remote;
                if (!request.remoteName.empty())
                {
                    const std::string name = toUtf8(request.remoteName);
                    if (name.empty()) return ERROR_NO_UNICODE_TRANSLATION;
                    remote = core::percentEncodeFtpSegment(name);
                    if (remote.empty()) return ERROR_INVALID_DATA;
                }
                if (!request.username.empty())
                {
                    SensitiveBytes credential;
                    SensitiveBytes password;
                    credential.value = toUtf8(request.username);
                    password.value = toUtf8(request.password);
                    if (credential.value.empty() ||
                        (!request.password.empty() && password.value.empty()))
                        return ERROR_NO_UNICODE_TRANSLATION;
                    credential.value.push_back(':');
                    credential.value.append(password.value);
                    if (!appendConfigValue(config.value, "user", credential.value))
                        return ERROR_INVALID_DATA;
                }
                if (request.requireTls && request.url.starts_with(u"ftp://"))
                    config.value += "ssl-reqd\n";

                switch (request.operation)
                {
                case core::FtpOperation::list:
                    config.value += "list-only\n";
                    if (!appendConfigValue(config.value, "url", baseUrl)) return ERROR_INVALID_DATA;
                    break;
                case core::FtpOperation::download:
                    if (!appendConfigValue(config.value, "url", baseUrl + remote) ||
                        !appendConfigValue(config.value, "output", toUtf8(
                            std::u16string_view(reinterpret_cast<const char16_t*>(temporary.path.data()),
                                temporary.path.size()))))
                        return ERROR_INVALID_DATA;
                    break;
                case core::FtpOperation::upload:
                    if (!appendConfigValue(config.value, "url", baseUrl + remote) ||
                        !appendConfigValue(config.value, "upload-file", toUtf8(request.localPath)))
                        return ERROR_INVALID_DATA;
                    break;
                case core::FtpOperation::makeDirectory:
                case core::FtpOperation::deleteFile:
                case core::FtpOperation::deleteDirectory:
                {
                    const char* verb = request.operation == core::FtpOperation::makeDirectory ?
                        "MKD " : request.operation == core::FtpOperation::deleteFile ? "DELE " : "RMD ";
                    const std::string name = toUtf8(request.remoteName);
                    if (name.empty() || !appendConfigValue(config.value, "quote", "+" +
                            std::string(verb) + name) ||
                        !appendConfigValue(config.value, "url", baseUrl) ||
                        !appendConfigValue(config.value, "output", "NUL"))
                        return ERROR_INVALID_DATA;
                    config.value += "list-only\n";
                    break;
                }
                case core::FtpOperation::count:
                    return ERROR_INVALID_PARAMETER;
                }
                if (!request.password.empty())
                    SecureZeroMemory(request.password.data(), request.password.size() * sizeof(char16_t));
                request.password.clear();
                if (!request.username.empty())
                    SecureZeroMemory(request.username.data(), request.username.size() * sizeof(char16_t));
                request.username.clear();
                return config.value.size() <= core::maxFtpRequestBytes ?
                    ERROR_SUCCESS : ERROR_BUFFER_OVERFLOW;
            }
            catch (...)
            {
                return ERROR_NOT_ENOUGH_MEMORY;
            }
        }

        DWORD WINAPI captureOutput(void* raw) noexcept
        {
            auto* context = static_cast<CaptureContext*>(raw);
            if (context == nullptr || context->input == nullptr || context->output == nullptr)
                return ERROR_INVALID_PARAMETER;
            char buffer[16384]{};
            DWORD stored{};
            DWORD result{ERROR_SUCCESS};
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
                const DWORD available = stored < maxCaptureBytes ? maxCaptureBytes - stored : 0;
                const DWORD keep = read < available ? read : available;
                if (keep != 0)
                {
                    DWORD written{};
                    if (!WriteFile(context->output, buffer, keep, &written, nullptr) || written != keep)
                        return GetLastError();
                    stored += keep;
                }
                if (keep != read) result = ERROR_FILE_TOO_LARGE;
            }
            return result;
        }

        [[nodiscard]] DWORD appendSavedPath(HANDLE output, const std::wstring& path) noexcept
        {
            const auto view = std::u16string_view(reinterpret_cast<const char16_t*>(path.data()),
                path.size());
            const std::string utf8 = toUtf8(view);
            if (utf8.empty()) return ERROR_NO_UNICODE_TRANSLATION;
            const std::string line = "\r\nSaved: " + utf8 + "\r\n";
            DWORD written{};
            return WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
                written == line.size() ? ERROR_SUCCESS : GetLastError();
        }

        [[nodiscard]] DWORD normalizeListOutput(HANDLE output) noexcept
        {
            if (!FlushFileBuffers(output)) return GetLastError();
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(output, &size)) return GetLastError();
            if (size.QuadPart < 0 || size.QuadPart > maxCaptureBytes)
                return ERROR_FILE_TOO_LARGE;
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(output, zero, nullptr, FILE_BEGIN)) return GetLastError();
            std::string bytes;
            try
            {
                bytes.resize(static_cast<std::size_t>(size.QuadPart));
            }
            catch (...)
            {
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            DWORD read{};
            if (!bytes.empty())
            {
                const BOOL readOk = ReadFile(output, bytes.data(),
                    static_cast<DWORD>(bytes.size()), &read, nullptr);
                if (!readOk || read != bytes.size())
                    return readOk ? ERROR_READ_FAULT : GetLastError();
            }
            std::wstring listing;
            if (!bytes.empty())
            {
                const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                if (required <= 0) return ERROR_NO_UNICODE_TRANSLATION;
                try
                {
                    listing.resize(static_cast<std::size_t>(required));
                }
                catch (...)
                {
                    return ERROR_NOT_ENOUGH_MEMORY;
                }
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                        static_cast<int>(bytes.size()), listing.data(), required) != required)
                    return ERROR_NO_UNICODE_TRANSLATION;
            }
            std::vector<std::wstring> names = core::parseFtpNameList(listing);
            SensitiveBytes normalized;
            try
            {
                normalized.value.reserve(bytes.size());
                for (const auto& name : names)
                {
                    const auto view = std::u16string_view(
                        reinterpret_cast<const char16_t*>(name.data()), name.size());
                    const std::string utf8 = toUtf8(view);
                    if (utf8.empty()) return ERROR_NO_UNICODE_TRANSLATION;
                    if (normalized.value.size() + utf8.size() + 1 > maxCaptureBytes)
                        return ERROR_FILE_TOO_LARGE;
                    normalized.value.append(utf8);
                    normalized.value.push_back('\n');
                }
            }
            catch (...)
            {
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            if (!SetFilePointerEx(output, zero, nullptr, FILE_BEGIN)) return GetLastError();
            DWORD written{};
            if (!normalized.value.empty())
            {
                const BOOL writeOk = WriteFile(output, normalized.value.data(),
                    static_cast<DWORD>(normalized.value.size()), &written, nullptr);
                if (!writeOk || written != normalized.value.size())
                    return writeOk ? ERROR_WRITE_FAULT : GetLastError();
            }
            if (!SetEndOfFile(output)) return GetLastError();
            return ERROR_SUCCESS;
        }

        [[nodiscard]] DWORD runCurl(const std::wstring& curl, core::FtpRequest& request,
            HANDLE output, HANDLE cancel) noexcept
        {
            TemporaryFile temporary;
            DWORD result = validateLocalPath(request, temporary);
            if (result != ERROR_SUCCESS) return result;
            SensitiveBytes config;
            result = buildConfig(request, temporary, config);
            if (result != ERROR_SUCCESS) return result;

            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.bInheritHandle = TRUE;
            HANDLE captureRead{};
            HANDLE captureWrite{};
            HANDLE inputRead{};
            HANDLE inputWrite{};
            if (!CreatePipe(&captureRead, &captureWrite, &security, 0)) return GetLastError();
            if (!CreatePipe(&inputRead, &inputWrite, &security, 16384))
            {
                result = GetLastError();
                CloseHandle(captureRead);
                CloseHandle(captureWrite);
                return result;
            }
            if (!SetHandleInformation(captureRead, HANDLE_FLAG_INHERIT, 0) ||
                !SetHandleInformation(inputWrite, HANDLE_FLAG_INHERIT, 0))
            {
                result = GetLastError();
                CloseHandle(captureRead);
                CloseHandle(captureWrite);
                CloseHandle(inputRead);
                CloseHandle(inputWrite);
                return result;
            }

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = captureWrite;
            startup.hStdError = captureWrite;
            startup.hStdInput = inputRead;
            PROCESS_INFORMATION process{};
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (job == nullptr)
            {
                result = GetLastError();
                CloseHandle(captureRead);
                CloseHandle(captureWrite);
                CloseHandle(inputRead);
                CloseHandle(inputWrite);
                return result;
            }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                    &limits, sizeof(limits)))
            {
                result = GetLastError();
                CloseHandle(job);
                CloseHandle(captureRead);
                CloseHandle(captureWrite);
                CloseHandle(inputRead);
                CloseHandle(inputWrite);
                return result;
            }
            std::wstring commandLine = core::quoteWindowsArgument(curl) + L" -q --config -";
            if (!CreateProcessW(curl.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
            {
                result = GetLastError();
                CloseHandle(job);
                CloseHandle(captureRead);
                CloseHandle(captureWrite);
                CloseHandle(inputRead);
                CloseHandle(inputWrite);
                return result;
            }
            if (!AssignProcessToJobObject(job, process.hProcess) ||
                ResumeThread(process.hThread) == static_cast<DWORD>(-1))
            {
                result = GetLastError();
                TerminateProcess(process.hProcess, result);
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                CloseHandle(job);
                CloseHandle(captureRead);
                CloseHandle(captureWrite);
                CloseHandle(inputRead);
                CloseHandle(inputWrite);
                return result;
            }
            CloseHandle(captureWrite);
            CloseHandle(inputRead);

            CaptureContext capture{captureRead, output};
            HANDLE captureThread = CreateThread(nullptr, 0, &captureOutput, &capture, 0, nullptr);
            if (captureThread == nullptr)
            {
                result = GetLastError();
                TerminateProcess(process.hProcess, result);
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(inputWrite);
                CloseHandle(captureRead);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                CloseHandle(job);
                return result;
            }

            DWORD written{};
            const BOOL wrote = WriteFile(inputWrite, config.value.data(),
                static_cast<DWORD>(config.value.size()), &written, nullptr);
            const DWORD writeFailure = wrote ? ERROR_SUCCESS : GetLastError();
            CloseHandle(inputWrite);
            if (!wrote || written != config.value.size())
            {
                result = wrote ? ERROR_WRITE_FAULT : writeFailure;
                TerminateProcess(process.hProcess, result);
            }
            const HANDLE waits[]{process.hProcess, cancel};
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0 + 1)
            {
                TerminateProcess(process.hProcess, ERROR_CANCELLED);
                WaitForSingleObject(process.hProcess, 5000);
                result = ERROR_CANCELLED;
            }
            else if (wait != WAIT_OBJECT_0)
            {
                result = GetLastError();
                TerminateProcess(process.hProcess, result);
            }
            else if (result == ERROR_SUCCESS)
            {
                DWORD exitCode{};
                if (!GetExitCodeProcess(process.hProcess, &exitCode))
                    result = GetLastError();
                else if (exitCode != 0)
                    result = ERROR_GEN_FAILURE;
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(job);
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
            if (result == ERROR_SUCCESS && request.operation == core::FtpOperation::list)
                result = normalizeListOutput(output);
            if (result == ERROR_SUCCESS && request.operation == core::FtpOperation::download)
            {
                std::wstring savedPath;
                result = commitDownload(temporary, fromUtf16(request.localPath), savedPath);
                if (result == ERROR_SUCCESS) (void)appendSavedPath(output, savedPath);
            }
            return result;
        }
    }

    int runFtpWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 7 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long tokenValue{};
        if (!parseUnsigned(arguments[2], windowValue) || !parseUnsigned(arguments[3], tokenValue) ||
            tokenValue == 0 || tokenValue > static_cast<unsigned long long>(UINTPTR_MAX) ||
            arguments[4] == nullptr || arguments[5] == nullptr || arguments[6] == nullptr ||
            std::wcslen(arguments[4]) >= 32767 || std::wcslen(arguments[5]) >= 260 ||
            std::wcslen(arguments[6]) >= 260)
            return 3;
        const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        const auto token = static_cast<std::uintptr_t>(tokenValue);
        HANDLE output = CreateFileW(arguments[4], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        DWORD result = output == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        core::FtpRequest request;
        const bool loaded = loadRequest(arguments[6], request);
        if (!loaded && result == ERROR_SUCCESS) result = ERROR_INVALID_DATA;
        HANDLE cancel = result == ERROR_SUCCESS ? OpenEventW(SYNCHRONIZE, FALSE, arguments[5]) : nullptr;
        if (result == ERROR_SUCCESS && cancel == nullptr) result = GetLastError();
        const std::wstring curl = result == ERROR_SUCCESS ? systemCurlPath() : std::wstring{};
        if (result == ERROR_SUCCESS && curl.empty()) result = ERROR_FILE_NOT_FOUND;
        if (result == ERROR_SUCCESS) result = runCurl(curl, request, output, cancel);
        if (!request.password.empty())
            SecureZeroMemory(request.password.data(), request.password.size() * sizeof(char16_t));
        request.password.clear();
        if (!request.username.empty())
            SecureZeroMemory(request.username.data(), request.username.size() * sizeof(char16_t));
        request.username.clear();
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (cancel != nullptr) CloseHandle(cancel);
        if (!PostMessageW(window, ftpCompleteMessage, static_cast<WPARAM>(token), result))
            DeleteFileW(arguments[4]);
        return result == ERROR_SUCCESS ? 0 : 4;
    }
}
