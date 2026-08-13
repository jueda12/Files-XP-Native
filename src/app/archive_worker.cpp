#include "archive_worker.h"
#include "../core/archive_request.h"
#include "../core/windows_command_line.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <objbase.h>
#include <string>
#include <vector>

namespace filesxp::app
{
    namespace
    {
        struct CaptureContext final
        {
            HANDLE input{};
            HANDLE output{};
            HWND window{};
            std::uint32_t generation{};
            int lastProgress{-1};
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

        [[nodiscard]] std::string toUtf8(const std::u16string& value)
        {
            static_assert(sizeof(wchar_t) == sizeof(char16_t));
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

        struct TemporaryFile final
        {
            std::wstring path;
            ~TemporaryFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
            TemporaryFile(const TemporaryFile&) = delete;
            TemporaryFile& operator=(const TemporaryFile&) = delete;
            TemporaryFile() = default;
        };

        struct SensitiveBytes final
        {
            std::string value;
            ~SensitiveBytes()
            {
                if (!value.empty()) SecureZeroMemory(value.data(), value.size());
            }
            SensitiveBytes(const SensitiveBytes&) = delete;
            SensitiveBytes& operator=(const SensitiveBytes&) = delete;
            SensitiveBytes() = default;
        };

        [[nodiscard]] bool writeListFile(const std::vector<std::u16string>& paths,
            TemporaryFile& file) noexcept
        {
            std::wstring temporary(32768, L'\0');
            const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
            if (length == 0 || length >= temporary.size()) return false;
            temporary.resize(length);
            GUID identifier{};
            if (FAILED(CoCreateGuid(&identifier))) return false;
            wchar_t identifierText[40]{};
            if (StringFromGUID2(identifier, identifierText, static_cast<int>(std::size(identifierText))) == 0)
                return false;
            file.path = temporary + L"FilesXPNative-Archive-" + identifierText + L".lst";

            std::string bytes;
            bytes.reserve(4096);
            for (const auto& path : paths)
            {
                const std::string encoded = toUtf8(path);
                if (encoded.empty() || bytes.size() + encoded.size() + 1 > core::maxArchiveRequestBytes)
                    return false;
                bytes += encoded;
                bytes.push_back('\n');
            }
            HANDLE output = CreateFileW(file.path.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (output == INVALID_HANDLE_VALUE) return false;
            DWORD written{};
            const bool success = WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()),
                &written, nullptr) != FALSE && written == bytes.size();
            CloseHandle(output);
            return success;
        }

        void postProgress(CaptureContext& context, const char* bytes, DWORD count) noexcept
        {
            for (DWORD index = 1; index < count; ++index)
            {
                if (bytes[index] != '%') continue;
                DWORD start = index;
                while (start != 0 && bytes[start - 1] >= '0' && bytes[start - 1] <= '9') --start;
                if (start == index) continue;
                unsigned value{};
                for (DWORD digit = start; digit < index && value <= 100; ++digit)
                    value = value * 10 + static_cast<unsigned>(bytes[digit] - '0');
                if (value <= 100 && static_cast<int>(value) != context.lastProgress)
                {
                    context.lastProgress = static_cast<int>(value);
                    PostMessageW(context.window, archiveProgressMessage,
                        static_cast<WPARAM>(context.generation), static_cast<LPARAM>(value));
                }
            }
        }

        DWORD WINAPI captureOutput(void* raw) noexcept
        {
            constexpr DWORD maxBytes = 64U * 1024U;
            constexpr char truncated[] = "\r\n[output truncated at 64 KiB]\r\n";
            constexpr DWORD markerBytes = static_cast<DWORD>(sizeof(truncated) - 1);
            constexpr DWORD payloadLimit = maxBytes - markerBytes;
            auto* context = static_cast<CaptureContext*>(raw);
            if (context == nullptr || context->input == nullptr || context->output == nullptr)
                return ERROR_INVALID_PARAMETER;
            char buffer[16384]{};
            DWORD stored{};
            bool didTruncate{};
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
                postProgress(*context, buffer, read);
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
                    if (!WriteFile(context->output, truncated, markerBytes, &written, nullptr) ||
                        written != markerBytes)
                        return GetLastError();
                    didTruncate = true;
                }
            }
            return ERROR_SUCCESS;
        }

        [[nodiscard]] bool loadRequest(const wchar_t* mappingName,
            core::ArchiveRequest& request) noexcept
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
                    if (size >= 24 && size <= core::maxArchiveRequestBytes && size <= memory.RegionSize)
                    {
                        valid = core::decodeArchiveRequest(data, size, request);
                        SecureZeroMemory(view, size);
                    }
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(mapping);
            return valid;
        }

        [[nodiscard]] std::wstring collisionTarget(std::wstring target,
            core::ArchiveCollision collision, DWORD& failure)
        {
            const DWORD attributes = GetFileAttributesW(target.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return target;
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                failure = ERROR_DIRECTORY;
                return {};
            }
            if (collision == core::ArchiveCollision::skip)
            {
                failure = ERROR_FILE_EXISTS;
                return {};
            }
            if (collision == core::ArchiveCollision::overwrite)
                return target;
            const std::size_t slash = target.find_last_of(L"\\/");
            const std::size_t dot = target.find_last_of(L'.');
            const bool hasExtension = dot != std::wstring::npos &&
                (slash == std::wstring::npos || dot > slash + 1);
            const std::wstring base = hasExtension ? target.substr(0, dot) : target;
            const std::wstring extension = hasExtension ? target.substr(dot) : std::wstring{};
            for (unsigned suffix = 2; suffix <= 9999; ++suffix)
            {
                std::wstring candidate = base + L" (" + std::to_wstring(suffix) + L")" + extension;
                if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
            }
            failure = ERROR_FILE_EXISTS;
            return {};
        }

        [[nodiscard]] std::wstring uniqueArchiveTemporaryPath(const std::wstring& directory)
        {
            GUID identifier{};
            if (FAILED(CoCreateGuid(&identifier))) return {};
            wchar_t identifierText[40]{};
            if (StringFromGUID2(identifier, identifierText, static_cast<int>(std::size(identifierText))) == 0)
                return {};
            return directory + L"\\.FilesXPNative-Archive-" + identifierText + L".tmp";
        }

        [[nodiscard]] std::vector<std::wstring> buildArguments(core::ArchiveRequest& request,
            const std::wstring& listFile, const std::wstring& commandTarget, DWORD& failure)
        {
            std::vector<std::wstring> arguments;
            arguments.reserve(request.paths.size() + 12);
            const auto collision = request.collision == core::ArchiveCollision::overwrite ? L"-aoa" :
                request.collision == core::ArchiveCollision::skip ? L"-aos" : L"-aou";
            if (request.operation == core::ArchiveOperation::extract)
            {
                if (request.paths.size() != 1)
                {
                    failure = ERROR_INVALID_PARAMETER;
                    return {};
                }
                arguments = {L"x", fromUtf16(request.paths.front()), L"-o" + fromUtf16(request.target),
                    collision, L"-y", L"-bb1", L"-bsp1", L"-sccUTF-8"};
            }
            else
            {
                const wchar_t* format = request.operation == core::ArchiveOperation::createZip ?
                    L"-tzip" : request.operation == core::ArchiveOperation::createTar ? L"-ttar" : L"-t7z";
                arguments = {L"a", format, commandTarget, L"-y", L"-bb1", L"-bsp1", L"-sccUTF-8"};
                if (!request.password.empty())
                {
                    arguments.push_back(L"-p");
                    if (request.operation == core::ArchiveOperation::create7z)
                        arguments.push_back(L"-mhe=on");
                    else if (request.operation == core::ArchiveOperation::createZip)
                        arguments.push_back(L"-mem=AES256");
                }
                arguments.push_back(L"-scsUTF-8");
                arguments.push_back(L"@" + listFile);
            }
            return arguments;
        }

        [[nodiscard]] DWORD run7Zip(const wchar_t* sevenZip, core::ArchiveRequest& request,
            HANDLE output, HANDLE cancel, HWND window, std::uint32_t generation) noexcept
        {
            DWORD result{};
            TemporaryFile listFile;
            TemporaryFile archiveFile;
            if (request.operation != core::ArchiveOperation::extract &&
                !writeListFile(request.paths, listFile))
                return ERROR_INVALID_DATA;
            std::wstring finalTarget;
            std::wstring commandTarget = fromUtf16(request.target);
            if (request.operation != core::ArchiveOperation::extract)
            {
                finalTarget = collisionTarget(commandTarget, request.collision, result);
                if (finalTarget.empty()) return result == ERROR_SUCCESS ? ERROR_FILE_EXISTS : result;
                archiveFile.path = uniqueArchiveTemporaryPath(fromUtf16(request.workingDirectory));
                if (archiveFile.path.empty()) return ERROR_INVALID_DATA;
                commandTarget = archiveFile.path;
            }
            std::vector<std::wstring> arguments = buildArguments(
                request, listFile.path, commandTarget, result);
            if (arguments.empty()) return result == ERROR_SUCCESS ? ERROR_INVALID_PARAMETER : result;
            SensitiveBytes input;
            input.value = toUtf8(request.password);
            if (!request.password.empty() && input.value.empty()) return ERROR_NO_UNICODE_TRANSLATION;
            if (request.operation != core::ArchiveOperation::extract && !input.value.empty())
                input.value += "\r\n" + input.value;
            input.value += "\r\n";
            if (!request.password.empty())
                SecureZeroMemory(request.password.data(), request.password.size() * sizeof(char16_t));
            request.password.clear();
            std::wstring commandLine = core::quoteWindowsArgument(sevenZip);
            for (const auto& argument : arguments)
            {
                commandLine.push_back(L' ');
                commandLine += core::quoteWindowsArgument(argument);
            }
            if (commandLine.size() >= 32767) return ERROR_FILENAME_EXCED_RANGE;

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
            const std::wstring directory = fromUtf16(request.workingDirectory);
            if (!CreateProcessW(sevenZip, commandLine.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
            {
                result = GetLastError();
                CloseHandle(captureRead);
                CloseHandle(captureWrite);
                CloseHandle(inputRead);
                CloseHandle(inputWrite);
                return result;
            }
            CloseHandle(captureWrite);
            CloseHandle(inputRead);
            DWORD written{};
            const BOOL inputWritten = WriteFile(inputWrite, input.value.data(),
                static_cast<DWORD>(input.value.size()), &written, nullptr);
            const DWORD inputFailure = inputWritten ? ERROR_SUCCESS : GetLastError();
            CloseHandle(inputWrite);
            if (!inputWritten || written != static_cast<DWORD>(input.value.size()))
            {
                result = inputWritten ? ERROR_WRITE_FAULT : inputFailure;
                TerminateProcess(process.hProcess, result);
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(captureRead);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                return result;
            }

            CaptureContext capture{captureRead, output, window, generation};
            HANDLE captureThread = CreateThread(nullptr, 0, &captureOutput, &capture, 0, nullptr);
            if (captureThread == nullptr)
            {
                result = GetLastError();
                TerminateProcess(process.hProcess, result);
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(captureRead);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                return result;
            }
            const HANDLE waits[]{process.hProcess, cancel};
            const DWORD wait = WaitForMultipleObjects(cancel != nullptr ? 2 : 1, waits, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0 + 1)
            {
                TerminateProcess(process.hProcess, ERROR_CANCELLED);
                WaitForSingleObject(process.hProcess, 5000);
                result = ERROR_CANCELLED;
            }
            else if (wait != WAIT_OBJECT_0)
                result = GetLastError();
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
            if ((result == ERROR_SUCCESS || result == 1) &&
                request.operation != core::ArchiveOperation::extract)
            {
                const DWORD flags = MOVEFILE_WRITE_THROUGH |
                    (request.collision == core::ArchiveCollision::overwrite ?
                        MOVEFILE_REPLACE_EXISTING : 0);
                if (!MoveFileExW(archiveFile.path.c_str(), finalTarget.c_str(), flags))
                    result = GetLastError();
                else
                    archiveFile.path.clear();
            }
            return result;
        }
    }

    int runArchiveWorker(int argumentCount, wchar_t** arguments) noexcept
    {
        if (argumentCount != 8 || arguments == nullptr) return 2;
        unsigned long long windowValue{};
        unsigned long long generationValue{};
        if (!parseUnsigned(arguments[2], windowValue) ||
            !parseUnsigned(arguments[3], generationValue) || generationValue > UINT_MAX ||
            arguments[4] == nullptr || arguments[5] == nullptr || arguments[6] == nullptr ||
            arguments[7] == nullptr || std::wcslen(arguments[4]) >= 32767 ||
            std::wcslen(arguments[5]) >= 260 || std::wcslen(arguments[6]) >= 260 ||
            std::wcslen(arguments[7]) >= 32767)
            return 3;
        const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(windowValue));
        const auto generation = static_cast<std::uint32_t>(generationValue);
        HANDLE output = CreateFileW(arguments[4], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        DWORD result = output == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        core::ArchiveRequest request;
        const bool loaded = loadRequest(arguments[6], request);
        if (!loaded && result == ERROR_SUCCESS) result = ERROR_INVALID_DATA;
        const DWORD attributes = GetFileAttributesW(arguments[7]);
        if ((attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) &&
            result == ERROR_SUCCESS)
            result = ERROR_FILE_NOT_FOUND;
        if (loaded)
        {
            const std::wstring directory = fromUtf16(request.workingDirectory);
            const DWORD directoryAttributes = GetFileAttributesW(directory.c_str());
            if ((directoryAttributes == INVALID_FILE_ATTRIBUTES ||
                    (directoryAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) && result == ERROR_SUCCESS)
                result = ERROR_PATH_NOT_FOUND;
        }
        HANDLE cancel = result == ERROR_SUCCESS ?
            OpenEventW(SYNCHRONIZE, FALSE, arguments[5]) : nullptr;
        if (result == ERROR_SUCCESS && cancel == nullptr) result = GetLastError();
        if (result == ERROR_SUCCESS)
            result = run7Zip(arguments[7], request, output, cancel, window, generation);
        if (!request.password.empty())
            SecureZeroMemory(request.password.data(), request.password.size() * sizeof(char16_t));
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (cancel != nullptr) CloseHandle(cancel);
        if (!PostMessageW(window, archiveCompleteMessage, generation, result)) DeleteFileW(arguments[4]);
        return result == ERROR_SUCCESS ? 0 : 4;
    }
}
