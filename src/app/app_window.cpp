#include "app_window.h"
#include "archive_worker.h"
#include "ftp_worker.h"
#include "git_worker.h"
#include "preview_worker.h"
#include "resource.h"
#include "session_store.h"
#include "tag_worker.h"
#include "xp_theme.h"
#include "../core/command_match.h"
#include "../core/clipboard_path.h"
#include "../core/copydata_path.h"
#include "../core/filename_policy.h"
#include "../core/ftp_request.h"
#include "../core/git_policy.h"
#include "../core/preview_policy.h"
#include "../core/shelf_order.h"
#include "../core/tag_codec.h"
#include "../core/tag_color.h"
#include "../core/tag_request.h"
#include "../core/tag_result_codec.h"
#include "../core/windows_command_line.h"

#include <commctrl.h>

#include <oleacc.h>

#include <UIAutomation.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <winnetwk.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <utility>

namespace filesxp::app
{
    namespace
    {
        constexpr std::size_t noTab = std::numeric_limits<std::size_t>::max();
        constexpr UINT_PTR previewTimerId = 2;
        constexpr UINT_PTR gitStatusTimerId = 3;
        constexpr UINT gitStatusDebounceMilliseconds = 200;
        constexpr UINT gitStatusBusyRetryMilliseconds = 1000;
        constexpr std::size_t maxTaskOutputCharacters = 64 * 1024;
        static_assert(core::maxShelfItems == core::maxShellOperationItems,
            "Shelf and Shell snapshot limits must stay aligned");

        struct WindowPlacement final

        {

            HWND window{};

            int x{};

            int y{};

            int width{};

            int height{};

        };



        class AddressEditProvider final : public IRawElementProviderSimple, public IValueProvider

        {

        public:

            explicit AddressEditProvider(HWND window) noexcept : window_(window) {}



            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override

            {

                if (result == nullptr) return E_POINTER;

                *result = nullptr;

                if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)

                    *result = static_cast<IRawElementProviderSimple*>(this);

                else if (iid == IID_IValueProvider)

                    *result = static_cast<IValueProvider*>(this);

                else

                    return E_NOINTERFACE;

                AddRef();

                return S_OK;

            }



            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }

            ULONG STDMETHODCALLTYPE Release() override { return 1; }



            HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override

            {

                if (result == nullptr) return E_POINTER;

                *result = ProviderOptions_ServerSideProvider;

                return S_OK;

            }



            HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** result) override

            {

                if (result == nullptr) return E_POINTER;

                *result = nullptr;

                if (pattern != UIA_ValuePatternId) return S_OK;

                *result = static_cast<IValueProvider*>(this);

                AddRef();

                return S_OK;

            }



            HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override

            {

                if (result == nullptr) return E_POINTER;

                VariantInit(result);

                switch (property)

                {

                case UIA_ControlTypePropertyId:

                    result->vt = VT_I4;

                    result->lVal = UIA_EditControlTypeId;

                    break;

                case UIA_IsControlElementPropertyId:

                case UIA_IsContentElementPropertyId:

                    result->vt = VT_BOOL;

                    result->boolVal = VARIANT_TRUE;

                    break;

                case UIA_IsEnabledPropertyId:

                    result->vt = VT_BOOL;

                    result->boolVal = IsWindowEnabled(window_) != FALSE ? VARIANT_TRUE : VARIANT_FALSE;

                    break;

                default:

                    break;

                }

                return S_OK;

            }



            HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override

            {

                if (result == nullptr) return E_POINTER;

                *result = nullptr;

                return UiaHostProviderFromHwnd(window_, result);

            }



            HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override

            {

                if (value == nullptr) return E_INVALIDARG;

                if (!IsWindow(window_)) return UIA_E_ELEMENTNOTAVAILABLE;

                if (IsWindowEnabled(window_) == FALSE) return UIA_E_ELEMENTNOTENABLED;

                return SetWindowTextW(window_, value) != FALSE ? S_OK : HRESULT_FROM_WIN32(GetLastError());

            }



            HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override

            {

                if (result == nullptr) return E_POINTER;

                *result = nullptr;

                if (!IsWindow(window_)) return UIA_E_ELEMENTNOTAVAILABLE;

                const int length = GetWindowTextLengthW(window_);

                if (length < 0) return HRESULT_FROM_WIN32(GetLastError());

                BSTR value = SysAllocStringLen(nullptr, static_cast<UINT>(length));

                if (value == nullptr && length != 0) return E_OUTOFMEMORY;

                if (GetWindowTextW(window_, value, length + 1) == 0 && length != 0)

                {

                    SysFreeString(value);

                    return HRESULT_FROM_WIN32(GetLastError());

                }

                *result = value;

                return S_OK;

            }



            HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override

            {

                if (result == nullptr) return E_POINTER;

                *result = (GetWindowLongPtrW(window_, GWL_STYLE) & ES_READONLY) != 0 ? TRUE : FALSE;

                return S_OK;

            }



        private:

            HWND window_{};

        };



        AddressEditProvider addressEditProvider{nullptr};


        struct AsyncProcessLaunch final
        {
            HWND completionWindow{};
            UINT completionMessage{};
            WPARAM completionToken{};
            std::wstring executable;
            std::wstring commandLine;
            std::wstring workingDirectory;
            DWORD creationFlags{};
            WORD showWindow{};
        };

        DWORD WINAPI launchProcessOnThreadPool(void* raw) noexcept
        {
            std::unique_ptr<AsyncProcessLaunch> launch(
                static_cast<AsyncProcessLaunch*>(raw));
            if (launch == nullptr) return ERROR_INVALID_PARAMETER;
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = launch->showWindow;
            PROCESS_INFORMATION process{};
            const BOOL created = CreateProcessW(launch->executable.c_str(),
                launch->commandLine.data(), nullptr, nullptr, FALSE,
                launch->creationFlags, nullptr,
                launch->workingDirectory.empty() ? nullptr : launch->workingDirectory.c_str(),
                &startup, &process);
            const DWORD result = created != FALSE ? ERROR_SUCCESS : GetLastError();
            if (created != FALSE)
            {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
            if (result != ERROR_SUCCESS)
                (void)PostMessageW(launch->completionWindow, launch->completionMessage,
                    launch->completionToken, static_cast<LPARAM>(result));
            return result;
        }

        class DeferredWindowLayout final
        {
        public:
            void place(HWND window, int x, int y, int width, int height) noexcept
            {
                if (window == nullptr) return;
                if (count_ >= placements_.size())
                {
                    SetWindowPos(window, nullptr, x, y, width, height,
                        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
                    return;
                }
                placements_[count_++] = WindowPlacement{window, x, y, width, height};
            }

            void apply() noexcept
            {
                if (count_ == 0) return;
                HDWP positions = BeginDeferWindowPos(static_cast<int>(count_));
                if (positions != nullptr)
                {
                    for (std::size_t index = 0; index < count_; ++index)
                    {
                        const auto& placement = placements_[index];
                        positions = DeferWindowPos(positions, placement.window, nullptr,
                            placement.x, placement.y, placement.width, placement.height,
                            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
                        if (positions == nullptr) break;
                    }
                    if (positions != nullptr && EndDeferWindowPos(positions) != FALSE) return;
                }

                // ponytail: allocation failure falls back to bounded individual moves; repaint
                // remains deferred until the caller's single RedrawWindow invocation.
                for (std::size_t index = 0; index < count_; ++index)
                {
                    const auto& placement = placements_[index];
                    SetWindowPos(placement.window, nullptr, placement.x, placement.y,
                        placement.width, placement.height,
                        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
                }
            }

        private:
            std::array<WindowPlacement, 32> placements_{};
            std::size_t count_{};
        };

        void boundTaskOutput(std::wstring& output)
        {
            if (output.size() <= maxTaskOutputCharacters) return;
            output.resize(maxTaskOutputCharacters);
            output += L"\r\n\r\n[Task output truncated at 64 KiB.]";
        }

        enum ControlId : int
        {
            back = 100,
            forward,
            up,
            refreshButton,
            newFolderButton,
            viewButton,
            address,
            go,
            search,
            places,
            tabs,
            gitCancel
        };

        enum ToolbarMask : std::uint32_t
        {
            toolbarBack = 1U << 0,
            toolbarForward = 1U << 1,
            toolbarUp = 1U << 2,
            toolbarRefresh = 1U << 3,
            toolbarNewFolder = 1U << 4,
            toolbarView = 1U << 5
        };

        enum CommandId : int
        {
            newTab = 300,
            newWindow,
            duplicateTab,
            reopenClosedTab,
            closeTab,
            closeOtherTabs,
            moveTabLeft,
            moveTabRight,
            exitApp,
            newFolder,
            newFile,
            createShortcut,
            createLibrary,
            cut,
            copy,
            copyPath,
            copyPathQuoted,
            paste,
            rename,
            bulkRename,
            folderFromSelection,
            flattenFolder,
            recycleDelete,
            permanentDelete,
            properties,
            editShortcut,
            editLibrary,
            undo,
            redo,
            selectAll,
            refresh,
            viewDetails,
            viewList,
            viewSmall,
            viewMedium,
            viewLarge,
            viewExtraLarge,
            viewTiles,
            viewContent,
            focusAddress,
            focusSearch,
            goBack,
            goForward,
            goUp,
            splitVertical,
            splitHorizontal,
            closePane,
            focusOtherPane,
            togglePreviewPane,
            toggleDetailsPane,
            settings,
            keyboardShortcuts,
            commandPalette,
            gitInit,
            gitClone,
            gitCreateBranch,
            gitSwitchBranch,
            gitStatus,
            gitFetch,
            gitPull,
            gitPush,
            gitSync,
            compressArchive,
            extractArchive,
            shelfCopy,
            shelfMove,
            shelfPaste,
            shelfClear,
            manageShelf,
            editTags,
            filterByTag,
            manageTagColor,
            mapNetworkDrive,
            disconnectNetworkDrive,
            hashSelection,
            showAlternateStreams,
            editAlternateStream,
            verifySignature,
            invertSelection,
            pasteShortcut,
            closeTabsLeft,
            closeTabsRight,
            closeAllTabs,
            openTerminal,
            openTerminalAdmin,
            toggleSidebar,
            fullScreen,
            autoSizeColumns,
            emptyRecycleBin,
            goHome,
            pinQuickAccess,
            unpinQuickAccess,
            pasteIntoFolder,
            showHiddenItems,
            showFileExtensions,
            clearSelection,
            restoreRecycleBin,
            openInNewTab,
            openInNewWindow,
            openInOtherPane,
            openCurrentFolderOtherPane,
            openFileLocation,
            restoreAllRecycleBin,
            playSelection,
            runAsAdministrator,
            runAsDifferentUser,
            runWithPowerShell,
            rotateLeft,
            rotateRight,
            installSelection,
            installCertificate,
            setDesktopWallpaper,
            setDesktopSlideshow,
            openStorageSense,
            ftpManager
        };

        constexpr std::array<CommandId, core::shortcutCount> shortcutCommands{
            CommandId::newTab, CommandId::closeTab, CommandId::focusAddress,
            CommandId::focusSearch, CommandId::commandPalette, CommandId::newFolder};
        constexpr std::array<int, core::shortcutCount> shortcutEditIds{
            IDC_SHORTCUTS_NEW_TAB, IDC_SHORTCUTS_CLOSE_TAB, IDC_SHORTCUTS_ADDRESS,
            IDC_SHORTCUTS_SEARCH, IDC_SHORTCUTS_PALETTE, IDC_SHORTCUTS_FOLDER};
        constexpr std::array<int, core::shortcutCount> shortcutLabelIds{
            IDC_SHORTCUTS_NEW_TAB_LABEL, IDC_SHORTCUTS_CLOSE_TAB_LABEL, IDC_SHORTCUTS_ADDRESS_LABEL,
            IDC_SHORTCUTS_SEARCH_LABEL, IDC_SHORTCUTS_PALETTE_LABEL, IDC_SHORTCUTS_FOLDER_LABEL};
        constexpr std::array<Text, core::shortcutCount> shortcutTexts{
            Text::newTab, Text::closeTab, Text::addressCommand,
            Text::searchCommand, Text::commandPalette, Text::newFolder};

        [[nodiscard]] int scale(int value, int dpi) noexcept
        {
            return MulDiv(value, dpi, 96);
        }

        void setFont(HWND window, HFONT font)
        {
            SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }

        [[nodiscard]] std::wstring windowText(HWND window)
        {
            const int length = GetWindowTextLengthW(window);
            std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
            GetWindowTextW(window, result.data(), length + 1);
            result.resize(static_cast<std::size_t>(length));
            return result;
        }

        [[nodiscard]] std::uint32_t shortcutChord(WPARAM key) noexcept
        {
            std::uint32_t chord = static_cast<std::uint32_t>(key) & core::shortcutKeyMask;
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) chord |= core::shortcutControl;
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) chord |= core::shortcutShift;
            if ((GetKeyState(VK_MENU) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_MENU) & 0x8000) != 0) chord |= core::shortcutAlt;
            return chord;
        }

        [[nodiscard]] std::wstring formatShortcut(std::uint32_t chord)
        {
            std::wstring result;
            if ((chord & core::shortcutControl) != 0) result += L"Ctrl+";
            if ((chord & core::shortcutAlt) != 0) result += L"Alt+";
            if ((chord & core::shortcutShift) != 0) result += L"Shift+";
            const UINT key = chord & core::shortcutKeyMask;
            wchar_t keyName[64]{};
            UINT scanCode = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
            if (key == VK_INSERT || key == VK_DELETE || key == VK_HOME || key == VK_END ||
                key == VK_PRIOR || key == VK_NEXT || key == VK_LEFT || key == VK_RIGHT ||
                key == VK_UP || key == VK_DOWN || key == VK_DIVIDE || key == VK_NUMLOCK)
            {
                scanCode |= 0x100;
            }
            const LONG keyData = static_cast<LONG>(scanCode << 16);
            if (GetKeyNameTextW(keyData, keyName, static_cast<int>(std::size(keyName))) > 0)
            {
                result += keyName;
            }
            else
            {
                result += L"VK ";
                result += std::to_wstring(key);
            }
            return result;
        }

        [[nodiscard]] std::wstring knownFolderPath(REFKNOWNFOLDERID folderId)
        {
            PWSTR raw{};
            if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &raw)))
            {
                return {};
            }
            std::wstring path(raw);
            CoTaskMemFree(raw);
            return path;
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
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size())
            {
                return {};
            }
            path.resize(length);
            const std::size_t separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
        }

        [[nodiscard]] std::wstring moduleExecutable()
        {
            std::wstring path(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size()) return {};
            path.resize(length);
            return path;
        }

        [[nodiscard]] std::wstring pendingTabTitle(std::wstring_view location)
        {
            while (location.size() > 1 &&
                (location.back() == L'\\' || location.back() == L'/'))
                location.remove_suffix(1);
            const std::size_t separator = location.find_last_of(L"\\/");
            std::wstring_view title = separator == std::wstring_view::npos ? location :
                location.substr(separator + 1);
            if (title.empty()) title = location;
            constexpr std::size_t maximumTitleCharacters = 128;
            if (title.size() > maximumTitleCharacters)
                title = title.substr(title.size() - maximumTitleCharacters);
            return std::wstring(title);
        }

        [[nodiscard]] std::u16string toUtf16(std::wstring_view value)
        {
            static_assert(sizeof(wchar_t) == sizeof(char16_t));
            return std::u16string(reinterpret_cast<const char16_t*>(value.data()), value.size());
        }

        [[nodiscard]] std::wstring environmentPath(const wchar_t* name)
        {
            std::wstring value(32768, L'\0');
            const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
            if (length == 0 || length >= value.size())
            {
                return {};
            }
            value.resize(length);
            return value;
        }

        [[nodiscard]] std::wstring find7ZipConsole()
        {
            const std::wstring applicationDirectory = moduleDirectory();
            if (!applicationDirectory.empty())
            {
                const std::wstring bundled = applicationDirectory + L"\\7z.exe";
                if (isRegularFile(bundled))
                {
                    return bundled;
                }
            }
            const std::array<std::wstring, 3> roots{
                environmentPath(L"ProgramW6432"),
                environmentPath(L"ProgramFiles"),
                environmentPath(L"ProgramFiles(x86)")};
            for (const auto& root : roots)
            {
                if (root.empty())
                {
                    continue;
                }
                const std::wstring candidate = root + L"\\7-Zip\\7z.exe";
                if (isRegularFile(candidate))
                {
                    return candidate;
                }
            }
            return {};
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

        [[nodiscard]] std::wstring systemExecutable(const wchar_t* name)
        {
            std::wstring directory(32768, L'\0');
            const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
            if (length == 0 || length >= directory.size())
            {
                return {};
            }
            directory.resize(length);
            const std::wstring executable = directory + L"\\" + name;
            return isRegularFile(executable) ? executable : std::wstring{};
        }

        [[nodiscard]] std::wstring systemCommandProcessor()
        {
            return systemExecutable(L"cmd.exe");
        }

        [[nodiscard]] std::wstring uniqueTemporaryPath(const wchar_t* purpose)
        {
            std::wstring temporary(32768, L'\0');
            const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
            if (length == 0 || length >= temporary.size()) return {};
            temporary.resize(length);
            GUID identifier{};
            if (FAILED(CoCreateGuid(&identifier))) return {};
            wchar_t identifierText[40]{};
            if (StringFromGUID2(identifier, identifierText, static_cast<int>(std::size(identifierText))) == 0)
                return {};
            return temporary + L"FilesXPNative-" + purpose + L"-" + identifierText + L".bin";
        }

        [[nodiscard]] std::wstring uniqueLocalObjectName(const wchar_t* purpose)
        {
            GUID identifier{};
            if (FAILED(CoCreateGuid(&identifier))) return {};
            wchar_t identifierText[40]{};
            if (StringFromGUID2(identifier, identifierText, static_cast<int>(std::size(identifierText))) == 0)
                return {};
            return L"Local\\FilesXPNative-" + std::wstring(purpose) + L"-" + identifierText;
        }

        [[nodiscard]] std::uintptr_t uniqueWorkerToken() noexcept
        {
            GUID identifier{};
            if (FAILED(CoCreateGuid(&identifier))) return 0;
            std::uintptr_t token{};
            static_assert(sizeof(token) <= sizeof(identifier));
            std::memcpy(&token, &identifier, sizeof(token));
            return token == 0 ? 1 : token;
        }

        [[nodiscard]] bool decodeBoundedText(std::string bytes, std::wstring& result) noexcept
        {
            try
            {
                const std::size_t offset = bytes.size() >= 3 &&
                    static_cast<unsigned char>(bytes[0]) == 0xef &&
                    static_cast<unsigned char>(bytes[1]) == 0xbb &&
                    static_cast<unsigned char>(bytes[2]) == 0xbf ? 3U : 0U;
                if (offset == bytes.size())
                {
                    result.clear();
                    return true;
                }
                UINT codePage = CP_UTF8;
                DWORD flags = MB_ERR_INVALID_CHARS;
                const char* const data = bytes.data() + offset;
                const int byteCount = static_cast<int>(bytes.size() - offset);
                int required = MultiByteToWideChar(codePage, flags, data,
                    byteCount, nullptr, 0);
                if (required == 0)
                {
                    codePage = CP_ACP;
                    flags = 0;
                    required = MultiByteToWideChar(codePage, flags, data,
                        byteCount, nullptr, 0);
                }
                if (required <= 0) return false;
                result.resize(static_cast<std::size_t>(required));
                return MultiByteToWideChar(codePage, flags, data,
                    byteCount, result.data(), required) == required;
            }
            catch (...)
            {
                result.clear();
                return false;
            }
        }

        [[nodiscard]] bool readBoundedUtf8File(const std::wstring& path, std::wstring& result) noexcept
        {
            // ponytail: Archive producers and the UI consumer share the same 64-KiB ceiling.
            constexpr DWORD maxBytes = 64U * 1024U;
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER size{};
            bool success = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
                size.QuadPart <= maxBytes;
            std::string bytes;
            if (success)
            {
                try
                {
                    bytes.resize(static_cast<std::size_t>(size.QuadPart));
                    DWORD read{};
                    success = bytes.empty() || (ReadFile(file, bytes.data(),
                        static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE &&
                        read == bytes.size());
                }
                catch (...)
                {
                    success = false;
                }
            }
            CloseHandle(file);
            if (!success) return false;
            return decodeBoundedText(std::move(bytes), result);
        }

        [[nodiscard]] bool decodeStrictUtf8(std::string bytes,
            std::wstring& result) noexcept
        {
            if (bytes.empty())
            {
                result.clear();
                return true;
            }
            const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (required <= 0) return false;
            try
            {
                result.resize(static_cast<std::size_t>(required));
            }
            catch (...)
            {
                result.clear();
                return false;
            }
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                static_cast<int>(bytes.size()), result.data(), required) == required;
        }

        [[nodiscard]] std::string wideToUtf8(std::wstring_view value)
        {
            if (value.empty()) return {};
            const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (required <= 0) return {};
            std::string result(static_cast<std::size_t>(required), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                    static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) != required)
                result.clear();
            return result;
        }

        [[nodiscard]] std::wstring normalizeFtpUrl(std::wstring value)
        {
            while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
            while (!value.empty() && iswspace(value.back())) value.pop_back();
            if (value.size() >= 6 && _wcsnicmp(value.c_str(), L"ftp://", 6) == 0)
                value.replace(0, 6, L"ftp://");
            else if (value.size() >= 7 && _wcsnicmp(value.c_str(), L"ftps://", 7) == 0)
                value.replace(0, 7, L"ftps://");
            if (!value.empty() && value.back() != L'/') value.push_back(L'/');
            return value;
        }

        [[nodiscard]] std::wstring chooseUploadFile(HWND owner)
        {
            IFileOpenDialog* dialog{};
            HRESULT status = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                reinterpret_cast<void**>(&dialog));
            if (FAILED(status) || dialog == nullptr) return {};
            FILEOPENDIALOGOPTIONS options{};
            status = dialog->GetOptions(&options);
            if (SUCCEEDED(status))
                status = dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
                    FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
            if (SUCCEEDED(status)) status = dialog->Show(owner);
            IShellItem* item{};
            if (SUCCEEDED(status)) status = dialog->GetResult(&item);
            PWSTR raw{};
            if (SUCCEEDED(status)) status = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
            std::wstring path;
            if (SUCCEEDED(status) && raw != nullptr) path = raw;
            if (raw != nullptr) CoTaskMemFree(raw);
            if (item != nullptr) item->Release();
            dialog->Release();
            return path;
        }

        void scrubFtpCredentials(core::FtpRequest& request) noexcept
        {
            if (!request.password.empty())
                SecureZeroMemory(request.password.data(),
                    request.password.size() * sizeof(char16_t));
            request.password.clear();
            if (!request.username.empty())
                SecureZeroMemory(request.username.data(),
                    request.username.size() * sizeof(char16_t));
            request.username.clear();
        }

        struct FtpCredentialGuard final
        {
            explicit FtpCredentialGuard(core::FtpRequest& value) noexcept : request(value) {}
            core::FtpRequest& request;
            ~FtpCredentialGuard() { scrubFtpCredentials(request); }
            FtpCredentialGuard(const FtpCredentialGuard&) = delete;
            FtpCredentialGuard& operator=(const FtpCredentialGuard&) = delete;
        };

        [[nodiscard]] DWORD writeClipboardText(HWND owner, std::wstring_view text) noexcept
        {
            if (text.empty()) return ERROR_INVALID_DATA;
            if (text.size() >= std::numeric_limits<std::size_t>::max() / sizeof(wchar_t) - 1)
                return ERROR_ARITHMETIC_OVERFLOW;
            const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (memory == nullptr) return ERROR_NOT_ENOUGH_MEMORY;
            void* destination = GlobalLock(memory);
            if (destination == nullptr)
            {
                GlobalFree(memory);
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
            static_cast<wchar_t*>(destination)[text.size()] = L'\0';
            GlobalUnlock(memory);

            if (!OpenClipboard(owner))
            {
                GlobalFree(memory);
                return ERROR_BUSY;
            }
            DWORD failure = ERROR_SUCCESS;
            if (!EmptyClipboard())
            {
                failure = GetLastError();
                if (failure == ERROR_SUCCESS) failure = ERROR_GEN_FAILURE;
            }
            else if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
            {
                failure = GetLastError();
                if (failure == ERROR_SUCCESS) failure = ERROR_GEN_FAILURE;
            }
            else
                memory = nullptr;
            CloseClipboard();
            if (memory != nullptr) GlobalFree(memory);
            return failure == ERROR_SUCCESS ? ERROR_SUCCESS : failure;
        }

        void appendMenuItem(HMENU menu, UINT id, const wchar_t* label)
        {
            AppendMenuW(menu, MF_STRING, id, label);
        }

        void setAccessibleName(HWND window, const wchar_t* name) noexcept

        {

            if (window == nullptr || name == nullptr) return;

            IAccPropServices* services{};

            if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,

                    IID_IAccPropServices, reinterpret_cast<void**>(&services))))

            {

                services->SetHwndPropStr(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                    PROPID_ACC_NAME, name);

                services->Release();

            }

        }



        void fillSettingsDialog(HWND dialog, const core::AppSettings& settings)
        {
            SendDlgItemMessageW(dialog, IDC_SETTINGS_LANGUAGE, CB_SETCURSEL,
                static_cast<WPARAM>(settings.locale), 0);
            SendDlgItemMessageW(dialog, IDC_SETTINGS_DEFAULT_VIEW, CB_SETCURSEL,
                settings.defaultView, 0);
            SendDlgItemMessageW(dialog, IDC_SETTINGS_PREVIEW_PROVIDER, CB_SETCURSEL,
                static_cast<WPARAM>(settings.previewProvider), 0);
            SetDlgItemTextW(dialog, IDC_SETTINGS_START, settings.startLocation.c_str());
            const auto check = [dialog, &settings](int control, core::SettingFlag flag)
            {
                CheckDlgButton(dialog, control, settings.enabled(flag) ? BST_CHECKED : BST_UNCHECKED);
            };
            check(IDC_SETTINGS_RESTORE, core::restoreSession);
            check(IDC_SETTINGS_PLACES, core::showPlaces);
            check(IDC_SETTINGS_COMPACT, core::compactToolbar);
            check(IDC_SETTINGS_CONFIRM_DELETE, core::confirmPermanentDelete);
            check(IDC_SETTINGS_GIT, core::enableGit);
            check(IDC_SETTINGS_ARCHIVES, core::enableArchives);
            check(IDC_SETTINGS_PREVIEW, core::enableQuickPreview);
            const auto toolbar = [dialog, &settings](int control, ToolbarMask mask)
            {
                CheckDlgButton(dialog, control,
                    (settings.toolbarButtons & mask) != 0 ? BST_CHECKED : BST_UNCHECKED);
            };
            toolbar(IDC_SETTINGS_TOOLBAR_BACK, toolbarBack);
            toolbar(IDC_SETTINGS_TOOLBAR_FORWARD, toolbarForward);
            toolbar(IDC_SETTINGS_TOOLBAR_UP, toolbarUp);
            toolbar(IDC_SETTINGS_TOOLBAR_REFRESH, toolbarRefresh);
            toolbar(IDC_SETTINGS_TOOLBAR_FOLDER, toolbarNewFolder);
            toolbar(IDC_SETTINGS_TOOLBAR_VIEW, toolbarView);
        }

        [[nodiscard]] bool readSettingsDialog(HWND dialog, core::AppSettings& settings)
        {
            const LRESULT locale = SendDlgItemMessageW(dialog, IDC_SETTINGS_LANGUAGE, CB_GETCURSEL, 0, 0);
            const LRESULT view = SendDlgItemMessageW(dialog, IDC_SETTINGS_DEFAULT_VIEW, CB_GETCURSEL, 0, 0);
            const LRESULT previewProvider = SendDlgItemMessageW(dialog,
                IDC_SETTINGS_PREVIEW_PROVIDER, CB_GETCURSEL, 0, 0);
            if (locale < 0 || locale > static_cast<LRESULT>(core::Locale::simplifiedChinese) ||
                view < 0 || view > static_cast<LRESULT>(core::SettingsCodec::maxView) ||
                previewProvider < 0 ||
                !core::validPreviewProvider(static_cast<std::uint32_t>(previewProvider)))
            {
                return false;
            }
            settings.flags = 0;
            settings.locale = static_cast<core::Locale>(locale);
            settings.defaultView = static_cast<std::uint32_t>(view);
            settings.previewProvider = static_cast<core::PreviewProvider>(previewProvider);
            settings.startLocation = windowText(GetDlgItem(dialog, IDC_SETTINGS_START));
            const auto checked = [dialog](int control)
            {
                return IsDlgButtonChecked(dialog, control) == BST_CHECKED;
            };
            settings.set(core::restoreSession, checked(IDC_SETTINGS_RESTORE));
            settings.set(core::showPlaces, checked(IDC_SETTINGS_PLACES));
            settings.set(core::compactToolbar, checked(IDC_SETTINGS_COMPACT));
            settings.set(core::confirmPermanentDelete, checked(IDC_SETTINGS_CONFIRM_DELETE));
            settings.set(core::enableGit, checked(IDC_SETTINGS_GIT));
            settings.set(core::enableArchives, checked(IDC_SETTINGS_ARCHIVES));
            settings.set(core::enableQuickPreview, checked(IDC_SETTINGS_PREVIEW));
            settings.toolbarButtons = 0;
            const auto toolbar = [dialog, &settings](int control, ToolbarMask mask)
            {
                if (IsDlgButtonChecked(dialog, control) == BST_CHECKED)
                {
                    settings.toolbarButtons |= mask;
                }
            };
            toolbar(IDC_SETTINGS_TOOLBAR_BACK, toolbarBack);
            toolbar(IDC_SETTINGS_TOOLBAR_FORWARD, toolbarForward);
            toolbar(IDC_SETTINGS_TOOLBAR_UP, toolbarUp);
            toolbar(IDC_SETTINGS_TOOLBAR_REFRESH, toolbarRefresh);
            toolbar(IDC_SETTINGS_TOOLBAR_FOLDER, toolbarNewFolder);
            toolbar(IDC_SETTINGS_TOOLBAR_VIEW, toolbarView);
            return !core::SettingsCodec::encode(settings).empty();
        }
    }

    AppWindow::AppWindow(HINSTANCE instance)
        : instance_(instance), settings_(SettingsStore::load()), localizer_(settings_.locale)
    {
    }

    AppWindow::~AppWindow()
    {
        clearShelf();
        releaseTabs();
        if (uiFont_ != nullptr)
        {
            DeleteObject(uiFont_);
        }
    }

    bool AppWindow::create(int showCommand, std::wstring initialPath)
    {
        if (!registerWindowClass())
        {
            return false;
        }

        window_ = CreateWindowExW(0, appWindowClassName, L"Files XP Native",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1220, 780,
            nullptr, nullptr, instance_, this);
        if (window_ == nullptr)
        {
            return false;
        }

        ShowWindow(window_, showCommand);
        UpdateWindow(window_);
        if (initialPath.empty() && settings_.enabled(core::restoreSession))
        {
            restoreSession();
        }
        else
        {
            addTab(std::move(initialPath));
        }
        return !tabs_.empty();
    }

    int AppWindow::runMessageLoop()
    {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (handleShortcut(message))
            {
                continue;
            }
            if (const auto browser = activeBrowser(); browser != nullptr && browser->translateAccelerator(message))
            {
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    bool AppWindow::registerWindowClass()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &AppWindow::windowProcedure;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = appWindowClassName;
        windowClass.hIconSm = windowClass.hIcon;
        return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    LRESULT CALLBACK AppWindow::windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        AppWindow* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<AppWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self != nullptr ? self->handleMessage(message, wParam, lParam)
                               : DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT CALLBACK AppWindow::editProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR, DWORD_PTR referenceData)
    {
        if (message == WM_GETOBJECT && lParam == static_cast<LPARAM>(UiaRootObjectId) &&
            GetDlgCtrlID(window) == ControlId::address)
        {
            addressEditProvider = AddressEditProvider{window};
            return UiaReturnRawElementProvider(window, wParam, lParam, &addressEditProvider);
        }
        if (message == WM_KEYDOWN && wParam == VK_RETURN)
        {
            const HWND owner = reinterpret_cast<HWND>(referenceData);
            const UINT notification = GetDlgCtrlID(window) == ControlId::search
                ? searchEnterMessage : addressEnterMessage;
            PostMessageW(owner, notification, 0, 0);
            return 0;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    LRESULT CALLBACK AppWindow::tabProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR, DWORD_PTR referenceData)
    {
        auto* self = reinterpret_cast<AppWindow*>(referenceData);
        if (self == nullptr)
        {
            return DefSubclassProc(window, message, wParam, lParam);
        }
        if (message == WM_LBUTTONDOWN)
        {
            TCHITTESTINFO hit{{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, 0};
            self->tabDragIndex_ = TabCtrl_HitTest(window, &hit);
            if (self->tabDragIndex_ >= 0)
            {
                SetCapture(window);
            }
        }
        else if (message == WM_MOUSEMOVE && self->tabDragIndex_ >= 0 && (wParam & MK_LBUTTON) != 0)
        {
            TCHITTESTINFO hit{{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, 0};
            const int destination = TabCtrl_HitTest(window, &hit);
            if (destination >= 0 && destination != self->tabDragIndex_)
            {
                self->moveTab(static_cast<std::size_t>(self->tabDragIndex_),
                    static_cast<std::size_t>(destination));
                self->tabDragIndex_ = destination;
            }
        }
        else if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED)
        {
            self->tabDragIndex_ = -1;
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
        }
        else if (message == WM_MBUTTONUP)
        {
            TCHITTESTINFO hit{{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, 0};
            const int index = TabCtrl_HitTest(window, &hit);
            if (index >= 0) self->closeTab(static_cast<std::size_t>(index));
            return 0;
        }
        else if (message == WM_LBUTTONDBLCLK)
        {
            TCHITTESTINFO hit{{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, 0};
            if (TabCtrl_HitTest(window, &hit) < 0) self->addTab();
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    INT_PTR CALLBACK AppWindow::settingsProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            const std::array<Text, 4> localeTexts{Text::systemLanguage, Text::english,
                Text::traditionalChinese, Text::simplifiedChinese};
            const std::array<Text, 6> viewTexts{Text::details, Text::list, Text::mediumIcons,
                Text::largeIcons, Text::tiles, Text::content};
            const std::array<Text, 5> previewProviderTexts{Text::previewAutomatic,
                Text::previewWindows, Text::previewQuickLook, Text::previewSeer,
                Text::previewPowerToys};
            for (Text text : localeTexts)
            {
                SendDlgItemMessageW(dialog, IDC_SETTINGS_LANGUAGE, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(self->localizer_(text)));
            }
            for (Text text : viewTexts)
            {
                SendDlgItemMessageW(dialog, IDC_SETTINGS_DEFAULT_VIEW, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(self->localizer_(text)));
            }
            for (Text text : previewProviderTexts)
            {
                SendDlgItemMessageW(dialog, IDC_SETTINGS_PREVIEW_PROVIDER, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(self->localizer_(text)));
            }
            std::wstring caption = self->localizer_(Text::title);
            caption += L" - ";
            caption += self->localizer_(Text::settings);
            SetWindowTextW(dialog, caption.c_str());
            SetDlgItemTextW(dialog, IDC_SETTINGS_LANGUAGE_LABEL, self->localizer_(Text::language));
            SetDlgItemTextW(dialog, IDC_SETTINGS_VIEW_LABEL, self->localizer_(Text::defaultView));
            SetDlgItemTextW(dialog, IDC_SETTINGS_START_LABEL, self->localizer_(Text::newTabLocation));
            SetDlgItemTextW(dialog, IDC_SETTINGS_RESTORE, self->localizer_(Text::restoreTabs));
            SetDlgItemTextW(dialog, IDC_SETTINGS_PLACES, self->localizer_(Text::showPlaces));
            SetDlgItemTextW(dialog, IDC_SETTINGS_COMPACT, self->localizer_(Text::compactToolbar));
            SetDlgItemTextW(dialog, IDC_SETTINGS_CONFIRM_DELETE, self->localizer_(Text::confirmDelete));
            SetDlgItemTextW(dialog, IDC_SETTINGS_GIT, self->localizer_(Text::enableGit));
            SetDlgItemTextW(dialog, IDC_SETTINGS_ARCHIVES, self->localizer_(Text::enableArchives));
            SetDlgItemTextW(dialog, IDC_SETTINGS_PREVIEW, self->localizer_(Text::spacePreview));
            SetDlgItemTextW(dialog, IDC_SETTINGS_PREVIEW_PROVIDER_LABEL,
                self->localizer_(Text::previewProvider));
            SetDlgItemTextW(dialog, IDC_SETTINGS_TOOLBAR_LABEL, self->localizer_(Text::toolbarButtons));
            SetDlgItemTextW(dialog, IDC_SETTINGS_TOOLBAR_BACK, self->localizer_(Text::back));
            SetDlgItemTextW(dialog, IDC_SETTINGS_TOOLBAR_FORWARD, self->localizer_(Text::forward));
            SetDlgItemTextW(dialog, IDC_SETTINGS_TOOLBAR_UP, self->localizer_(Text::up));
            SetDlgItemTextW(dialog, IDC_SETTINGS_TOOLBAR_REFRESH, self->localizer_(Text::refresh));
            SetDlgItemTextW(dialog, IDC_SETTINGS_TOOLBAR_FOLDER, self->localizer_(Text::newFolder));
            SetDlgItemTextW(dialog, IDC_SETTINGS_TOOLBAR_VIEW, self->localizer_(Text::view));
            SetDlgItemTextW(dialog, IDC_SETTINGS_RESET, self->localizer_(Text::reset));
            SetDlgItemTextW(dialog, IDOK, self->localizer_(Text::ok));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::cancel));
            SendDlgItemMessageW(dialog, IDC_SETTINGS_START, EM_SETLIMITTEXT,
                core::SettingsCodec::maxStartLocationLength, 0);
            fillSettingsDialog(dialog, self->settings_);
            return TRUE;
        }
        if (self == nullptr || message != WM_COMMAND)
        {
            return FALSE;
        }
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            core::AppSettings candidate = self->settings_;
            if (!readSettingsDialog(dialog, candidate))
            {
                MessageBoxW(dialog, L"The settings are invalid or too long.", L"Files XP Native",
                    MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (!SettingsStore::save(candidate))
            {
                self->showError(L"Save settings", HRESULT_FROM_WIN32(GetLastError()));
                return TRUE;
            }
            self->settings_ = std::move(candidate);
            self->applySettings();
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDC_SETTINGS_RESET:
            if (!SettingsStore::reset())
            {
                self->showError(L"Reset settings", HRESULT_FROM_WIN32(GetLastError()));
                return TRUE;
            }
            self->settings_ = {};
            self->applySettings();
            fillSettingsDialog(dialog, self->settings_);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    }

    LRESULT CALLBACK AppWindow::shortcutEditProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_GETDLGCODE)
        {
            return DLGC_WANTALLKEYS;
        }
        if (message == WM_CHAR || message == WM_SYSCHAR)
        {
            return 0;
        }
        if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN)
        {
            return DefSubclassProc(window, message, wParam, lParam);
        }
        if (wParam == VK_TAB)
        {
            const bool previous = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            SetFocus(GetNextDlgTabItem(GetParent(window), window, previous));
            return 0;
        }
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
        {
            SendMessageW(GetParent(window), WM_COMMAND,
                MAKEWPARAM(wParam == VK_RETURN ? IDOK : IDCANCEL, BN_CLICKED), 0);
            return 0;
        }
        if (wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL ||
            wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
            wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU)
        {
            return 0;
        }
        const std::uint32_t chord = shortcutChord(wParam);
        if (!core::validShortcut(chord))
        {
            MessageBeep(MB_ICONWARNING);
            return 0;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, static_cast<LONG_PTR>(chord));
        const std::wstring label = formatShortcut(chord);
        SetWindowTextW(window, label.c_str());
        return 0;
    }

    INT_PTR CALLBACK AppWindow::shortcutProcedure(HWND dialog, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            SetWindowTextW(dialog, self->localizer_(Text::keyboardShortcuts));
            SetDlgItemTextW(dialog, IDC_SHORTCUTS_INSTRUCTIONS,
                self->localizer_(Text::shortcutInstructions));
            SetDlgItemTextW(dialog, IDOK, self->localizer_(Text::ok));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::cancel));
            for (std::size_t index = 0; index < core::shortcutCount; ++index)
            {
                SetDlgItemTextW(dialog, shortcutLabelIds[index], self->localizer_(shortcutTexts[index]));
                HWND edit = GetDlgItem(dialog, shortcutEditIds[index]);
                SetWindowLongPtrW(edit, GWLP_USERDATA,
                    static_cast<LONG_PTR>(self->settings_.shortcuts[index]));
                const std::wstring label = formatShortcut(self->settings_.shortcuts[index]);
                SetWindowTextW(edit, label.c_str());
                SetWindowSubclass(edit, &AppWindow::shortcutEditProcedure, 1, 0);
            }
            SetFocus(GetDlgItem(dialog, shortcutEditIds.front()));
            return FALSE;
        }
        if (self == nullptr || message != WM_COMMAND)
        {
            return FALSE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            core::AppSettings candidate = self->settings_;
            for (std::size_t index = 0; index < core::shortcutCount; ++index)
            {
                candidate.shortcuts[index] = static_cast<std::uint32_t>(GetWindowLongPtrW(
                    GetDlgItem(dialog, shortcutEditIds[index]), GWLP_USERDATA));
            }
            if (!core::validShortcutMap(candidate.shortcuts))
            {
                MessageBoxW(dialog, self->localizer_(Text::shortcutConflict),
                    self->localizer_(Text::title), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (!SettingsStore::save(candidate))
            {
                self->showError(L"Save shortcuts", HRESULT_FROM_WIN32(GetLastError()));
                return TRUE;
            }
            self->settings_ = std::move(candidate);
            self->applySettings();
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK AppWindow::nameProcedure(HWND dialog, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            SetWindowTextW(dialog, self->localizer_(self->namePromptTitle_));
            SetDlgItemTextW(dialog, IDC_NAME_PROMPT, self->localizer_(self->namePromptInstructions_));
            SetDlgItemTextW(dialog, IDOK, self->localizer_(Text::ok));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::cancel));
            SendDlgItemMessageW(dialog, IDC_NAME_EDIT, EM_SETLIMITTEXT,
                self->nameGenericText_ ? core::maxGitInputLength : core::maxFilenameLength, 0);
            SetFocus(GetDlgItem(dialog, IDC_NAME_EDIT));
            return FALSE;
        }
        if (self == nullptr || message != WM_COMMAND)
        {
            return FALSE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            const std::wstring value = windowText(GetDlgItem(dialog, IDC_NAME_EDIT));
            const bool valid = self->nameGenericText_ ?
                core::validGitRepositoryInput(value) : self->nameAlternateStream_ ?
                    core::validAlternateStreamName(value) :
                    core::validWindowsFilename(value, self->nameAllowDot_);
            if (!valid)
            {
                MessageBoxW(dialog, self->localizer_(Text::invalidName),
                    self->localizer_(Text::title), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            self->nameInput_ = value;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK AppWindow::linkProcedure(HWND dialog, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            SetWindowTextW(dialog, self->localizer_(Text::createShortcut));
            SetDlgItemTextW(dialog, IDC_LINK_NAME_LABEL, self->localizer_(Text::shortcutName));
            SetDlgItemTextW(dialog, IDC_LINK_TARGET_LABEL, self->localizer_(Text::shortcutTarget));
            SetDlgItemTextW(dialog, IDC_LINK_ARGUMENTS_LABEL, self->localizer_(Text::shortcutArguments));
            SetDlgItemTextW(dialog, IDC_LINK_WORKING_LABEL,
                self->localizer_(Text::shortcutWorkingDirectory));
            SetDlgItemTextW(dialog, IDC_LINK_ICON_LABEL, self->localizer_(Text::shortcutIcon));
            SetDlgItemTextW(dialog, IDOK, self->localizer_(Text::ok));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::cancel));
            SendDlgItemMessageW(dialog, IDC_LINK_NAME, EM_SETLIMITTEXT, core::maxFilenameLength, 0);
            for (int control : {IDC_LINK_TARGET, IDC_LINK_ARGUMENTS, IDC_LINK_WORKING, IDC_LINK_ICON})
            {
                SendDlgItemMessageW(dialog, control, EM_SETLIMITTEXT, 32767, 0);
            }
            SetFocus(GetDlgItem(dialog, IDC_LINK_NAME));
            return FALSE;
        }
        if (self == nullptr || message != WM_COMMAND)
        {
            return FALSE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            const std::wstring name = windowText(GetDlgItem(dialog, IDC_LINK_NAME));
            const std::wstring target = windowText(GetDlgItem(dialog, IDC_LINK_TARGET));
            if (!core::validWindowsFilename(name) || target.empty())
            {
                MessageBoxW(dialog, self->localizer_(Text::shortcutInvalid),
                    self->localizer_(Text::title), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            self->linkName_ = name;
            self->linkTarget_ = target;
            self->linkArguments_ = windowText(GetDlgItem(dialog, IDC_LINK_ARGUMENTS));
            self->linkWorkingDirectory_ = windowText(GetDlgItem(dialog, IDC_LINK_WORKING));
            self->linkIcon_ = windowText(GetDlgItem(dialog, IDC_LINK_ICON));
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK AppWindow::commandPaletteProcedure(HWND dialog, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            SetWindowTextW(dialog, self->localizer_(Text::commandPalette));
            SetDlgItemTextW(dialog, IDOK, self->localizer_(Text::runCommand));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::cancel));
            SendDlgItemMessageW(dialog, IDC_PALETTE_QUERY, EM_SETLIMITTEXT, 128, 0);
            self->refreshCommandPalette(dialog);
            SetFocus(GetDlgItem(dialog, IDC_PALETTE_QUERY));
            return FALSE;
        }
        if (self == nullptr || message != WM_COMMAND)
        {
            return FALSE;
        }
        if (LOWORD(wParam) == IDC_PALETTE_QUERY && HIWORD(wParam) == EN_CHANGE)
        {
            self->refreshCommandPalette(dialog);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK ||
            (LOWORD(wParam) == IDC_PALETTE_LIST && HIWORD(wParam) == LBN_DBLCLK))
        {
            const LRESULT selected = SendDlgItemMessageW(dialog, IDC_PALETTE_LIST, LB_GETCURSEL, 0, 0);
            if (selected >= 0 && static_cast<std::size_t>(selected) < self->paletteCommands_.size())
            {
                EndDialog(dialog, self->paletteCommands_[static_cast<std::size_t>(selected)]);
            }
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK AppWindow::tagProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            SetWindowTextW(dialog, self->localizer_(self->singleTagPrompt_ ? Text::filterByTag : Text::editTags));
            SetDlgItemTextW(dialog, IDC_TAGS_PROMPT, self->singleTagPrompt_
                ? self->localizer_(Text::filterByTag) : self->localizer_(Text::tagInstructions));
            SetDlgItemTextW(dialog, IDOK, self->localizer_(Text::ok));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::cancel));
            SendDlgItemMessageW(dialog, IDC_TAGS_EDIT, EM_SETLIMITTEXT,
                core::maxTags * (core::maxTagLength + 2), 0);
            SetFocus(GetDlgItem(dialog, IDC_TAGS_EDIT));
            return FALSE;
        }
        if (self == nullptr || message != WM_COMMAND)
        {
            return FALSE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            std::vector<std::wstring> tags;
            if (!core::normalizeTags(windowText(GetDlgItem(dialog, IDC_TAGS_EDIT)), tags) ||
                (self->singleTagPrompt_ && tags.size() != 1))
            {
                MessageBoxW(dialog, self->localizer_(Text::invalidTags), self->localizer_(Text::title),
                    MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            self->tagInput_ = core::joinTags(tags);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK AppWindow::tagColorProcedure(HWND dialog, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            SetWindowTextW(dialog, self->localizer_(Text::manageTagColor));
            SetDlgItemTextW(dialog, IDC_TAG_COLOR_LABEL, self->tagInput_.c_str());
            SetDlgItemTextW(dialog, IDOK, self->localizer_(Text::ok));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::cancel));
            const std::array<Text, 8> colors{Text::colorNone, Text::colorRed, Text::colorOrange,
                Text::colorYellow, Text::colorGreen, Text::colorBlue, Text::colorPurple,
                Text::colorGray};
            for (Text color : colors)
                SendDlgItemMessageW(dialog, IDC_TAG_COLOR_COMBO, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(self->localizer_(color)));
            SendDlgItemMessageW(dialog, IDC_TAG_COLOR_COMBO, CB_SETCURSEL,
                self->tagColorChoice_, 0);
            return TRUE;
        }
        if (self == nullptr || message != WM_COMMAND) return FALSE;
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            const LRESULT selected = SendDlgItemMessageW(dialog, IDC_TAG_COLOR_COMBO,
                CB_GETCURSEL, 0, 0);
            if (selected < 0 || !core::validTagColor(static_cast<std::uint32_t>(selected)))
                return TRUE;
            self->tagColorChoice_ = static_cast<std::uint32_t>(selected);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK AppWindow::archiveProcedure(HWND dialog, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            SetWindowTextW(dialog, self->localizer_(self->archiveCreating_ ? Text::compress : Text::extract));
            SetDlgItemTextW(dialog, IDC_ARCHIVE_NAME_LABEL,
                self->localizer_(self->archiveCreating_ ? Text::archiveName : Text::destinationFolder));
            SetDlgItemTextW(dialog, IDC_ARCHIVE_FORMAT_LABEL, self->localizer_(Text::archiveFormat));
            SetDlgItemTextW(dialog, IDC_ARCHIVE_PASSWORD_LABEL, self->localizer_(Text::password));
            SetDlgItemTextW(dialog, IDC_ARCHIVE_COLLISION_LABEL, self->localizer_(Text::existingItems));
            SetDlgItemTextW(dialog, IDOK, self->localizer_(Text::ok));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::cancel));
            for (const wchar_t* format : {L"7z", L"ZIP", L"TAR"})
                SendDlgItemMessageW(dialog, IDC_ARCHIVE_FORMAT, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(format));
            for (Text collision : {Text::renameExisting, Text::overwriteExisting, Text::skipExisting})
                SendDlgItemMessageW(dialog, IDC_ARCHIVE_COLLISION, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(self->localizer_(collision)));
            SendDlgItemMessageW(dialog, IDC_ARCHIVE_FORMAT, CB_SETCURSEL,
                static_cast<WPARAM>(self->archiveOperationChoice_), 0);
            SendDlgItemMessageW(dialog, IDC_ARCHIVE_COLLISION, CB_SETCURSEL,
                static_cast<WPARAM>(self->archiveCollisionChoice_), 0);
            SetDlgItemTextW(dialog, IDC_ARCHIVE_NAME, self->archiveName_.c_str());
            SendDlgItemMessageW(dialog, IDC_ARCHIVE_NAME, EM_SETLIMITTEXT, core::maxFilenameLength, 0);
            SendDlgItemMessageW(dialog, IDC_ARCHIVE_PASSWORD, EM_SETLIMITTEXT,
                core::maxArchivePasswordCharacters, 0);
            if (!self->archiveCreating_)
            {
                EnableWindow(GetDlgItem(dialog, IDC_ARCHIVE_FORMAT), FALSE);
                EnableWindow(GetDlgItem(dialog, IDC_ARCHIVE_FORMAT_LABEL), FALSE);
            }
            SetFocus(GetDlgItem(dialog, IDC_ARCHIVE_NAME));
            return FALSE;
        }
        if (self == nullptr || message != WM_COMMAND) return FALSE;
        if (LOWORD(wParam) == IDC_ARCHIVE_FORMAT && HIWORD(wParam) == CBN_SELCHANGE &&
            self->archiveCreating_)
        {
            const LRESULT selected = SendDlgItemMessageW(dialog, IDC_ARCHIVE_FORMAT, CB_GETCURSEL, 0, 0);
            const bool tar = selected == static_cast<LRESULT>(core::ArchiveOperation::createTar);
            EnableWindow(GetDlgItem(dialog, IDC_ARCHIVE_PASSWORD), tar ? FALSE : TRUE);
            if (tar) SetDlgItemTextW(dialog, IDC_ARCHIVE_PASSWORD, L"");
            std::wstring name = windowText(GetDlgItem(dialog, IDC_ARCHIVE_NAME));
            const std::size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos) name.resize(dot);
            const wchar_t* extension = selected == 1 ? L".zip" : selected == 2 ? L".tar" : L".7z";
            name += extension;
            SetDlgItemTextW(dialog, IDC_ARCHIVE_NAME, name.c_str());
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            std::wstring name = windowText(GetDlgItem(dialog, IDC_ARCHIVE_NAME));
            const std::wstring password = windowText(GetDlgItem(dialog, IDC_ARCHIVE_PASSWORD));
            const LRESULT format = self->archiveCreating_ ?
                SendDlgItemMessageW(dialog, IDC_ARCHIVE_FORMAT, CB_GETCURSEL, 0, 0) :
                static_cast<LRESULT>(core::ArchiveOperation::extract);
            const LRESULT collision = SendDlgItemMessageW(dialog, IDC_ARCHIVE_COLLISION, CB_GETCURSEL, 0, 0);
            if (format < 0 || format >= static_cast<LRESULT>(core::ArchiveOperation::count) ||
                collision < 0 || collision >= static_cast<LRESULT>(core::ArchiveCollision::count) ||
                !core::validWindowsFilename(name) ||
                !core::validArchivePassword(static_cast<core::ArchiveOperation>(format),
                    toUtf16(password)))
            {
                MessageBoxW(dialog, self->localizer_(Text::archiveInvalid), self->localizer_(Text::title),
                    MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (self->archiveCreating_)
            {
                const wchar_t* extension = format == 1 ? L".zip" : format == 2 ? L".tar" : L".7z";
                const std::size_t dot = name.find_last_of(L'.');
                if (dot != std::wstring::npos) name.resize(dot);
                name += extension;
            }
            self->archiveName_ = std::move(name);
            self->archivePassword_ = password;
            self->archiveOperationChoice_ = static_cast<core::ArchiveOperation>(format);
            self->archiveCollisionChoice_ = static_cast<core::ArchiveCollision>(collision);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK AppWindow::shelfProcedure(HWND dialog, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            if (self == nullptr) return FALSE;
            SetWindowTextW(dialog, self->localizer_(Text::shelf));
            SetDlgItemTextW(dialog, IDC_SHELF_REMOVE,
                self->localizer_(Text::removeShelfItem));
            SetDlgItemTextW(dialog, IDC_SHELF_UP, self->localizer_(Text::moveUp));
            SetDlgItemTextW(dialog, IDC_SHELF_DOWN, self->localizer_(Text::moveDown));
            SetDlgItemTextW(dialog, IDC_SHELF_PASTE, self->localizer_(Text::shelfPaste));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::close));
            setAccessibleName(GetDlgItem(dialog, IDC_SHELF_LIST),
                self->localizer_(Text::shelf));
            self->shelfDialogCursor_ = 0;
            self->shelfDialogLoading_ = true;
            self->updateShelfDialogButtons(dialog);
            if (!PostMessageW(dialog, shelfLoadMessage, 0, 0))
            {
                self->shelfDialogLoading_ = false;
                self->updateShelfDialogButtons(dialog);
            }
            return TRUE;
        }
        if (self == nullptr) return FALSE;
        if (message == shelfLoadMessage)
        {
            self->loadShelfDialogBatch(dialog);
            return TRUE;
        }
        if (message == WM_DESTROY)
        {
            self->shelfDialogLoading_ = false;
            self->shelfDialogCursor_ = 0;
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wParam))
        {
        case IDC_SHELF_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE)
                self->updateShelfDialogButtons(dialog);
            return TRUE;
        case IDC_SHELF_REMOVE:
        {
            if (self->shelfDialogLoading_) return TRUE;
            const LRESULT selected = SendDlgItemMessageW(dialog, IDC_SHELF_LIST,
                LB_GETCURSEL, 0, 0);
            if (selected == LB_ERR || static_cast<std::size_t>(selected) >=
                    self->shelfOrder_.size())
                return TRUE;
            self->shelfOrder_.erase(self->shelfOrder_.begin() + selected);
            self->shelfCount_ = static_cast<DWORD>(self->shelfOrder_.size());
            SendDlgItemMessageW(dialog, IDC_SHELF_LIST, LB_DELETESTRING, selected, 0);
            if (!self->shelfOrder_.empty())
            {
                const std::size_t replacement = std::min<std::size_t>(
                    static_cast<std::size_t>(selected), self->shelfOrder_.size() - 1);
                SendDlgItemMessageW(dialog, IDC_SHELF_LIST, LB_SETCURSEL, replacement, 0);
            }
            self->updateShelfDialogButtons(dialog);
            return TRUE;
        }
        case IDC_SHELF_UP:
            self->moveShelfDialogItem(dialog, -1);
            return TRUE;
        case IDC_SHELF_DOWN:
            self->moveShelfDialogItem(dialog, 1);
            return TRUE;
        case IDC_SHELF_PASTE:
            if (!self->shelfDialogLoading_ && !self->shelfOrder_.empty())
                EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK AppWindow::ftpProcedure(HWND dialog, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<AppWindow*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            if (self == nullptr) return FALSE;
            self->ftpDialog_ = dialog;
            SetWindowTextW(dialog, self->localizer_(Text::ftpManager));
            SetDlgItemTextW(dialog, IDC_FTP_URL_LABEL, self->localizer_(Text::ftpServerUrl));
            SetDlgItemTextW(dialog, IDC_FTP_USERNAME_LABEL, self->localizer_(Text::ftpUsername));
            SetDlgItemTextW(dialog, IDC_FTP_PASSWORD_LABEL, self->localizer_(Text::password));
            SetDlgItemTextW(dialog, IDC_FTP_REQUIRE_TLS, self->localizer_(Text::ftpRequireTls));
            SetDlgItemTextW(dialog, IDC_FTP_CONNECT, self->localizer_(Text::ftpConnect));
            SetDlgItemTextW(dialog, IDC_FTP_UP, self->localizer_(Text::up));
            SetDlgItemTextW(dialog, IDC_FTP_REFRESH, self->localizer_(Text::refresh));
            SetDlgItemTextW(dialog, IDC_FTP_OPEN, self->localizer_(Text::ftpOpenFolder));
            SetDlgItemTextW(dialog, IDC_FTP_DOWNLOAD, self->localizer_(Text::ftpDownloadHere));
            SetDlgItemTextW(dialog, IDC_FTP_UPLOAD, self->localizer_(Text::ftpUploadFile));
            SetDlgItemTextW(dialog, IDC_FTP_NEW_FOLDER, self->localizer_(Text::newFolder));
            SetDlgItemTextW(dialog, IDC_FTP_DELETE_FILE, self->localizer_(Text::ftpDeleteFile));
            SetDlgItemTextW(dialog, IDC_FTP_DELETE_FOLDER, self->localizer_(Text::ftpDeleteFolder));
            SetDlgItemTextW(dialog, IDC_FTP_CANCEL_TRANSFER,
                self->localizer_(Text::ftpCancelTransfer));
            SetDlgItemTextW(dialog, IDCANCEL, self->localizer_(Text::close));
            SetDlgItemTextW(dialog, IDC_FTP_URL, self->ftpUrl_.c_str());
            SetDlgItemTextW(dialog, IDC_FTP_USERNAME, self->ftpUsername_.c_str());
            SetDlgItemTextW(dialog, IDC_FTP_PASSWORD, self->ftpPassword_.c_str());
            CheckDlgButton(dialog, IDC_FTP_REQUIRE_TLS,
                self->ftpRequireTls_ ? BST_CHECKED : BST_UNCHECKED);
            SendDlgItemMessageW(dialog, IDC_FTP_URL, EM_SETLIMITTEXT,
                core::maxFtpUrlCharacters, 0);
            SendDlgItemMessageW(dialog, IDC_FTP_USERNAME, EM_SETLIMITTEXT,
                core::maxFtpUsernameCharacters, 0);
            SendDlgItemMessageW(dialog, IDC_FTP_PASSWORD, EM_SETLIMITTEXT,
                core::maxFtpPasswordCharacters, 0);
            setAccessibleName(GetDlgItem(dialog, IDC_FTP_LIST),
                self->localizer_(Text::ftpManager));
            self->ftpDialogLoading_ = false;
            self->updateFtpDialogButtons(dialog);
            return TRUE;
        }
        if (self == nullptr) return FALSE;
        if (message == ftpLoadMessage)
        {
            self->loadFtpDialogBatch(dialog);
            return TRUE;
        }
        if (message == WM_CLOSE)
        {
            SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
            return TRUE;
        }
        if (message == WM_DESTROY)
        {
            if (self->ftpDialog_ == dialog) self->ftpDialog_ = nullptr;
            self->ftpDialogLoading_ = false;
            self->ftpListingCursor_.cancel();
            self->ftpListing_.clear();
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        const WORD command = LOWORD(wParam);
        if (command == IDC_FTP_LIST && HIWORD(wParam) == LBN_SELCHANGE)
        {
            self->updateFtpDialogButtons(dialog);
            return TRUE;
        }
        if (command == IDC_FTP_LIST && HIWORD(wParam) == LBN_DBLCLK)
        {
            SendMessageW(dialog, WM_COMMAND, IDC_FTP_OPEN, 0);
            return TRUE;
        }
        if (command == IDCANCEL)
        {
            if (self->ftpWorkerActive_ && self->ftpCancelEvent_ != nullptr)
                SetEvent(self->ftpCancelEvent_);
            SetDlgItemTextW(dialog, IDC_FTP_PASSWORD, L"");
            self->clearFtpCredentials();
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (command == IDC_FTP_CANCEL_TRANSFER)
        {
            if (self->ftpWorkerActive_ && self->ftpCancelEvent_ != nullptr)
            {
                SetEvent(self->ftpCancelEvent_);
                EnableWindow(GetDlgItem(dialog, IDC_FTP_CANCEL_TRANSFER), FALSE);
            }
            return TRUE;
        }
        if (self->ftpWorkerActive_ || self->ftpDialogLoading_) return TRUE;

        if (command == IDC_FTP_CONNECT)
        {
            const std::wstring url = normalizeFtpUrl(windowText(GetDlgItem(dialog, IDC_FTP_URL)));
            std::wstring username = windowText(GetDlgItem(dialog, IDC_FTP_USERNAME));
            std::wstring password = windowText(GetDlgItem(dialog, IDC_FTP_PASSWORD));
            const bool requireTls = IsDlgButtonChecked(dialog,
                IDC_FTP_REQUIRE_TLS) == BST_CHECKED;
            core::FtpRequest validation;
            validation.url = toUtf16(url);
            validation.username = toUtf16(username);
            validation.password = toUtf16(password);
            const bool valid = core::validFtpRequest(validation);
            scrubFtpCredentials(validation);
            if (!valid)
            {
                if (!password.empty())
                    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
                MessageBoxW(dialog, self->localizer_(Text::ftpInvalid),
                    self->localizer_(Text::title), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (!requireTls && validation.url.starts_with(u"ftp://") &&
                MessageBoxW(dialog, self->localizer_(Text::ftpPlainWarning),
                    self->localizer_(Text::title), MB_YESNO | MB_ICONWARNING |
                    MB_DEFBUTTON2) != IDYES)
            {
                if (!password.empty())
                    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
                return TRUE;
            }
            self->clearFtpCredentials();
            self->ftpUsername_ = std::move(username);
            self->ftpPassword_ = std::move(password);
            self->ftpRequireTls_ = requireTls;
            self->ftpConnected_ = false;
            SetDlgItemTextW(dialog, IDC_FTP_URL, url.c_str());
            self->startFtpList(url);
            return TRUE;
        }
        if (command == IDC_FTP_REFRESH)
        {
            self->startFtpList(self->ftpUrl_);
            return TRUE;
        }
        if (command == IDC_FTP_UP)
        {
            const std::u16string parent = core::parentFtpDirectoryUrl(toUtf16(self->ftpUrl_));
            if (!parent.empty())
                self->startFtpList(std::wstring(reinterpret_cast<const wchar_t*>(parent.data()),
                    parent.size()));
            return TRUE;
        }

        const std::wstring remoteName = self->selectedFtpName(dialog);
        if (command == IDC_FTP_OPEN)
        {
            const std::string name = wideToUtf8(remoteName);
            const std::string encoded = core::percentEncodeFtpSegment(name);
            if (remoteName.empty() || encoded.empty()) return TRUE;
            self->startFtpList(self->ftpUrl_ +
                std::wstring(encoded.begin(), encoded.end()) + L"/");
            return TRUE;
        }
        core::FtpRequest request;
        FtpCredentialGuard credentialGuard{request};
        request.requireTls = self->ftpRequireTls_;
        request.url = toUtf16(self->ftpUrl_);
        request.username = toUtf16(self->ftpUsername_);
        request.password = toUtf16(self->ftpPassword_);
        request.remoteName = toUtf16(remoteName);
        if (command == IDC_FTP_DOWNLOAD)
        {
            ExplorerBrowserHost* browser = self->activeBrowser();
            const std::wstring folder = browser == nullptr ? std::wstring{} : browser->filesystemPath();
            if (folder.empty() || !core::validWindowsFilename(remoteName))
            {
                MessageBoxW(dialog, self->localizer_(Text::invalidName),
                    self->localizer_(Text::title), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            request.operation = core::FtpOperation::download;
            request.localPath = toUtf16(folder + L"\\" + remoteName);
        }
        else if (command == IDC_FTP_UPLOAD)
        {
            const std::wstring path = chooseUploadFile(dialog);
            if (path.empty()) return TRUE;
            const wchar_t* leaf = PathFindFileNameW(path.c_str());
            if (leaf == nullptr || !core::validFtpRemoteName(std::wstring_view(leaf)))
            {
                MessageBoxW(dialog, self->localizer_(Text::invalidName),
                    self->localizer_(Text::title), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            request.operation = core::FtpOperation::upload;
            request.localPath = toUtf16(path);
            request.remoteName = toUtf16(leaf);
        }
        else if (command == IDC_FTP_NEW_FOLDER)
        {
            if (!self->promptForName(Text::newFolder, Text::invalidName, false,
                    false, false, dialog))
                return TRUE;
            request.operation = core::FtpOperation::makeDirectory;
            request.remoteName = toUtf16(self->nameInput_);
        }
        else if (command == IDC_FTP_DELETE_FILE || command == IDC_FTP_DELETE_FOLDER)
        {
            if (remoteName.empty()) return TRUE;
            std::wstring prompt = command == IDC_FTP_DELETE_FILE ?
                self->localizer_(Text::ftpDeleteFile) : self->localizer_(Text::ftpDeleteFolder);
            prompt += L"?\r\n\r\n" + remoteName;
            if (MessageBoxW(dialog, prompt.c_str(), self->localizer_(Text::title),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return TRUE;
            request.operation = command == IDC_FTP_DELETE_FILE ?
                core::FtpOperation::deleteFile : core::FtpOperation::deleteDirectory;
        }
        else
            return FALSE;
        (void)self->startFtpWorker(std::move(request));
        return TRUE;
    }

    LRESULT AppWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            dpi_ = GetDpiForWindow(window_);
            uiFont_ = xp::createUiFont(dpi_);
            createChildren();
            applySettings();
            return 0;

        case WM_SIZE:
            clientWidth_ = LOWORD(lParam);
            clientHeight_ = HIWORD(lParam);
            layoutChildren(clientWidth_, clientHeight_);
            return 0;

        case WM_DPICHANGED:
        {
            dpi_ = HIWORD(wParam);
            const auto suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            HFONT replacement = xp::createUiFont(dpi_);
            for (HWND child : {backButton_, forwardButton_, upButton_, refreshButton_, newFolderButton_,
                     viewButton_, addressLabel_, addressEdit_, goButton_, searchEdit_, placesHeader_,
                     placesList_, tabControl_, statusBar_, textPreviewHeader_, textPreviewEdit_, tagChip_,
                     gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
            {
                setFont(child, replacement);
            }
            DeleteObject(uiFont_);
            uiFont_ = replacement;
            layoutChildren(clientWidth_, clientHeight_);
            return 0;
        }

        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            InvalidateRect(window_, nullptr, TRUE);
            InvalidateRect(topBand_, nullptr, TRUE);
            InvalidateRect(placesHeader_, nullptr, TRUE);
            InvalidateRect(placesList_, nullptr, TRUE);
            return 0;

        case WM_ACTIVATEAPP:
            if (wParam != 0) scheduleGitStatusRefresh();
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) >= CommandId::newTab)
            {
                dispatchCommand(LOWORD(wParam));
                return 0;
            }
            switch (LOWORD(wParam))
            {
            case ControlId::back:
                dispatchCommand(CommandId::goBack);
                return 0;
            case ControlId::forward:
                dispatchCommand(CommandId::goForward);
                return 0;
            case ControlId::up:
                dispatchCommand(CommandId::goUp);
                return 0;
            case ControlId::refreshButton:
                dispatchCommand(CommandId::refresh);
                return 0;
            case ControlId::newFolderButton:
                dispatchCommand(CommandId::newFolder);
                return 0;
            case ControlId::viewButton:
            {
                HMENU menu = CreatePopupMenu();
                appendMenuItem(menu, CommandId::viewDetails, localizer_(Text::details));
                appendMenuItem(menu, CommandId::viewList, localizer_(Text::list));
                appendMenuItem(menu, CommandId::viewSmall, localizer_(Text::smallIcons));
                appendMenuItem(menu, CommandId::viewMedium, localizer_(Text::mediumIcons));
                appendMenuItem(menu, CommandId::viewLarge, localizer_(Text::largeIcons));
                appendMenuItem(menu, CommandId::viewExtraLarge, localizer_(Text::extraLargeIcons));
                appendMenuItem(menu, CommandId::viewTiles, localizer_(Text::tiles));
                appendMenuItem(menu, CommandId::viewContent, localizer_(Text::content));
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                appendMenuItem(menu, CommandId::showHiddenItems,
                    localizer_(Text::showHiddenItems));
                appendMenuItem(menu, CommandId::showFileExtensions,
                    localizer_(Text::showFileExtensions));
                SHELLSTATE shellState{};
                SHGetSetSettings(&shellState, SSF_SHOWALLOBJECTS | SSF_SHOWEXTENSIONS, FALSE);
                CheckMenuItem(menu, CommandId::showHiddenItems, MF_BYCOMMAND |
                    (shellState.fShowAllObjects ? MF_CHECKED : MF_UNCHECKED));
                CheckMenuItem(menu, CommandId::showFileExtensions, MF_BYCOMMAND |
                    (shellState.fShowExtensions ? MF_CHECKED : MF_UNCHECKED));
                RECT bounds{};
                GetWindowRect(viewButton_, &bounds);
                const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    bounds.left, bounds.bottom, 0, window_, nullptr);
                DestroyMenu(menu);
                if (command != 0)
                {
                    dispatchCommand(static_cast<int>(command));
                }
                return 0;
            }
            case ControlId::go:
                browseAddress();
                return 0;
            case ControlId::places:
                if (HIWORD(wParam) == LBN_DBLCLK)
                {
                    openPlace(static_cast<int>(SendMessageW(placesList_, LB_GETCURSEL, 0, 0)));
                }
                return 0;
            case ControlId::gitCancel:
                if (shellSnapshotActive_)
                {
                    finishShellOperationSnapshot(ERROR_CANCELLED);
                }
                else if (tagResultsActive_)
                {
                    finishTagResultMaterialization(ERROR_CANCELLED);
                }
                else if (tagSearchCancelEvent_ != nullptr && !tagSearchPath_.empty())
                {
                    SetEvent(tagSearchCancelEvent_);
                    EnableWindow(gitCancelButton_, FALSE);
                }
                else if (gitWorkerActive_ && gitCancelEvent_ != nullptr)
                {
                    SetEvent(gitCancelEvent_);
                    EnableWindow(gitCancelButton_, FALSE);
                }
                else if (archiveWorkerActive_ && archiveCancelEvent_ != nullptr)
                {
                    SetEvent(archiveCancelEvent_);
                    EnableWindow(gitCancelButton_, FALSE);
                }
                else
                {
                    gitPaneVisible_ = false;
                    ShowWindow(gitPanelHeader_, SW_HIDE);
                    ShowWindow(gitProgress_, SW_HIDE);
                    ShowWindow(gitOutput_, SW_HIDE);
                    ShowWindow(gitCancelButton_, SW_HIDE);
                    layoutChildren(clientWidth_, clientHeight_);
                }
                return 0;
            default:
                break;
            }
            break;

        case addressEnterMessage:
            browseAddress();
            return 0;

        case searchEnterMessage:
            runSearch();
            return 0;

        case WM_TIMER:
            if (wParam == gitStatusTimerId)
            {
                KillTimer(window_, gitStatusTimerId);
                if (!settings_.enabled(core::enableGit) || activeBrowser() == nullptr ||
                    activeBrowser()->isSearch() || activeBrowser()->filesystemPath().empty())
                    return 0;
                if (backgroundTaskActive())
                {
                    (void)SetTimer(window_, gitStatusTimerId,
                        gitStatusBusyRetryMilliseconds, nullptr);
                    return 0;
                }
                runGit(core::GitOperation::status, Text::gitStatus, true);
                return 0;
            }
            if (wParam == previewTimerId)
            {
                KillTimer(window_, previewTimerId);
                startTextPreviewWorker();
                return 0;
            }
            break;

        case textPreviewCompleteMessage:
        {
            const auto generation = static_cast<std::uint32_t>(wParam);
            if (!previewWorkerActive_ || generation != previewWorkerGeneration_)
            {
                return 0;
            }
            DWORD failure = static_cast<DWORD>(lParam);
            std::wstring textValue;
            HANDLE file = INVALID_HANDLE_VALUE;
            if (failure == ERROR_SUCCESS)
            {
                file = CreateFileW(previewResultPath_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) failure = GetLastError();
            }
            LARGE_INTEGER bytes{};
            if (failure == ERROR_SUCCESS && (!GetFileSizeEx(file, &bytes) || bytes.QuadPart < 2 ||
                bytes.QuadPart > static_cast<LONGLONG>(
                    (core::maxTextPreviewCharacters + 128) * sizeof(wchar_t)) ||
                bytes.QuadPart % sizeof(wchar_t) != 0))
                failure = ERROR_INVALID_DATA;
            if (failure == ERROR_SUCCESS)
            {
                textValue.resize(static_cast<std::size_t>(bytes.QuadPart / sizeof(wchar_t)));
                DWORD read{};
                if (!ReadFile(file, textValue.data(), static_cast<DWORD>(bytes.QuadPart), &read, nullptr) ||
                    read != static_cast<DWORD>(bytes.QuadPart) || textValue.back() != L'\0')
                    failure = ERROR_INVALID_DATA;
                else
                    textValue.pop_back();
            }
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (!previewResultPath_.empty()) DeleteFileW(previewResultPath_.c_str());
            previewResultPath_.clear();
            previewWorkerActive_ = false;
            if (textPreviewVisible_ && generation == previewGeneration_)
            {
                SetWindowTextW(textPreviewEdit_, failure == ERROR_SUCCESS ? textValue.c_str() :
                    localizer_(Text::previewUnavailable));
            }
            else if (textPreviewVisible_ && generation != previewGeneration_)
            {
                startTextPreviewWorker();
            }
            return 0;
        }

        case previewPopupCompleteMessage:
            completePreviewPopup(wParam != 0);
            return 0;

        case gitCompleteMessage:
            beginGitResultRead(static_cast<std::uint32_t>(wParam), static_cast<DWORD>(lParam));
            return 0;

        case gitResultReadMessage:
            processGitResultRead(static_cast<std::uint32_t>(wParam));
            return 0;

        case tabRetireMessage:
            processRetiredTab();
            return 0;

        case archiveProgressMessage:
            if (archiveWorkerActive_ && static_cast<std::uint32_t>(wParam) == archiveWorkerGeneration_ &&
                lParam >= 0 && lParam <= 100)
            {
                SendMessageW(gitProgress_, PBM_SETMARQUEE, FALSE, 0);
                const LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
                SetWindowLongPtrW(gitProgress_, GWL_STYLE,
                    style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
                SendMessageW(gitProgress_, PBM_SETPOS, static_cast<WPARAM>(lParam), 0);
                const std::wstring title = std::wstring(localizer_(gitPanelTitleText_)) + L" — " +
                    std::to_wstring(lParam) + L"%";
                SetWindowTextW(gitPanelHeader_, title.c_str());
            }
            return 0;

        case shellSnapshotMessage:
            processShellOperationSnapshot(static_cast<std::uint32_t>(wParam));
            return 0;

        case archiveCompleteMessage:
        {
            const auto generation = static_cast<std::uint32_t>(wParam);
            if (!archiveWorkerActive_ || generation != archiveWorkerGeneration_) return 0;
            std::wstring output;
            const bool read = !archiveResultPath_.empty() &&
                readBoundedUtf8File(archiveResultPath_, output);
            if (!archiveResultPath_.empty()) DeleteFileW(archiveResultPath_.c_str());
            archiveResultPath_.clear();
            if (archiveCancelEvent_ != nullptr)
            {
                CloseHandle(archiveCancelEvent_);
                archiveCancelEvent_ = nullptr;
            }
            if (archiveRequestMapping_ != nullptr)
            {
                CloseHandle(archiveRequestMapping_);
                archiveRequestMapping_ = nullptr;
            }
            if (!archivePassword_.empty())
                SecureZeroMemory(archivePassword_.data(), archivePassword_.size() * sizeof(wchar_t));
            archivePassword_.clear();
            archiveWorkerActive_ = false;
            ShowWindow(gitProgress_, SW_HIDE);
            EnableWindow(gitCancelButton_, TRUE);
            SetWindowTextW(gitCancelButton_, localizer_(Text::close));
            const DWORD result = static_cast<DWORD>(lParam);
            const bool clearCompletedShelf = result == ERROR_SUCCESS &&
                clearShelfOnOperationSuccess_;
            clearShelfOnOperationSuccess_ = false;
            if (clearCompletedShelf) clearShelf();
            std::wstring summary;
            if (result == ERROR_SUCCESS)
            {
                summary = localizer_(Text::gitCompleted);
            }
            else if (result == ERROR_CANCELLED)
                summary = localizer_(Text::gitCanceled);
            else
                summary = std::wstring(localizer_(Text::gitFailed)) + std::to_wstring(result);
            if (!read) output.clear();
            boundTaskOutput(output);
            if (!output.empty()) summary += L"\r\n\r\n" + output;
            SetWindowTextW(gitPanelHeader_, localizer_(gitPanelTitleText_));
            SetWindowTextW(gitOutput_, summary.c_str());
            if (const auto browser = activeBrowser(); browser != nullptr) browser->refresh();
            updateStatus();
            layoutChildren(clientWidth_, clientHeight_);
            return 0;
        }

        case ftpCompleteMessage:
            if (ftpWorkerActive_ &&
                static_cast<std::uintptr_t>(wParam) == ftpWorkerToken_)
                beginFtpResultRead(static_cast<DWORD>(lParam));
            return 0;

        case ftpResultReadMessage:
            processFtpResultRead(static_cast<std::uintptr_t>(wParam));
            return 0;

        case tagSearchCompleteMessage:
            if (tagSearchWorkerToken_ != 0 &&
                static_cast<std::uintptr_t>(wParam) == tagSearchWorkerToken_)
            {
                tagSearchWorkerToken_ = 0;
                beginTagResultMaterialization(static_cast<DWORD>(lParam));
            }
            return 0;

        case tagResultBatchMessage:
            processTagResultMaterialization(static_cast<std::uint32_t>(wParam));
            return 0;

        case shellNavigationMessage:
        {
            auto* browser = reinterpret_cast<ExplorerBrowserHost*>(wParam);
            const auto [tabIndex, paneIndex] = findBrowser(browser);
            if (tabIndex == noTab) return 0;
            browser->notificationDelivered(shellNavigationMessage);
            const bool searchNavigationFailed = browser->consumeSearchNavigationFailure();
            std::wstring fallbackRoot;
            std::wstring fallbackQuery;
            if (paneIndex >= 0)
            {
                if (searchNavigationFailed)
                {
                    fallbackRoot = std::move(tabs_[tabIndex].pendingSearchRoot[paneIndex]);
                    fallbackQuery = std::move(tabs_[tabIndex].pendingSearchQuery[paneIndex]);
                }
                else
                {
                    tabs_[tabIndex].pendingSearchRoot[paneIndex].clear();
                    tabs_[tabIndex].pendingSearchQuery[paneIndex].clear();
                }
            }
            if (tabIndex == activeTab_ && paneIndex >= 0)
            {
                tabs_[tabIndex].activePane = paneIndex;
            }
            updateTab(browser);
            if (searchNavigationFailed && tabIndex == activeTab_ &&
                paneIndex == tabs_[tabIndex].activePane)
            {
                if (!fallbackRoot.empty() && !fallbackQuery.empty())
                    startFallbackSearch(std::move(fallbackRoot), std::move(fallbackQuery), browser);
                else
                    showError(L"Search", HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            }
            if (tabIndex == activeTab_ && paneIndex == tabs_[tabIndex].activePane &&
                !searchNavigationFailed)
                scheduleGitStatusRefresh();
            if (textPreviewVisible_ && tabIndex == activeTab_) scheduleTextPreview();
            if (!browser->isSearch() && !tagChipText_.empty())
            {
                tagChipText_.clear();
                currentTagColor_ = 0;
                updateTagChip();
            }
            return 0;
        }

        case shellSelectionMessage:
        {
            auto* browser = reinterpret_cast<ExplorerBrowserHost*>(wParam);
            const auto [tabIndex, paneIndex] = findBrowser(browser);
            if (tabIndex == noTab) return 0;
            browser->notificationDelivered(shellSelectionMessage);
            if (tabIndex == activeTab_ && paneIndex >= 0)
            {
                const bool paneChanged = tabs_[tabIndex].activePane != paneIndex;
                tabs_[tabIndex].activePane = paneIndex;
                if (paneChanged) updateChrome();
                else updateStatus();
                if (textPreviewVisible_) scheduleTextPreview();
                if (externalPreviewActive_) launchPreviewPopup(true);
            }
            return 0;
        }

        case WM_NOTIFY:
        {
            const auto header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom == tabControl_ && header->code == TCN_SELCHANGE)
            {
                const int selected = TabCtrl_GetCurSel(tabControl_);
                if (selected >= 0)
                {
                    activateTab(static_cast<std::size_t>(selected));
                }
                return 0;
            }
            break;
        }

        case WM_COPYDATA:
        {
            const auto data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data == nullptr || data->dwData != openLocationMessage || data->lpData == nullptr ||
                data->cbData < sizeof(wchar_t) || data->cbData > 32768 * sizeof(wchar_t) ||
                data->cbData % sizeof(wchar_t) != 0)
            {
                return FALSE;
            }
            const auto text = static_cast<const wchar_t*>(data->lpData);
            const std::size_t characters = data->cbData / sizeof(wchar_t);
            if (!core::validCopyDataPath(text, characters))
            {
                return FALSE;
            }
            addTab(std::wstring(text, characters - 1));
            ShowWindow(window_, SW_RESTORE);
            SetForegroundWindow(window_);
            return TRUE;
        }

        case WM_DRAWITEM:
        {
            const auto draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw->CtlID == ControlId::places)
            {
                xp::drawPlacesItem(*draw);
            }
            else
            {
                xp::drawButton(*draw, draw->CtlID == ControlId::go);
            }
            return TRUE;
        }

        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lParam) == tagChip_ && !xp::isHighContrast() &&
                core::validTagColor(currentTagColor_) && currentTagColor_ != 0)
            {
                static const std::array<COLORREF, 8> colors{
                    RGB(255, 255, 255), RGB(210, 52, 52), RGB(230, 121, 36), RGB(245, 200, 40),
                    RGB(42, 150, 75), RGB(43, 106, 191), RGB(126, 74, 173), RGB(105, 105, 105)};
                static const std::array<HBRUSH, 8> brushes{
                    CreateSolidBrush(colors[0]), CreateSolidBrush(colors[1]),
                    CreateSolidBrush(colors[2]), CreateSolidBrush(colors[3]),
                    CreateSolidBrush(colors[4]), CreateSolidBrush(colors[5]),
                    CreateSolidBrush(colors[6]), CreateSolidBrush(colors[7])};
                const COLORREF background = colors[currentTagColor_];
                SetTextColor(reinterpret_cast<HDC>(wParam), currentTagColor_ == 3 ?
                    RGB(0, 0, 0) : RGB(255, 255, 255));
                SetBkColor(reinterpret_cast<HDC>(wParam), background);
                return reinterpret_cast<LRESULT>(brushes[currentTagColor_]);
            }
            if (reinterpret_cast<HWND>(lParam) == placesHeader_)
            {
                if (xp::isHighContrast())
                {
                    SetTextColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOWTEXT));
                    SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
                }
                SetTextColor(reinterpret_cast<HDC>(wParam), RGB(255, 255, 255));
                SetBkColor(reinterpret_cast<HDC>(wParam), xp::paneHeader);
                static HBRUSH headerBrush = CreateSolidBrush(xp::paneHeader);
                return reinterpret_cast<LRESULT>(headerBrush);
            }
            if (xp::isHighContrast())
            {
                SetTextColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            SetBkColor(reinterpret_cast<HDC>(wParam), xp::toolbarBeige);
            static HBRUSH toolbarBrush = CreateSolidBrush(xp::toolbarBeige);
            return reinterpret_cast<LRESULT>(toolbarBrush);

        case WM_ERASEBKGND:
        {
            RECT bounds{};
            GetClientRect(window_, &bounds);
            if (xp::isHighContrast())
            {
                FillRect(reinterpret_cast<HDC>(wParam), &bounds, GetSysColorBrush(COLOR_WINDOW));
            }
            else
            {
                HBRUSH background = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
                SetDCBrushColor(reinterpret_cast<HDC>(wParam), xp::toolbarBeige);
                FillRect(reinterpret_cast<HDC>(wParam), &bounds, background);
            }
            return TRUE;
        }

        case WM_DESTROY:
            if (fullScreenMenu_ != nullptr)
            {
                DestroyMenu(fullScreenMenu_);
                fullScreenMenu_ = nullptr;
            }
            KillTimer(window_, previewTimerId);
            KillTimer(window_, gitStatusTimerId);
            shellSnapshotActive_ = false;
            shellSnapshotCursor_.cancel();
            shellSnapshotRequest_ = {};
            shellSnapshotClipboardText_.clear();
            shellSnapshotQuotesClipboardPaths_ = false;
            shellSnapshotPurpose_ = ShellSnapshotPurpose::shellOperation;
            shellSnapshotPathAction_ = PathSnapshotAction::none;
            shellSnapshotFileSystemPaths_.clear();
            shellSnapshotFileSystemCharacters_ = 0;
            shellSnapshotContext_.clear();
            shellSnapshotOrder_.clear();
            if (shellSnapshotItems_ != nullptr)
            {
                shellSnapshotItems_->Release();
                shellSnapshotItems_ = nullptr;
            }
            tagResultsActive_ = false;
            tagResultCursor_.cancel();
            if (tagResultsBrowser_ != nullptr)
            {
                tagResultsBrowser_->cancelResults();
                tagResultsBrowser_->Release();
                tagResultsBrowser_ = nullptr;
            }
            if (tagResultView_ != nullptr)
            {
                UnmapViewOfFile(tagResultView_);
                tagResultView_ = nullptr;
            }
            if (tagResultMapping_ != nullptr)
            {
                CloseHandle(tagResultMapping_);
                tagResultMapping_ = nullptr;
            }
            if (tagSearchCancelEvent_ != nullptr)
            {
                SetEvent(tagSearchCancelEvent_);
                CloseHandle(tagSearchCancelEvent_);
                tagSearchCancelEvent_ = nullptr;
            }
            if (tagSearchRequestMapping_ != nullptr)
            {
                CloseHandle(tagSearchRequestMapping_);
                tagSearchRequestMapping_ = nullptr;
            }
            if (tagSearchTargetBrowser_ != nullptr)
            {
                tagSearchTargetBrowser_->Release();
                tagSearchTargetBrowser_ = nullptr;
            }
            tagSearchWorkerToken_ = 0;
            if (!tagSearchPath_.empty())
            {
                DeleteFileW(tagSearchPath_.c_str());
                tagSearchPath_.clear();
            }
            if (gitWorkerActive_ && gitCancelEvent_ != nullptr) SetEvent(gitCancelEvent_);
            if (gitCancelEvent_ != nullptr)
            {
                CloseHandle(gitCancelEvent_);
                gitCancelEvent_ = nullptr;
            }
            gitResultReadActive_ = false;
            gitResultReadCursor_.cancel();
            gitResultBytes_.clear();
            if (gitResultReadFile_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(gitResultReadFile_);
                gitResultReadFile_ = INVALID_HANDLE_VALUE;
            }
            if (!gitResultPath_.empty())
            {
                DeleteFileW(gitResultPath_.c_str());
                gitResultPath_.clear();
            }
            if (archiveWorkerActive_ && archiveCancelEvent_ != nullptr) SetEvent(archiveCancelEvent_);
            if (archiveCancelEvent_ != nullptr)
            {
                CloseHandle(archiveCancelEvent_);
                archiveCancelEvent_ = nullptr;
            }
            if (archiveRequestMapping_ != nullptr)
            {
                CloseHandle(archiveRequestMapping_);
                archiveRequestMapping_ = nullptr;
            }
            if (ftpWorkerActive_ && ftpCancelEvent_ != nullptr) SetEvent(ftpCancelEvent_);
            if (ftpCancelEvent_ != nullptr)
            {
                CloseHandle(ftpCancelEvent_);
                ftpCancelEvent_ = nullptr;
            }
            if (ftpRequestMapping_ != nullptr)
            {
                CloseHandle(ftpRequestMapping_);
                ftpRequestMapping_ = nullptr;
            }
            ftpResultReadActive_ = false;
            ftpResultReadCursor_.cancel();
            ftpResultBytes_.clear();
            if (ftpResultReadFile_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(ftpResultReadFile_);
                ftpResultReadFile_ = INVALID_HANDLE_VALUE;
            }
            ftpWorkerToken_ = 0;
            if (!ftpResultPath_.empty())
            {
                DeleteFileW(ftpResultPath_.c_str());
                ftpResultPath_.clear();
            }
            ftpListingCursor_.cancel();
            ftpListing_.clear();
            clearFtpCredentials();
            if (!archivePassword_.empty())
                SecureZeroMemory(archivePassword_.data(), archivePassword_.size() * sizeof(wchar_t));
            if (!previewWorkerActive_ && !previewResultPath_.empty())
                DeleteFileW(previewResultPath_.c_str());
            previewPopupQueue_.clear();
            saveSession();
            releaseTabs();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    bool AppWindow::handleShortcut(const MSG& message)
    {
        if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN)
        {
            return false;
        }
        const std::uint32_t chord = shortcutChord(message.wParam);
        const bool control = (chord & core::shortcutControl) != 0;
        const bool shift = (chord & core::shortcutShift) != 0;
        const bool alt = (chord & core::shortcutAlt) != 0;
        const HWND focus = GetFocus();
        const bool editing = focus == addressEdit_ || focus == searchEdit_ || focus == textPreviewEdit_;

        int command{};
        for (std::size_t index = 0; index < settings_.shortcuts.size(); ++index)
        {
            if (chord == settings_.shortcuts[index])
            {
                command = shortcutCommands[index];
                break;
            }
        }
        if (command != 0)
        {
            dispatchCommand(command);
            return true;
        }

        if (chord == (core::shortcutControl | core::shortcutShift | L'T'))
            command = CommandId::reopenClosedTab;
        else if (chord == (core::shortcutControl | core::shortcutShift | VK_PRIOR))
            command = CommandId::moveTabLeft;
        else if (chord == (core::shortcutControl | core::shortcutShift | VK_NEXT))
            command = CommandId::moveTabRight;
        else if (chord == (core::shortcutControl | L'N')) command = CommandId::newWindow;
        else if (!editing && chord == (core::shortcutShift | VK_DELETE))
            command = CommandId::permanentDelete;
        else if (control && !alt && message.wParam == VK_TAB)
        {
            cycleTab(shift ? -1 : 1);
            return true;
        }
        else if (chord == VK_F6) command = CommandId::focusOtherPane;
        else if (chord == VK_F11) command = CommandId::fullScreen;
        else if (chord == (core::shortcutAlt | VK_LEFT)) command = CommandId::goBack;
        else if (chord == (core::shortcutAlt | VK_RIGHT)) command = CommandId::goForward;
        else if (chord == (core::shortcutAlt | VK_UP)) command = CommandId::goUp;
        else if (chord == (core::shortcutAlt | VK_HOME)) command = CommandId::goHome;
        else if (chord == VK_F5) command = CommandId::refresh;
        else if (!editing && !control && !alt && !shift && message.wParam == VK_SPACE &&
            settings_.enabled(core::enableQuickPreview))
            command = CommandId::togglePreviewPane;
        else if (!editing && chord == VK_F2) command = CommandId::rename;
        else if (!editing && chord == VK_DELETE) command = CommandId::recycleDelete;
        else if (!editing && chord == (core::shortcutControl | L'C')) command = CommandId::copy;
        else if (!editing && chord == (core::shortcutControl | L'X')) command = CommandId::cut;
        else if (!editing && chord == (core::shortcutControl | L'V')) command = CommandId::paste;
        else if (!editing && chord == (core::shortcutControl | L'A')) command = CommandId::selectAll;
        else if (!editing && chord == (core::shortcutControl | L'Z')) command = CommandId::undo;
        else if (!editing && chord == (core::shortcutControl | L'Y')) command = CommandId::redo;

        if (command == 0)
        {
            return false;
        }
        dispatchCommand(command);
        return true;
    }

    void AppWindow::createChildren()
    {
        topBand_ = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);

        auto createButton = [this](const wchar_t* text, int id)
        {
            return CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        };
        backButton_ = createButton(localizer_(Text::back), ControlId::back);
        forwardButton_ = createButton(localizer_(Text::forward), ControlId::forward);
        upButton_ = createButton(localizer_(Text::up), ControlId::up);
        refreshButton_ = createButton(localizer_(Text::refresh), ControlId::refreshButton);
        newFolderButton_ = createButton(localizer_(Text::newFolder), ControlId::newFolderButton);
        viewButton_ = createButton(localizer_(Text::view), ControlId::viewButton);

        addressLabel_ = CreateWindowExW(0, L"STATIC", localizer_(Text::address), WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        addressEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ControlId::address)), instance_, nullptr);
        SetWindowSubclass(addressEdit_, &AppWindow::editProcedure, 1, reinterpret_cast<DWORD_PTR>(window_));
        goButton_ = createButton(localizer_(Text::go), ControlId::go);
        searchEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ControlId::search)), instance_, nullptr);
        SendMessageW(searchEdit_, EM_SETLIMITTEXT, core::maxSearchQueryCharacters, 0);
        SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(localizer_(Text::searchHint)));
        SetWindowSubclass(searchEdit_, &AppWindow::editProcedure, 2, reinterpret_cast<DWORD_PTR>(window_));

        placesHeader_ = CreateWindowExW(0, L"STATIC", localizer_(Text::places),
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        placesList_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | WS_VSCROLL,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ControlId::places)), instance_, nullptr);
        tabControl_ = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_TOOLTIPS,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ControlId::tabs)), instance_, nullptr);
        SetWindowSubclass(tabControl_, &AppWindow::tabProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        statusBar_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        textPreviewHeader_ = CreateWindowExW(0, L"STATIC", localizer_(Text::quickPreview),
            WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        textPreviewEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        tagChip_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", nullptr,
            WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        gitPanelHeader_ = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        gitProgress_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | PBS_MARQUEE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        gitOutput_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        gitCancelButton_ = CreateWindowExW(0, L"BUTTON", localizer_(Text::cancel),
            WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ControlId::gitCancel)), instance_, nullptr);

        for (HWND child : {backButton_, forwardButton_, upButton_, refreshButton_, newFolderButton_,
                 viewButton_, addressLabel_, addressEdit_, goButton_, searchEdit_, placesHeader_,
                 placesList_, tabControl_, statusBar_, textPreviewHeader_, textPreviewEdit_, tagChip_,
                 gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
        {
            setFont(child, uiFont_);
        }
        ShowWindow(textPreviewHeader_, SW_HIDE);
        ShowWindow(textPreviewEdit_, SW_HIDE);
        ShowWindow(tagChip_, SW_HIDE);
        ShowWindow(gitPanelHeader_, SW_HIDE);
        ShowWindow(gitProgress_, SW_HIDE);
        ShowWindow(gitOutput_, SW_HIDE);
        ShowWindow(gitCancelButton_, SW_HIDE);
        setAccessibleName(placesList_, localizer_(Text::places));
        setAccessibleName(tabControl_, localizer_(Text::tabsLabel));
        setAccessibleName(addressEdit_, localizer_(Text::address));
        setAccessibleName(searchEdit_, localizer_(Text::searchCommand));
        setAccessibleName(statusBar_, localizer_(Text::statusLabel));
        setAccessibleName(textPreviewEdit_, localizer_(Text::quickPreview));
        setAccessibleName(gitOutput_, localizer_(Text::taskOutputLabel));
        populatePlaces();
    }

    void AppWindow::createMainMenu()
    {
        HMENU menuBar = CreateMenu();
        auto add = [this](HMENU menu, CommandId id, Text text, const wchar_t* shortcut = nullptr)
        {
            std::wstring label = localizer_(text);
            if (shortcut != nullptr)
            {
                label += shortcut;
            }
            appendMenuItem(menu, id, label.c_str());
        };
        auto addConfigured = [this, &add](HMENU menu, CommandId id, Text text, std::size_t index)
        {
            const std::wstring shortcut = L"\t" + formatShortcut(settings_.shortcuts[index]);
            add(menu, id, text, shortcut.c_str());
        };
        HMENU fileMenu = CreatePopupMenu();
        addConfigured(fileMenu, CommandId::newTab, Text::newTab, 0);
        add(fileMenu, CommandId::newWindow, Text::newWindow, L"\tCtrl+N");
        add(fileMenu, CommandId::duplicateTab, Text::duplicateTab);
        add(fileMenu, CommandId::openInNewTab, Text::openInNewTab);
        add(fileMenu, CommandId::openInNewWindow, Text::openInNewWindow);
        add(fileMenu, CommandId::reopenClosedTab, Text::reopenClosedTab, L"\tCtrl+Shift+T");
        addConfigured(fileMenu, CommandId::closeTab, Text::closeTab, 1);
        add(fileMenu, CommandId::closeOtherTabs, Text::closeOtherTabs);
        add(fileMenu, CommandId::closeTabsLeft, Text::closeTabsLeft);
        add(fileMenu, CommandId::closeTabsRight, Text::closeTabsRight);
        add(fileMenu, CommandId::closeAllTabs, Text::closeAllTabs);
        add(fileMenu, CommandId::moveTabLeft, Text::moveTabLeft, L"\tCtrl+Shift+PgUp");
        add(fileMenu, CommandId::moveTabRight, Text::moveTabRight, L"\tCtrl+Shift+PgDn");
        AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
        addConfigured(fileMenu, CommandId::newFolder, Text::newFolder, 5);
        add(fileMenu, CommandId::newFile, Text::newTextFile);
        add(fileMenu, CommandId::createShortcut, Text::createShortcut);
        add(fileMenu, CommandId::createLibrary, Text::createLibrary);
        add(fileMenu, CommandId::emptyRecycleBin, Text::emptyRecycleBin);
        add(fileMenu, CommandId::restoreAllRecycleBin, Text::restoreAllRecycleBin);
        AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
        add(fileMenu, CommandId::exitApp, Text::exitApp);

        HMENU editMenu = CreatePopupMenu();
        add(editMenu, CommandId::undo, Text::undo, L"\tCtrl+Z");
        add(editMenu, CommandId::redo, Text::redo, L"\tCtrl+Y");
        AppendMenuW(editMenu, MF_SEPARATOR, 0, nullptr);
        add(editMenu, CommandId::cut, Text::cut, L"\tCtrl+X");
        add(editMenu, CommandId::copy, Text::copy, L"\tCtrl+C");
        add(editMenu, CommandId::paste, Text::paste, L"\tCtrl+V");
        add(editMenu, CommandId::pasteShortcut, Text::pasteShortcut);
        add(editMenu, CommandId::pasteIntoFolder, Text::pasteIntoFolder);
        add(editMenu, CommandId::copyPath, Text::copyPath);
        add(editMenu, CommandId::copyPathQuoted, Text::copyPathQuoted);
        AppendMenuW(editMenu, MF_SEPARATOR, 0, nullptr);
        add(editMenu, CommandId::rename, Text::rename, L"\tF2");
        add(editMenu, CommandId::bulkRename, Text::bulkRename);
        add(editMenu, CommandId::folderFromSelection, Text::folderFromSelection);
        add(editMenu, CommandId::flattenFolder, Text::flattenFolder);
        add(editMenu, CommandId::recycleDelete, Text::deleteItem, L"\tDel");
        add(editMenu, CommandId::permanentDelete, Text::permanentDelete, L"\tShift+Del");
        add(editMenu, CommandId::properties, Text::properties);
        add(editMenu, CommandId::editShortcut, Text::editShortcut);
        add(editMenu, CommandId::editLibrary, Text::editLibrary);
        add(editMenu, CommandId::pinQuickAccess, Text::pinQuickAccess);
        add(editMenu, CommandId::unpinQuickAccess, Text::unpinQuickAccess);
        AppendMenuW(editMenu, MF_SEPARATOR, 0, nullptr);
        add(editMenu, CommandId::selectAll, Text::selectAll, L"\tCtrl+A");
        add(editMenu, CommandId::clearSelection, Text::clearSelection);
        add(editMenu, CommandId::invertSelection, Text::invertSelection);
        add(editMenu, CommandId::restoreRecycleBin, Text::restoreRecycleBin);
        add(editMenu, CommandId::restoreAllRecycleBin, Text::restoreAllRecycleBin);
        AppendMenuW(editMenu, MF_SEPARATOR, 0, nullptr);
        add(editMenu, CommandId::shelfCopy, Text::shelfCopy);
        add(editMenu, CommandId::shelfMove, Text::shelfMove);
        add(editMenu, CommandId::shelfPaste, Text::shelfPaste);
        add(editMenu, CommandId::shelfClear, Text::shelfClear);
        add(editMenu, CommandId::manageShelf, Text::manageShelf);
        AppendMenuW(editMenu, MF_SEPARATOR, 0, nullptr);
        add(editMenu, CommandId::editTags, Text::editTags);
        add(editMenu, CommandId::manageTagColor, Text::manageTagColor);

        HMENU viewMenu = CreatePopupMenu();
        add(viewMenu, CommandId::viewDetails, Text::details);
        add(viewMenu, CommandId::viewList, Text::list);
        add(viewMenu, CommandId::viewSmall, Text::smallIcons);
        add(viewMenu, CommandId::viewMedium, Text::mediumIcons);
        add(viewMenu, CommandId::viewLarge, Text::largeIcons);
        add(viewMenu, CommandId::viewExtraLarge, Text::extraLargeIcons);
        add(viewMenu, CommandId::viewTiles, Text::tiles);
        add(viewMenu, CommandId::viewContent, Text::content);
        add(viewMenu, CommandId::autoSizeColumns, Text::autoSizeColumns);
        AppendMenuW(viewMenu, MF_SEPARATOR, 0, nullptr);
        add(viewMenu, CommandId::togglePreviewPane, Text::quickPreview, L"\tSpace");
        add(viewMenu, CommandId::toggleDetailsPane, Text::detailsPane);
        add(viewMenu, CommandId::showHiddenItems, Text::showHiddenItems);
        add(viewMenu, CommandId::showFileExtensions, Text::showFileExtensions);
        SHELLSTATE shellState{};
        SHGetSetSettings(&shellState, SSF_SHOWALLOBJECTS | SSF_SHOWEXTENSIONS, FALSE);
        CheckMenuItem(viewMenu, CommandId::showHiddenItems, MF_BYCOMMAND |
            (shellState.fShowAllObjects ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(viewMenu, CommandId::showFileExtensions, MF_BYCOMMAND |
            (shellState.fShowExtensions ? MF_CHECKED : MF_UNCHECKED));
        AppendMenuW(viewMenu, MF_SEPARATOR, 0, nullptr);
        add(viewMenu, CommandId::splitVertical, Text::splitVertical);
        add(viewMenu, CommandId::splitHorizontal, Text::splitHorizontal);
        add(viewMenu, CommandId::openInOtherPane, Text::openInOtherPane);
        add(viewMenu, CommandId::openCurrentFolderOtherPane, Text::openCurrentFolderOtherPane);
        add(viewMenu, CommandId::focusOtherPane, Text::focusOtherPane, L"\tF6");
        add(viewMenu, CommandId::closePane, Text::closePane);
        add(viewMenu, CommandId::toggleSidebar, Text::toggleSidebar);
        add(viewMenu, CommandId::fullScreen, Text::fullScreen, L"\tF11");
        AppendMenuW(viewMenu, MF_SEPARATOR, 0, nullptr);
        add(viewMenu, CommandId::refresh, Text::refresh, L"\tF5");

        HMENU goMenu = CreatePopupMenu();
        add(goMenu, CommandId::goHome, Text::goHome, L"\tAlt+Home");
        add(goMenu, CommandId::goBack, Text::back, L"\tAlt+Left");
        add(goMenu, CommandId::goForward, Text::forward, L"\tAlt+Right");
        add(goMenu, CommandId::goUp, Text::up, L"\tAlt+Up");
        add(goMenu, CommandId::openFileLocation, Text::openFileLocation);
        AppendMenuW(goMenu, MF_SEPARATOR, 0, nullptr);
        addConfigured(goMenu, CommandId::focusAddress, Text::addressCommand, 2);
        addConfigured(goMenu, CommandId::focusSearch, Text::searchCommand, 3);
        add(goMenu, CommandId::filterByTag, Text::filterByTag);

        HMENU toolsMenu = CreatePopupMenu();
        add(toolsMenu, CommandId::settings, Text::settings);
        add(toolsMenu, CommandId::keyboardShortcuts, Text::keyboardShortcuts);
        addConfigured(toolsMenu, CommandId::commandPalette, Text::commandPalette, 4);
        AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
        add(toolsMenu, CommandId::compressArchive, Text::compress);
        add(toolsMenu, CommandId::extractArchive, Text::extract);
        AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
        add(toolsMenu, CommandId::mapNetworkDrive, Text::mapNetworkDrive);
        add(toolsMenu, CommandId::disconnectNetworkDrive, Text::disconnectNetworkDrive);
        add(toolsMenu, CommandId::ftpManager, Text::ftpManager);
        add(toolsMenu, CommandId::openTerminal, Text::openTerminal);
        add(toolsMenu, CommandId::openTerminalAdmin, Text::openTerminalAdmin);
        AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
        HMENU actionsMenu = CreatePopupMenu();
        add(actionsMenu, CommandId::playSelection, Text::playSelection);
        add(actionsMenu, CommandId::runAsAdministrator, Text::runAsAdministrator);
        add(actionsMenu, CommandId::runAsDifferentUser, Text::runAsDifferentUser);
        add(actionsMenu, CommandId::runWithPowerShell, Text::runWithPowerShell);
        add(actionsMenu, CommandId::rotateLeft, Text::rotateLeft);
        add(actionsMenu, CommandId::rotateRight, Text::rotateRight);
        add(actionsMenu, CommandId::installSelection, Text::installSelection);
        add(actionsMenu, CommandId::installCertificate, Text::installCertificate);
        add(actionsMenu, CommandId::setDesktopWallpaper, Text::setDesktopWallpaper);
        add(actionsMenu, CommandId::setDesktopSlideshow, Text::setDesktopSlideshow);
        add(actionsMenu, CommandId::openStorageSense, Text::openStorageSense);
        AppendMenuW(toolsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(actionsMenu),
            localizer_(Text::actionsMenu));
        AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
        add(toolsMenu, CommandId::hashSelection, Text::sha256);
        add(toolsMenu, CommandId::showAlternateStreams, Text::alternateStreams);
        add(toolsMenu, CommandId::editAlternateStream, Text::editAlternateStream);
        add(toolsMenu, CommandId::verifySignature, Text::verifySignature);
        AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
        add(toolsMenu, CommandId::gitInit, Text::gitInit);
        add(toolsMenu, CommandId::gitClone, Text::gitClone);
        add(toolsMenu, CommandId::gitCreateBranch, Text::gitCreateBranch);
        add(toolsMenu, CommandId::gitSwitchBranch, Text::gitSwitchBranch);
        add(toolsMenu, CommandId::gitStatus, Text::gitStatus);
        add(toolsMenu, CommandId::gitFetch, Text::gitFetch);
        add(toolsMenu, CommandId::gitPull, Text::gitPull);
        add(toolsMenu, CommandId::gitPush, Text::gitPush);
        add(toolsMenu, CommandId::gitSync, Text::gitSync);

        AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), localizer_(Text::fileMenu));
        AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(editMenu), localizer_(Text::editMenu));
        AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), localizer_(Text::viewMenu));
        AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(goMenu), localizer_(Text::goMenu));
        AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(toolsMenu), localizer_(Text::toolsMenu));
        SetMenu(window_, menuBar);
    }

    void AppWindow::populatePlaces()
    {
        SendMessageW(placesList_, LB_RESETCONTENT, 0, 0);
        places_.clear();
        auto add = [this](const wchar_t* label, REFKNOWNFOLDERID folderId)
        {
            SendMessageW(placesList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            places_.push_back(Place{.label = label, .folderId = folderId, .parsingName = {}});
        };
        auto addPath = [this](const wchar_t* label, const wchar_t* parsingName)
        {
            SendMessageW(placesList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            places_.push_back(Place{.label = label, .parsingName = parsingName});
        };
        addPath(localizer_(Text::quickAccess), L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}");
        add(localizer_(Text::desktop), FOLDERID_Desktop);
        add(localizer_(Text::documents), FOLDERID_Documents);
        add(localizer_(Text::downloads), FOLDERID_Downloads);
        add(localizer_(Text::pictures), FOLDERID_Pictures);
        add(localizer_(Text::music), FOLDERID_Music);
        add(localizer_(Text::videos), FOLDERID_Videos);
        add(localizer_(Text::thisPC), FOLDERID_ComputerFolder);
        add(localizer_(Text::libraries), FOLDERID_Libraries);
        add(localizer_(Text::network), FOLDERID_NetworkFolder);
        addPath(localizer_(Text::wsl), L"\\\\wsl.localhost");
        add(localizer_(Text::recycleBin), FOLDERID_RecycleBinFolder);
        SendMessageW(placesList_, LB_SETITEMHEIGHT, 0, scale(30, dpi_));
    }

    void AppWindow::layoutChildren(int width, int height)
    {
        if (statusBar_ == nullptr)
        {
            return;
        }
        SendMessageW(statusBar_, WM_SIZE, 0, 0);
        RECT statusBounds{};
        GetWindowRect(statusBar_, &statusBounds);
        const int statusHeight = statusBounds.bottom - statusBounds.top;
        const bool compact = settings_.enabled(core::compactToolbar);
        const int topHeight = scale(compact ? 64 : 76, dpi_);
        const int tabHeight = scale(29, dpi_);
        const bool showPlaces = settings_.enabled(core::showPlaces);
        const int placesWidth = showPlaces ? scale(194, dpi_) : 0;
        for (HWND control : {placesHeader_, placesList_})
        {
            if ((IsWindowVisible(control) != FALSE) != showPlaces)
                ShowWindow(control, showPlaces ? SW_SHOW : SW_HIDE);
        }
        const int margin = scale(6, dpi_);
        const int firstRowY = scale(compact ? 4 : 7, dpi_);
        const int buttonHeight = scale(compact ? 24 : 28, dpi_);
        const int secondRowY = scale(compact ? 34 : 43, dpi_);
        const int addressHeight = scale(compact ? 23 : 25, dpi_);
        const int searchWidth = std::min(scale(245, dpi_), std::max(scale(150, dpi_), width / 4));

        DeferredWindowLayout layout;
        layout.place(topBand_, 0, 0, width, topHeight);
        int buttonX = margin;
        const auto placeButton = [this, &layout, &buttonX, firstRowY, buttonHeight](HWND button,
            int logicalWidth, ToolbarMask mask)
        {
            const bool visible = (settings_.toolbarButtons & mask) != 0;
            if ((IsWindowVisible(button) != FALSE) != visible)
                ShowWindow(button, visible ? SW_SHOW : SW_HIDE);
            if (visible)
            {
                const int width = scale(logicalWidth, dpi_);
                layout.place(button, buttonX, firstRowY, width, buttonHeight);
                buttonX += width + scale(6, dpi_);
            }
        };
        placeButton(backButton_, 78, toolbarBack);
        placeButton(forwardButton_, 86, toolbarForward);
        placeButton(upButton_, 60, toolbarUp);
        placeButton(refreshButton_, 82, toolbarRefresh);
        placeButton(newFolderButton_, 106, toolbarNewFolder);
        placeButton(viewButton_, 72, toolbarView);
        layout.place(addressLabel_, margin, secondRowY, scale(55, dpi_), addressHeight);
        layout.place(searchEdit_, std::max(scale(300, dpi_), width - searchWidth - margin), secondRowY,
            searchWidth, addressHeight);
        layout.place(goButton_, std::max(scale(250, dpi_), width - searchWidth - scale(53, dpi_)), secondRowY,
            scale(45, dpi_), addressHeight);
        layout.place(addressEdit_, scale(64, dpi_), secondRowY,
            std::max(scale(120, dpi_), width - searchWidth - scale(123, dpi_)), addressHeight);

        const int contentBottom = std::max(topHeight, height - statusHeight);
        layout.place(placesHeader_, 0, topHeight, placesWidth, scale(34, dpi_));
        layout.place(placesList_, 0, topHeight + scale(34, dpi_), placesWidth,
            std::max(0, contentBottom - topHeight - scale(34, dpi_)));
        layout.place(tabControl_, placesWidth + margin, topHeight,
            std::max(0, width - placesWidth - margin - (tagChipText_.empty() ? 0 : scale(150, dpi_))),
            tabHeight);
        if (!tagChipText_.empty())
        {
            layout.place(tagChip_, std::max(0, width - scale(144, dpi_)), topHeight + scale(3, dpi_),
                scale(138, dpi_), scale(23, dpi_));
        }

        if (textPreviewVisible_)
        {
            const RECT viewBounds = browserBounds();
            const int previewLeft = viewBounds.right + margin;
            const int previewTop = topHeight + tabHeight;
            const int previewBottom = browserBounds().bottom;
            const int headerHeight = scale(28, dpi_);
            layout.place(textPreviewHeader_, previewLeft, previewTop,
                std::max(0, width - previewLeft), headerHeight);
            layout.place(textPreviewEdit_, previewLeft, previewTop + headerHeight,
                std::max(0, width - previewLeft),
                std::max(0, previewBottom - previewTop - headerHeight));
        }

        if (gitPaneVisible_)
        {
            const int panelLeft = placesWidth + margin;
            const int panelTop = browserBounds().bottom + margin;
            const int panelRight = width - margin;
            const int headerHeight = scale(27, dpi_);
            const int buttonWidth = scale(82, dpi_);
            const int progressHeight = scale(17, dpi_);
            layout.place(gitPanelHeader_, panelLeft, panelTop,
                std::max(0, panelRight - panelLeft - buttonWidth - margin), headerHeight);
            layout.place(gitCancelButton_, std::max(panelLeft, panelRight - buttonWidth), panelTop,
                buttonWidth, scale(25, dpi_));
            layout.place(gitProgress_, panelLeft, panelTop + headerHeight,
                std::max(0, panelRight - panelLeft), progressHeight);
            const int outputTop = panelTop + headerHeight +
                (backgroundTaskActive() ? progressHeight + margin : 0);
            layout.place(gitOutput_, panelLeft, outputTop,
                std::max(0, panelRight - panelLeft), std::max(0, contentBottom - outputTop));
        }

        layout.apply();

        if (activeTab_ < tabs_.size())
        {
            layoutTab(tabs_[activeTab_], false);
        }
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    RECT AppWindow::browserBounds() const noexcept
    {
        const int topHeight = scale(settings_.enabled(core::compactToolbar) ? 64 : 76, dpi_);
        const int tabHeight = scale(29, dpi_);
        const int placesWidth = settings_.enabled(core::showPlaces) ? scale(194, dpi_) : 0;
        const int margin = scale(6, dpi_);
        int statusHeight = scale(22, dpi_);
        if (statusBar_ != nullptr)
        {
            RECT bounds{};
            GetWindowRect(statusBar_, &bounds);
            statusHeight = static_cast<int>(std::max(1L, bounds.bottom - bounds.top));
        }
        int right = clientWidth_;
        if (textPreviewVisible_)
        {
            const int desired = std::min(scale(420, dpi_), std::max(scale(280, dpi_), clientWidth_ / 3));
            right = std::max(placesWidth + scale(240, dpi_), clientWidth_ - desired - margin);
        }
        const int gitHeight = gitPaneVisible_ ? scale(190, dpi_) : 0;
        return RECT{placesWidth + margin, topHeight + tabHeight,
            right, std::max(topHeight + tabHeight, clientHeight_ - statusHeight - gitHeight)};
    }

    void AppWindow::addTab(std::wstring initialPath)
    {
        if (tabs_.size() >= core::SessionCodec::maxTabs)
        {
            MessageBoxW(window_, L"A window can contain up to 64 tabs.",
                localizer_(Text::newTab), MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::size_t previous = activeTab_;
        try
        {
            if (initialPath.empty())
            {
                initialPath = settings_.startLocation.empty()
                    ? knownFolderPath(FOLDERID_Documents) : settings_.startLocation;
            }
            std::wstring title = pendingTabTitle(initialPath);
            if (title.empty()) title = localizer_(Text::newTab);
            tabs_.push_back(Tab{.title = std::move(title),
                .pendingLocation = std::move(initialPath), .pendingSearchRoot = {},
                .pendingSearchQuery = {}});
        }
        catch (...)
        {
            MessageBoxW(window_, L"There is not enough memory to open another tab.",
                localizer_(Text::newTab), MB_OK | MB_ICONERROR);
            return;
        }
        if (tabs_.size() > 1)
        {
            if (tabs_[previous].browser != nullptr) tabs_[previous].browser->show(false);
            if (tabs_[previous].secondaryBrowser != nullptr)
                tabs_[previous].secondaryBrowser->show(false);
        }
        activeTab_ = tabs_.size() - 1;

        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = tabs_.back().title.data();
        TabCtrl_InsertItem(tabControl_, static_cast<int>(activeTab_), &item);
        TabCtrl_SetCurSel(tabControl_, static_cast<int>(activeTab_));

        (void)ensureTabBrowser(activeTab_);
        updateChrome();
    }

    bool AppWindow::ensureTabBrowser(std::size_t index)
    {
        if (index >= tabs_.size()) return false;
        auto& tab = tabs_[index];
        if (tab.browser != nullptr) return true;
        ExplorerBrowserHost* browser{};
        const HRESULT status = ExplorerBrowserHost::create(window_, window_, shellNavigationMessage,
            shellSelectionMessage, browserBounds(), &browser);
        if (FAILED(status))
        {
            showError(L"Create tab", status);
            return false;
        }
        browser->show(index == activeTab_);
        tab.browser = browser;
        std::wstring location = tab.pendingLocation;
        HRESULT browseStatus = location.empty() ? E_INVALIDARG : browser->browsePath(location);
        if (FAILED(browseStatus) && !location.empty())
        {
            // ponytail: A routed file is retried at its lexical parent without a synchronous
            // GetFileAttributes probe, keeping remote/session launch off the filesystem path.
            std::wstring parent = location;
            if (PathRemoveFileSpecW(parent.data()))
            {
                parent.resize(std::wcslen(parent.c_str()));
                if (!parent.empty() && _wcsicmp(parent.c_str(), location.c_str()) != 0)
                    browseStatus = browser->browsePath(parent);
            }
        }
        if (FAILED(browseStatus))
            browseStatus = browser->browseKnownFolder(FOLDERID_ComputerFolder);
        if (FAILED(browseStatus)) showError(L"Open initial location", browseStatus);
        return true;
    }

    void AppWindow::launchNewWindow(std::wstring location)
    {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size())
        {
            showError(L"New window", HRESULT_FROM_WIN32(GetLastError()));
            return;
        }
        executable.resize(length);
        if (location.empty())
            if (const auto browser = activeBrowser(); browser != nullptr)
                location = browser->restorableName();
        const std::wstring parameters = location.empty() ? std::wstring{} :
            L"--new-window " + core::quoteWindowsArgument(location);
        const HINSTANCE result = ShellExecuteW(window_, L"open", executable.c_str(),
            parameters.empty() ? nullptr : parameters.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            showError(L"New window", E_FAIL);
        }
    }

    void AppWindow::duplicateActiveTab()
    {
        if (const auto browser = activeBrowser(); browser != nullptr)
        {
            addTab(browser->editingName());
        }
    }

    void AppWindow::openSelectedInNewTab()
    {
        const std::wstring location = selectedShellItemName(SIGDN_DESKTOPABSOLUTEPARSING, true);
        if (!location.empty()) addTab(location);
    }

    void AppWindow::openSelectedInNewWindow()
    {
        const std::wstring location = selectedShellItemName(SIGDN_DESKTOPABSOLUTEPARSING, true);
        if (!location.empty()) launchNewWindow(location);
    }

    void AppWindow::openInOtherPane(std::wstring location)
    {
        if (location.empty() || activeTab_ >= tabs_.size()) return;
        Tab& tab = tabs_[activeTab_];
        if (tab.browser == nullptr && !ensureTabBrowser(activeTab_)) return;
        const int sourcePane = tab.activePane;
        if (tab.secondaryBrowser == nullptr)
        {
            splitActiveTab(true, std::move(location));
            if (tab.secondaryBrowser != nullptr) tab.secondaryBrowser->focus();
            return;
        }
        ExplorerBrowserHost* target = sourcePane == 0 ? tab.secondaryBrowser : tab.browser;
        const HRESULT status = target->browsePath(location);
        if (FAILED(status))
        {
            showError(L"Open in other pane", status);
            return;
        }
        tab.activePane = target == tab.browser ? 0 : 1;
        updateChrome();
        target->focus();
    }

    void AppWindow::openSelectedInOtherPane()
    {
        openInOtherPane(selectedShellItemName(SIGDN_DESKTOPABSOLUTEPARSING, true));
    }

    void AppWindow::openCurrentFolderInOtherPane()
    {
        ExplorerBrowserHost* browser = activeBrowser();
        if (browser != nullptr) openInOtherPane(browser->restorableName());
    }

    void AppWindow::openFileLocation()
    {
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring path = selectedShellItemName(SIGDN_DESKTOPABSOLUTEPARSING);
        if (browser == nullptr || path.empty()) return;
        const HRESULT status = browser->browseParentAndSelect(path);
        if (FAILED(status)) showError(L"Open file location", status);
    }

    void AppWindow::reopenClosedTab()
    {
        if (closedTabs_.empty())
        {
            return;
        }
        std::wstring location = std::move(closedTabs_.back());
        closedTabs_.pop_back();
        addTab(std::move(location));
    }

    void AppWindow::closeActiveTab()
    {
        closeTab(activeTab_);
    }

    void AppWindow::closeTab(std::size_t index)
    {
        if (index >= tabs_.size()) return;
        const bool active = index == activeTab_;
        retireTab(std::move(tabs_[index]), true);
        TabCtrl_DeleteItem(tabControl_, static_cast<int>(index));
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
        if (tabs_.empty())
        {
            DestroyWindow(window_);
            return;
        }
        if (index < activeTab_) --activeTab_;
        else if (active) activeTab_ = std::min(index, tabs_.size() - 1);
        if (active)
            activateTab(activeTab_);
        else
            TabCtrl_SetCurSel(tabControl_, static_cast<int>(activeTab_));
    }

    void AppWindow::closeOtherTabs()
    {
        if (activeTab_ >= tabs_.size() || tabs_.size() < 2)
        {
            return;
        }
        Tab retained = std::move(tabs_[activeTab_]);
        for (std::size_t index = 0; index < tabs_.size(); ++index)
        {
            if (index == activeTab_)
            {
                continue;
            }
            retireTab(std::move(tabs_[index]), true);
        }
        tabs_.clear();
        tabs_.push_back(std::move(retained));
        activeTab_ = 0;
        rebuildTabControl();
        activateTab(0);
    }

    void AppWindow::closeTabsToLeft()
    {
        if (activeTab_ == 0 || activeTab_ >= tabs_.size()) return;
        const std::size_t count = activeTab_;
        for (std::size_t index = 0; index < count; ++index)
            retireTab(std::move(tabs_[index]), true);
        tabs_.erase(tabs_.begin(), tabs_.begin() + static_cast<std::ptrdiff_t>(count));
        activeTab_ = 0;
        rebuildTabControl();
        activateTab(0);
    }

    void AppWindow::closeTabsToRight()
    {
        if (activeTab_ >= tabs_.size() || activeTab_ + 1 >= tabs_.size()) return;
        const std::size_t first = activeTab_ + 1;
        for (std::size_t index = first; index < tabs_.size(); ++index)
            retireTab(std::move(tabs_[index]), true);
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(first), tabs_.end());
        rebuildTabControl();
        activateTab(activeTab_);
    }

    void AppWindow::retireTab(Tab&& tab, bool remember) noexcept
    {
        if (remember)
        {
            try
            {
                std::wstring location = tab.browser != nullptr ? tab.browser->restorableName() :
                    std::wstring{};
                if (location.empty()) location = tab.pendingLocation;
                if (!location.empty()) closedTabs_.push_back(std::move(location));
                if (closedTabs_.size() > 10)
                    closedTabs_.erase(closedTabs_.begin(), closedTabs_.end() - 10);
            }
            catch (...)
            {
                // Remembering a location is optional; releasing its Shell host is not.
            }
        }
        if (tab.browser != nullptr) tab.browser->show(false);
        if (tab.secondaryBrowser != nullptr) tab.secondaryBrowser->show(false);
        const bool schedule = retiredTabs_.empty();
        try
        {
            retiredTabs_.push_back(std::move(tab));
        }
        catch (...)
        {
            releaseTab(tab);
            return;
        }
        // ponytail: Shell/provider teardown remains on its owning STA, but only one retired
        // host is destroyed per message so Close Other/Left/Right never stacks 64 cleanups.
        if (schedule && !PostMessageW(window_, tabRetireMessage, 0, 0)) processRetiredTab();
    }

    void AppWindow::processRetiredTab() noexcept
    {
        if (retiredTabs_.empty()) return;
        releaseTab(retiredTabs_.back());
        retiredTabs_.pop_back();
        if (!retiredTabs_.empty() && !PostMessageW(window_, tabRetireMessage, 0, 0))
        {
            // The message queue is unavailable, so finish the bounded teardown synchronously.
            while (!retiredTabs_.empty())
            {
                releaseTab(retiredTabs_.back());
                retiredTabs_.pop_back();
            }
        }
    }

    void AppWindow::releaseTab(Tab& tab) noexcept
    {
        if (tab.browser != nullptr)
        {
            tab.browser->shutdown();
            tab.browser->Release();
            tab.browser = nullptr;
        }
        if (tab.secondaryBrowser != nullptr)
        {
            tab.secondaryBrowser->shutdown();
            tab.secondaryBrowser->Release();
            tab.secondaryBrowser = nullptr;
        }
    }

    void AppWindow::activateTab(std::size_t index)
    {
        if (index >= tabs_.size())
        {
            return;
        }
        if (activeTab_ < tabs_.size() && activeTab_ != index)
        {
            if (tabs_[activeTab_].browser != nullptr) tabs_[activeTab_].browser->show(false);
            if (tabs_[activeTab_].secondaryBrowser != nullptr)
            {
                tabs_[activeTab_].secondaryBrowser->show(false);
            }
        }
        activeTab_ = index;
        if (!ensureTabBrowser(activeTab_))
        {
            updateChrome();
            return;
        }
        tabs_[activeTab_].browser->show(true);
        if (tabs_[activeTab_].secondaryBrowser != nullptr)
        {
            tabs_[activeTab_].secondaryBrowser->show(true);
        }
        layoutTab(tabs_[activeTab_]);
        TabCtrl_SetCurSel(tabControl_, static_cast<int>(activeTab_));
        updateChrome();
        scheduleGitStatusRefresh();
    }

    void AppWindow::cycleTab(int delta)
    {
        if (tabs_.size() < 2)
        {
            return;
        }
        const auto count = static_cast<int>(tabs_.size());
        int next = (static_cast<int>(activeTab_) + delta) % count;
        if (next < 0)
        {
            next += count;
        }
        activateTab(static_cast<std::size_t>(next));
    }

    void AppWindow::moveActiveTab(int delta)
    {
        if (tabs_.size() < 2)
        {
            return;
        }
        const int destination = static_cast<int>(activeTab_) + delta;
        if (destination < 0 || destination >= static_cast<int>(tabs_.size()))
        {
            return;
        }
        moveTab(activeTab_, static_cast<std::size_t>(destination));
    }

    void AppWindow::moveTab(std::size_t source, std::size_t destination)
    {
        if (source >= tabs_.size() || destination >= tabs_.size() || source == destination)
        {
            return;
        }
        Tab moving = std::move(tabs_[source]);
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(source));
        tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(destination), std::move(moving));

        if (activeTab_ == source)
        {
            activeTab_ = destination;
        }
        else if (source < activeTab_ && activeTab_ <= destination)
        {
            --activeTab_;
        }
        else if (destination <= activeTab_ && activeTab_ < source)
        {
            ++activeTab_;
        }
        rebuildTabControl();
        activateTab(activeTab_);
    }

    void AppWindow::rebuildTabControl()
    {
        SendMessageW(tabControl_, WM_SETREDRAW, FALSE, 0);
        TabCtrl_DeleteAllItems(tabControl_);
        for (std::size_t index = 0; index < tabs_.size(); ++index)
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = tabs_[index].title.data();
            TabCtrl_InsertItem(tabControl_, static_cast<int>(index), &item);
        }
        TabCtrl_SetCurSel(tabControl_, static_cast<int>(activeTab_));
        SendMessageW(tabControl_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(tabControl_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    void AppWindow::splitActiveTab(bool vertical, std::wstring initialLocation)
    {
        if (activeTab_ >= tabs_.size() || !ensureTabBrowser(activeTab_))
        {
            return;
        }
        auto& tab = tabs_[activeTab_];
        tab.verticalSplit = vertical;
        if (tab.secondaryBrowser == nullptr)
        {
            ExplorerBrowserHost* secondary{};
            const HRESULT createStatus = ExplorerBrowserHost::create(window_, window_, shellNavigationMessage,
                shellSelectionMessage, browserBounds(), &secondary);
            if (FAILED(createStatus))
            {
                showError(L"Split pane", createStatus);
                return;
            }
            tab.secondaryBrowser = secondary;
            tab.activePane = 1;
            if (initialLocation.empty()) initialLocation = tab.browser->restorableName();
            const HRESULT browseStatus = initialLocation.empty()
                ? secondary->browseKnownFolder(FOLDERID_ComputerFolder)
                : secondary->browsePath(initialLocation);
            if (FAILED(browseStatus))
            {
                showError(L"Open split pane", browseStatus);
            }
        }
        layoutTab(tab);
        updateChrome();
    }

    void AppWindow::closeActivePane()
    {
        if (activeTab_ >= tabs_.size())
        {
            return;
        }
        auto& tab = tabs_[activeTab_];
        if (tab.secondaryBrowser == nullptr)
        {
            return;
        }
        if (tab.activePane == 0)
        {
            tab.browser->shutdown();
            tab.browser->Release();
            tab.browser = tab.secondaryBrowser;
        }
        else
        {
            tab.secondaryBrowser->shutdown();
            tab.secondaryBrowser->Release();
        }
        tab.secondaryBrowser = nullptr;
        tab.activePane = 0;
        layoutTab(tab);
        updateTab(tab.browser);
    }

    void AppWindow::focusOtherPane()
    {
        if (activeTab_ >= tabs_.size())
        {
            return;
        }
        auto& tab = tabs_[activeTab_];
        if (tab.secondaryBrowser == nullptr)
        {
            return;
        }
        tab.activePane = tab.activePane == 0 ? 1 : 0;
        updateChrome();
        activeBrowser()->focus();
    }

    void AppWindow::toggleSidebar()
    {
        settings_.set(core::showPlaces, !settings_.enabled(core::showPlaces));
        if (!SettingsStore::save(settings_))
        {
            settings_.set(core::showPlaces, !settings_.enabled(core::showPlaces));
            showError(localizer_(Text::settings), HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
            return;
        }
        layoutChildren(clientWidth_, clientHeight_);
    }

    void AppWindow::toggleFullScreen()
    {
        if (!fullScreen_)
        {
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            previousWindowPlacement_.length = sizeof(previousWindowPlacement_);
            const HMONITOR target = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
            if (!GetWindowPlacement(window_, &previousWindowPlacement_) ||
                !GetMonitorInfoW(target, &monitor))
                return;
            previousWindowStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
            fullScreenMenu_ = GetMenu(window_);
            SetMenu(window_, nullptr);
            SetWindowLongPtrW(window_, GWL_STYLE,
                (previousWindowStyle_ & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                    WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP);
            fullScreen_ = true;
            SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left,
                monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            return;
        }
        SetWindowLongPtrW(window_, GWL_STYLE, previousWindowStyle_);
        SetMenu(window_, fullScreenMenu_);
        fullScreenMenu_ = nullptr;
        SetWindowPlacement(window_, &previousWindowPlacement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                SWP_FRAMECHANGED);
        fullScreen_ = false;
    }

    void AppWindow::toggleShellVisibility(DWORD setting)
    {
        if (setting != SSF_SHOWALLOBJECTS && setting != SSF_SHOWEXTENSIONS) return;
        SHELLSTATE state{};
        SHGetSetSettings(&state, setting, FALSE);
        if (setting == SSF_SHOWALLOBJECTS)
            state.fShowAllObjects = !state.fShowAllObjects;
        else
            state.fShowExtensions = !state.fShowExtensions;
        // ponytail: Windows 10 Explorer stores both switches in the Shell state. Reusing that
        // state keeps native namespace views consistent without enumerating or renaming items.
        SHGetSetSettings(&state, setting, TRUE);
        SHGetSetSettings(&state, SSF_SHOWALLOBJECTS | SSF_SHOWEXTENSIONS, FALSE);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        if (ExplorerBrowserHost* browser = activeBrowser(); browser != nullptr)
            (void)browser->refresh();
        HMENU menuBar = fullScreen_ ? fullScreenMenu_ : GetMenu(window_);
        HMENU viewMenu = menuBar != nullptr ? GetSubMenu(menuBar, 2) : nullptr;
        if (viewMenu != nullptr)
        {
            CheckMenuItem(viewMenu, CommandId::showHiddenItems, MF_BYCOMMAND |
                (state.fShowAllObjects ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(viewMenu, CommandId::showFileExtensions, MF_BYCOMMAND |
                (state.fShowExtensions ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void AppWindow::layoutTab(Tab& tab, bool redraw) noexcept
    {
        if (tab.browser == nullptr) return;
        const RECT whole = browserBounds();
        if (tab.secondaryBrowser == nullptr)
        {
            tab.browser->setBounds(whole, redraw);
            return;
        }
        const int gap = scale(4, dpi_);
        RECT primary = whole;
        RECT secondary = whole;
        if (tab.verticalSplit)
        {
            const int middle = whole.left + (whole.right - whole.left - gap) / 2;
            primary.right = middle;
            secondary.left = middle + gap;
        }
        else
        {
            const int middle = whole.top + (whole.bottom - whole.top - gap) / 2;
            primary.bottom = middle;
            secondary.top = middle + gap;
        }
        tab.browser->setBounds(primary, redraw);
        tab.secondaryBrowser->setBounds(secondary, redraw);
    }

    void AppWindow::releaseTabs() noexcept
    {
        for (auto& tab : tabs_) releaseTab(tab);
        tabs_.clear();
        for (auto& tab : retiredTabs_) releaseTab(tab);
        retiredTabs_.clear();
    }

    void AppWindow::restoreSession()
    {
        const auto session = SessionStore::load();
        for (const auto& location : session.locations)
        {
            std::wstring title = pendingTabTitle(location);
            if (title.empty()) title = localizer_(Text::newTab);
            tabs_.push_back(Tab{.title = std::move(title), .pendingLocation = location,
                .pendingSearchRoot = {}, .pendingSearchQuery = {}});
        }
        if (tabs_.empty())
        {
            addTab();
            return;
        }
        activeTab_ = std::min(session.activeIndex, tabs_.size() - 1);
        rebuildTabControl();
        activateTab(activeTab_);
    }

    void AppWindow::saveSession() const noexcept
    {
        core::SessionSnapshot session;
        session.activeIndex = activeTab_;
        session.locations.reserve(tabs_.size());
        for (const auto& tab : tabs_)
        {
            std::wstring location = tab.browser != nullptr ? tab.browser->restorableName() :
                std::wstring{};
            if (location.empty()) location = tab.pendingLocation;
            if (!location.empty())
            {
                session.locations.push_back(std::move(location));
            }
            else
            {
                session.locations.push_back(knownFolderPath(FOLDERID_Documents));
            }
        }
        if (session.locations.empty())
        {
            return;
        }
        session.activeIndex = std::min(session.activeIndex, session.locations.size() - 1);
        SessionStore::save(session);
    }

    ExplorerBrowserHost* AppWindow::activeBrowser() const noexcept
    {
        if (activeTab_ >= tabs_.size())
        {
            return nullptr;
        }
        const auto& tab = tabs_[activeTab_];
        return tab.activePane == 1 && tab.secondaryBrowser != nullptr ? tab.secondaryBrowser : tab.browser;
    }

    std::pair<std::size_t, int> AppWindow::findBrowser(const ExplorerBrowserHost* browser) const noexcept
    {
        for (std::size_t index = 0; index < tabs_.size(); ++index)
        {
            if (tabs_[index].browser == browser)
            {
                return {index, 0};
            }
            if (tabs_[index].secondaryBrowser == browser)
            {
                return {index, 1};
            }
        }
        return {noTab, -1};
    }

    void AppWindow::browseAddress()
    {
        const auto browser = activeBrowser();
        if (browser == nullptr)
        {
            return;
        }
        const HRESULT status = browser->browsePath(windowText(addressEdit_));
        if (FAILED(status))
        {
            showError(L"Open address", status);
        }
    }

    void AppWindow::runSearch()
    {
        ExplorerBrowserHost* browser = activeBrowser();
        if (browser == nullptr)
        {
            return;
        }
        std::wstring query;
        try
        {
            query = windowText(searchEdit_);
        }
        catch (...)
        {
            showError(L"Search", E_OUTOFMEMORY);
            return;
        }
        const auto [tabIndex, paneIndex] = findBrowser(browser);
        if (tabIndex == noTab || paneIndex < 0) return;
        auto& tab = tabs_[tabIndex];
        tab.pendingSearchRoot[paneIndex].clear();
        tab.pendingSearchQuery[paneIndex].clear();
        if (!query.empty())
        {
            try
            {
                tab.pendingSearchRoot[paneIndex] = browser->filesystemPath();
                tab.pendingSearchQuery[paneIndex] = query;
            }
            catch (...)
            {
                showError(L"Search", E_OUTOFMEMORY);
                return;
            }
        }
        const HRESULT status = browser->search(query);
        if (FAILED(status))
        {
            std::wstring root = std::move(tab.pendingSearchRoot[paneIndex]);
            query = std::move(tab.pendingSearchQuery[paneIndex]);
            if (!root.empty() && !query.empty())
                startFallbackSearch(std::move(root), std::move(query), browser);
            else
                showError(L"Search", status);
        }
    }

    void AppWindow::openPlace(int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= places_.size())
        {
            return;
        }
        if (const auto browser = activeBrowser(); browser != nullptr)
        {
            const auto& place = places_[static_cast<std::size_t>(index)];
            const HRESULT status = place.parsingName.empty()
                ? browser->browseKnownFolder(place.folderId)
                : browser->browsePath(place.parsingName);
            if (FAILED(status))
            {
                showError(L"Open location", status);
            }
        }
    }

    void AppWindow::updateTab(ExplorerBrowserHost* browser)
    {
        const auto [index, paneIndex] = findBrowser(browser);
        if (index == noTab)
        {
            return;
        }
        if (paneIndex != tabs_[index].activePane)
        {
            return;
        }
        std::wstring title = browser->displayName();
        if (title.empty())
        {
            title = localizer_(Text::title);
        }
        tabs_[index].title = std::move(title);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = tabs_[index].title.data();
        TabCtrl_SetItem(tabControl_, static_cast<int>(index), &item);
        if (index == activeTab_)
        {
            updateChrome();
        }
    }

    void AppWindow::updateChrome()
    {
        const auto browser = activeBrowser();
        const bool available = browser != nullptr;
        EnableWindow(backButton_, available && browser->canGoBack());
        EnableWindow(forwardButton_, available && browser->canGoForward());
        for (HWND button : {upButton_, refreshButton_, newFolderButton_, viewButton_})
        {
            EnableWindow(button, available);
        }
        if (!available)
        {
            SetWindowTextW(addressEdit_, L"");
            if (!statusText_.empty())
            {
                statusText_.clear();
                SetWindowTextW(statusBar_, L"");
            }
            return;
        }
        const std::wstring address = browser->editingName();
        SetWindowTextW(addressEdit_, address.c_str());
        const std::wstring title = tabs_[activeTab_].title + L" - " + localizer_(Text::title);
        SetWindowTextW(window_, title.c_str());
        updateStatus();
    }

    void AppWindow::updateStatus()
    {
        const auto browser = activeBrowser();
        if (browser == nullptr)
        {
            return;
        }
        int items{};
        int selected{};
        browser->itemCounts(items, selected);
        std::wstring status = std::to_wstring(items) + L" " +
            localizer_(items == 1 ? Text::object : Text::objects);
        if (selected > 0)
        {
            status += L"    " + std::to_wstring(selected) + L" " + localizer_(Text::selected);
        }
        if (browser->isSearch())
        {
            status += L"    ";
            status += localizer_(Text::searchResults);
        }
        if (shelfCount_ > 0)
        {
            status += L"    ";
            status += localizer_(Text::shelf);
            status += L": " + std::to_wstring(shelfCount_) + L" ";
            status += localizer_(shelfMove_ ? Text::toMove : Text::toCopy);
        }
        if (status != statusText_)
        {
            statusText_ = std::move(status);
            SetWindowTextW(statusBar_, statusText_.c_str());
        }
    }

    void AppWindow::showError(const wchar_t* operation, HRESULT status) const
    {
        wchar_t* raw{};
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(status), 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
        std::wstring message = operation;
        message += L" failed.\n\n";
        if (length != 0 && raw != nullptr)
        {
            message.append(raw, length);
            LocalFree(raw);
        }
        else
        {
            wchar_t code[16]{};
            swprintf_s(code, L"0x%08X", static_cast<unsigned int>(status));
            message += code;
        }
        MessageBoxW(window_, message.c_str(), L"Files XP Native", MB_OK | MB_ICONWARNING);
    }

    void AppWindow::copySelectedPaths(bool quoted)
    {
        core::ShellOperationRequest request;
        beginShellOperationSnapshot(std::move(request),
            quoted ? Text::copyPathQuoted : Text::copyPath, nullptr, false,
            ShellSnapshotPurpose::clipboardPaths, quoted);
    }

    std::wstring AppWindow::selectedShellItemName(SIGDN name, bool requireFolder) const
    {
        ExplorerBrowserHost* browser = activeBrowser();
        if (browser == nullptr)
        {
            return {};
        }
        IShellItemArray* items{};
        if (FAILED(browser->selectedItems(&items)) || items == nullptr)
        {
            return {};
        }
        DWORD count{};
        if (FAILED(items->GetCount(&count)) || count != 1)
        {
            items->Release();
            return {};
        }
        std::wstring path;
        IShellItem* item{};
        PWSTR raw{};
        SFGAOF attributes{};
        HRESULT status = items->GetItemAt(0, &item);
        if (SUCCEEDED(status) && requireFolder)
            status = item->GetAttributes(SFGAO_FOLDER, &attributes);
        if (SUCCEEDED(status) && (!requireFolder || (attributes & SFGAO_FOLDER) != 0))
            status = item->GetDisplayName(name, &raw);
        if (SUCCEEDED(status) && raw != nullptr)
        {
            try
            {
                path.assign(raw);
            }
            catch (...)
            {
                path.clear();
            }
            CoTaskMemFree(raw);
        }
        if (item != nullptr) item->Release();
        items->Release();
        return path;
    }

    std::wstring AppWindow::selectedFileSystemPath() const
    {
        return selectedShellItemName(SIGDN_FILESYSPATH);
    }

    bool AppWindow::backgroundTaskActive() const noexcept
    {
        return (gitWorkerActive_ && !gitWorkerQuiet_) || archiveWorkerActive_ ||
            ftpWorkerActive_ || shellSnapshotActive_ || tagResultsActive_ || !tagSearchPath_.empty();
    }

    bool AppWindow::launchProcess(const std::wstring& executable,
        const std::vector<std::wstring>& arguments, const std::wstring& workingDirectory,
        DWORD creationFlags, WORD showWindow) const
    {
        if (executable.empty())
        {
            return false;
        }
        std::wstring commandLine = core::quoteWindowsArgument(executable);
        for (const auto& argument : arguments)
        {
            commandLine.push_back(L' ');
            commandLine += core::quoteWindowsArgument(argument);
        }
        if (commandLine.size() >= 32767)
        {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return false;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = showWindow;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
            FALSE, creationFlags, nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup, &process);
        if (created)
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        return created != FALSE;
    }

    bool AppWindow::queueProcessLaunch(const std::wstring& executable,
        const std::vector<std::wstring>& arguments, const std::wstring& workingDirectory,
        DWORD creationFlags, WORD showWindow, UINT completionMessage,
        WPARAM completionToken) const
    {
        if (executable.empty() || completionMessage < WM_APP)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        std::unique_ptr<AsyncProcessLaunch> launch;
        try
        {
            std::wstring commandLine = core::quoteWindowsArgument(executable);
            for (const auto& argument : arguments)
            {
                commandLine.push_back(L' ');
                commandLine += core::quoteWindowsArgument(argument);
            }
            if (commandLine.size() >= 32767)
            {
                SetLastError(ERROR_FILENAME_EXCED_RANGE);
                return false;
            }
            launch = std::make_unique<AsyncProcessLaunch>(AsyncProcessLaunch{
                window_, completionMessage, completionToken, executable,
                std::move(commandLine), workingDirectory, creationFlags, showWindow});
        }
        catch (...)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
        AsyncProcessLaunch* const queued = launch.release();
        if (!QueueUserWorkItem(&launchProcessOnThreadPool, queued, WT_EXECUTEDEFAULT))
        {
            const DWORD failure = GetLastError();
            delete queued;
            SetLastError(failure);
            return false;
        }
        return true;
    }

    void AppWindow::beginGitResultRead(std::uint32_t generation, DWORD workerResult)
    {
        if (!gitWorkerActive_ || gitResultReadActive_ || generation != gitWorkerGeneration_) return;
        if (gitCancelEvent_ != nullptr)
        {
            CloseHandle(gitCancelEvent_);
            gitCancelEvent_ = nullptr;
        }
        gitWorkerResult_ = workerResult;
        gitResultReadActive_ = true;
        gitResultReadCursor_.cancel();
        if (!gitWorkerQuiet_) EnableWindow(gitCancelButton_, FALSE);

        bool valid = !gitResultPath_.empty();
        if (valid)
        {
            gitResultReadFile_ = CreateFileW(gitResultPath_.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            valid = gitResultReadFile_ != INVALID_HANDLE_VALUE;
        }
        LARGE_INTEGER bytes{};
        if (valid)
            valid = GetFileSizeEx(gitResultReadFile_, &bytes) != FALSE && bytes.QuadPart >= 0 &&
                bytes.QuadPart <= 4LL * 1024LL * 1024LL;
        if (valid)
        {
            try
            {
                gitResultBytes_.resize(static_cast<std::size_t>(bytes.QuadPart));
            }
            catch (...)
            {
                valid = false;
            }
        }
        if (!valid || gitResultBytes_.empty())
        {
            finishGitResultRead(generation, valid);
            return;
        }
        if (!gitResultReadCursor_.start(gitResultBytes_.size(), 4U * 1024U * 1024U))
        {
            finishGitResultRead(generation, false);
            return;
        }
        if (!PostMessageW(window_, gitResultReadMessage, generation, 0))
            finishGitResultRead(generation, false);
    }

    void AppWindow::processGitResultRead(std::uint32_t generation)
    {
        if (!gitResultReadActive_ || generation != gitWorkerGeneration_ ||
            gitResultReadFile_ == INVALID_HANDLE_VALUE)
            return;
        constexpr std::size_t chunkBytes = 64U * 1024U;
        constexpr std::size_t chunksPerDispatch = 2;
        const ULONGLONG deadline = GetTickCount64() + 6;
        for (std::size_t chunk = 0; chunk < chunksPerDispatch &&
            gitResultReadCursor_.active(); ++chunk)
        {
            const core::IndexBatch batch = gitResultReadCursor_.next(chunkBytes);
            const DWORD requested = static_cast<DWORD>(batch.count);
            DWORD read{};
            if (!ReadFile(gitResultReadFile_, gitResultBytes_.data() + batch.first,
                    requested, &read, nullptr) || read != requested)
            {
                finishGitResultRead(generation, false);
                return;
            }
            if (GetTickCount64() >= deadline) break;
        }
        if (!gitResultReadCursor_.active())
        {
            finishGitResultRead(generation, true);
            return;
        }
        if (!PostMessageW(window_, gitResultReadMessage, generation, 0))
            finishGitResultRead(generation, false);
    }

    void AppWindow::finishGitResultRead(std::uint32_t generation, bool read)
    {
        if (!gitResultReadActive_ || generation != gitWorkerGeneration_) return;
        if (gitResultReadFile_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(gitResultReadFile_);
            gitResultReadFile_ = INVALID_HANDLE_VALUE;
        }
        if (!gitResultPath_.empty()) DeleteFileW(gitResultPath_.c_str());
        gitResultPath_.clear();
        std::wstring output;
        if (read) read = decodeBoundedText(std::move(gitResultBytes_), output);
        gitResultBytes_.clear();
        gitResultReadCursor_.cancel();
        gitResultReadActive_ = false;
        gitWorkerActive_ = false;
        const bool quiet = gitWorkerQuiet_;
        gitWorkerQuiet_ = false;
        DWORD result = gitWorkerResult_;
        if (!read && result == ERROR_SUCCESS) result = ERROR_READ_FAULT;
        gitWorkerResult_ = ERROR_SUCCESS;
        constexpr std::wstring_view statusMarker = L"\r\n[FilesXPNative-GitStatus]\r\n";
        std::wstring displayOutput;
        if (!quiet)
            displayOutput.assign(output.data(),
                std::min(output.size(), maxTaskOutputCharacters + 1));
        const auto matchingBrowser = [this]() noexcept -> ExplorerBrowserHost*
        {
            ExplorerBrowserHost* browser = activeBrowser();
            return browser != nullptr && _wcsicmp(browser->filesystemPath().c_str(),
                gitWorkingDirectory_.c_str()) == 0 ? browser : nullptr;
        };
        if (result == ERROR_SUCCESS && read)
        {
            if (gitWorkerOperation_ == core::GitOperation::status)
            {
                if (ExplorerBrowserHost* browser = matchingBrowser(); browser != nullptr)
                    browser->setGitDecorations(std::move(output), gitWorkingDirectory_);
            }
            else if (const std::size_t marker = output.find(statusMarker);
                marker != std::wstring::npos)
            {
                std::wstring statusOutput = output.substr(marker + statusMarker.size());
                if (!quiet && marker < displayOutput.size()) displayOutput.resize(marker);
                if (ExplorerBrowserHost* browser = matchingBrowser(); browser != nullptr)
                    browser->setGitDecorations(std::move(statusOutput), gitWorkingDirectory_);
            }
        }
        else if (gitWorkerOperation_ == core::GitOperation::status)
        {
            if (ExplorerBrowserHost* browser = matchingBrowser(); browser != nullptr)
                browser->setGitDecorations({}, {});
        }
        if (gitWorkerOperation_ == core::GitOperation::status)
        {
            if (quiet) return;
            boundTaskOutput(displayOutput);
            std::wstring readable;
            readable.reserve(displayOutput.size() + 32);
            for (wchar_t character : displayOutput)
            {
                if (character == L'\0') readable += L"\r\n";
                else readable.push_back(character);
            }
            displayOutput = std::move(readable);
        }
        else
        {
            if (quiet) return;
            boundTaskOutput(displayOutput);
        }
        SendMessageW(gitProgress_, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(gitProgress_, SW_HIDE);
        EnableWindow(gitCancelButton_, TRUE);
        SetWindowTextW(gitCancelButton_, localizer_(Text::close));
        std::wstring summary;
        if (result == ERROR_SUCCESS)
            summary = localizer_(Text::gitCompleted);
        else if (result == ERROR_CANCELLED)
            summary = localizer_(Text::gitCanceled);
        else
            summary = std::wstring(localizer_(Text::gitFailed)) + std::to_wstring(result);
        if (!read) displayOutput.clear();
        if (!displayOutput.empty())
        {
            summary += L"\r\n\r\n";
            summary += displayOutput;
        }
        SetWindowTextW(gitOutput_, summary.c_str());
        layoutChildren(clientWidth_, clientHeight_);
    }

    void AppWindow::scheduleGitStatusRefresh() noexcept
    {
        if (window_ == nullptr) return;
        KillTimer(window_, gitStatusTimerId);
        if (!settings_.enabled(core::enableGit) || activeBrowser() == nullptr ||
            activeBrowser()->isSearch() ||
            activeBrowser()->filesystemPath().empty())
            return;
        // ponytail: Coalesce activation/tab/navigation bursts before launching repository work.
        (void)SetTimer(window_, gitStatusTimerId, gitStatusDebounceMilliseconds, nullptr);
    }

    void AppWindow::runGit(core::GitOperation operation, Text title, bool quiet)
    {
        KillTimer(window_, gitStatusTimerId);
        // ponytail: Repository access and process creation both stay outside the UI thread.
        if (gitWorkerActive_ || backgroundTaskActive())
        {
            if (!quiet)
                MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                    MB_OK | MB_ICONINFORMATION);
            return;
        }
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring workingDirectory = browser != nullptr ? browser->filesystemPath() : std::wstring{};
        if (workingDirectory.empty())
        {
            if (!quiet)
                MessageBoxW(window_, L"Git commands require a file-system folder.", L"Files XP Native",
                    MB_OK | MB_ICONINFORMATION);
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (quiet && operation == core::GitOperation::status &&
            !core::shouldRefreshGitStatus(lastGitStatusDirectory_, workingDirectory,
                lastGitStatusTick_, now))
            return;
        const std::wstring executable = moduleExecutable();
        const std::wstring eventName = uniqueLocalObjectName(L"GitCancel");
        const std::wstring resultPath = uniqueTemporaryPath(L"Git");
        if (executable.empty() || eventName.empty() || resultPath.empty())
        {
            if (!quiet) showError(L"Git", HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
            return;
        }
        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (cancel == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (!quiet) showError(L"Git", HRESULT_FROM_WIN32(GetLastError()));
            if (cancel != nullptr) CloseHandle(cancel);
            return;
        }
        const std::uint32_t generation = ++gitWorkerGeneration_;
        const std::vector<std::wstring> arguments{
            L"--git-worker",
            std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
            std::to_wstring(generation),
            resultPath,
            eventName,
            workingDirectory,
            std::to_wstring(static_cast<std::uint32_t>(operation))};
        gitCancelEvent_ = cancel;
        gitResultPath_ = resultPath;
        gitWorkerOperation_ = operation;
        gitWorkingDirectory_ = workingDirectory;
        if (quiet && operation == core::GitOperation::status)
        {
            lastGitStatusDirectory_ = workingDirectory;
            lastGitStatusTick_ = now;
        }
        gitWorkerQuiet_ = quiet;
        gitPanelTitleText_ = title;
        gitWorkerActive_ = true;
        if (!quiet)
        {
            gitPaneVisible_ = true;
            SetWindowTextW(gitPanelHeader_, localizer_(gitPanelTitleText_));
            SetWindowTextW(gitOutput_, localizer_(Text::gitRunning));
            SetWindowTextW(gitCancelButton_, localizer_(Text::cancel));
            EnableWindow(gitCancelButton_, TRUE);
            ShowWindow(gitPanelHeader_, SW_SHOW);
            ShowWindow(gitProgress_, SW_SHOW);
            ShowWindow(gitOutput_, SW_SHOW);
            ShowWindow(gitCancelButton_, SW_SHOW);
            LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
            SetWindowLongPtrW(gitProgress_, GWL_STYLE, style | PBS_MARQUEE);
            SetWindowPos(gitProgress_, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                SWP_FRAMECHANGED);
            SendMessageW(gitProgress_, PBM_SETMARQUEE, TRUE, 45);
            layoutChildren(clientWidth_, clientHeight_);
        }
        if (!queueProcessLaunch(executable, arguments, {}, CREATE_NO_WINDOW, SW_HIDE,
                gitCompleteMessage, generation))
        {
            const DWORD failure = GetLastError();
            beginGitResultRead(generation, failure);
        }
    }

    void AppWindow::cloneGitRepository()
    {
        if (!promptForName(Text::gitClone, Text::gitRepositoryInstructions, true, false, true))
        {
            return;
        }
        const std::wstring repository = nameInput_;
        IFileOpenDialog* dialog{};
        HRESULT status = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(status)) status = dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
        if (SUCCEEDED(status)) status = dialog->SetTitle(localizer_(Text::gitRepositoryInstructions));
        if (SUCCEEDED(status)) status = dialog->Show(window_);
        IShellItem* folder{};
        if (SUCCEEDED(status)) status = dialog->GetResult(&folder);
        PWSTR rawPath{};
        if (SUCCEEDED(status)) status = folder->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
        const std::wstring parent = rawPath != nullptr ? rawPath : L"";
        if (rawPath != nullptr) CoTaskMemFree(rawPath);
        if (folder != nullptr) folder->Release();
        if (dialog != nullptr) dialog->Release();
        if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        if (FAILED(status) || parent.empty())
        {
            if (FAILED(status)) showError(L"Choose clone folder", status);
            return;
        }
        const std::wstring git = findGitExecutable();
        if (git.empty() || !launchProcess(git,
                {L"clone", L"--progress", L"--", repository}, parent,
                CREATE_NEW_CONSOLE, SW_SHOWNORMAL))
        {
            showError(L"Git clone", HRESULT_FROM_WIN32(
                git.empty() ? ERROR_FILE_NOT_FOUND : GetLastError()));
        }
    }

    void AppWindow::changeGitBranch(bool create)
    {
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring directory = browser != nullptr ? browser->filesystemPath() : std::wstring{};
        if (directory.empty() || !promptForName(create ? Text::gitCreateBranch : Text::gitSwitchBranch,
                Text::gitBranchInstructions, true, false, true))
        {
            return;
        }
        if (!core::validGitBranchName(nameInput_))
        {
            MessageBoxW(window_, localizer_(Text::gitBranchInstructions),
                localizer_(Text::title), MB_OK | MB_ICONWARNING);
            return;
        }
        const std::wstring git = findGitExecutable();
        const std::vector<std::wstring> arguments = create ?
            std::vector<std::wstring>{L"switch", L"-c", nameInput_} :
            std::vector<std::wstring>{L"switch", L"--", nameInput_};
        if (git.empty() || !launchProcess(git, arguments, directory,
                CREATE_NEW_CONSOLE, SW_SHOWNORMAL))
        {
            showError(create ? L"Create branch" : L"Switch branch", HRESULT_FROM_WIN32(
                git.empty() ? ERROR_FILE_NOT_FOUND : GetLastError()));
        }
    }

    void AppWindow::compressSelection()
    {
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring workingDirectory = browser != nullptr ? browser->filesystemPath() : std::wstring{};
        if (workingDirectory.empty())
        {
            MessageBoxW(window_, L"Select one or more file-system items first.", L"Files XP Native",
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        beginFileSystemPathSnapshot(PathSnapshotAction::compress, Text::compress,
            workingDirectory);
    }

    void AppWindow::extractSelection()
    {
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring workingDirectory = browser != nullptr ? browser->filesystemPath() : std::wstring{};
        const std::wstring path = selectedFileSystemPath();
        if (workingDirectory.empty() || path.empty())
        {
            MessageBoxW(window_, L"Select exactly one archive first.", L"Files XP Native",
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring folderName = PathFindFileNameW(path.c_str());
        PathRemoveExtensionW(folderName.data());
        folderName.resize(std::wcslen(folderName.c_str()));
        archiveName_ = folderName.empty() ? L"Extracted" : folderName;
        if (!promptForArchive(false)) return;
        core::ArchiveRequest request;
        request.operation = core::ArchiveOperation::extract;
        request.collision = archiveCollisionChoice_;
        request.workingDirectory = toUtf16(workingDirectory);
        request.target = toUtf16(workingDirectory + L"\\" + archiveName_);
        request.password = toUtf16(archivePassword_);
        request.paths.push_back(toUtf16(path));
        startArchiveWorker(std::move(request), Text::extract);
    }

    bool AppWindow::promptForArchive(bool creating)
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return false;
        }
        archiveCreating_ = creating;
        archivePassword_.clear();
        archiveCollisionChoice_ = core::ArchiveCollision::rename;
        if (creating)
        {
            archiveOperationChoice_ = core::ArchiveOperation::create7z;
            archiveName_ = L"Archive.7z";
        }
        else
            archiveOperationChoice_ = core::ArchiveOperation::extract;
        const INT_PTR result = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_ARCHIVE), window_,
            &AppWindow::archiveProcedure, reinterpret_cast<LPARAM>(this));
        if (result == IDOK) return true;
        if (!archivePassword_.empty())
            SecureZeroMemory(archivePassword_.data(), archivePassword_.size() * sizeof(wchar_t));
        archivePassword_.clear();
        return false;
    }

    void AppWindow::startArchiveWorker(core::ArchiveRequest request, Text title)
    {
        // ponytail: The worker owns archive I/O; the UI only creates a bounded pagefile mapping.
        std::vector<std::uint8_t> encoded = core::encodeArchiveRequest(request);
        if (!request.password.empty())
            SecureZeroMemory(request.password.data(), request.password.size() * sizeof(char16_t));
        request.password.clear();
        if (!archivePassword_.empty())
            SecureZeroMemory(archivePassword_.data(), archivePassword_.size() * sizeof(wchar_t));
        archivePassword_.clear();
        if (encoded.empty())
        {
            showError(L"Archive", HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return;
        }
        const std::wstring sevenZip = find7ZipConsole();
        const std::wstring executable = moduleExecutable();
        const std::wstring mappingName = uniqueLocalObjectName(L"ArchiveRequest");
        const std::wstring eventName = uniqueLocalObjectName(L"ArchiveCancel");
        const std::wstring resultPath = uniqueTemporaryPath(L"Archive");
        if (sevenZip.empty() || executable.empty() || mappingName.empty() || eventName.empty() ||
            resultPath.empty())
        {
            SecureZeroMemory(encoded.data(), encoded.size());
            showError(L"Archive", HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
            return;
        }
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(encoded.size()), mappingName.c_str());
        DWORD failure = mapping == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (mapping != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) failure = ERROR_ALREADY_EXISTS;
        void* view = failure == ERROR_SUCCESS ?
            MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, encoded.size()) : nullptr;
        if (failure == ERROR_SUCCESS && view == nullptr) failure = GetLastError();
        if (view != nullptr)
        {
            std::memcpy(view, encoded.data(), encoded.size());
            UnmapViewOfFile(view);
        }
        SecureZeroMemory(encoded.data(), encoded.size());
        HANDLE cancel = failure == ERROR_SUCCESS ?
            CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()) : nullptr;
        if (failure == ERROR_SUCCESS &&
            (cancel == nullptr || GetLastError() == ERROR_ALREADY_EXISTS))
            failure = cancel == nullptr ? GetLastError() : ERROR_ALREADY_EXISTS;
        const std::uint32_t generation = ++archiveWorkerGeneration_;
        const std::vector<std::wstring> arguments{
            L"--archive-worker",
            std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
            std::to_wstring(generation), resultPath, eventName, mappingName, sevenZip};
        if (failure != ERROR_SUCCESS)
        {
            if (cancel != nullptr) CloseHandle(cancel);
            if (mapping != nullptr) CloseHandle(mapping);
            showError(L"Archive", HRESULT_FROM_WIN32(failure));
            return;
        }
        archiveCancelEvent_ = cancel;
        archiveRequestMapping_ = mapping;
        archiveResultPath_ = resultPath;
        archiveWorkerActive_ = true;
        gitPaneVisible_ = true;
        gitPanelTitleText_ = title;
        SetWindowTextW(gitPanelHeader_, localizer_(gitPanelTitleText_));
        SetWindowTextW(gitOutput_, localizer_(Text::gitRunning));
        SetWindowTextW(gitCancelButton_, localizer_(Text::cancel));
        EnableWindow(gitCancelButton_, TRUE);
        LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
        SetWindowLongPtrW(gitProgress_, GWL_STYLE, style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
        SetWindowPos(gitProgress_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(gitProgress_, PBM_SETRANGE32, 0, 100);
        SendMessageW(gitProgress_, PBM_SETPOS, 0, 0);
        for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
            ShowWindow(control, SW_SHOW);
        layoutChildren(clientWidth_, clientHeight_);
        if (!queueProcessLaunch(executable, arguments, {}, CREATE_NO_WINDOW, SW_HIDE,
                archiveCompleteMessage, generation))
            (void)PostMessageW(window_, archiveCompleteMessage, generation, GetLastError());
    }

    void AppWindow::startFlattenWorker(std::wstring rootPath)
    {
        // ponytail: Reuse the bounded file-task surface; traversal and IFileOperation stay in
        // an isolated STA process so the frame never owns recursive disk work.
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::wstring executable = moduleExecutable();
        const std::wstring eventName = uniqueLocalObjectName(L"FlattenCancel");
        const std::wstring resultPath = uniqueTemporaryPath(L"Flatten");
        if (executable.empty() || eventName.empty() || resultPath.empty() ||
            rootPath.empty() || rootPath.size() >= 32767)
        {
            showError(L"Flatten folder", HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return;
        }
        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        DWORD failure = cancel == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (cancel != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
            failure = ERROR_ALREADY_EXISTS;
        const std::uint32_t generation = ++archiveWorkerGeneration_;
        const std::vector<std::wstring> arguments{
            L"--flatten-worker",
            std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
            std::to_wstring(generation), resultPath, eventName, std::move(rootPath)};
        if (failure != ERROR_SUCCESS)
        {
            if (cancel != nullptr) CloseHandle(cancel);
            showError(L"Flatten folder", HRESULT_FROM_WIN32(failure));
            return;
        }
        archiveCancelEvent_ = cancel;
        archiveResultPath_ = resultPath;
        archiveWorkerActive_ = true;
        gitPaneVisible_ = true;
        gitPanelTitleText_ = Text::flattenFolder;
        SetWindowTextW(gitPanelHeader_, localizer_(gitPanelTitleText_));
        SetWindowTextW(gitOutput_, localizer_(Text::gitRunning));
        SetWindowTextW(gitCancelButton_, localizer_(Text::cancel));
        EnableWindow(gitCancelButton_, TRUE);
        LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
        SetWindowLongPtrW(gitProgress_, GWL_STYLE, style | PBS_MARQUEE);
        SetWindowPos(gitProgress_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(gitProgress_, PBM_SETMARQUEE, TRUE, 45);
        for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
            ShowWindow(control, SW_SHOW);
        layoutChildren(clientWidth_, clientHeight_);
        if (!queueProcessLaunch(executable, arguments, {}, CREATE_NO_WINDOW, SW_HIDE,
                archiveCompleteMessage, generation))
            (void)PostMessageW(window_, archiveCompleteMessage, generation, GetLastError());
    }

    bool AppWindow::startMappedFileWorker(std::vector<std::uint8_t> encoded,
        const wchar_t* workerSwitch, const wchar_t* purpose, Text title)
    {
        // ponytail: All mapped file workers share the same bounded transport, cleanup, and
        // task-panel lifecycle so a new operation cannot accidentally weaken those guarantees.
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            if (!encoded.empty()) SecureZeroMemory(encoded.data(), encoded.size());
            return false;
        }
        if (encoded.empty() || workerSwitch == nullptr || *workerSwitch == L'\0' ||
            purpose == nullptr || *purpose == L'\0')
        {
            showError(localizer_(title), HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return false;
        }
        std::wstring mappingPurpose = purpose;
        mappingPurpose += L"Request";
        std::wstring cancelPurpose = purpose;
        cancelPurpose += L"Cancel";
        const std::wstring executable = moduleExecutable();
        const std::wstring mappingName = uniqueLocalObjectName(mappingPurpose.c_str());
        const std::wstring eventName = uniqueLocalObjectName(cancelPurpose.c_str());
        const std::wstring resultPath = uniqueTemporaryPath(purpose);
        if (executable.empty() || mappingName.empty() || eventName.empty() || resultPath.empty())
        {
            SecureZeroMemory(encoded.data(), encoded.size());
            showError(localizer_(title), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
            return false;
        }
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(encoded.size()), mappingName.c_str());
        DWORD failure = mapping == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (mapping != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
            failure = ERROR_ALREADY_EXISTS;
        void* view = failure == ERROR_SUCCESS ?
            MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, encoded.size()) : nullptr;
        if (failure == ERROR_SUCCESS && view == nullptr) failure = GetLastError();
        if (view != nullptr)
        {
            std::memcpy(view, encoded.data(), encoded.size());
            UnmapViewOfFile(view);
        }
        SecureZeroMemory(encoded.data(), encoded.size());
        HANDLE cancel = failure == ERROR_SUCCESS ?
            CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()) : nullptr;
        if (failure == ERROR_SUCCESS &&
            (cancel == nullptr || GetLastError() == ERROR_ALREADY_EXISTS))
            failure = cancel == nullptr ? GetLastError() : ERROR_ALREADY_EXISTS;
        const std::uint32_t generation = ++archiveWorkerGeneration_;
        const std::vector<std::wstring> arguments{
            workerSwitch,
            std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
            std::to_wstring(generation), resultPath, eventName, mappingName};
        if (failure != ERROR_SUCCESS)
        {
            if (cancel != nullptr) CloseHandle(cancel);
            if (mapping != nullptr) CloseHandle(mapping);
            showError(localizer_(title), HRESULT_FROM_WIN32(failure));
            return false;
        }
        archiveCancelEvent_ = cancel;
        archiveRequestMapping_ = mapping;
        archiveResultPath_ = resultPath;
        archiveWorkerActive_ = true;
        gitPaneVisible_ = true;
        gitPanelTitleText_ = title;
        SetWindowTextW(gitPanelHeader_, localizer_(gitPanelTitleText_));
        SetWindowTextW(gitOutput_, localizer_(Text::gitRunning));
        SetWindowTextW(gitCancelButton_, localizer_(Text::cancel));
        EnableWindow(gitCancelButton_, TRUE);
        LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
        SetWindowLongPtrW(gitProgress_, GWL_STYLE, style | PBS_MARQUEE);
        SetWindowPos(gitProgress_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(gitProgress_, PBM_SETMARQUEE, TRUE, 45);
        for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
            ShowWindow(control, SW_SHOW);
        layoutChildren(clientWidth_, clientHeight_);
        if (!queueProcessLaunch(executable, arguments, {}, CREATE_NO_WINDOW, SW_HIDE,
                archiveCompleteMessage, generation))
            (void)PostMessageW(window_, archiveCompleteMessage, generation, GetLastError());
        return true;
    }

    void AppWindow::startBulkRenameWorker(core::BulkRenameRequest request)
    {
        // ponytail: The UI serializes only a bounded snapshot; the STA worker owns path lookup
        // and every Shell rename operation.
        (void)startMappedFileWorker(core::encodeBulkRenameRequest(request),
            L"--bulk-rename-worker", L"BulkRename", Text::bulkRename);
    }

    void AppWindow::startFolderSelectionWorker(core::FolderSelectionRequest request)
    {
        // ponytail: Creating the folder and moving its bounded selection are one cancellable
        // background task, never synchronous file-system work in the window procedure.
        (void)startMappedFileWorker(core::encodeFolderSelectionRequest(request),
            L"--folder-selection-worker", L"FolderSelection", Text::folderFromSelection);
    }

    void AppWindow::startShellArtifactWorker(core::ShellArtifactRequest request, Text title)
    {
        // ponytail: Shell link/library serialization remains native COM, but redirected-profile,
        // antivirus, and disk work belongs to the isolated STA task rather than the frame pump.
        (void)startMappedFileWorker(core::encodeShellArtifactRequest(request),
            L"--shell-artifact-worker", L"ShellArtifact", title);
    }

    bool AppWindow::startShellOperation(core::ShellOperationRequest request, Text title,
        bool clearShelfOnSuccess)
    {
        if (startMappedFileWorker(core::encodeShellOperationRequest(request),
                L"--shell-operation-worker", L"ShellOperation", title))
        {
            clearShelfOnOperationSuccess_ = clearShelfOnSuccess;
            return true;
        }
        return false;
    }

    void AppWindow::beginFileSystemPathSnapshot(PathSnapshotAction action, Text title,
        std::wstring context)
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        shellSnapshotPathAction_ = action;
        shellSnapshotContext_ = std::move(context);
        core::ShellOperationRequest request;
        beginShellOperationSnapshot(std::move(request), title, nullptr, false,
            ShellSnapshotPurpose::fileSystemPaths, false, 256);
        if (!shellSnapshotActive_ || shellSnapshotPurpose_ != ShellSnapshotPurpose::fileSystemPaths)
        {
            shellSnapshotPathAction_ = PathSnapshotAction::none;
            shellSnapshotContext_.clear();
        }
    }

    void AppWindow::finishFileSystemPathSnapshot(PathSnapshotAction action,
        std::vector<std::wstring> paths, std::wstring context)
    {
        switch (action)
        {
        case PathSnapshotAction::compress:
        {
            if (context.empty() || paths.empty())
            {
                MessageBoxW(window_, L"Select one or more file-system items first.",
                    localizer_(Text::title), MB_OK | MB_ICONINFORMATION);
                return;
            }
            if (!promptForArchive(true)) return;
            core::ArchiveRequest request;
            request.operation = archiveOperationChoice_;
            request.collision = archiveCollisionChoice_;
            request.workingDirectory = toUtf16(context);
            request.target = toUtf16(context + L"\\" + archiveName_);
            request.password = toUtf16(archivePassword_);
            request.paths.reserve(paths.size());
            for (const auto& path : paths) request.paths.push_back(toUtf16(path));
            startArchiveWorker(std::move(request), Text::compress);
            return;
        }
        case PathSnapshotAction::bulkRename:
        {
            if (paths.size() < 2)
            {
                MessageBoxW(window_, L"Bulk rename supports 2 to 256 file-system items.",
                    localizer_(Text::title), MB_OK | MB_ICONINFORMATION);
                return;
            }
            if (!promptForName(Text::bulkRename, Text::bulkRenameInstructions, false)) return;
            core::BulkRenameRequest request;
            request.baseName = toUtf16(nameInput_);
            request.paths.reserve(paths.size());
            for (const auto& path : paths) request.paths.push_back(toUtf16(path));
            startBulkRenameWorker(std::move(request));
            return;
        }
        case PathSnapshotAction::folderFromSelection:
        {
            if (context.empty() || paths.empty())
            {
                MessageBoxW(window_, L"This command supports 1 to 256 file-system items.",
                    localizer_(Text::title), MB_OK | MB_ICONINFORMATION);
                return;
            }
            if (!promptForName(Text::folderFromSelection,
                    Text::folderFromSelectionInstructions, true)) return;
            core::FolderSelectionRequest request;
            request.workingDirectory = toUtf16(context);
            request.folderName = toUtf16(nameInput_);
            request.paths.reserve(paths.size());
            for (const auto& path : paths) request.paths.push_back(toUtf16(path));
            startFolderSelectionWorker(std::move(request));
            return;
        }
        case PathSnapshotAction::editTags:
        {
            if (paths.empty())
            {
                MessageBoxW(window_, L"Select one or more file-system items first.",
                    localizer_(Text::title), MB_OK | MB_ICONINFORMATION);
                return;
            }
            if (!promptForTags(false)) return;
            std::vector<std::wstring> normalizedTags;
            if (!core::normalizeTags(tagInput_, normalizedTags))
            {
                showError(localizer_(Text::editTags), HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
                return;
            }
            core::TagRequest request;
            request.tags.reserve(normalizedTags.size());
            request.paths.reserve(paths.size());
            for (const auto& tag : normalizedTags) request.tags.push_back(toUtf16(tag));
            for (const auto& path : paths) request.paths.push_back(toUtf16(path));
            // ponytail: The bounded mapping avoids Windows' command-line ceiling and gives
            // large tag edits the same cancellable, isolated task lifecycle as file operations.
            (void)startMappedFileWorker(core::encodeTagRequest(request),
                L"--tag-set-worker", L"TagSet", Text::editTags);
            return;
        }
        case PathSnapshotAction::none:
            return;
        }
    }

    void AppWindow::beginShellOperationSnapshot(core::ShellOperationRequest request, Text title,
        IShellItemArray* source, bool clearShelfOnSuccess, ShellSnapshotPurpose purpose,
        bool quoteClipboardPaths, std::size_t maximumItems,
        const std::vector<std::uint32_t>* sourceOrder)
    {
        // ponytail: Shell identity resolution is cooperative and bounded. Retaining the array
        // preserves virtual/provider items while every tick yields back to the frame pump.
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        IShellItemArray* items = source;
        if (items != nullptr)
            items->AddRef();
        else
        {
            ExplorerBrowserHost* browser = activeBrowser();
            if (browser == nullptr || FAILED(browser->selectedItems(&items))) items = nullptr;
        }
        DWORD sourceCount{};
        const HRESULT countStatus = items != nullptr ? items->GetCount(&sourceCount) : E_FAIL;
        DWORD count = sourceOrder == nullptr ? sourceCount :
            static_cast<DWORD>(sourceOrder->size());
        bool validOrder = sourceOrder == nullptr;
        shellSnapshotOrder_.clear();
        if (SUCCEEDED(countStatus) && sourceOrder != nullptr)
        {
            try
            {
                validOrder = core::validShelfOrder(*sourceOrder, sourceCount, maximumItems);
                if (validOrder) shellSnapshotOrder_ = *sourceOrder;
            }
            catch (...)
            {
                if (items != nullptr) items->Release();
                showError(localizer_(title), E_OUTOFMEMORY);
                return;
            }
        }
        if (FAILED(countStatus) || !validOrder || count == 0 || count > maximumItems)
        {
            if (items != nullptr) items->Release();
            shellSnapshotOrder_.clear();
            const std::wstring message = L"Select 1 to " + std::to_wstring(maximumItems) +
                L" Shell items first.";
            MessageBoxW(window_, message.c_str(),
                localizer_(Text::title), MB_OK | MB_ICONINFORMATION);
            return;
        }
        request.items.clear();
        try
        {
            if (purpose == ShellSnapshotPurpose::shellOperation)
                request.items.reserve(count);
            else if (purpose == ShellSnapshotPurpose::fileSystemPaths)
                shellSnapshotFileSystemPaths_.reserve(count);
        }
        catch (...)
        {
            items->Release();
            shellSnapshotOrder_.clear();
            showError(localizer_(title), E_OUTOFMEMORY);
            return;
        }
        if (!shellSnapshotCursor_.start(count, maximumItems))
        {
            items->Release();
            shellSnapshotOrder_.clear();
            showError(localizer_(title), HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return;
        }
        shellSnapshotItems_ = items;
        shellSnapshotRequest_ = std::move(request);
        shellSnapshotTitle_ = title;
        shellSnapshotClearsShelf_ = clearShelfOnSuccess;
        shellSnapshotPurpose_ = purpose;
        shellSnapshotQuotesClipboardPaths_ = quoteClipboardPaths;
        shellSnapshotClipboardText_.clear();
        shellSnapshotFileSystemPaths_.clear();
        shellSnapshotFileSystemCharacters_ = 0;
        if (purpose != ShellSnapshotPurpose::fileSystemPaths)
        {
            shellSnapshotPathAction_ = PathSnapshotAction::none;
            shellSnapshotContext_.clear();
        }
        shellSnapshotActive_ = true;
        const std::uint32_t generation = ++shellSnapshotGeneration_;
        gitPaneVisible_ = true;
        gitPanelTitleText_ = title;
        SetWindowTextW(gitPanelHeader_, localizer_(title));
        SetWindowTextW(gitOutput_, localizer_(Text::gitRunning));
        SetWindowTextW(gitCancelButton_, localizer_(Text::cancel));
        EnableWindow(gitCancelButton_, TRUE);
        LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
        SetWindowLongPtrW(gitProgress_, GWL_STYLE, style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
        SetWindowPos(gitProgress_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(gitProgress_, PBM_SETRANGE32, 0, 100);
        SendMessageW(gitProgress_, PBM_SETPOS, 0, 0);
        for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
            ShowWindow(control, SW_SHOW);
        layoutChildren(clientWidth_, clientHeight_);
        if (!PostMessageW(window_, shellSnapshotMessage, generation, 0))
        {
            const DWORD failure = GetLastError();
            finishShellOperationSnapshot(failure == ERROR_SUCCESS ?
                ERROR_INVALID_WINDOW_HANDLE : failure);
        }
    }

    void AppWindow::processShellOperationSnapshot(std::uint32_t generation)
    {
        if (!shellSnapshotActive_ || generation != shellSnapshotGeneration_ ||
            shellSnapshotItems_ == nullptr)
            return;
        constexpr std::size_t itemsPerTick = 16;
        const ULONGLONG deadline = GetTickCount64() + 8;
        DWORD failure = ERROR_SUCCESS;
        for (std::size_t offset = 0; offset < itemsPerTick &&
            shellSnapshotCursor_.active(); ++offset)
        {
            const core::IndexBatch itemBatch = shellSnapshotCursor_.next(1);
            IShellItem* item{};
            PWSTR raw{};
            const DWORD sourceIndex = shellSnapshotOrder_.empty() ?
                static_cast<DWORD>(itemBatch.first) : shellSnapshotOrder_[itemBatch.first];
            const HRESULT itemStatus = shellSnapshotItems_->GetItemAt(sourceIndex, &item);
            HRESULT nameStatus = itemStatus;
            if (SUCCEEDED(itemStatus) && item != nullptr)
            {
                if (shellSnapshotPurpose_ == ShellSnapshotPurpose::clipboardPaths ||
                    shellSnapshotPurpose_ == ShellSnapshotPurpose::fileSystemPaths)
                {
                    nameStatus = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
                    if (FAILED(nameStatus) &&
                        shellSnapshotPurpose_ == ShellSnapshotPurpose::clipboardPaths)
                    {
                        if (raw != nullptr) CoTaskMemFree(raw);
                        raw = nullptr;
                        nameStatus = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw);
                    }
                }
                else
                {
                    nameStatus = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw);
                }
            }
            if (SUCCEEDED(nameStatus) && raw != nullptr && *raw != L'\0')
            {
                try
                {
                    if (shellSnapshotPurpose_ == ShellSnapshotPurpose::clipboardPaths)
                    {
                        const std::size_t pathLength = std::wcslen(raw);
                        if (!core::appendClipboardPath(shellSnapshotClipboardText_,
                                std::wstring_view(raw, pathLength),
                                shellSnapshotQuotesClipboardPaths_))
                            failure = ERROR_BUFFER_OVERFLOW;
                    }
                    else if (shellSnapshotPurpose_ == ShellSnapshotPurpose::fileSystemPaths)
                    {
                        const std::size_t pathLength = std::wcslen(raw);
                        if (pathLength > core::maxClipboardPathCharacters -
                                std::min(core::maxClipboardPathCharacters,
                                    shellSnapshotFileSystemCharacters_))
                        {
                            failure = ERROR_BUFFER_OVERFLOW;
                        }
                        else
                        {
                            shellSnapshotFileSystemPaths_.emplace_back(raw, pathLength);
                            shellSnapshotFileSystemCharacters_ += pathLength;
                        }
                    }
                    else
                    {
                        shellSnapshotRequest_.items.push_back(toUtf16(raw));
                    }
                }
                catch (...)
                {
                    failure = ERROR_NOT_ENOUGH_MEMORY;
                }
            }
            else
            {
                failure = HRESULT_FACILITY(nameStatus) == FACILITY_WIN32 ?
                    HRESULT_CODE(nameStatus) : ERROR_INVALID_DATA;
            }
            if (raw != nullptr) CoTaskMemFree(raw);
            if (item != nullptr) item->Release();
            if (failure != ERROR_SUCCESS) break;
            if (GetTickCount64() >= deadline) break;
        }
        if (failure != ERROR_SUCCESS)
        {
            finishShellOperationSnapshot(failure);
            return;
        }
        const std::size_t total = shellSnapshotCursor_.total();
        const std::size_t processed = shellSnapshotCursor_.processed();
        const WPARAM percent = total == 0 ? 100 : static_cast<WPARAM>((processed * 100) / total);
        SendMessageW(gitProgress_, PBM_SETPOS, percent, 0);
        if (shellSnapshotCursor_.active())
        {
            if (!PostMessageW(window_, shellSnapshotMessage, generation, 0))
            {
                const DWORD postFailure = GetLastError();
                finishShellOperationSnapshot(postFailure == ERROR_SUCCESS ?
                    ERROR_INVALID_WINDOW_HANDLE : postFailure);
            }
            return;
        }
        finishShellOperationSnapshot(ERROR_SUCCESS);
    }

    void AppWindow::finishShellOperationSnapshot(DWORD result)
    {
        if (!shellSnapshotActive_) return;
        core::ShellOperationRequest request = std::move(shellSnapshotRequest_);
        const Text title = shellSnapshotTitle_;
        const bool clearShelfOnSuccess = shellSnapshotClearsShelf_;
        const ShellSnapshotPurpose purpose = shellSnapshotPurpose_;
        std::wstring clipboardText = std::move(shellSnapshotClipboardText_);
        const PathSnapshotAction pathAction = shellSnapshotPathAction_;
        std::vector<std::wstring> fileSystemPaths =
            std::move(shellSnapshotFileSystemPaths_);
        std::wstring snapshotContext = std::move(shellSnapshotContext_);
        shellSnapshotRequest_ = {};
        shellSnapshotClearsShelf_ = false;
        shellSnapshotPurpose_ = ShellSnapshotPurpose::shellOperation;
        shellSnapshotQuotesClipboardPaths_ = false;
        shellSnapshotClipboardText_.clear();
        shellSnapshotPathAction_ = PathSnapshotAction::none;
        shellSnapshotFileSystemPaths_.clear();
        shellSnapshotFileSystemCharacters_ = 0;
        shellSnapshotContext_.clear();
        shellSnapshotOrder_.clear();
        shellSnapshotCursor_.cancel();
        shellSnapshotActive_ = false;
        if (shellSnapshotItems_ != nullptr)
        {
            shellSnapshotItems_->Release();
            shellSnapshotItems_ = nullptr;
        }
        if (result == ERROR_SUCCESS && purpose == ShellSnapshotPurpose::clipboardPaths)
        {
            result = writeClipboardText(window_, clipboardText);
            if (result == ERROR_SUCCESS)
            {
                gitPaneVisible_ = false;
                for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
                    ShowWindow(control, SW_HIDE);
                updateStatus();
                layoutChildren(clientWidth_, clientHeight_);
                return;
            }
        }
        if (result == ERROR_SUCCESS && purpose == ShellSnapshotPurpose::fileSystemPaths)
        {
            gitPaneVisible_ = false;
            for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
                ShowWindow(control, SW_HIDE);
            layoutChildren(clientWidth_, clientHeight_);
            finishFileSystemPathSnapshot(pathAction, std::move(fileSystemPaths),
                std::move(snapshotContext));
            return;
        }
        if (result == ERROR_SUCCESS)
        {
            if (!startShellOperation(std::move(request), title, clearShelfOnSuccess))
            {
                ShowWindow(gitProgress_, SW_HIDE);
                SetWindowTextW(gitCancelButton_, localizer_(Text::close));
                SetWindowTextW(gitOutput_, localizer_(Text::gitFailed));
                layoutChildren(clientWidth_, clientHeight_);
            }
            return;
        }
        ShowWindow(gitProgress_, SW_HIDE);
        SetWindowTextW(gitCancelButton_, localizer_(Text::close));
        EnableWindow(gitCancelButton_, TRUE);
        const std::wstring summary = result == ERROR_CANCELLED ? localizer_(Text::gitCanceled) :
            std::wstring(localizer_(Text::gitFailed)) + std::to_wstring(result);
        SetWindowTextW(gitPanelHeader_, localizer_(title));
        SetWindowTextW(gitOutput_, summary.c_str());
        layoutChildren(clientWidth_, clientHeight_);
    }

    void AppWindow::beginTagResultMaterialization(DWORD workerResult)
    {
        tagResultExpectedCount_ = 0;
        if (tagSearchCancelEvent_ != nullptr)
        {
            CloseHandle(tagSearchCancelEvent_);
            tagSearchCancelEvent_ = nullptr;
        }
        if (tagSearchRequestMapping_ != nullptr)
        {
            CloseHandle(tagSearchRequestMapping_);
            tagSearchRequestMapping_ = nullptr;
        }
        DWORD failure = workerResult;
        if (failure == ERROR_SUCCESS && (tagSearchPath_.empty() || tagResultsActive_))
            failure = ERROR_INVALID_DATA;
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mapping{};
        const wchar_t* view{};
        std::size_t characters{};
        if (failure == ERROR_SUCCESS)
        {
            file = CreateFileW(tagSearchPath_.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (file == INVALID_HANDLE_VALUE) failure = GetLastError();
        }
        LARGE_INTEGER bytes{};
        if (failure == ERROR_SUCCESS && (!GetFileSizeEx(file, &bytes) ||
                bytes.QuadPart < static_cast<LONGLONG>((core::tagResultHeaderCharacters + 1) *
                    sizeof(wchar_t)) ||
                bytes.QuadPart > static_cast<LONGLONG>(
                    core::maxTagResultCharacters * sizeof(wchar_t)) ||
                bytes.QuadPart % sizeof(wchar_t) != 0))
            failure = ERROR_INVALID_DATA;
        if (failure == ERROR_SUCCESS)
        {
            mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping == nullptr) failure = GetLastError();
        }
        if (failure == ERROR_SUCCESS)
        {
            view = static_cast<const wchar_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
            if (view == nullptr) failure = GetLastError();
            else characters = static_cast<std::size_t>(bytes.QuadPart / sizeof(wchar_t));
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (failure == ERROR_SUCCESS)
        {
            if (!tagResultCursor_.start(view, characters))
                failure = ERROR_INVALID_DATA;
            else
                tagResultExpectedCount_ = tagResultCursor_.expectedCount();
        }
        ExplorerBrowserHost* browser = failure == ERROR_SUCCESS ? tagSearchTargetBrowser_ : nullptr;
        if (failure == ERROR_SUCCESS && browser == nullptr) failure = ERROR_INVALID_WINDOW_HANDLE;
        if (failure == ERROR_SUCCESS)
        {
            std::wstring title;
            try
            {
                title = std::wstring(localizer_(Text::searchResults)) + L": " + tagSearchTag_;
            }
            catch (...)
            {
                failure = ERROR_NOT_ENOUGH_MEMORY;
            }
            if (failure == ERROR_SUCCESS)
            {
                const HRESULT status = browser->beginResults(title);
                if (FAILED(status)) failure = HRESULT_FACILITY(status) == FACILITY_WIN32 ?
                    HRESULT_CODE(status) : ERROR_GEN_FAILURE;
            }
        }
        if (failure != ERROR_SUCCESS)
        {
            tagResultCursor_.cancel();
            if (view != nullptr) UnmapViewOfFile(view);
            if (mapping != nullptr) CloseHandle(mapping);
            if (!tagSearchPath_.empty()) DeleteFileW(tagSearchPath_.c_str());
            tagSearchPath_.clear();
            tagSearchTag_.clear();
            if (tagSearchTargetBrowser_ != nullptr)
            {
                tagSearchTargetBrowser_->Release();
                tagSearchTargetBrowser_ = nullptr;
            }
            ShowWindow(gitProgress_, SW_HIDE);
            SetWindowTextW(gitCancelButton_, localizer_(Text::close));
            EnableWindow(gitCancelButton_, TRUE);
            if (failure == ERROR_CANCELLED)
            {
                SetWindowTextW(gitOutput_, localizer_(Text::gitCanceled));
            }
            else
            {
                SetWindowTextW(gitOutput_, localizer_(Text::gitFailed));
                showError(tagSearchUpdatesChip_ ? L"Find tags" : L"Search",
                    HRESULT_FROM_WIN32(failure));
            }
            layoutChildren(clientWidth_, clientHeight_);
            return;
        }
        tagResultsBrowser_ = browser;
        tagSearchTargetBrowser_ = nullptr;
        tagResultMapping_ = mapping;
        tagResultView_ = view;
        tagResultsActive_ = true;
        const std::uint32_t generation = ++tagResultGeneration_;
        gitPaneVisible_ = true;
        gitPanelTitleText_ = tagSearchTaskTitle_;
        SetWindowTextW(gitPanelHeader_, localizer_(tagSearchTaskTitle_));
        SetWindowTextW(gitOutput_, localizer_(Text::gitRunning));
        SetWindowTextW(gitCancelButton_, localizer_(Text::cancel));
        EnableWindow(gitCancelButton_, TRUE);
        LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
        SetWindowLongPtrW(gitProgress_, GWL_STYLE, style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
        SetWindowPos(gitProgress_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(gitProgress_, PBM_SETRANGE32, 0, 100);
        SendMessageW(gitProgress_, PBM_SETPOS, 0, 0);
        for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
            ShowWindow(control, SW_SHOW);
        layoutChildren(clientWidth_, clientHeight_);
        if (!PostMessageW(window_, tagResultBatchMessage, generation, 0))
        {
            const DWORD postFailure = GetLastError();
            finishTagResultMaterialization(postFailure == ERROR_SUCCESS ?
                ERROR_INVALID_WINDOW_HANDLE : postFailure);
        }
    }

    void AppWindow::processTagResultMaterialization(std::uint32_t generation)
    {
        if (!tagResultsActive_ || generation != tagResultGeneration_ ||
            tagResultsBrowser_ == nullptr)
            return;
        constexpr std::size_t itemsPerDispatch = 8;
        const ULONGLONG deadline = GetTickCount64() + 8;
        for (std::size_t index = 0; index < itemsPerDispatch; ++index)
        {
            std::wstring_view path;
            const core::TagResultStep step = tagResultCursor_.next(path);
            if (step == core::TagResultStep::complete)
            {
                finishTagResultMaterialization(tagResultCursor_.count() ==
                    tagResultExpectedCount_ ? ERROR_SUCCESS : ERROR_INVALID_DATA);
                return;
            }
            if (step == core::TagResultStep::invalid)
            {
                finishTagResultMaterialization(ERROR_INVALID_DATA);
                return;
            }
            const HRESULT status = tagResultsBrowser_->addResult(path);
            if (FAILED(status))
            {
                finishTagResultMaterialization(HRESULT_FACILITY(status) == FACILITY_WIN32 ?
                    HRESULT_CODE(status) : ERROR_GEN_FAILURE);
                return;
            }
            if (GetTickCount64() >= deadline) break;
        }
        const std::size_t processed = tagResultCursor_.count();
        const WPARAM percent = tagResultExpectedCount_ == 0 ? 100 :
            static_cast<WPARAM>((processed * 100) / tagResultExpectedCount_);
        SendMessageW(gitProgress_, PBM_SETPOS, percent > 100 ? 100 : percent, 0);
        if (!PostMessageW(window_, tagResultBatchMessage, generation, 0))
        {
            const DWORD postFailure = GetLastError();
            finishTagResultMaterialization(postFailure == ERROR_SUCCESS ?
                ERROR_INVALID_WINDOW_HANDLE : postFailure);
        }
    }

    void AppWindow::finishTagResultMaterialization(DWORD result)
    {
        if (!tagResultsActive_) return;
        if (result == ERROR_SUCCESS && tagResultsBrowser_ != nullptr)
        {
            const HRESULT status = tagResultsBrowser_->finishResults();
            if (FAILED(status)) result = HRESULT_FACILITY(status) == FACILITY_WIN32 ?
                HRESULT_CODE(status) : ERROR_GEN_FAILURE;
        }
        else if (tagResultsBrowser_ != nullptr)
        {
            tagResultsBrowser_->abortResults();
        }
        if (tagResultsBrowser_ != nullptr)
        {
            tagResultsBrowser_->Release();
            tagResultsBrowser_ = nullptr;
        }
        tagResultsActive_ = false;
        tagResultExpectedCount_ = 0;
        tagResultCursor_.cancel();
        if (tagResultView_ != nullptr)
        {
            UnmapViewOfFile(tagResultView_);
            tagResultView_ = nullptr;
        }
        if (tagResultMapping_ != nullptr)
        {
            CloseHandle(tagResultMapping_);
            tagResultMapping_ = nullptr;
        }
        if (!tagSearchPath_.empty()) DeleteFileW(tagSearchPath_.c_str());
        tagSearchPath_.clear();
        ShowWindow(gitProgress_, SW_HIDE);
        SetWindowTextW(gitCancelButton_, localizer_(Text::close));
        EnableWindow(gitCancelButton_, TRUE);
        const std::wstring summary = result == ERROR_SUCCESS ? localizer_(Text::gitCompleted) :
            (result == ERROR_CANCELLED ? std::wstring(localizer_(Text::gitCanceled)) :
                std::wstring(localizer_(Text::gitFailed)) + std::to_wstring(result));
        SetWindowTextW(gitPanelHeader_, localizer_(tagSearchTaskTitle_));
        SetWindowTextW(gitOutput_, summary.c_str());
        if (result == ERROR_SUCCESS && tagSearchUpdatesChip_)
        {
            tagChipText_ = tagSearchTag_;
            currentTagColor_ = loadTagColor(tagSearchTag_);
            updateTagChip();
        }
        tagSearchTag_.clear();
        updateStatus();
        layoutChildren(clientWidth_, clientHeight_);
    }

    void AppWindow::createShellItem(bool folder)
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring destination = browser != nullptr ? browser->parsingName() : std::wstring{};
        if (destination.empty())
        {
            MessageBoxW(window_, L"This location cannot create file-system items.",
                localizer_(Text::title), MB_OK | MB_ICONINFORMATION);
            return;
        }
        core::ShellOperationRequest request;
        request.operation = folder ? core::ShellOperation::createFolder : core::ShellOperation::createFile;
        request.destination = toUtf16(destination);
        std::wstring name = localizer_(folder ? Text::newFolder : Text::newTextFile);
        if (!folder) name += L".txt";
        request.newName = toUtf16(name);
        (void)startShellOperation(std::move(request),
            folder ? Text::newFolder : Text::newTextFile);
    }

    void AppWindow::deleteSelection(bool permanent)
    {
        core::ShellOperationRequest request;
        request.operation = permanent ? core::ShellOperation::deletePermanent :
            core::ShellOperation::deleteRecycle;
        request.confirmPermanent = settings_.enabled(core::confirmPermanentDelete);
        beginShellOperationSnapshot(std::move(request),
            permanent ? Text::permanentDelete : Text::deleteItem);
    }

    void AppWindow::emptyRecycleBin()
    {
        core::ShellOperationRequest request;
        request.operation = core::ShellOperation::emptyRecycleBin;
        (void)startShellOperation(std::move(request), Text::emptyRecycleBin);
    }

    void AppWindow::restoreAllRecycleBin()
    {
        core::ShellOperationRequest request;
        request.operation = core::ShellOperation::restoreRecycleBin;
        (void)startShellOperation(std::move(request), Text::restoreAllRecycleBin);
    }

    void AppWindow::showSettings()
    {
        DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SETTINGS), window_,
            &AppWindow::settingsProcedure, reinterpret_cast<LPARAM>(this));
    }

    void AppWindow::showKeyboardShortcuts()
    {
        DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SHORTCUTS), window_,
            &AppWindow::shortcutProcedure, reinterpret_cast<LPARAM>(this));
    }

    bool AppWindow::promptForName(Text title, Text instructions, bool allowDot,
        bool alternateStream, bool genericText, HWND owner)
    {
        nameInput_.clear();
        namePromptTitle_ = title;
        namePromptInstructions_ = instructions;
        nameAllowDot_ = allowDot;
        nameAlternateStream_ = alternateStream;
        nameGenericText_ = genericText;
        return DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_NAME_PROMPT),
            owner != nullptr ? owner : window_,
            &AppWindow::nameProcedure, reinterpret_cast<LPARAM>(this)) == IDOK;
    }

    void AppWindow::bulkRename()
    {
        ExplorerBrowserHost* browser = activeBrowser();
        if (browser == nullptr || browser->selectedCount() < 2)
        {
            MessageBoxW(window_, L"Select at least two items first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        beginFileSystemPathSnapshot(PathSnapshotAction::bulkRename, Text::bulkRename);
    }

    void AppWindow::createFolderFromSelection()
    {
        ExplorerBrowserHost* browser = activeBrowser();
        if (browser == nullptr || browser->selectedCount() == 0)
        {
            MessageBoxW(window_, L"Select one or more items first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::wstring workingDirectory = browser->filesystemPath();
        if (workingDirectory.empty())
        {
            MessageBoxW(window_, L"This command supports 1 to 256 file-system items.",
                localizer_(Text::title), MB_OK | MB_ICONINFORMATION);
            return;
        }
        beginFileSystemPathSnapshot(PathSnapshotAction::folderFromSelection,
            Text::folderFromSelection, workingDirectory);
    }

    void AppWindow::flattenFolder()
    {
        const std::wstring path = selectedFileSystemPath();
        if (path.empty())
        {
            MessageBoxW(window_, L"Select exactly one folder first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (MessageBoxW(window_, localizer_(Text::flattenWarning),
                localizer_(Text::flattenFolder), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        {
            return;
        }
        startFlattenWorker(path);
    }

    void AppWindow::editAlternateStream()
    {
        const std::wstring path = selectedFileSystemPath();
        if (path.empty())
        {
            MessageBoxW(window_, L"Select exactly one file first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!promptForName(Text::editAlternateStream, Text::alternateStreamInstructions,
                true, true))
        {
            return;
        }
        const std::wstring streamPath = path + L":" + nameInput_;
        const std::wstring executable = systemExecutable(L"notepad.exe");
        const std::wstring workingDirectory = activeBrowser() != nullptr ?
            activeBrowser()->filesystemPath() : std::wstring{};
        if (streamPath.size() >= 32767)
        {
            showError(L"Edit alternate stream", HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE));
            return;
        }
        if (executable.empty())
        {
            showError(L"Edit alternate stream", HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
            return;
        }
        if (!launchProcess(executable, {streamPath}, workingDirectory, 0, SW_SHOWNORMAL))
        {
            showError(L"Edit alternate stream", HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void AppWindow::toggleQuickPreview()
    {
        if (settings_.previewProvider == core::PreviewProvider::windows)
        {
            toggleWindowsPreview();
            return;
        }
        std::wstring path = selectedFileSystemPath();
        if (path.empty())
        {
            toggleWindowsPreview();
            return;
        }
        if (externalPreviewActive_)
        {
            launchPreviewPopup(false, std::move(path));
            externalPreviewActive_ = false;
            return;
        }
        launchPreviewPopup(false, std::move(path));
        externalPreviewActive_ = true;
    }

    void AppWindow::toggleWindowsPreview()
    {
        if (textPreviewVisible_)
        {
            textPreviewVisible_ = false;
            ++previewGeneration_;
            KillTimer(window_, previewTimerId);
            ShowWindow(textPreviewHeader_, SW_HIDE);
            ShowWindow(textPreviewEdit_, SW_HIDE);
            SetWindowTextW(textPreviewEdit_, L"");
            layoutChildren(clientWidth_, clientHeight_);
            return;
        }
        const std::wstring path = selectedFileSystemPath();
        if (path.empty() || !core::supportsTextPreview(path))
        {
            if (ExplorerBrowserHost* browser = activeBrowser(); browser != nullptr)
                browser->togglePreviewPane();
            return;
        }
        if (ExplorerBrowserHost* browser = activeBrowser(); browser != nullptr &&
            browser->previewPaneVisible())
            browser->togglePreviewPane();
        textPreviewVisible_ = true;
        ShowWindow(textPreviewHeader_, SW_SHOW);
        ShowWindow(textPreviewEdit_, SW_SHOW);
        layoutChildren(clientWidth_, clientHeight_);
        scheduleTextPreview();
    }

    void AppWindow::launchPreviewPopup(bool switchSelection, std::wstring path)
    {
        if (path.empty()) path = selectedFileSystemPath();
        if (path.empty()) return;
        const bool launchNow = previewPopupQueue_.submit(core::PreviewRequest{
            switchSelection, !switchSelection && externalPreviewActive_, std::move(path)});
        if (launchNow) startQueuedPreviewPopup();
    }

    void AppWindow::startQueuedPreviewPopup()
    {
        const core::PreviewRequest* const request = previewPopupQueue_.current();
        if (request == nullptr) return;
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size())
        {
            completePreviewPopup(false);
            return;
        }
        executable.resize(length);
        if (!queueProcessLaunch(executable, {L"--preview-popup",
                std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
                std::to_wstring(static_cast<std::uint32_t>(settings_.previewProvider)),
                request->switchSelection ? L"1" : L"0", request->path}, {},
                CREATE_NO_WINDOW, SW_HIDE, previewPopupCompleteMessage, 0))
        {
            completePreviewPopup(false);
        }
    }

    void AppWindow::completePreviewPopup(bool success)
    {
        const core::PreviewCompletion completion = previewPopupQueue_.complete();
        if (!completion.recognized) return;
        if (!success && !completion.switchSelection && !completion.closing)
        {
            previewPopupQueue_.clear();
            externalPreviewActive_ = false;
            toggleWindowsPreview();
            return;
        }
        if (completion.launchNext) startQueuedPreviewPopup();
    }

    void AppWindow::scheduleTextPreview()
    {
        if (!textPreviewVisible_) return;
        ++previewGeneration_;
        if (previewGeneration_ == 0) ++previewGeneration_;
        SetWindowTextW(textPreviewEdit_, localizer_(Text::previewLoading));
        KillTimer(window_, previewTimerId);
        SetTimer(window_, previewTimerId, 150, nullptr);
    }

    void AppWindow::startTextPreviewWorker()
    {
        if (!textPreviewVisible_ || previewWorkerActive_) return;
        const std::wstring path = selectedFileSystemPath();
        if (path.empty() || !core::supportsTextPreview(path))
        {
            SetWindowTextW(textPreviewHeader_, localizer_(Text::quickPreview));
            SetWindowTextW(textPreviewEdit_, localizer_(Text::previewUnavailable));
            return;
        }
        SetWindowTextW(textPreviewHeader_, PathFindFileNameW(path.c_str()));
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        previewResultPath_ = uniqueTemporaryPath(L"Preview");
        if (length == 0 || length >= executable.size() || previewResultPath_.empty())
        {
            previewResultPath_.clear();
            SetWindowTextW(textPreviewEdit_, localizer_(Text::previewUnavailable));
            return;
        }
        executable.resize(length);
        previewWorkerGeneration_ = previewGeneration_;
        previewWorkerActive_ = true;
        if (!queueProcessLaunch(executable, {L"--preview-text",
                std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
                std::to_wstring(previewWorkerGeneration_), path, previewResultPath_},
                {}, CREATE_NO_WINDOW, SW_HIDE, textPreviewCompleteMessage,
                previewWorkerGeneration_))
            (void)PostMessageW(window_, textPreviewCompleteMessage,
                previewWorkerGeneration_, GetLastError());
    }

    void AppWindow::createShortcut()
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring folder = browser != nullptr ? browser->filesystemPath() : std::wstring{};
        if (folder.empty())
        {
            MessageBoxW(window_, L"Open a file-system folder first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SHORTCUT_EDITOR), window_,
                &AppWindow::linkProcedure, reinterpret_cast<LPARAM>(this)) != IDOK)
        {
            return;
        }
        std::wstring filename = linkName_;
        if (_wcsicmp(PathFindExtensionW(filename.c_str()), L".lnk") != 0)
        {
            filename += L".lnk";
        }
        if (!core::validWindowsFilename(filename) || folder.size() + filename.size() + 2 >= 32767)
        {
            showError(L"Create shortcut", HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE));
            return;
        }
        core::ShellArtifactRequest request;
        request.operation = core::ShellArtifactOperation::createShortcut;
        request.destinationFolder = toUtf16(folder);
        request.name = toUtf16(filename);
        request.target = toUtf16(linkTarget_);
        request.arguments = toUtf16(linkArguments_);
        request.workingDirectory = toUtf16(linkWorkingDirectory_);
        request.icon = toUtf16(linkIcon_);
        startShellArtifactWorker(std::move(request), Text::createShortcut);
    }

    void AppWindow::editShortcut()
    {
        const std::wstring path = selectedFileSystemPath();
        if (path.empty() || _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") != 0)
        {
            MessageBoxW(window_, L"Select exactly one shortcut first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (ExplorerBrowserHost* browser = activeBrowser(); browser != nullptr)
        {
            const HRESULT status = browser->invokeSelectionVerb("properties");
            if (FAILED(status)) showError(L"Edit shortcut", status);
        }
    }

    void AppWindow::createLibrary()
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!promptForName(Text::createLibrary, Text::libraryInstructions, true))
        {
            return;
        }
        core::ShellArtifactRequest request;
        request.operation = core::ShellArtifactOperation::createLibrary;
        request.name = toUtf16(nameInput_);
        startShellArtifactWorker(std::move(request), Text::createLibrary);
    }

    void AppWindow::editLibrary()
    {
        const std::wstring path = selectedFileSystemPath();
        if (path.empty() ||
            _wcsicmp(PathFindExtensionW(path.c_str()), L".library-ms") != 0)
        {
            MessageBoxW(window_, L"Select exactly one library first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (ExplorerBrowserHost* browser = activeBrowser(); browser != nullptr)
        {
            const HRESULT status = browser->invokeSelectionVerb("properties");
            if (FAILED(status)) showError(L"Edit library", status);
        }
    }

    void AppWindow::showCommandPalette()
    {
        const INT_PTR command = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_COMMAND_PALETTE),
            window_, &AppWindow::commandPaletteProcedure, reinterpret_cast<LPARAM>(this));
        paletteCommands_.clear();
        if (command >= CommandId::newTab)
        {
            dispatchCommand(static_cast<int>(command));
        }
    }

    void AppWindow::refreshCommandPalette(HWND dialog)
    {
        struct Entry final { CommandId command; Text text; };
        static constexpr std::array entries{
            Entry{CommandId::newTab, Text::newTab}, Entry{CommandId::newWindow, Text::newWindow},
            Entry{CommandId::duplicateTab, Text::duplicateTab},
            Entry{CommandId::openInNewTab, Text::openInNewTab},
            Entry{CommandId::openInNewWindow, Text::openInNewWindow},
            Entry{CommandId::openInOtherPane, Text::openInOtherPane},
            Entry{CommandId::openCurrentFolderOtherPane, Text::openCurrentFolderOtherPane},
            Entry{CommandId::openFileLocation, Text::openFileLocation},
            Entry{CommandId::reopenClosedTab, Text::reopenClosedTab},
            Entry{CommandId::closeTab, Text::closeTab}, Entry{CommandId::closeOtherTabs, Text::closeOtherTabs},
            Entry{CommandId::closeTabsLeft, Text::closeTabsLeft},
            Entry{CommandId::closeTabsRight, Text::closeTabsRight},
            Entry{CommandId::closeAllTabs, Text::closeAllTabs},
            Entry{CommandId::newFolder, Text::newFolder}, Entry{CommandId::newFile, Text::newTextFile},
            Entry{CommandId::createShortcut, Text::createShortcut},
            Entry{CommandId::createLibrary, Text::createLibrary},
            Entry{CommandId::emptyRecycleBin, Text::emptyRecycleBin},
            Entry{CommandId::undo, Text::undo}, Entry{CommandId::redo, Text::redo},
            Entry{CommandId::cut, Text::cut}, Entry{CommandId::copy, Text::copy},
            Entry{CommandId::paste, Text::paste},
            Entry{CommandId::pasteShortcut, Text::pasteShortcut},
            Entry{CommandId::pasteIntoFolder, Text::pasteIntoFolder},
            Entry{CommandId::copyPath, Text::copyPath},
            Entry{CommandId::copyPathQuoted, Text::copyPathQuoted}, Entry{CommandId::rename, Text::rename},
            Entry{CommandId::bulkRename, Text::bulkRename},
            Entry{CommandId::folderFromSelection, Text::folderFromSelection},
            Entry{CommandId::flattenFolder, Text::flattenFolder},
            Entry{CommandId::recycleDelete, Text::deleteItem},
            Entry{CommandId::permanentDelete, Text::permanentDelete},
            Entry{CommandId::properties, Text::properties}, Entry{CommandId::selectAll, Text::selectAll},
            Entry{CommandId::clearSelection, Text::clearSelection},
            Entry{CommandId::invertSelection, Text::invertSelection},
            Entry{CommandId::restoreRecycleBin, Text::restoreRecycleBin},
            Entry{CommandId::restoreAllRecycleBin, Text::restoreAllRecycleBin},
            Entry{CommandId::editShortcut, Text::editShortcut},
            Entry{CommandId::editLibrary, Text::editLibrary},
            Entry{CommandId::pinQuickAccess, Text::pinQuickAccess},
            Entry{CommandId::unpinQuickAccess, Text::unpinQuickAccess},
            Entry{CommandId::shelfCopy, Text::shelfCopy}, Entry{CommandId::shelfMove, Text::shelfMove},
            Entry{CommandId::shelfPaste, Text::shelfPaste}, Entry{CommandId::shelfClear, Text::shelfClear},
            Entry{CommandId::manageShelf, Text::manageShelf},
            Entry{CommandId::viewDetails, Text::details}, Entry{CommandId::viewList, Text::list},
            Entry{CommandId::viewSmall, Text::smallIcons}, Entry{CommandId::viewMedium, Text::mediumIcons},
            Entry{CommandId::viewLarge, Text::largeIcons},
            Entry{CommandId::viewExtraLarge, Text::extraLargeIcons},
            Entry{CommandId::viewTiles, Text::tiles}, Entry{CommandId::viewContent, Text::content},
            Entry{CommandId::autoSizeColumns, Text::autoSizeColumns},
            Entry{CommandId::togglePreviewPane, Text::quickPreview},
            Entry{CommandId::toggleDetailsPane, Text::detailsPane},
            Entry{CommandId::splitVertical, Text::splitVertical},
            Entry{CommandId::splitHorizontal, Text::splitHorizontal},
            Entry{CommandId::focusOtherPane, Text::focusOtherPane},
            Entry{CommandId::closePane, Text::closePane}, Entry{CommandId::refresh, Text::refresh},
            Entry{CommandId::goBack, Text::back}, Entry{CommandId::goForward, Text::forward},
            Entry{CommandId::goUp, Text::up}, Entry{CommandId::focusAddress, Text::addressCommand},
            Entry{CommandId::goHome, Text::goHome},
            Entry{CommandId::focusSearch, Text::searchCommand}, Entry{CommandId::editTags, Text::editTags},
            Entry{CommandId::filterByTag, Text::filterByTag},
            Entry{CommandId::openTerminal, Text::openTerminal},
            Entry{CommandId::openTerminalAdmin, Text::openTerminalAdmin},
            Entry{CommandId::toggleSidebar, Text::toggleSidebar},
            Entry{CommandId::fullScreen, Text::fullScreen},
            Entry{CommandId::showHiddenItems, Text::showHiddenItems},
            Entry{CommandId::showFileExtensions, Text::showFileExtensions},
            Entry{CommandId::playSelection, Text::playSelection},
            Entry{CommandId::runAsAdministrator, Text::runAsAdministrator},
            Entry{CommandId::runAsDifferentUser, Text::runAsDifferentUser},
            Entry{CommandId::runWithPowerShell, Text::runWithPowerShell},
            Entry{CommandId::rotateLeft, Text::rotateLeft},
            Entry{CommandId::rotateRight, Text::rotateRight},
            Entry{CommandId::installSelection, Text::installSelection},
            Entry{CommandId::installCertificate, Text::installCertificate},
            Entry{CommandId::setDesktopWallpaper, Text::setDesktopWallpaper},
            Entry{CommandId::setDesktopSlideshow, Text::setDesktopSlideshow},
            Entry{CommandId::openStorageSense, Text::openStorageSense},
            Entry{CommandId::manageTagColor, Text::manageTagColor},
            Entry{CommandId::settings, Text::settings},
            Entry{CommandId::keyboardShortcuts, Text::keyboardShortcuts},
            Entry{CommandId::compressArchive, Text::compress}, Entry{CommandId::extractArchive, Text::extract},
            Entry{CommandId::mapNetworkDrive, Text::mapNetworkDrive},
            Entry{CommandId::disconnectNetworkDrive, Text::disconnectNetworkDrive},
            Entry{CommandId::ftpManager, Text::ftpManager},
            Entry{CommandId::hashSelection, Text::sha256},
            Entry{CommandId::showAlternateStreams, Text::alternateStreams},
            Entry{CommandId::editAlternateStream, Text::editAlternateStream},
            Entry{CommandId::verifySignature, Text::verifySignature},
            Entry{CommandId::gitInit, Text::gitInit}, Entry{CommandId::gitClone, Text::gitClone},
            Entry{CommandId::gitCreateBranch, Text::gitCreateBranch},
            Entry{CommandId::gitSwitchBranch, Text::gitSwitchBranch},
            Entry{CommandId::gitStatus, Text::gitStatus},
            Entry{CommandId::gitFetch, Text::gitFetch}, Entry{CommandId::gitPull, Text::gitPull},
            Entry{CommandId::gitPush, Text::gitPush}, Entry{CommandId::gitSync, Text::gitSync}};
        struct Match final { int score; int command; const wchar_t* label; };
        const std::wstring query = windowText(GetDlgItem(dialog, IDC_PALETTE_QUERY));
        std::vector<Match> matches;
        matches.reserve(entries.size());
        for (const auto& entry : entries)
        {
            const wchar_t* label = localizer_(entry.text);
            const int score = core::commandMatchScore(label, query);
            if (score != core::noCommandMatch)
            {
                matches.push_back(Match{score, static_cast<int>(entry.command), label});
            }
        }
        std::sort(matches.begin(), matches.end(), [](const Match& left, const Match& right)
        {
            if (left.score != right.score)
            {
                return left.score < right.score;
            }
            return std::wstring_view(left.label) < std::wstring_view(right.label);
        });
        SendDlgItemMessageW(dialog, IDC_PALETTE_LIST, LB_RESETCONTENT, 0, 0);
        paletteCommands_.clear();
        paletteCommands_.reserve(matches.size());
        for (const auto& match : matches)
        {
            SendDlgItemMessageW(dialog, IDC_PALETTE_LIST, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(match.label));
            paletteCommands_.push_back(match.command);
        }
        if (!matches.empty())
        {
            SendDlgItemMessageW(dialog, IDC_PALETTE_LIST, LB_SETCURSEL, 0, 0);
        }
        EnableWindow(GetDlgItem(dialog, IDOK), !matches.empty());
    }

    bool AppWindow::promptForTags(bool singleTag)
    {
        singleTagPrompt_ = singleTag;
        tagInput_.clear();
        return DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_TAGS), window_,
            &AppWindow::tagProcedure, reinterpret_cast<LPARAM>(this)) == IDOK;
    }

    void AppWindow::editTags()
    {
        beginFileSystemPathSnapshot(PathSnapshotAction::editTags, Text::editTags);
    }

    std::uint32_t AppWindow::loadTagColor(std::wstring_view tag) const noexcept
    {
        if (tag.empty() || tag.size() > core::maxTagLength) return 0;
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\FilesXPNative\\TagColors", 0,
                KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return 0;
        DWORD type{};
        DWORD value{};
        DWORD bytes = sizeof(value);
        const std::wstring name(tag);
        const LSTATUS status = RegQueryValueExW(key, name.c_str(), nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &bytes);
        RegCloseKey(key);
        return status == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(value) &&
            core::validTagColor(value) ? value : 0;
    }

    bool AppWindow::saveTagColor(std::wstring_view tag, std::uint32_t color) const noexcept
    {
        if (tag.empty() || tag.size() > core::maxTagLength || !core::validTagColor(color))
            return false;
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\FilesXPNative\\TagColors", 0,
                nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        const std::wstring name(tag);
        const LSTATUS status = color == 0 ? RegDeleteValueW(key, name.c_str()) :
            RegSetValueExW(key, name.c_str(), 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&color), sizeof(color));
        RegCloseKey(key);
        if (status == ERROR_SUCCESS || (color == 0 && status == ERROR_FILE_NOT_FOUND)) return true;
        SetLastError(status);
        return false;
    }

    void AppWindow::updateTagChip()
    {
        SetWindowTextW(tagChip_, tagChipText_.c_str());
        ShowWindow(tagChip_, tagChipText_.empty() ? SW_HIDE : SW_SHOW);
        InvalidateRect(tagChip_, nullptr, TRUE);
        layoutChildren(clientWidth_, clientHeight_);
    }

    void AppWindow::manageTagColor()
    {
        if (!promptForTags(true)) return;
        tagColorChoice_ = loadTagColor(tagInput_);
        if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_TAG_COLOR), window_,
                &AppWindow::tagColorProcedure, reinterpret_cast<LPARAM>(this)) != IDOK)
            return;
        if (!saveTagColor(tagInput_, tagColorChoice_))
        {
            showError(L"Save tag color", HRESULT_FROM_WIN32(GetLastError()));
            return;
        }
        if (core::sameTag(tagInput_, tagChipText_))
        {
            currentTagColor_ = tagColorChoice_;
            updateTagChip();
        }
    }

    void AppWindow::startFallbackSearch(std::wstring root, std::wstring query,
        ExplorerBrowserHost* target)
    {
        // ponytail: Directory traversal stays in a cancellable process. The UI thread only
        // transfers a bounded, versioned request and incrementally materializes Shell items.
        if (target == nullptr)
        {
            showError(L"Search", E_INVALIDARG);
            return;
        }
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        core::SearchRequest request;
        std::vector<std::uint8_t> encoded;
        try
        {
            request.root = toUtf16(root);
            request.query = toUtf16(query);
            SHELLSTATE shellState{};
            SHGetSetSettings(&shellState, SSF_SHOWALLOBJECTS, FALSE);
            request.includeHidden = shellState.fShowAllObjects != FALSE;
            encoded = core::encodeSearchRequest(request);
        }
        catch (...)
        {
            showError(L"Search", E_OUTOFMEMORY);
            return;
        }
        if (encoded.empty())
        {
            showError(L"Search", HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return;
        }
        std::wstring executable;
        std::wstring mappingName;
        std::wstring eventName;
        std::wstring resultPath;
        try
        {
            executable = moduleExecutable();
            mappingName = uniqueLocalObjectName(L"SearchRequest");
            eventName = uniqueLocalObjectName(L"SearchCancel");
            resultPath = uniqueTemporaryPath(L"Search");
        }
        catch (...)
        {
            SecureZeroMemory(encoded.data(), encoded.size());
            showError(L"Search", E_OUTOFMEMORY);
            return;
        }
        if (executable.empty() || mappingName.empty() || eventName.empty() || resultPath.empty())
        {
            SecureZeroMemory(encoded.data(), encoded.size());
            showError(L"Search", HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY));
            return;
        }
        SetLastError(ERROR_SUCCESS);
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(encoded.size()), mappingName.c_str());
        DWORD failure = mapping == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (mapping != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
            failure = ERROR_ALREADY_EXISTS;
        void* view = failure == ERROR_SUCCESS ?
            MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, encoded.size()) : nullptr;
        if (failure == ERROR_SUCCESS && view == nullptr) failure = GetLastError();
        if (view != nullptr)
        {
            std::memcpy(view, encoded.data(), encoded.size());
            UnmapViewOfFile(view);
        }
        SecureZeroMemory(encoded.data(), encoded.size());
        SetLastError(ERROR_SUCCESS);
        HANDLE cancel = failure == ERROR_SUCCESS ?
            CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()) : nullptr;
        if (failure == ERROR_SUCCESS &&
            (cancel == nullptr || GetLastError() == ERROR_ALREADY_EXISTS))
            failure = cancel == nullptr ? GetLastError() : ERROR_ALREADY_EXISTS;
        const std::uintptr_t workerToken = uniqueWorkerToken();
        if (failure == ERROR_SUCCESS && workerToken == 0) failure = ERROR_GEN_FAILURE;
        std::vector<std::wstring> arguments;
        if (failure == ERROR_SUCCESS)
        {
            try
            {
                arguments = {L"--fallback-search",
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
                    std::to_wstring(workerToken),
                    resultPath, eventName, mappingName};
            }
            catch (...)
            {
                failure = ERROR_NOT_ENOUGH_MEMORY;
            }
        }
        if (failure != ERROR_SUCCESS)
        {
            if (cancel != nullptr) CloseHandle(cancel);
            if (mapping != nullptr) CloseHandle(mapping);
            DeleteFileW(resultPath.c_str());
            showError(L"Search", HRESULT_FROM_WIN32(failure));
            return;
        }
        tagSearchPath_ = std::move(resultPath);
        tagSearchTag_ = std::move(query);
        tagSearchCancelEvent_ = cancel;
        tagSearchRequestMapping_ = mapping;
        target->AddRef();
        tagSearchTargetBrowser_ = target;
        tagSearchWorkerToken_ = workerToken;
        tagSearchTaskTitle_ = Text::searchCommand;
        tagSearchUpdatesChip_ = false;
        gitPaneVisible_ = true;
        gitPanelTitleText_ = tagSearchTaskTitle_;
        SetWindowTextW(gitPanelHeader_, localizer_(tagSearchTaskTitle_));
        SetWindowTextW(gitOutput_, localizer_(Text::gitRunning));
        SetWindowTextW(gitCancelButton_, localizer_(Text::cancel));
        EnableWindow(gitCancelButton_, TRUE);
        LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
        SetWindowLongPtrW(gitProgress_, GWL_STYLE, style | PBS_MARQUEE);
        SetWindowPos(gitProgress_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(gitProgress_, PBM_SETMARQUEE, TRUE, 45);
        for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
            ShowWindow(control, SW_SHOW);
        layoutChildren(clientWidth_, clientHeight_);
        if (!queueProcessLaunch(executable, arguments, {}, CREATE_NO_WINDOW, SW_HIDE,
                tagSearchCompleteMessage, workerToken))
            (void)PostMessageW(window_, tagSearchCompleteMessage,
                workerToken, GetLastError());
    }

    void AppWindow::filterByTag()
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!promptForTags(true))
        {
            return;
        }
        ExplorerBrowserHost* target = activeBrowser();
        if (target == nullptr)
        {
            showError(L"Find tags", E_UNEXPECTED);
            return;
        }
        if (!tagSearchPath_.empty())
        {
            MessageBoxW(window_, L"A tag search is already running.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        tagSearchPath_ = uniqueTemporaryPath(L"Tag");
        const std::wstring eventName = uniqueLocalObjectName(L"TagSearchCancel");
        if (length == 0 || length >= executable.size() || tagSearchPath_.empty() ||
            eventName.empty())
        {
            tagSearchPath_.clear();
            showError(L"Find tags", HRESULT_FROM_WIN32(GetLastError()));
            return;
        }
        executable.resize(length);
        tagSearchTag_ = tagInput_;
        tagSearchTaskTitle_ = Text::filterByTag;
        tagSearchUpdatesChip_ = true;
        const std::uintptr_t workerToken = uniqueWorkerToken();
        if (workerToken == 0)
        {
            tagSearchPath_.clear();
            tagSearchTag_.clear();
            showError(L"Find tags", E_FAIL);
            return;
        }
        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        DWORD failure = cancel == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (cancel != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
            failure = ERROR_ALREADY_EXISTS;
        if (failure != ERROR_SUCCESS)
        {
            if (cancel != nullptr) CloseHandle(cancel);
            tagSearchPath_.clear();
            tagSearchTag_.clear();
            showError(L"Find tags", HRESULT_FROM_WIN32(failure));
            return;
        }
        const std::vector<std::wstring> arguments{L"--find-tag",
                std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
                std::to_wstring(workerToken), tagSearchTag_, tagSearchPath_, eventName};
        tagSearchCancelEvent_ = cancel;
        target->AddRef();
        tagSearchTargetBrowser_ = target;
        tagSearchWorkerToken_ = workerToken;
        gitPaneVisible_ = true;
        gitPanelTitleText_ = Text::filterByTag;
        SetWindowTextW(gitPanelHeader_, localizer_(Text::filterByTag));
        SetWindowTextW(gitOutput_, localizer_(Text::gitRunning));
        SetWindowTextW(gitCancelButton_, localizer_(Text::cancel));
        EnableWindow(gitCancelButton_, TRUE);
        LONG_PTR style = GetWindowLongPtrW(gitProgress_, GWL_STYLE);
        SetWindowLongPtrW(gitProgress_, GWL_STYLE, style | PBS_MARQUEE);
        SetWindowPos(gitProgress_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SendMessageW(gitProgress_, PBM_SETMARQUEE, TRUE, 45);
        for (HWND control : {gitPanelHeader_, gitProgress_, gitOutput_, gitCancelButton_})
            ShowWindow(control, SW_SHOW);
        layoutChildren(clientWidth_, clientHeight_);
        if (!queueProcessLaunch(executable, arguments, {}, CREATE_NO_WINDOW, SW_HIDE,
                tagSearchCompleteMessage, workerToken))
            (void)PostMessageW(window_, tagSearchCompleteMessage,
                workerToken, GetLastError());
    }

    void AppWindow::mapNetworkDrive()
    {
        CONNECTDLGSTRUCTW dialog{};
        dialog.cbStructure = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.dwFlags = CONNDLG_USE_MRU;
        const DWORD result = WNetConnectionDialog1W(&dialog);
        if (result != NO_ERROR && result != ERROR_CANCELLED)
        {
            showError(L"Map network drive", HRESULT_FROM_WIN32(result));
        }
    }

    void AppWindow::disconnectNetworkDrive()
    {
        DISCDLGSTRUCTW dialog{};
        dialog.cbStructure = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.dwFlags = DISC_UPDATE_PROFILE;
        const DWORD result = WNetDisconnectDialog1W(&dialog);
        if (result != NO_ERROR && result != ERROR_CANCELLED)
        {
            showError(L"Disconnect network drive", HRESULT_FROM_WIN32(result));
        }
    }

    void AppWindow::openTerminal(bool elevated)
    {
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring directory = browser != nullptr ? browser->filesystemPath() :
            std::wstring{};
        if (directory.empty())
        {
            MessageBoxW(window_, L"Open a file-system folder first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring terminal;
        const std::wstring localAppData = environmentPath(L"LOCALAPPDATA");
        if (!localAppData.empty())
        {
            terminal = localAppData + L"\\Microsoft\\WindowsApps\\wt.exe";
            const DWORD attributes = GetFileAttributesW(terminal.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                terminal.clear();
        }
        std::vector<std::wstring> arguments;
        if (!terminal.empty()) arguments = {L"-d", directory};
        else
        {
            terminal = systemExecutable(L"cmd.exe");
            arguments = {L"/D", L"/K"};
        }
        if (terminal.empty())
        {
            showError(localizer_(elevated ? Text::openTerminalAdmin : Text::openTerminal),
                HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
            return;
        }
        if (!elevated)
        {
            if (!launchProcess(terminal, arguments, directory,
                    CREATE_NEW_CONSOLE, SW_SHOWNORMAL))
                showError(localizer_(Text::openTerminal), HRESULT_FROM_WIN32(GetLastError()));
            return;
        }
        std::wstring parameters;
        for (const auto& argument : arguments)
        {
            if (!parameters.empty()) parameters.push_back(L' ');
            parameters += core::quoteWindowsArgument(argument);
        }
        const HINSTANCE result = ShellExecuteW(window_, L"runas", terminal.c_str(),
            parameters.c_str(), directory.c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            const auto failure = static_cast<DWORD>(reinterpret_cast<INT_PTR>(result));
            showError(localizer_(Text::openTerminalAdmin), HRESULT_FROM_WIN32(
                failure == ERROR_SUCCESS ? ERROR_GEN_FAILURE : failure));
        }
    }

    void AppWindow::setDesktopBackground(bool slideshow)
    {
        ExplorerBrowserHost* browser = activeBrowser();
        if (browser == nullptr) return;
        IDesktopWallpaper* desktop{};
        HRESULT status = CoCreateInstance(CLSID_DesktopWallpaper, nullptr,
            CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&desktop));
        if (SUCCEEDED(status) && slideshow)
        {
            IShellItemArray* items{};
            status = browser->selectedItems(&items);
            DWORD count{};
            if (SUCCEEDED(status) && items != nullptr) status = items->GetCount(&count);
            if (SUCCEEDED(status) && (count < 2 || count > core::maxShellOperationItems))
                status = E_INVALIDARG;
            if (SUCCEEDED(status)) status = desktop->SetSlideshow(items);
            if (items != nullptr) items->Release();
        }
        else if (SUCCEEDED(status))
        {
            const std::wstring path = selectedFileSystemPath();
            status = path.empty() ? E_INVALIDARG : desktop->SetWallpaper(nullptr, path.c_str());
        }
        if (desktop != nullptr) desktop->Release();
        if (FAILED(status)) showError(localizer_(slideshow ?
            Text::setDesktopSlideshow : Text::setDesktopWallpaper), status);
    }

    void AppWindow::hashSelection()
    {
        const std::wstring path = selectedFileSystemPath();
        if (path.empty())
        {
            MessageBoxW(window_, L"Select exactly one file first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::wstring executable = systemExecutable(L"certutil.exe");
        if (executable.empty() || !launchProcess(executable,
                {L"-hashfile", path, L"SHA256"}, {}, CREATE_NEW_CONSOLE, SW_SHOWNORMAL))
        {
            showError(L"SHA-256", HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void AppWindow::showAlternateStreams()
    {
        ExplorerBrowserHost* browser = activeBrowser();
        const std::wstring directory = browser != nullptr ? browser->filesystemPath() : std::wstring{};
        const std::wstring executable = systemCommandProcessor();
        if (directory.empty() || executable.empty() || !launchProcess(executable,
                {L"/D", L"/K", L"dir /r"}, directory, CREATE_NEW_CONSOLE, SW_SHOWNORMAL))
        {
            showError(L"Alternate streams", HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void AppWindow::verifySignature()
    {
        const std::wstring path = selectedFileSystemPath();
        if (path.empty())
        {
            MessageBoxW(window_, L"Select exactly one file first.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::wstring executable = systemExecutable(L"WindowsPowerShell\\v1.0\\powershell.exe");
        constexpr wchar_t command[] =
            L"& { Get-AuthenticodeSignature -LiteralPath $args[0] | Format-List *; Read-Host 'Press Enter' }";
        if (executable.empty() || !launchProcess(executable,
                {L"-NoLogo", L"-NoProfile", L"-Command", command, path}, {},
                CREATE_NEW_CONSOLE, SW_SHOWNORMAL))
        {
            showError(L"Digital signature", HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void AppWindow::applySettings()
    {
        localizer_.setLocale(settings_.locale);
        SetWindowTextW(backButton_, localizer_(Text::back));
        SetWindowTextW(forwardButton_, localizer_(Text::forward));
        SetWindowTextW(upButton_, localizer_(Text::up));
        SetWindowTextW(refreshButton_, localizer_(Text::refresh));
        SetWindowTextW(newFolderButton_, localizer_(Text::newFolder));
        SetWindowTextW(viewButton_, localizer_(Text::view));
        SetWindowTextW(addressLabel_, localizer_(Text::address));
        SetWindowTextW(goButton_, localizer_(Text::go));
        SetWindowTextW(placesHeader_, localizer_(Text::places));
        SetWindowTextW(tagChip_, tagChipText_.c_str());
        if (gitPaneVisible_) SetWindowTextW(gitPanelHeader_, localizer_(gitPanelTitleText_));
        SetWindowTextW(gitCancelButton_, localizer_(
            backgroundTaskActive() ? Text::cancel : Text::close));
        setAccessibleName(placesList_, localizer_(Text::places));
        setAccessibleName(tabControl_, localizer_(Text::tabsLabel));
        setAccessibleName(addressEdit_, localizer_(Text::address));
        setAccessibleName(searchEdit_, localizer_(Text::searchCommand));
        setAccessibleName(statusBar_, localizer_(Text::statusLabel));
        setAccessibleName(textPreviewEdit_, localizer_(Text::quickPreview));
        setAccessibleName(gitOutput_, localizer_(Text::taskOutputLabel));
        if (!textPreviewVisible_) SetWindowTextW(textPreviewHeader_, localizer_(Text::quickPreview));
        SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(localizer_(Text::searchHint)));
        populatePlaces();
        HMENU previous = fullScreen_ ? fullScreenMenu_ : GetMenu(window_);
        fullScreenMenu_ = nullptr;
        if (previous != nullptr)
        {
            if (!fullScreen_) SetMenu(window_, nullptr);
            DestroyMenu(previous);
        }
        createMainMenu();
        if (fullScreen_)
        {
            fullScreenMenu_ = GetMenu(window_);
            SetMenu(window_, nullptr);
        }
        const bool placesVisible = settings_.enabled(core::showPlaces);
        ShowWindow(placesHeader_, placesVisible ? SW_SHOW : SW_HIDE);
        ShowWindow(placesList_, placesVisible ? SW_SHOW : SW_HIDE);
        HMENU menu = fullScreen_ ? fullScreenMenu_ : GetMenu(window_);
        if (menu != nullptr)
        {
            const UINT gitState = MF_BYCOMMAND |
                (settings_.enabled(core::enableGit) ? MF_ENABLED : MF_GRAYED);
            for (UINT command : {static_cast<UINT>(CommandId::gitInit),
                     static_cast<UINT>(CommandId::gitClone),
                     static_cast<UINT>(CommandId::gitCreateBranch),
                     static_cast<UINT>(CommandId::gitSwitchBranch),
                     static_cast<UINT>(CommandId::gitStatus), static_cast<UINT>(CommandId::gitFetch),
                     static_cast<UINT>(CommandId::gitPull), static_cast<UINT>(CommandId::gitPush),
                     static_cast<UINT>(CommandId::gitSync)})
            {
                EnableMenuItem(menu, command, gitState);
            }
            const UINT archiveState = MF_BYCOMMAND |
                (settings_.enabled(core::enableArchives) ? MF_ENABLED : MF_GRAYED);
            EnableMenuItem(menu, CommandId::compressArchive, archiveState);
            EnableMenuItem(menu, CommandId::extractArchive, archiveState);
            EnableMenuItem(menu, CommandId::togglePreviewPane, MF_BYCOMMAND |
                (settings_.enabled(core::enableQuickPreview) ? MF_ENABLED : MF_GRAYED));
            DrawMenuBar(window_);
        }
        if (clientWidth_ > 0 && clientHeight_ > 0)
            layoutChildren(clientWidth_, clientHeight_);
        updateChrome();
        if (!settings_.enabled(core::enableGit))
        {
            KillTimer(window_, gitStatusTimerId);
            for (auto& tab : tabs_)
            {
                if (tab.browser != nullptr) tab.browser->setGitDecorations({}, {});
                if (tab.secondaryBrowser != nullptr) tab.secondaryBrowser->setGitDecorations({}, {});
            }
        }
        else
            scheduleGitStatusRefresh();
    }

    void AppWindow::stageShelf(bool move)
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        ExplorerBrowserHost* browser = activeBrowser();
        IShellItemArray* selectedItems{};
        DWORD selectedCount{};
        if (browser == nullptr || FAILED(browser->selectedItems(&selectedItems)) ||
            selectedItems == nullptr || FAILED(selectedItems->GetCount(&selectedCount)) ||
            selectedCount == 0 || selectedCount > core::maxShellOperationItems)
        {
            if (selectedItems != nullptr) selectedItems->Release();
            MessageBoxW(window_, L"Select 1 to 4096 items first.", L"Files XP Native",
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::vector<std::uint32_t> order;
        try
        {
            order.resize(selectedCount);
            std::iota(order.begin(), order.end(), 0U);
        }
        catch (...)
        {
            selectedItems->Release();
            showError(localizer_(move ? Text::shelfMove : Text::shelfCopy), E_OUTOFMEMORY);
            return;
        }
        clearShelf();
        shelfItems_ = selectedItems;
        shelfOrder_ = std::move(order);
        shelfCount_ = selectedCount;
        // ponytail: Retain Shell identities, not path strings, so virtual/provider items stay valid.
        shelfMove_ = move;
        updateStatus();
    }

    void AppWindow::pasteShelf()
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        ExplorerBrowserHost* browser = activeBrowser();
        if (browser == nullptr || shelfItems_ == nullptr || shelfOrder_.empty())
        {
            MessageBoxW(window_, L"The shelf is empty.", L"Files XP Native", MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::wstring destination = browser->parsingName();
        if (destination.empty())
        {
            MessageBoxW(window_, L"This location cannot receive Shell items.",
                localizer_(Text::title), MB_OK | MB_ICONINFORMATION);
            return;
        }
        core::ShellOperationRequest request;
        request.operation = shelfMove_ ? core::ShellOperation::move : core::ShellOperation::copy;
        request.destination = toUtf16(destination);
        beginShellOperationSnapshot(std::move(request), Text::shelfPaste, shelfItems_, shelfMove_,
            ShellSnapshotPurpose::shellOperation, false, core::maxShellOperationItems,
            &shelfOrder_);
        updateStatus();
    }

    void AppWindow::showShelf()
    {
        if (shelfItems_ == nullptr || shelfOrder_.empty())
        {
            MessageBoxW(window_, L"The shelf is empty.", localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        const INT_PTR result = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SHELF),
            window_, &AppWindow::shelfProcedure, reinterpret_cast<LPARAM>(this));
        shelfDialogLoading_ = false;
        shelfDialogCursor_ = 0;
        if (shelfOrder_.empty()) clearShelf();
        updateStatus();
        if (result == IDOK) pasteShelf();
    }

    void AppWindow::loadShelfDialogBatch(HWND dialog) noexcept
    {
        if (!shelfDialogLoading_ || shelfItems_ == nullptr) return;
        constexpr std::size_t itemsPerDispatch = 16;
        const ULONGLONG deadline = GetTickCount64() + 8;
        std::size_t processed{};
        while (shelfDialogCursor_ < shelfOrder_.size() && processed < itemsPerDispatch)
        {
            IShellItem* item{};
            PWSTR raw{};
            HRESULT status = shelfItems_->GetItemAt(shelfOrder_[shelfDialogCursor_], &item);
            if (SUCCEEDED(status) && item != nullptr)
                status = item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw);
            if (FAILED(status) && item != nullptr)
            {
                if (raw != nullptr) CoTaskMemFree(raw);
                raw = nullptr;
                status = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw);
            }
            const wchar_t* label = SUCCEEDED(status) && raw != nullptr && *raw != L'\0' ?
                raw : localizer_(Text::unavailableItem);
            const LRESULT added = SendDlgItemMessageW(dialog, IDC_SHELF_LIST,
                LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            if (raw != nullptr) CoTaskMemFree(raw);
            if (item != nullptr) item->Release();
            if (added == LB_ERR || added == LB_ERRSPACE)
            {
                shelfDialogLoading_ = false;
                showError(localizer_(Text::manageShelf), E_OUTOFMEMORY);
                EndDialog(dialog, IDCANCEL);
                return;
            }
            ++shelfDialogCursor_;
            ++processed;
            if (GetTickCount64() >= deadline) break;
        }
        if (shelfDialogCursor_ >= shelfOrder_.size())
        {
            shelfDialogLoading_ = false;
            shelfDialogCursor_ = 0;
            if (!shelfOrder_.empty())
                SendDlgItemMessageW(dialog, IDC_SHELF_LIST, LB_SETCURSEL, 0, 0);
            updateShelfDialogButtons(dialog);
            return;
        }
        if (!PostMessageW(dialog, shelfLoadMessage, 0, 0))
        {
            shelfDialogLoading_ = false;
            updateShelfDialogButtons(dialog);
        }
    }

    void AppWindow::updateShelfDialogButtons(HWND dialog) noexcept
    {
        const LRESULT selected = SendDlgItemMessageW(dialog, IDC_SHELF_LIST,
            LB_GETCURSEL, 0, 0);
        const bool available = !shelfDialogLoading_ && selected != LB_ERR &&
            static_cast<std::size_t>(selected) < shelfOrder_.size();
        EnableWindow(GetDlgItem(dialog, IDC_SHELF_REMOVE), available);
        EnableWindow(GetDlgItem(dialog, IDC_SHELF_UP), available && selected > 0);
        EnableWindow(GetDlgItem(dialog, IDC_SHELF_DOWN), available &&
            static_cast<std::size_t>(selected + 1) < shelfOrder_.size());
        EnableWindow(GetDlgItem(dialog, IDC_SHELF_PASTE),
            !shelfDialogLoading_ && !shelfOrder_.empty());
    }

    void AppWindow::moveShelfDialogItem(HWND dialog, int delta) noexcept
    {
        if (shelfDialogLoading_ || (delta != -1 && delta != 1)) return;
        const LRESULT selected = SendDlgItemMessageW(dialog, IDC_SHELF_LIST,
            LB_GETCURSEL, 0, 0);
        if (selected == LB_ERR) return;
        const int target = static_cast<int>(selected) + delta;
        if (target < 0 || static_cast<std::size_t>(target) >= shelfOrder_.size()) return;
        const LRESULT length = SendDlgItemMessageW(dialog, IDC_SHELF_LIST,
            LB_GETTEXTLEN, selected, 0);
        if (length == LB_ERR) return;
        try
        {
            std::wstring label(static_cast<std::size_t>(length) + 1, L'\0');
            if (SendDlgItemMessageW(dialog, IDC_SHELF_LIST, LB_GETTEXT, selected,
                    reinterpret_cast<LPARAM>(label.data())) == LB_ERR)
                return;
            label.resize(static_cast<std::size_t>(length));
            SendDlgItemMessageW(dialog, IDC_SHELF_LIST, LB_DELETESTRING, selected, 0);
            const LRESULT inserted = SendDlgItemMessageW(dialog, IDC_SHELF_LIST,
                LB_INSERTSTRING, target, reinterpret_cast<LPARAM>(label.c_str()));
            if (inserted == LB_ERR || inserted == LB_ERRSPACE)
            {
                const LRESULT restored = SendDlgItemMessageW(dialog, IDC_SHELF_LIST,
                    LB_INSERTSTRING, selected, reinterpret_cast<LPARAM>(label.c_str()));
                if (restored == LB_ERR || restored == LB_ERRSPACE)
                    EndDialog(dialog, IDCANCEL);
                return;
            }
            std::swap(shelfOrder_[static_cast<std::size_t>(selected)],
                shelfOrder_[static_cast<std::size_t>(target)]);
            SendDlgItemMessageW(dialog, IDC_SHELF_LIST, LB_SETCURSEL, target, 0);
            updateShelfDialogButtons(dialog);
        }
        catch (...)
        {
            showError(localizer_(Text::manageShelf), E_OUTOFMEMORY);
        }
    }

    void AppWindow::clearShelf() noexcept
    {
        if (shelfItems_ != nullptr)
        {
            shelfItems_->Release();
            shelfItems_ = nullptr;
        }
        shelfMove_ = false;
        shelfCount_ = 0;
        shelfOrder_.clear();
    }

    void AppWindow::showFtpManager()
    {
        if (backgroundTaskActive())
        {
            MessageBoxW(window_, localizer_(Text::gitRunning), localizer_(Text::title),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
        DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_FTP), window_,
            &AppWindow::ftpProcedure, reinterpret_cast<LPARAM>(this));
        ftpDialog_ = nullptr;
        ftpConnected_ = false;
        ftpDialogLoading_ = false;
        ftpListingCursor_.cancel();
        ftpListing_.clear();
        ftpEntries_.clear();
        ftpUrl_ = L"ftp://";
        clearFtpCredentials();
    }

    bool AppWindow::startFtpWorker(core::FtpRequest request, std::wstring pendingUrl)
    {
        FtpCredentialGuard credentialGuard{request};
        // ponytail: Credentials live in a bounded pagefile mapping only until the isolated
        // worker consumes and zeroes it; curl receives them over anonymous stdin, never argv.
        if (backgroundTaskActive())
        {
            if (ftpDialog_ != nullptr)
                MessageBoxW(ftpDialog_, localizer_(Text::gitRunning), localizer_(Text::title),
                    MB_OK | MB_ICONINFORMATION);
            return false;
        }
        const core::FtpOperation operation = request.operation;
        if (pendingUrl.empty() && operation == core::FtpOperation::list)
            pendingUrl.assign(reinterpret_cast<const wchar_t*>(request.url.data()),
                request.url.size());
        std::vector<std::uint8_t> encoded = core::encodeFtpRequest(request);
        scrubFtpCredentials(request);
        if (encoded.empty())
        {
            showError(localizer_(Text::ftpManager), HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return false;
        }
        const std::wstring executable = moduleExecutable();
        const std::wstring mappingName = uniqueLocalObjectName(L"FtpRequest");
        const std::wstring eventName = uniqueLocalObjectName(L"FtpCancel");
        const std::wstring resultPath = uniqueTemporaryPath(L"Ftp");
        const std::uintptr_t token = uniqueWorkerToken();
        if (executable.empty() || mappingName.empty() || eventName.empty() ||
            resultPath.empty() || token == 0)
        {
            SecureZeroMemory(encoded.data(), encoded.size());
            showError(localizer_(Text::ftpManager), HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return false;
        }
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(encoded.size()), mappingName.c_str());
        DWORD failure = mapping == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (mapping != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
            failure = ERROR_ALREADY_EXISTS;
        void* view = failure == ERROR_SUCCESS ?
            MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, encoded.size()) : nullptr;
        if (failure == ERROR_SUCCESS && view == nullptr) failure = GetLastError();
        if (view != nullptr)
        {
            std::memcpy(view, encoded.data(), encoded.size());
            UnmapViewOfFile(view);
        }
        SecureZeroMemory(encoded.data(), encoded.size());
        HANDLE cancel = failure == ERROR_SUCCESS ?
            CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()) : nullptr;
        if (failure == ERROR_SUCCESS &&
            (cancel == nullptr || GetLastError() == ERROR_ALREADY_EXISTS))
            failure = cancel == nullptr ? GetLastError() : ERROR_ALREADY_EXISTS;
        const std::vector<std::wstring> arguments{
            L"--ftp-worker", std::to_wstring(reinterpret_cast<std::uintptr_t>(window_)),
            std::to_wstring(token), resultPath, eventName, mappingName};
        if (failure != ERROR_SUCCESS)
        {
            if (cancel != nullptr) CloseHandle(cancel);
            if (mapping != nullptr) CloseHandle(mapping);
            DeleteFileW(resultPath.c_str());
            showError(localizer_(Text::ftpManager), HRESULT_FROM_WIN32(failure));
            return false;
        }
        ftpWorkerActive_ = true;
        ftpWorkerToken_ = token;
        ftpCancelEvent_ = cancel;
        ftpRequestMapping_ = mapping;
        ftpResultPath_ = resultPath;
        ftpPendingOperation_ = operation;
        ftpPendingUrl_ = std::move(pendingUrl);
        if (ftpDialog_ != nullptr)
        {
            SetDlgItemTextW(ftpDialog_, IDC_FTP_STATUS, localizer_(Text::ftpLoading));
            updateFtpDialogButtons(ftpDialog_);
        }
        if (!queueProcessLaunch(executable, arguments, {}, CREATE_NO_WINDOW, SW_HIDE,
                ftpCompleteMessage, token))
            (void)PostMessageW(window_, ftpCompleteMessage, token, GetLastError());
        return true;
    }

    void AppWindow::beginFtpResultRead(DWORD workerResult)
    {
        if (!ftpWorkerActive_ || ftpResultReadActive_) return;
        if (ftpCancelEvent_ != nullptr)
        {
            CloseHandle(ftpCancelEvent_);
            ftpCancelEvent_ = nullptr;
        }
        if (ftpRequestMapping_ != nullptr)
        {
            CloseHandle(ftpRequestMapping_);
            ftpRequestMapping_ = nullptr;
        }
        ftpWorkerResult_ = workerResult;
        ftpResultReadActive_ = true;
        ftpResultReadCursor_.cancel();
        bool valid = !ftpResultPath_.empty();
        if (valid)
        {
            ftpResultReadFile_ = CreateFileW(ftpResultPath_.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            valid = ftpResultReadFile_ != INVALID_HANDLE_VALUE;
        }
        LARGE_INTEGER bytes{};
        // ponytail: A hostile server can produce at most 4 MiB, read in 64-KiB UI quanta.
        constexpr LONGLONG maxResultBytes = 4LL * 1024LL * 1024LL;
        if (valid)
            valid = GetFileSizeEx(ftpResultReadFile_, &bytes) != FALSE &&
                bytes.QuadPart >= 0 && bytes.QuadPart <= maxResultBytes;
        if (valid)
        {
            try
            {
                ftpResultBytes_.resize(static_cast<std::size_t>(bytes.QuadPart));
            }
            catch (...)
            {
                valid = false;
            }
        }
        if (ftpDialog_ != nullptr) updateFtpDialogButtons(ftpDialog_);
        if (!valid || ftpResultBytes_.empty())
        {
            finishFtpResultRead(valid);
            return;
        }
        if (!ftpResultReadCursor_.start(ftpResultBytes_.size(),
                static_cast<std::size_t>(maxResultBytes)) ||
            !PostMessageW(window_, ftpResultReadMessage, ftpWorkerToken_, 0))
            finishFtpResultRead(false);
    }

    void AppWindow::processFtpResultRead(std::uintptr_t token)
    {
        if (!ftpResultReadActive_ || token != ftpWorkerToken_ ||
            ftpResultReadFile_ == INVALID_HANDLE_VALUE)
            return;
        constexpr std::size_t chunkBytes = 64U * 1024U;
        constexpr std::size_t chunksPerDispatch = 2;
        const ULONGLONG deadline = GetTickCount64() + 6;
        for (std::size_t chunk = 0; chunk < chunksPerDispatch &&
            ftpResultReadCursor_.active(); ++chunk)
        {
            const core::IndexBatch batch = ftpResultReadCursor_.next(chunkBytes);
            const DWORD requested = static_cast<DWORD>(batch.count);
            DWORD read{};
            if (!ReadFile(ftpResultReadFile_, ftpResultBytes_.data() + batch.first,
                    requested, &read, nullptr) || read != requested)
            {
                finishFtpResultRead(false);
                return;
            }
            if (GetTickCount64() >= deadline) break;
        }
        if (!ftpResultReadCursor_.active())
        {
            finishFtpResultRead(true);
            return;
        }
        if (!PostMessageW(window_, ftpResultReadMessage, ftpWorkerToken_, 0))
            finishFtpResultRead(false);
    }

    void AppWindow::finishFtpResultRead(bool read)
    {
        if (!ftpResultReadActive_) return;
        if (ftpResultReadFile_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(ftpResultReadFile_);
            ftpResultReadFile_ = INVALID_HANDLE_VALUE;
        }
        if (!ftpResultPath_.empty()) DeleteFileW(ftpResultPath_.c_str());
        ftpResultPath_.clear();
        std::wstring output;
        if (read) read = decodeStrictUtf8(std::move(ftpResultBytes_), output);
        ftpResultBytes_.clear();
        ftpResultReadCursor_.cancel();
        ftpResultReadActive_ = false;
        presentFtpWorkerResult(ftpWorkerResult_, std::move(output), read);
    }

    void AppWindow::presentFtpWorkerResult(DWORD result, std::wstring output, bool read)
    {
        const core::FtpOperation operation = ftpPendingOperation_;
        const std::wstring pendingUrl = std::move(ftpPendingUrl_);
        ftpPendingUrl_.clear();
        ftpWorkerActive_ = false;
        ftpWorkerToken_ = 0;
        if (result == ERROR_SUCCESS && !read) result = ERROR_INVALID_DATA;
        if (ftpDialog_ == nullptr) return;

        if (result == ERROR_CANCELLED)
        {
            SetDlgItemTextW(ftpDialog_, IDC_FTP_STATUS, localizer_(Text::gitCanceled));
            updateFtpDialogButtons(ftpDialog_);
            return;
        }
        if (result != ERROR_SUCCESS)
        {
            boundTaskOutput(output);
            std::wstring message = localizer_(Text::gitFailed);
            message += std::to_wstring(result);
            if (!output.empty()) message += L"\r\n\r\n" + output;
            MessageBoxW(ftpDialog_, message.c_str(), localizer_(Text::ftpManager),
                MB_OK | MB_ICONERROR);
            SetDlgItemTextW(ftpDialog_, IDC_FTP_STATUS, localizer_(Text::gitFailed));
            updateFtpDialogButtons(ftpDialog_);
            return;
        }
        if (operation == core::FtpOperation::list)
        {
            ftpUrl_ = pendingUrl;
            ftpConnected_ = true;
            ftpListing_ = std::move(output);
            ftpEntries_.clear();
            (void)ftpListingCursor_.start(ftpListing_);
            SetDlgItemTextW(ftpDialog_, IDC_FTP_URL, ftpUrl_.c_str());
            SendDlgItemMessageW(ftpDialog_, IDC_FTP_LIST, LB_RESETCONTENT, 0, 0);
            const std::size_t estimatedItems = std::min(core::maxFtpListingItems,
                ftpListing_.size() / 8 + 1);
            SendDlgItemMessageW(ftpDialog_, IDC_FTP_LIST, LB_INITSTORAGE,
                estimatedItems, ftpListing_.size() * sizeof(wchar_t));
            ftpDialogLoading_ = ftpListingCursor_.active();
            if (ftpDialogLoading_ && !PostMessageW(ftpDialog_, ftpLoadMessage, 0, 0))
            {
                ftpDialogLoading_ = false;
                ftpListingCursor_.cancel();
                ftpListing_.clear();
            }
            if (!ftpDialogLoading_)
                SetDlgItemTextW(ftpDialog_, IDC_FTP_STATUS, localizer_(Text::ftpReady));
            updateFtpDialogButtons(ftpDialog_);
            return;
        }
        SetDlgItemTextW(ftpDialog_, IDC_FTP_STATUS, localizer_(Text::gitCompleted));
        if (operation == core::FtpOperation::download)
        {
            if (!output.empty())
            {
                boundTaskOutput(output);
                MessageBoxW(ftpDialog_, output.c_str(), localizer_(Text::ftpManager),
                    MB_OK | MB_ICONINFORMATION);
            }
            if (ExplorerBrowserHost* browser = activeBrowser(); browser != nullptr)
                browser->refresh();
            updateFtpDialogButtons(ftpDialog_);
            return;
        }
        startFtpList(ftpUrl_);
    }

    void AppWindow::updateFtpDialogButtons(HWND dialog) noexcept
    {
        if (dialog == nullptr) return;
        const bool busy = ftpWorkerActive_ || ftpDialogLoading_;
        const bool connected = ftpConnected_ &&
            core::validFtpDirectoryUrl(toUtf16(ftpUrl_));
        const LRESULT selected = SendDlgItemMessageW(dialog, IDC_FTP_LIST,
            LB_GETCURSEL, 0, 0);
        const bool hasSelection = !busy && connected && selected != LB_ERR &&
            static_cast<std::size_t>(selected) < ftpEntries_.size();
        for (int control : {IDC_FTP_URL, IDC_FTP_USERNAME, IDC_FTP_PASSWORD,
                 IDC_FTP_REQUIRE_TLS, IDC_FTP_CONNECT})
            EnableWindow(GetDlgItem(dialog, control), !busy);
        const std::u16string parent = connected ?
            core::parentFtpDirectoryUrl(toUtf16(ftpUrl_)) : std::u16string{};
        EnableWindow(GetDlgItem(dialog, IDC_FTP_UP), !busy && connected &&
            parent != toUtf16(ftpUrl_));
        EnableWindow(GetDlgItem(dialog, IDC_FTP_REFRESH), !busy && connected);
        for (int control : {IDC_FTP_OPEN, IDC_FTP_DOWNLOAD,
                 IDC_FTP_DELETE_FILE, IDC_FTP_DELETE_FOLDER})
            EnableWindow(GetDlgItem(dialog, control), hasSelection);
        EnableWindow(GetDlgItem(dialog, IDC_FTP_UPLOAD), !busy && connected);
        EnableWindow(GetDlgItem(dialog, IDC_FTP_NEW_FOLDER), !busy && connected);
        EnableWindow(GetDlgItem(dialog, IDC_FTP_CANCEL_TRANSFER),
            ftpWorkerActive_ && ftpCancelEvent_ != nullptr);
    }

    void AppWindow::loadFtpDialogBatch(HWND dialog) noexcept
    {
        if (!ftpDialogLoading_ || dialog == nullptr) return;
        // ponytail: Validation, allocation, and list insertion yield after 64 remote lines
        // or 8 ms so hostile/large listings cannot monopolize the modal UI thread.
        constexpr std::size_t linesPerDispatch = 64;
        const ULONGLONG deadline = GetTickCount64() + 8;
        std::size_t processed{};
        bool failed{};
        SendDlgItemMessageW(dialog, IDC_FTP_LIST, WM_SETREDRAW, FALSE, 0);
        while (ftpListingCursor_.active() && processed < linesPerDispatch)
        {
            std::wstring_view name;
            const core::FtpNameListStep step = ftpListingCursor_.next(name);
            ++processed;
            if (step == core::FtpNameListStep::item)
            {
                try
                {
                    ftpEntries_.emplace_back(name);
                }
                catch (...)
                {
                    failed = true;
                    break;
                }
                const LRESULT added = SendDlgItemMessageW(dialog, IDC_FTP_LIST, LB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(ftpEntries_.back().c_str()));
                if (added == LB_ERR || added == LB_ERRSPACE)
                {
                    ftpEntries_.pop_back();
                    failed = true;
                    break;
                }
            }
            if (GetTickCount64() >= deadline) break;
        }
        SendDlgItemMessageW(dialog, IDC_FTP_LIST, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(GetDlgItem(dialog, IDC_FTP_LIST), nullptr, TRUE);
        if (failed)
        {
            ftpDialogLoading_ = false;
            ftpListingCursor_.cancel();
            ftpListing_.clear();
            SetDlgItemTextW(dialog, IDC_FTP_STATUS, localizer_(Text::gitFailed));
            updateFtpDialogButtons(dialog);
            return;
        }
        if (!ftpListingCursor_.active())
        {
            ftpDialogLoading_ = false;
            ftpListingCursor_.cancel();
            ftpListing_.clear();
            if (!ftpEntries_.empty())
                SendDlgItemMessageW(dialog, IDC_FTP_LIST, LB_SETCURSEL, 0, 0);
            SetDlgItemTextW(dialog, IDC_FTP_STATUS, localizer_(Text::ftpReady));
            updateFtpDialogButtons(dialog);
            return;
        }
        if (!ftpDialogLoading_ || !PostMessageW(dialog, ftpLoadMessage, 0, 0))
        {
            ftpDialogLoading_ = false;
            ftpListingCursor_.cancel();
            ftpListing_.clear();
            updateFtpDialogButtons(dialog);
        }
    }

    void AppWindow::startFtpList(std::wstring url)
    {
        core::FtpRequest request;
        FtpCredentialGuard credentialGuard{request};
        request.operation = core::FtpOperation::list;
        request.requireTls = ftpRequireTls_;
        request.url = toUtf16(url);
        request.username = toUtf16(ftpUsername_);
        request.password = toUtf16(ftpPassword_);
        (void)startFtpWorker(std::move(request), std::move(url));
    }

    std::wstring AppWindow::selectedFtpName(HWND dialog) const
    {
        const LRESULT selected = SendDlgItemMessageW(dialog, IDC_FTP_LIST,
            LB_GETCURSEL, 0, 0);
        return selected == LB_ERR || static_cast<std::size_t>(selected) >= ftpEntries_.size() ?
            std::wstring{} : ftpEntries_[static_cast<std::size_t>(selected)];
    }

    void AppWindow::clearFtpCredentials() noexcept
    {
        if (!ftpPassword_.empty())
            SecureZeroMemory(ftpPassword_.data(), ftpPassword_.size() * sizeof(wchar_t));
        ftpPassword_.clear();
        if (!ftpUsername_.empty())
            SecureZeroMemory(ftpUsername_.data(), ftpUsername_.size() * sizeof(wchar_t));
        ftpUsername_.clear();
    }

    void AppWindow::dispatchCommand(int command)
    {
        ExplorerBrowserHost* browser = activeBrowser();
        HRESULT status = S_OK;
        switch (command)
        {
        case CommandId::newTab:
            addTab();
            return;
        case CommandId::newWindow:
            launchNewWindow();
            return;
        case CommandId::duplicateTab:
            duplicateActiveTab();
            return;
        case CommandId::openInNewTab:
            openSelectedInNewTab();
            return;
        case CommandId::openInNewWindow:
            openSelectedInNewWindow();
            return;
        case CommandId::openInOtherPane:
            openSelectedInOtherPane();
            return;
        case CommandId::openCurrentFolderOtherPane:
            openCurrentFolderInOtherPane();
            return;
        case CommandId::openFileLocation:
            openFileLocation();
            return;
        case CommandId::reopenClosedTab:
            reopenClosedTab();
            return;
        case CommandId::closeTab:
            closeActiveTab();
            return;
        case CommandId::closeOtherTabs:
            closeOtherTabs();
            return;
        case CommandId::closeTabsLeft:
            closeTabsToLeft();
            return;
        case CommandId::closeTabsRight:
            closeTabsToRight();
            return;
        case CommandId::closeAllTabs:
            DestroyWindow(window_);
            return;
        case CommandId::moveTabLeft:
            moveActiveTab(-1);
            return;
        case CommandId::moveTabRight:
            moveActiveTab(1);
            return;
        case CommandId::exitApp:
            DestroyWindow(window_);
            return;
        case CommandId::focusAddress:
            SetFocus(addressEdit_);
            SendMessageW(addressEdit_, EM_SETSEL, 0, -1);
            return;
        case CommandId::focusSearch:
            SetFocus(searchEdit_);
            SendMessageW(searchEdit_, EM_SETSEL, 0, -1);
            return;
        case CommandId::splitVertical:
            splitActiveTab(true);
            return;
        case CommandId::splitHorizontal:
            splitActiveTab(false);
            return;
        case CommandId::closePane:
            closeActivePane();
            return;
        case CommandId::focusOtherPane:
            focusOtherPane();
            return;
        case CommandId::toggleSidebar:
            toggleSidebar();
            return;
        case CommandId::fullScreen:
            toggleFullScreen();
            return;
        case CommandId::showHiddenItems:
            toggleShellVisibility(SSF_SHOWALLOBJECTS);
            return;
        case CommandId::showFileExtensions:
            toggleShellVisibility(SSF_SHOWEXTENSIONS);
            return;
        case CommandId::togglePreviewPane:
            toggleQuickPreview();
            return;
        case CommandId::toggleDetailsPane:
            if (browser != nullptr) browser->toggleDetailsPane();
            return;
        case CommandId::settings:
            showSettings();
            return;
        case CommandId::keyboardShortcuts:
            showKeyboardShortcuts();
            return;
        case CommandId::bulkRename:
            bulkRename();
            return;
        case CommandId::folderFromSelection:
            createFolderFromSelection();
            return;
        case CommandId::flattenFolder:
            flattenFolder();
            return;
        case CommandId::createShortcut:
            createShortcut();
            return;
        case CommandId::editShortcut:
            editShortcut();
            return;
        case CommandId::createLibrary:
            createLibrary();
            return;
        case CommandId::editLibrary:
            editLibrary();
            return;
        case CommandId::commandPalette:
            showCommandPalette();
            return;
        case CommandId::editTags:
            editTags();
            return;
        case CommandId::filterByTag:
            filterByTag();
            return;
        case CommandId::manageTagColor:
            manageTagColor();
            return;
        case CommandId::mapNetworkDrive:
            mapNetworkDrive();
            return;
        case CommandId::disconnectNetworkDrive:
            disconnectNetworkDrive();
            return;
        case CommandId::ftpManager:
            showFtpManager();
            return;
        case CommandId::openTerminal:
            openTerminal(false);
            return;
        case CommandId::openTerminalAdmin:
            openTerminal(true);
            return;
        case CommandId::setDesktopWallpaper:
            setDesktopBackground(false);
            return;
        case CommandId::setDesktopSlideshow:
            setDesktopBackground(true);
            return;
        case CommandId::openStorageSense:
        {
            const HINSTANCE launched = ShellExecuteW(window_, L"open",
                L"ms-settings:storagesense", nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(launched) <= 32)
            {
                const auto failure = static_cast<DWORD>(reinterpret_cast<INT_PTR>(launched));
                showError(localizer_(Text::openStorageSense),
                    HRESULT_FROM_WIN32(failure == ERROR_SUCCESS ? ERROR_GEN_FAILURE : failure));
            }
            return;
        }
        case CommandId::runWithPowerShell:
        {
            const std::wstring path = selectedFileSystemPath();
            const std::wstring executable = systemExecutable(L"powershell.exe");
            const wchar_t* extension = path.empty() ? L"" : PathFindExtensionW(path.c_str());
            DWORD failure{};
            if (path.empty() || lstrcmpiW(extension, L".ps1") != 0)
                failure = ERROR_INVALID_PARAMETER;
            else if (executable.empty())
                failure = ERROR_FILE_NOT_FOUND;
            else if (!launchProcess(executable,
                {L"-NoLogo", L"-NoProfile", L"-File", path},
                browser != nullptr ? browser->filesystemPath() : std::wstring{},
                CREATE_NEW_CONSOLE, SW_SHOWNORMAL))
                failure = GetLastError();
            if (failure != ERROR_SUCCESS)
            {
                showError(localizer_(Text::runWithPowerShell),
                    HRESULT_FROM_WIN32(failure));
            }
            return;
        }
        case CommandId::hashSelection:
            hashSelection();
            return;
        case CommandId::showAlternateStreams:
            showAlternateStreams();
            return;
        case CommandId::editAlternateStream:
            editAlternateStream();
            return;
        case CommandId::verifySignature:
            verifySignature();
            return;
        case CommandId::copyPath:
            copySelectedPaths(false);
            return;
        case CommandId::copyPathQuoted:
            copySelectedPaths(true);
            return;
        case CommandId::gitClone: if (settings_.enabled(core::enableGit)) cloneGitRepository(); return;
        case CommandId::gitCreateBranch:
            if (settings_.enabled(core::enableGit)) changeGitBranch(true);
            return;
        case CommandId::gitSwitchBranch:
            if (settings_.enabled(core::enableGit)) changeGitBranch(false);
            return;
        case CommandId::gitInit: if (settings_.enabled(core::enableGit)) runGit(core::GitOperation::init, Text::gitInit); return;
        case CommandId::gitStatus: if (settings_.enabled(core::enableGit)) runGit(core::GitOperation::status, Text::gitStatus); return;
        case CommandId::gitFetch: if (settings_.enabled(core::enableGit)) runGit(core::GitOperation::fetch, Text::gitFetch); return;
        case CommandId::gitPull: if (settings_.enabled(core::enableGit)) runGit(core::GitOperation::pull, Text::gitPull); return;
        case CommandId::gitPush: if (settings_.enabled(core::enableGit)) runGit(core::GitOperation::push, Text::gitPush); return;
        case CommandId::gitSync: if (settings_.enabled(core::enableGit)) runGit(core::GitOperation::sync, Text::gitSync); return;
        case CommandId::compressArchive: if (settings_.enabled(core::enableArchives)) compressSelection(); return;
        case CommandId::extractArchive: if (settings_.enabled(core::enableArchives)) extractSelection(); return;
        case CommandId::shelfCopy: stageShelf(false); return;
        case CommandId::shelfMove: stageShelf(true); return;
        case CommandId::shelfPaste: pasteShelf(); return;
        case CommandId::shelfClear: clearShelf(); updateStatus(); return;
        case CommandId::manageShelf: showShelf(); return;
        default:
            break;
        }
        if (browser == nullptr)
        {
            return;
        }
        if (command == CommandId::rename && browser->selectedCount() > 1)
        {
            bulkRename();
            return;
        }

        switch (command)
        {
        case CommandId::newFolder: createShellItem(true); return;
        case CommandId::newFile: createShellItem(false); return;
        case CommandId::cut: status = browser->copySelectionToClipboard(true); break;
        case CommandId::copy: status = browser->copySelectionToClipboard(false); break;
        case CommandId::paste: status = browser->pasteClipboard(); break;
        case CommandId::pasteShortcut:
            status = browser->invokeBackgroundVerb("pastelink");
            break;
        case CommandId::rename: status = browser->renameSelection(); break;
        case CommandId::recycleDelete: deleteSelection(false); return;
        case CommandId::permanentDelete:
            deleteSelection(true);
            return;
        case CommandId::emptyRecycleBin:
            emptyRecycleBin();
            return;
        case CommandId::properties: status = browser->invokeSelectionVerb("properties"); break;
        case CommandId::undo: status = browser->executeOleCommand(OLECMDID_UNDO); break;
        case CommandId::redo: status = browser->executeOleCommand(OLECMDID_REDO); break;
        case CommandId::selectAll: status = browser->executeOleCommand(OLECMDID_SELECTALL); break;
        case CommandId::clearSelection: status = browser->clearSelection(); break;
        case CommandId::invertSelection: status = browser->invertSelection(); break;
        case CommandId::restoreRecycleBin:
            status = browser->invokeSelectionVerb("undelete");
            break;
        case CommandId::restoreAllRecycleBin:
            restoreAllRecycleBin();
            return;
        case CommandId::refresh: status = browser->refresh(); break;
        case CommandId::viewDetails: status = browser->setView(FVM_DETAILS); break;
        case CommandId::viewList: status = browser->setView(FVM_LIST); break;
        case CommandId::viewSmall: status = browser->setView(FVM_ICON, 16); break;
        case CommandId::viewMedium: status = browser->setView(FVM_ICON, 48); break;
        case CommandId::viewLarge: status = browser->setView(FVM_ICON, 96); break;
        case CommandId::viewExtraLarge: status = browser->setView(FVM_ICON, 256); break;
        case CommandId::viewTiles: status = browser->setView(FVM_TILE); break;
        case CommandId::viewContent: status = browser->setView(FVM_CONTENT); break;
        case CommandId::autoSizeColumns: status = browser->autoSizeColumns(); break;
        case CommandId::goBack: status = browser->browseBack(); break;
        case CommandId::goForward: status = browser->browseForward(); break;
        case CommandId::goUp: status = browser->browseUp(); break;
        case CommandId::goHome:
            status = browser->browsePath(
                L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}");
            break;
        case CommandId::pinQuickAccess:
            status = browser->invokeSelectionVerb("pintohome");
            break;
        case CommandId::unpinQuickAccess:
            status = browser->invokeSelectionVerb("unpinfromhome");
            break;
        case CommandId::pasteIntoFolder:
            status = browser->invokeSelectionVerb("paste");
            break;
        case CommandId::playSelection:
            status = browser->invokeSelectionVerb("open");
            break;
        case CommandId::runAsAdministrator:
            status = browser->invokeSelectionVerb("runas");
            break;
        case CommandId::runAsDifferentUser:
            status = browser->invokeSelectionVerb("runasuser");
            break;
        case CommandId::rotateLeft:
            status = browser->invokeSelectionVerb("rotateleft");
            break;
        case CommandId::rotateRight:
            status = browser->invokeSelectionVerb("rotateright");
            break;
        case CommandId::installSelection:
            status = browser->invokeSelectionVerb("install");
            break;
        case CommandId::installCertificate:
            status = browser->invokeSelectionVerb("add");
            break;
        default: return;
        }
        if (FAILED(status) && status != E_FAIL)
        {
            showError(L"Command", status);
        }
        else if (command == CommandId::refresh)
            scheduleGitStatusRefresh();
    }
}
