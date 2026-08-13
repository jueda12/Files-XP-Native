#include "app_window.h"
#include "archive_worker.h"
#include "bulk_rename_worker.h"
#include "flatten_worker.h"
#include "folder_selection_worker.h"
#include "ftp_worker.h"
#include "git_worker.h"
#include "preview_worker.h"
#include "search_worker.h"
#include "shell_artifact_worker.h"
#include "shell_operation_worker.h"
#include "tag_worker.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <string>
#include <string_view>
#include <utility>

namespace
{
    class ComApartment final
    {
    public:
        ComApartment()
            : result_(OleInitialize(nullptr))
        {
        }

        ~ComApartment()
        {
            if (SUCCEEDED(result_))
            {
                OleUninitialize();
            }
        }

        [[nodiscard]] bool ready() const noexcept
        {
            return SUCCEEDED(result_);
        }

    private:
        HRESULT result_{};
    };
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES |
        ICC_PROGRESS_CLASS;
    if (!InitCommonControlsEx(&controls))
    {
        return 1;
    }

    ComApartment apartment;
    if (!apartment.ready())
    {
        return 2;
    }

    std::wstring initialPath;
    bool forceNewWindow{};
    int argumentCount{};
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments != nullptr)
    {
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--tag-set-worker")
        {
            const int result = filesxp::app::runTagSetWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--find-tag")
        {
            const int result = filesxp::app::runTagSearchWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--preview-text")
        {
            const int result = filesxp::app::runTextPreviewWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--preview-popup")
        {
            const int result = filesxp::app::runPreviewPopupWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--fallback-search")
        {
            const int result = filesxp::app::runFallbackSearchWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--git-worker")
        {
            const int result = filesxp::app::runGitWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--archive-worker")
        {
            const int result = filesxp::app::runArchiveWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--ftp-worker")
        {
            const int result = filesxp::app::runFtpWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--flatten-worker")
        {
            const int result = filesxp::app::runFlattenWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--bulk-rename-worker")
        {
            const int result = filesxp::app::runBulkRenameWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--folder-selection-worker")
        {
            const int result = filesxp::app::runFolderSelectionWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--shell-operation-worker")
        {
            const int result = filesxp::app::runShellOperationWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--shell-artifact-worker")
        {
            const int result = filesxp::app::runShellArtifactWorker(argumentCount, arguments);
            LocalFree(arguments);
            return result;
        }
        int pathIndex = 1;
        if (argumentCount > 1 && std::wstring_view(arguments[1]) == L"--new-window")
        {
            forceNewWindow = true;
            pathIndex = 2;
        }
        if (argumentCount > pathIndex)
        {
            initialPath = arguments[pathIndex];
        }
        LocalFree(arguments);
    }

    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\FilesXPNative.PrimaryInstance");
    const bool anotherInstance = instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    if (!forceNewWindow && anotherInstance && initialPath.size() < 32768)
    {
        if (const HWND existing = FindWindowW(filesxp::app::appWindowClassName, nullptr); existing != nullptr)
        {
            COPYDATASTRUCT data{};
            data.dwData = filesxp::app::openLocationMessage;
            data.cbData = static_cast<DWORD>((initialPath.size() + 1) * sizeof(wchar_t));
            data.lpData = initialPath.data();
            DWORD_PTR ignored{};
            if (SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored) != 0)
            {
                if (instanceMutex != nullptr) CloseHandle(instanceMutex);
                return 0;
            }
        }
    }

    filesxp::app::AppWindow window(instance);
    if (!window.create(showCommand, std::move(initialPath)))
    {
        MessageBoxW(nullptr, L"Files XP Native could not create its main window.", L"Files XP Native",
            MB_OK | MB_ICONERROR);
        if (instanceMutex != nullptr) CloseHandle(instanceMutex);
        return 3;
    }
    const int result = window.runMessageLoop();
    if (instanceMutex != nullptr) CloseHandle(instanceMutex);
    return result;
}

