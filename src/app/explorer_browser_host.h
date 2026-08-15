#pragma once

#include <windows.h>
#include <shobjidl.h>

#include "../core/git_status.h"
#include "../core/coalescing_gate.h"
#include "../core/selection_inversion.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace filesxp::app
{
    class ExplorerBrowserHost final : public IServiceProvider,
                                      public ICommDlgBrowser2,
                                      public IExplorerBrowserEvents,
                                      public IExplorerPaneVisibility
    {
    public:
        static HRESULT create(HWND parent, HWND notificationWindow, UINT navigationMessage,
            UINT selectionMessage, const RECT& bounds, ExplorerBrowserHost** result) noexcept;

        ExplorerBrowserHost(const ExplorerBrowserHost&) = delete;
        ExplorerBrowserHost& operator=(const ExplorerBrowserHost&) = delete;

        void shutdown() noexcept;
        void setBounds(const RECT& bounds, bool redraw = true) noexcept;
        void show(bool visible) noexcept;
        void focus() noexcept;
        [[nodiscard]] bool translateAccelerator(MSG& message) noexcept;

        HRESULT browsePath(std::wstring_view path) noexcept;
        HRESULT browseParentAndSelect(std::wstring_view path) noexcept;
        HRESULT browseKnownFolder(REFKNOWNFOLDERID folderId) noexcept;
        HRESULT browseBack() noexcept;
        HRESULT browseForward() noexcept;
        HRESULT browseUp() noexcept;
        HRESULT refresh() noexcept;
        HRESULT search(std::wstring_view query) noexcept;
        [[nodiscard]] bool consumeSearchNavigationFailure() noexcept;
        HRESULT beginResults(std::wstring_view displayName) noexcept;
        HRESULT addResult(std::wstring_view path) noexcept;
        HRESULT finishResults() noexcept;
        void cancelResults() noexcept;
        void abortResults() noexcept;
        void setGitDecorations(std::wstring statusOutput,
            std::wstring_view workingDirectory) noexcept;
        void notificationDelivered(UINT message) noexcept;

        HRESULT renameSelection() noexcept;
        HRESULT clearSelection() noexcept;
        HRESULT invertSelection() noexcept;
        HRESULT executeOleCommand(DWORD command) noexcept;
        HRESULT copySelectionToClipboard(bool move) noexcept;
        HRESULT pasteClipboard() noexcept;
        HRESULT invokeSelectionVerb(const char* verb) noexcept;
        HRESULT invokeBackgroundVerb(const char* verb) noexcept;
        HRESULT autoSizeColumns() noexcept;
        HRESULT setView(FOLDERVIEWMODE mode, int iconSize = -1) noexcept;
        void setInitialView(std::uint32_t view) noexcept;
        void togglePreviewPane() noexcept;
        void toggleDetailsPane() noexcept;
        [[nodiscard]] bool previewPaneVisible() const noexcept { return previewPaneVisible_; }
        [[nodiscard]] bool detailsPaneVisible() const noexcept { return detailsPaneVisible_; }
        [[nodiscard]] bool canGoBack() const noexcept
        {
            return !customResultRestorePath_.empty() || historyIndex_ > 0;
        }
        [[nodiscard]] bool canGoForward() const noexcept
        {
            return customResultName_.empty() && historyIndex_ + 1 < history_.size();
        }

        [[nodiscard]] std::wstring editingName() const;
        [[nodiscard]] std::wstring restorableName() const;
        [[nodiscard]] std::wstring filesystemPath() const;
        [[nodiscard]] std::wstring parsingName() const;
        [[nodiscard]] std::wstring displayName() const;
        void itemCounts(int& total, int& selected) const noexcept;
        [[nodiscard]] int selectedCount() const noexcept;
        HRESULT selectedItems(IShellItemArray** items) const noexcept;
        [[nodiscard]] bool isSearch() const noexcept
        {
            return searchScope_ != nullptr || !customResultName_.empty();
        }

        IFACEMETHODIMP QueryInterface(REFIID interfaceId, void** result) noexcept override;
        IFACEMETHODIMP_(ULONG) AddRef() noexcept override;
        IFACEMETHODIMP_(ULONG) Release() noexcept override;

        IFACEMETHODIMP QueryService(REFGUID serviceId, REFIID interfaceId, void** result) noexcept override;

        IFACEMETHODIMP OnDefaultCommand(IShellView* shellView) noexcept override;
        IFACEMETHODIMP OnStateChange(IShellView* shellView, ULONG change) noexcept override;
        IFACEMETHODIMP IncludeObject(IShellView* shellView, PCUITEMID_CHILD item) noexcept override;
        IFACEMETHODIMP Notify(IShellView* shellView, DWORD notificationType) noexcept override;
        IFACEMETHODIMP GetDefaultMenuText(IShellView* shellView, PWSTR text, int textLength) noexcept override;
        IFACEMETHODIMP GetViewFlags(DWORD* flags) noexcept override;

        IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE folder) noexcept override;
        IFACEMETHODIMP OnViewCreated(IShellView* shellView) noexcept override;
        IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE folder) noexcept override;
        IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE folder) noexcept override;

        IFACEMETHODIMP GetPaneState(REFEXPLORERPANE pane, EXPLORERPANESTATE* state) noexcept override;

    private:
        struct GitBadgeVisual final
        {
            RECT bounds{};
            COLORREF color{};
            wchar_t label{};
        };

        static constexpr UINT gitDecorationLayoutMessage = WM_APP + 1;
        static constexpr UINT gitDecorationBuildMessage = WM_APP + 2;

        ExplorerBrowserHost() = default;
        ~ExplorerBrowserHost();

        HRESULT initialize(HWND parent, HWND notificationWindow, UINT navigationMessage,
            UINT selectionMessage, const RECT& bounds) noexcept;
        HRESULT browseObject(IUnknown* object, UINT flags, bool clearSearch,
            bool preservePendingSelection = false) noexcept;
        HRESULT currentFolderItem(IShellItem** item) const noexcept;
        HRESULT currentFolderView(IFolderView2** view) const noexcept;
        HRESULT createSearchFolder(std::wstring_view query, IShellItem** item) noexcept;
        void clearCurrentFolder() noexcept;
        void clearPendingSelection() noexcept;
        void refreshCurrentFolderNames() noexcept;
        void clearSearchScope() noexcept;
        void notify(UINT message) noexcept;
        void refreshPaneState() noexcept;
        void postGitDecorationBuild() noexcept;
        void processGitDecorationBuild(std::uint32_t generation) noexcept;
        void scheduleGitDecorationLayout() noexcept;
        [[nodiscard]] bool postGitDecorationLayout() noexcept;
        void processGitDecorationLayout(std::uint32_t generation) noexcept;
        void paintGitDecorations(HDC deviceContext) noexcept;
        void processSelectionInversion() noexcept;
        void flushSelectionInversionNotification() noexcept;
        static LRESULT CALLBACK overlayProcedure(HWND window, UINT message, WPARAM wParam,
            LPARAM lParam, UINT_PTR subclassId, DWORD_PTR referenceData);
        static LRESULT CALLBACK shellViewProcedure(HWND window, UINT message, WPARAM wParam,
            LPARAM lParam, UINT_PTR subclassId, DWORD_PTR referenceData);

        std::atomic_ulong references_{1};
        HWND parent_{};
        HWND container_{};
        HWND overlay_{};
        HWND shellViewWindow_{};
        HWND notificationWindow_{};
        UINT navigationMessage_{};
        UINT selectionMessage_{};
        core::CoalescingGate navigationNotificationGate_;
        core::CoalescingGate selectionNotificationGate_;
        IExplorerBrowser* browser_{};
        IFolderView2* resultView_{};
        IResultsFolder* resultsFolder_{};
        std::wstring resultDisplayName_;
        std::wstring resultRestorePath_;
        PIDLIST_ABSOLUTE currentFolder_{};
        PIDLIST_ABSOLUTE pendingSelection_{};
        PIDLIST_ABSOLUTE searchScope_{};
        std::wstring cachedEditingName_;
        std::wstring cachedFileSystemPath_;
        std::wstring cachedParsingName_;
        std::wstring cachedDisplayName_;
        DWORD eventCookie_{};
        bool advised_{};
        bool initialized_{};
        RECT bounds_{};
        bool boundsValid_{};
        bool visible_{true};
        bool previewPaneVisible_{};
        bool detailsPaneVisible_{};
        bool searchNavigationPending_{};
        bool searchNavigationFailed_{};
        std::vector<std::wstring> history_;
        std::size_t historyIndex_{};
        int pendingTravelDelta_{};
        std::uint32_t initialView_{};
        bool initialViewPending_{};
        std::wstring customResultName_;
        std::wstring customResultRestorePath_;
        core::GitDecorationMap gitDecorations_;
        std::wstring gitDecorationRoot_;
        core::GitDecorationBuilder gitDecorationBuilder_;
        std::wstring pendingGitDecorationRoot_;
        std::uint32_t gitBuildGeneration_{};
        std::vector<GitBadgeVisual> gitBadgeVisuals_;
        std::vector<GitBadgeVisual> pendingGitBadgeVisuals_;
        HFONT gitBadgeFont_{};
        int gitBadgeFontDpi_{};
        std::uint32_t gitLayoutGeneration_{};
        core::CoalescingGate gitLayoutMessageGate_;
        int gitLayoutCurrentItem_{-1};
        std::size_t gitLayoutExamined_{};
        bool gitLayoutActive_{};
        core::SelectionInversionCursor selectionInversion_;
        int selectionInversionSnapshotCurrent_{-1};
        bool selectionInversionNotificationDeferred_{};
    };
}
