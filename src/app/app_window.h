#pragma once

#include "explorer_browser_host.h"
#include "localization.h"
#include "settings_store.h"
#include "../core/archive_request.h"
#include "../core/batch_cursor.h"
#include "../core/bulk_rename_request.h"
#include "../core/folder_selection_request.h"
#include "../core/ftp_request.h"
#include "../core/git_policy.h"
#include "../core/preview_queue.h"
#include "../core/shell_artifact_request.h"
#include "../core/shell_operation_request.h"
#include "../core/search_request.h"
#include "../core/tag_result_codec.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace filesxp::app
{
    inline constexpr wchar_t appWindowClassName[] = L"FilesXPNativeWindow";
    inline constexpr ULONG_PTR openLocationMessage = 0x4658504e;

    class AppWindow final
    {
    public:
        explicit AppWindow(HINSTANCE instance);
        ~AppWindow();

        AppWindow(const AppWindow&) = delete;
        AppWindow& operator=(const AppWindow&) = delete;

        [[nodiscard]] bool create(int showCommand, std::wstring initialPath = {});
        int runMessageLoop();

    private:
        struct Tab final
        {
            ExplorerBrowserHost* browser{};
            ExplorerBrowserHost* secondaryBrowser{};
            std::wstring title;
            std::wstring pendingLocation;
            std::wstring pendingSearchRoot[2];
            std::wstring pendingSearchQuery[2];
            int activePane{};
            bool verticalSplit{true};
        };

        struct Place final
        {
            std::wstring label;
            KNOWNFOLDERID folderId{};
            std::wstring parsingName;
        };

        enum class ShellSnapshotPurpose : std::uint8_t
        {
            shellOperation,
            clipboardPaths,
            fileSystemPaths
        };

        enum class PathSnapshotAction : std::uint8_t
        {
            none,
            compress,
            bulkRename,
            folderFromSelection,
            editTags
        };

        static constexpr UINT shellNavigationMessage = WM_APP + 1;
        static constexpr UINT shellSelectionMessage = WM_APP + 2;
        static constexpr UINT addressEnterMessage = WM_APP + 3;
        static constexpr UINT searchEnterMessage = WM_APP + 4;
        static constexpr UINT shellSnapshotMessage = WM_APP + 12;
        static constexpr UINT tagResultBatchMessage = WM_APP + 13;
        static constexpr UINT gitResultReadMessage = WM_APP + 14;
        static constexpr UINT tabRetireMessage = WM_APP + 15;
        static constexpr UINT shelfLoadMessage = WM_APP + 16;
        static constexpr UINT ftpResultReadMessage = WM_APP + 18;
        static constexpr UINT ftpLoadMessage = WM_APP + 19;

        static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        static LRESULT CALLBACK editProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
            UINT_PTR subclassId, DWORD_PTR referenceData);
        static LRESULT CALLBACK tabProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
            UINT_PTR subclassId, DWORD_PTR referenceData);
        static INT_PTR CALLBACK settingsProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK shortcutProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static LRESULT CALLBACK shortcutEditProcedure(HWND window, UINT message, WPARAM wParam,
            LPARAM lParam, UINT_PTR subclassId, DWORD_PTR referenceData);
        static INT_PTR CALLBACK commandPaletteProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK nameProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK linkProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK tagProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK tagColorProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK archiveProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK shelfProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK ftpProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

        LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
        [[nodiscard]] bool handleShortcut(const MSG& message);
        [[nodiscard]] bool registerWindowClass();
        void createChildren();
        void createMainMenu();
        void layoutChildren(int width, int height);
        void populatePlaces();

        void addTab(std::wstring initialPath = {});
        [[nodiscard]] bool ensureTabBrowser(std::size_t index);
        void launchNewWindow(std::wstring location = {});
        void duplicateActiveTab();
        void openSelectedInNewTab();
        void openSelectedInNewWindow();
        void openSelectedInOtherPane();
        void openCurrentFolderInOtherPane();
        void openFileLocation();
        void openInOtherPane(std::wstring location);
        void reopenClosedTab();
        void closeActiveTab();
        void closeTab(std::size_t index);
        void closeOtherTabs();
        void closeTabsToLeft();
        void closeTabsToRight();
        void retireTab(Tab&& tab, bool remember) noexcept;
        void processRetiredTab() noexcept;
        static void releaseTab(Tab& tab) noexcept;
        void activateTab(std::size_t index);
        void cycleTab(int delta);
        void moveActiveTab(int delta);
        void moveTab(std::size_t source, std::size_t destination);
        void rebuildTabControl();
        void splitActiveTab(bool vertical, std::wstring initialLocation = {});
        void closeActivePane();
        void focusOtherPane();
        void toggleSidebar();
        void toggleFullScreen();
        void toggleShellVisibility(DWORD setting);
        void layoutTab(Tab& tab, bool redraw = true) noexcept;
        void releaseTabs() noexcept;
        void restoreSession();
        void saveSession() const noexcept;
        [[nodiscard]] ExplorerBrowserHost* activeBrowser() const noexcept;
        [[nodiscard]] std::pair<std::size_t, int> findBrowser(const ExplorerBrowserHost* browser) const noexcept;
        [[nodiscard]] RECT browserBounds() const noexcept;

        void browseAddress();
        void runSearch();
        void openPlace(int index);
        void updateTab(ExplorerBrowserHost* browser);
        void updateChrome();
        void updateStatus();
        void copySelectedPaths(bool quoted);
        [[nodiscard]] std::wstring selectedShellItemName(SIGDN name,
            bool requireFolder = false) const;
        [[nodiscard]] std::wstring selectedFileSystemPath() const;
        [[nodiscard]] bool backgroundTaskActive() const noexcept;
        [[nodiscard]] bool launchProcess(const std::wstring& executable,
            const std::vector<std::wstring>& arguments, const std::wstring& workingDirectory,
            DWORD creationFlags, WORD showWindow) const;
        [[nodiscard]] bool queueProcessLaunch(const std::wstring& executable,
            const std::vector<std::wstring>& arguments, const std::wstring& workingDirectory,
            DWORD creationFlags, WORD showWindow, UINT completionMessage,
            WPARAM completionToken) const;
        void scheduleGitStatusRefresh() noexcept;
        void runGit(core::GitOperation operation, Text title, bool quiet = false);
        void beginGitResultRead(std::uint32_t generation, DWORD workerResult);
        void processGitResultRead(std::uint32_t generation);
        void finishGitResultRead(std::uint32_t generation, bool read);
        void cloneGitRepository();
        void changeGitBranch(bool create);
        void compressSelection();
        void extractSelection();
        [[nodiscard]] bool promptForArchive(bool creating);
        void startArchiveWorker(core::ArchiveRequest request, Text title);
        void startFlattenWorker(std::wstring rootPath);
        [[nodiscard]] bool startMappedFileWorker(std::vector<std::uint8_t> encoded,
            const wchar_t* workerSwitch, const wchar_t* purpose, Text title);
        void startBulkRenameWorker(core::BulkRenameRequest request);
        void startFolderSelectionWorker(core::FolderSelectionRequest request);
        void startShellArtifactWorker(core::ShellArtifactRequest request, Text title);
        [[nodiscard]] bool startShellOperation(core::ShellOperationRequest request, Text title,
            bool clearShelfOnSuccess = false);
        void beginShellOperationSnapshot(core::ShellOperationRequest request, Text title,
            IShellItemArray* source = nullptr, bool clearShelfOnSuccess = false,
            ShellSnapshotPurpose purpose = ShellSnapshotPurpose::shellOperation,
            bool quoteClipboardPaths = false,
            std::size_t maximumItems = core::maxShellOperationItems,
            const std::vector<std::uint32_t>* sourceOrder = nullptr);
        void beginFileSystemPathSnapshot(PathSnapshotAction action, Text title,
            std::wstring context = {});
        void finishFileSystemPathSnapshot(PathSnapshotAction action,
            std::vector<std::wstring> paths, std::wstring context);
        void processShellOperationSnapshot(std::uint32_t generation);
        void finishShellOperationSnapshot(DWORD result);
        void beginTagResultMaterialization(DWORD workerResult);
        void processTagResultMaterialization(std::uint32_t generation);
        void finishTagResultMaterialization(DWORD result);
        void createShellItem(bool folder);
        void deleteSelection(bool permanent);
        void emptyRecycleBin();
        void restoreAllRecycleBin();
        void showSettings();
        void showKeyboardShortcuts();
        [[nodiscard]] bool promptForName(Text title, Text instructions, bool allowDot,
            bool alternateStream = false, bool genericText = false, HWND owner = nullptr);
        void bulkRename();
        void createFolderFromSelection();
        void flattenFolder();
        void editAlternateStream();
        void createShortcut();
        void editShortcut();
        void createLibrary();
        void editLibrary();
        void showCommandPalette();
        void refreshCommandPalette(HWND dialog);
        [[nodiscard]] bool promptForTags(bool singleTag);
        void editTags();
        void filterByTag();
        void startFallbackSearch(std::wstring root, std::wstring query,
            ExplorerBrowserHost* target);
        void manageTagColor();
        [[nodiscard]] std::uint32_t loadTagColor(std::wstring_view tag) const noexcept;
        [[nodiscard]] bool saveTagColor(std::wstring_view tag, std::uint32_t color) const noexcept;
        void updateTagChip();
        void mapNetworkDrive();
        void disconnectNetworkDrive();
        void openTerminal(bool elevated);
        void setDesktopBackground(bool slideshow);
        void hashSelection();
        void showAlternateStreams();
        void verifySignature();
        void toggleQuickPreview();
        void toggleWindowsPreview();
        void launchPreviewPopup(bool switchSelection, std::wstring path = {});
        void startQueuedPreviewPopup();
        void completePreviewPopup(bool success);
        void scheduleTextPreview();
        void startTextPreviewWorker();
        void applySettings();
        void stageShelf(bool move);
        void pasteShelf();
        void showShelf();
        void loadShelfDialogBatch(HWND dialog) noexcept;
        void updateShelfDialogButtons(HWND dialog) noexcept;
        void moveShelfDialogItem(HWND dialog, int delta) noexcept;
        void clearShelf() noexcept;
        void showFtpManager();
        [[nodiscard]] bool startFtpWorker(core::FtpRequest request,
            std::wstring pendingUrl = {});
        void beginFtpResultRead(DWORD workerResult);
        void processFtpResultRead(std::uintptr_t token);
        void finishFtpResultRead(bool read);
        void presentFtpWorkerResult(DWORD result, std::wstring output, bool read);
        void updateFtpDialogButtons(HWND dialog) noexcept;
        void loadFtpDialogBatch(HWND dialog) noexcept;
        void startFtpList(std::wstring url);
        [[nodiscard]] std::wstring selectedFtpName(HWND dialog) const;
        void clearFtpCredentials() noexcept;
        void showError(const wchar_t* operation, HRESULT status) const;
        void dispatchCommand(int command);

        HINSTANCE instance_{};
        HWND window_{};
        HWND topBand_{};
        HWND backButton_{};
        HWND forwardButton_{};
        HWND upButton_{};
        HWND refreshButton_{};
        HWND newFolderButton_{};
        HWND viewButton_{};
        HWND addressLabel_{};
        HWND addressEdit_{};
        HWND goButton_{};
        HWND searchEdit_{};
        HWND placesHeader_{};
        HWND placesList_{};
        HWND tabControl_{};
        HWND statusBar_{};
        HWND textPreviewHeader_{};
        HWND textPreviewEdit_{};
        HWND tagChip_{};
        HWND gitPanelHeader_{};
        HWND gitProgress_{};
        HWND gitOutput_{};
        HWND gitCancelButton_{};
        HFONT uiFont_{};
        int dpi_{96};
        int clientWidth_{};
        int clientHeight_{};
        core::AppSettings settings_;
        Localizer localizer_;

        std::vector<Tab> tabs_;
        std::vector<Tab> retiredTabs_;
        std::vector<std::wstring> closedTabs_;
        std::size_t activeTab_{};
        int tabDragIndex_{-1};
        std::vector<Place> places_;
        IShellItemArray* shelfItems_{};
        bool shelfMove_{};
        DWORD shelfCount_{};
        std::vector<std::uint32_t> shelfOrder_;
        std::size_t shelfDialogCursor_{};
        bool shelfDialogLoading_{};
        std::vector<int> paletteCommands_;
        std::wstring tagInput_;
        std::wstring tagSearchPath_;
        std::wstring tagSearchTag_;
        HANDLE tagSearchCancelEvent_{};
        HANDLE tagSearchRequestMapping_{};
        ExplorerBrowserHost* tagSearchTargetBrowser_{};
        std::uintptr_t tagSearchWorkerToken_{};
        Text tagSearchTaskTitle_{Text::filterByTag};
        bool tagSearchUpdatesChip_{true};
        bool tagResultsActive_{};
        std::uint32_t tagResultGeneration_{};
        std::size_t tagResultExpectedCount_{};
        core::TagResultCursor tagResultCursor_;
        HANDLE tagResultMapping_{};
        const wchar_t* tagResultView_{};
        ExplorerBrowserHost* tagResultsBrowser_{};
        bool singleTagPrompt_{};
        std::uint32_t tagColorChoice_{};
        std::uint32_t currentTagColor_{};
        std::wstring tagChipText_;
        std::wstring statusText_;
        std::wstring nameInput_;
        Text namePromptTitle_{Text::title};
        Text namePromptInstructions_{Text::invalidName};
        bool nameAllowDot_{};
        bool nameAlternateStream_{};
        bool nameGenericText_{};
        bool textPreviewVisible_{};
        bool previewWorkerActive_{};
        bool externalPreviewActive_{};
        core::PreviewQueue previewPopupQueue_;
        std::uint32_t previewGeneration_{};
        std::uint32_t previewWorkerGeneration_{};
        std::wstring previewResultPath_;
        bool gitPaneVisible_{};
        bool fullScreen_{};
        WINDOWPLACEMENT previousWindowPlacement_{};
        LONG_PTR previousWindowStyle_{};
        HMENU fullScreenMenu_{};
        bool gitWorkerActive_{};
        std::uint32_t gitWorkerGeneration_{};
        HANDLE gitCancelEvent_{};
        std::wstring gitResultPath_;
        HANDLE gitResultReadFile_{INVALID_HANDLE_VALUE};
        std::string gitResultBytes_;
        core::BatchCursor gitResultReadCursor_;
        DWORD gitWorkerResult_{};
        bool gitResultReadActive_{};
        Text gitPanelTitleText_{Text::gitStatus};
        core::GitOperation gitWorkerOperation_{core::GitOperation::status};
        std::wstring gitWorkingDirectory_;
        std::wstring lastGitStatusDirectory_;
        ULONGLONG lastGitStatusTick_{};
        bool gitWorkerQuiet_{};
        bool archiveCreating_{};
        bool archiveWorkerActive_{};
        std::uint32_t archiveWorkerGeneration_{};
        HANDLE archiveCancelEvent_{};
        HANDLE archiveRequestMapping_{};
        std::wstring archiveResultPath_;
        bool clearShelfOnOperationSuccess_{};
        bool shellSnapshotActive_{};
        std::uint32_t shellSnapshotGeneration_{};
        core::BatchCursor shellSnapshotCursor_;
        IShellItemArray* shellSnapshotItems_{};
        std::vector<std::uint32_t> shellSnapshotOrder_;
        core::ShellOperationRequest shellSnapshotRequest_;
        Text shellSnapshotTitle_{Text::title};
        bool shellSnapshotClearsShelf_{};
        ShellSnapshotPurpose shellSnapshotPurpose_{ShellSnapshotPurpose::shellOperation};
        bool shellSnapshotQuotesClipboardPaths_{};
        std::wstring shellSnapshotClipboardText_;
        PathSnapshotAction shellSnapshotPathAction_{PathSnapshotAction::none};
        std::vector<std::wstring> shellSnapshotFileSystemPaths_;
        std::size_t shellSnapshotFileSystemCharacters_{};
        std::wstring shellSnapshotContext_;
        std::wstring archiveName_;
        std::wstring archivePassword_;
        core::ArchiveOperation archiveOperationChoice_{core::ArchiveOperation::create7z};
        core::ArchiveCollision archiveCollisionChoice_{core::ArchiveCollision::rename};
        std::wstring linkName_;
        std::wstring linkTarget_;
        std::wstring linkArguments_;
        std::wstring linkWorkingDirectory_;
        std::wstring linkIcon_;
        HWND ftpDialog_{};
        bool ftpWorkerActive_{};
        std::uintptr_t ftpWorkerToken_{};
        HANDLE ftpCancelEvent_{};
        HANDLE ftpRequestMapping_{};
        std::wstring ftpResultPath_;
        HANDLE ftpResultReadFile_{INVALID_HANDLE_VALUE};
        std::string ftpResultBytes_;
        core::BatchCursor ftpResultReadCursor_;
        DWORD ftpWorkerResult_{};
        bool ftpResultReadActive_{};
        core::FtpOperation ftpPendingOperation_{core::FtpOperation::list};
        std::wstring ftpPendingUrl_;
        std::wstring ftpUrl_{L"ftp://"};
        std::wstring ftpUsername_;
        std::wstring ftpPassword_;
        bool ftpRequireTls_{true};
        bool ftpConnected_{};
        std::wstring ftpListing_;
        core::FtpNameListCursor ftpListingCursor_;
        std::vector<std::wstring> ftpEntries_;
        bool ftpDialogLoading_{};
    };
}
