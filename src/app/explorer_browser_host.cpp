#include "explorer_browser_host.h"
#include "../core/filename_policy.h"
#include "../core/tag_result_codec.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <propkey.h>
#include <commctrl.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace filesxp::app
{
    namespace
    {
        [[nodiscard]] std::wstring trimAndExpand(std::wstring_view input)
        {
            const auto first = std::find_if_not(input.begin(), input.end(), [](wchar_t value)
            {
                return std::iswspace(value) != 0;
            });
            const auto last = std::find_if_not(input.rbegin(), input.rend(), [](wchar_t value)
            {
                return std::iswspace(value) != 0;
            }).base();
            if (first >= last)
            {
                return {};
            }

            std::wstring value(first, last);
            if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
            {
                value = value.substr(1, value.size() - 2);
            }

            const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
            if (required == 0)
            {
                return value;
            }
            std::wstring expanded(required, L'\0');
            if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required) == 0)
            {
                return value;
            }
            expanded.resize(required - 1);
            return expanded;
        }

        [[nodiscard]] std::wstring nameFromPidl(PCIDLIST_ABSOLUTE folder, SIGDN kind)
        {
            if (folder == nullptr)
            {
                return {};
            }
            PWSTR raw{};
            if (FAILED(SHGetNameFromIDList(folder, kind, &raw)) || raw == nullptr)
            {
                return {};
            }
            std::wstring result(raw);
            CoTaskMemFree(raw);
            return result;
        }

        [[nodiscard]] std::wstring urlEncode(std::wstring_view value)
        {
            if (value.empty())
            {
                return {};
            }
            const int bytesRequired = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (bytesRequired <= 0)
            {
                return {};
            }
            std::string utf8(static_cast<std::size_t>(bytesRequired), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                    static_cast<int>(value.size()), utf8.data(), bytesRequired, nullptr, nullptr) <= 0)
            {
                return {};
            }

            constexpr wchar_t hex[] = L"0123456789ABCDEF";
            std::wstring encoded;
            encoded.reserve(utf8.size() * 3);
            for (unsigned char character : utf8)
            {
                const bool unreserved = (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                    character == '.' || character == '~';
                if (unreserved)
                {
                    encoded.push_back(static_cast<wchar_t>(character));
                }
                else
                {
                    encoded.push_back(L'%');
                    encoded.push_back(hex[character >> 4]);
                    encoded.push_back(hex[character & 0x0f]);
                }
            }
            return encoded;
        }

        void releaseIfPresent(IUnknown*& object) noexcept
        {
            if (object != nullptr)
            {
                object->Release();
                object = nullptr;
            }
        }

        [[nodiscard]] UINT selectionInversionMessage() noexcept
        {
            static const UINT message = RegisterWindowMessageW(
                L"FilesXPNative.SelectionInversion.7A465E0C-5771-48C5-BE65-2F9203F79A4E");
            return message;
        }
    }

    HRESULT ExplorerBrowserHost::create(HWND parent, HWND notificationWindow, UINT navigationMessage,
        UINT selectionMessage, const RECT& bounds, ExplorerBrowserHost** result) noexcept
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }
        *result = nullptr;
        auto host = new (std::nothrow) ExplorerBrowserHost();
        if (host == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        const HRESULT status = host->initialize(parent, notificationWindow, navigationMessage,
            selectionMessage, bounds);
        if (FAILED(status))
        {
            host->Release();
            return status;
        }
        *result = host;
        return S_OK;
    }

    ExplorerBrowserHost::~ExplorerBrowserHost()
    {
        shutdown();
    }

    HRESULT ExplorerBrowserHost::initialize(HWND parent, HWND notificationWindow, UINT navigationMessage,
        UINT selectionMessage, const RECT& bounds) noexcept
    {
        parent_ = parent;
        notificationWindow_ = notificationWindow;
        navigationMessage_ = navigationMessage;
        selectionMessage_ = selectionMessage;

        container_ = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
            std::max(0L, bounds.bottom - bounds.top), parent, nullptr,
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
        if (container_ == nullptr)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        bounds_ = bounds;
        boundsValid_ = true;

        HRESULT status = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&browser_));
        if (FAILED(status))
        {
            return status;
        }

        status = IUnknown_SetSite(browser_, static_cast<IServiceProvider*>(this));
        if (FAILED(status))
        {
            return status;
        }

        browser_->SetOptions(EBO_SHOWFRAMES);

        RECT client{};
        GetClientRect(container_, &client);
        FOLDERSETTINGS folderSettings{};
        folderSettings.ViewMode = FVM_DETAILS;
        folderSettings.fFlags = FWF_AUTOARRANGE | FWF_NOWEBVIEW | FWF_SHOWSELALWAYS;
        status = browser_->Initialize(container_, &client, &folderSettings);
        if (FAILED(status))
        {
            return status;
        }
        initialized_ = true;

        // ponytail: Shell-owned view state is the deliberate ceiling; custom per-item view models are excluded.
        browser_->SetPropertyBag(L"FilesXPNative.View");
        status = browser_->Advise(this, &eventCookie_);
        if (SUCCEEDED(status))
        {
            advised_ = true;
        }
        return status;
    }

    void ExplorerBrowserHost::shutdown() noexcept
    {
        cancelResults();
        ++gitBuildGeneration_;
        gitDecorationBuilder_.cancel();
        pendingGitDecorationRoot_.clear();
        ++gitLayoutGeneration_;
        gitLayoutMessageGate_.reset();
        gitLayoutActive_ = false;
        gitBadgeVisuals_.clear();
        pendingGitBadgeVisuals_.clear();
        clearSearchScope();
        clearCurrentFolder();
        clearPendingSelection();
        selectionInversion_.cancel();
        selectionInversionSnapshotCurrent_ = -1;
        selectionInversionNotificationDeferred_ = false;
        navigationNotificationGate_.reset();
        selectionNotificationGate_.reset();
        if (shellViewWindow_ != nullptr)
        {
            RemoveWindowSubclass(shellViewWindow_, &ExplorerBrowserHost::shellViewProcedure, 1);
            shellViewWindow_ = nullptr;
        }
        if (overlay_ != nullptr)
        {
            KillTimer(overlay_, 1);
            RemoveWindowSubclass(overlay_, &ExplorerBrowserHost::overlayProcedure, 1);
            DestroyWindow(overlay_);
            overlay_ = nullptr;
        }
        if (gitBadgeFont_ != nullptr)
        {
            DeleteObject(gitBadgeFont_);
            gitBadgeFont_ = nullptr;
            gitBadgeFontDpi_ = 0;
        }
        if (browser_ != nullptr)
        {
            if (advised_)
            {
                browser_->Unadvise(eventCookie_);
                advised_ = false;
            }
            IUnknown_SetSite(browser_, nullptr);
            if (initialized_)
            {
                browser_->Destroy();
                initialized_ = false;
            }
            IUnknown* unknown = browser_;
            browser_ = nullptr;
            releaseIfPresent(unknown);
        }
        if (container_ != nullptr)
        {
            DestroyWindow(container_);
            container_ = nullptr;
        }
    }

    void ExplorerBrowserHost::setBounds(const RECT& bounds, bool redraw) noexcept
    {
        if (container_ == nullptr)
        {
            return;
        }
        if (boundsValid_ != false && EqualRect(&bounds_, &bounds) != FALSE)
        {
            return;
        }
        bounds_ = bounds;
        boundsValid_ = true;
        const int width = std::max(0L, bounds.right - bounds.left);
        const int height = std::max(0L, bounds.bottom - bounds.top);
        SetWindowPos(container_, nullptr, bounds.left, bounds.top, width, height,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
        if (browser_ != nullptr && initialized_)
        {
            RECT client{0, 0, width, height};
            browser_->SetRect(nullptr, client);
        }
        if (overlay_ != nullptr)
        {
            SetWindowPos(overlay_, nullptr, 0, 0, width, height,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
            SetWindowPos(overlay_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                    (redraw ? 0U : static_cast<UINT>(SWP_NOREDRAW)));
            scheduleGitDecorationLayout();
        }
        if (redraw)
            RedrawWindow(container_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void ExplorerBrowserHost::show(bool visible) noexcept
    {
        visible_ = visible;
        if (container_ != nullptr)
        {
            ShowWindow(container_, visible ? SW_SHOW : SW_HIDE);
        }
        if (visible) scheduleGitDecorationLayout();
    }

    void ExplorerBrowserHost::focus() noexcept
    {
        if (browser_ == nullptr)
        {
            return;
        }
        IShellView* view{};
        HWND viewWindow{};
        if (SUCCEEDED(browser_->GetCurrentView(IID_PPV_ARGS(&view))) &&
            SUCCEEDED(view->GetWindow(&viewWindow)))
        {
            SetFocus(viewWindow);
        }
        if (view != nullptr)
        {
            view->Release();
        }
    }

    bool ExplorerBrowserHost::translateAccelerator(MSG& message) noexcept
    {
        if (browser_ == nullptr)
        {
            return false;
        }
        IInputObject* input{};
        if (FAILED(browser_->QueryInterface(IID_PPV_ARGS(&input))))
        {
            return false;
        }
        const bool handled = input->TranslateAcceleratorIO(&message) == S_OK;
        input->Release();
        return handled;
    }

    HRESULT ExplorerBrowserHost::browseObject(IUnknown* object, UINT flags, bool clearSearch,
        bool preservePendingSelection) noexcept
    {
        if (browser_ == nullptr || object == nullptr)
        {
            return E_INVALIDARG;
        }
        cancelResults();
        if (!preservePendingSelection) clearPendingSelection();
        if (clearSearch)
        {
            searchNavigationPending_ = false;
            searchNavigationFailed_ = false;
            clearSearchScope();
            customResultName_.clear();
            customResultRestorePath_.clear();
        }
        return browser_->BrowseToObject(object, flags);
    }

    HRESULT ExplorerBrowserHost::browsePath(std::wstring_view path) noexcept
    {
        const std::wstring expanded = trimAndExpand(path);
        if (expanded.empty())
        {
            return E_INVALIDARG;
        }
        IShellItem* item{};
        const HRESULT status = SHCreateItemFromParsingName(expanded.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(status))
        {
            return status;
        }
        const HRESULT browseStatus = browseObject(item, SBSP_ABSOLUTE, true);
        item->Release();
        return browseStatus;
    }

    HRESULT ExplorerBrowserHost::browseParentAndSelect(std::wstring_view path) noexcept
    {
        if (browser_ == nullptr || path.empty() || path.size() >= 32767) return E_INVALIDARG;
        std::wstring expanded;
        try
        {
            expanded = trimAndExpand(path);
        }
        catch (...)
        {
            return E_OUTOFMEMORY;
        }
        if (expanded.empty()) return E_INVALIDARG;
        PIDLIST_ABSOLUTE item{};
        HRESULT status = SHParseDisplayName(expanded.c_str(), nullptr, &item, 0, nullptr);
        PIDLIST_ABSOLUTE parent = SUCCEEDED(status) ? ILCloneFull(item) : nullptr;
        if (SUCCEEDED(status) && parent == nullptr) status = E_OUTOFMEMORY;
        if (SUCCEEDED(status) && !ILRemoveLastID(parent)) status = E_INVALIDARG;
        IShellItem* parentItem{};
        if (SUCCEEDED(status))
            status = SHCreateItemFromIDList(parent, IID_PPV_ARGS(&parentItem));
        if (SUCCEEDED(status))
        {
            clearPendingSelection();
            pendingSelection_ = item;
            item = nullptr;
            status = browseObject(parentItem, SBSP_ABSOLUTE, true, true);
            if (FAILED(status)) clearPendingSelection();
        }
        if (parentItem != nullptr) parentItem->Release();
        if (parent != nullptr) ILFree(parent);
        if (item != nullptr) ILFree(item);
        return status;
    }

    HRESULT ExplorerBrowserHost::browseKnownFolder(REFKNOWNFOLDERID folderId) noexcept
    {
        IShellItem* item{};
        const HRESULT status = SHGetKnownFolderItem(folderId, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item));
        if (FAILED(status))
        {
            return status;
        }
        const HRESULT browseStatus = browseObject(item, SBSP_ABSOLUTE, true);
        item->Release();
        return browseStatus;
    }

    HRESULT ExplorerBrowserHost::browseBack() noexcept
    {
        if (browser_ == nullptr)
        {
            return E_UNEXPECTED;
        }
        if (!customResultName_.empty())
        {
            return customResultRestorePath_.empty() ? S_FALSE : browsePath(customResultRestorePath_);
        }
        if (!canGoBack())
        {
            return S_FALSE;
        }
        pendingTravelDelta_ = -1;
        const HRESULT status = browser_->BrowseToIDList(nullptr, SBSP_NAVIGATEBACK);
        if (FAILED(status))
        {
            pendingTravelDelta_ = 0;
        }
        return status;
    }

    HRESULT ExplorerBrowserHost::browseForward() noexcept
    {
        if (browser_ == nullptr)
        {
            return E_UNEXPECTED;
        }
        if (!canGoForward())
        {
            return S_FALSE;
        }
        pendingTravelDelta_ = 1;
        const HRESULT status = browser_->BrowseToIDList(nullptr, SBSP_NAVIGATEFORWARD);
        if (FAILED(status))
        {
            pendingTravelDelta_ = 0;
        }
        return status;
    }

    HRESULT ExplorerBrowserHost::browseUp() noexcept
    {
        if (!customResultName_.empty())
        {
            return customResultRestorePath_.empty() ? S_FALSE : browsePath(customResultRestorePath_);
        }
        return browser_ != nullptr ? browser_->BrowseToIDList(nullptr, SBSP_PARENT) : E_UNEXPECTED;
    }

    HRESULT ExplorerBrowserHost::refresh() noexcept
    {
        if (browser_ == nullptr)
        {
            return E_UNEXPECTED;
        }
        IShellView* view{};
        const HRESULT status = browser_->GetCurrentView(IID_PPV_ARGS(&view));
        if (FAILED(status))
        {
            return status;
        }
        const HRESULT refreshStatus = view->Refresh();
        view->Release();
        return refreshStatus;
    }

    HRESULT ExplorerBrowserHost::currentFolderItem(IShellItem** item) const noexcept
    {
        if (item == nullptr)
        {
            return E_POINTER;
        }
        *item = nullptr;
        return currentFolder_ != nullptr
            ? SHCreateItemFromIDList(currentFolder_, IID_PPV_ARGS(item))
            : E_UNEXPECTED;
    }

    HRESULT ExplorerBrowserHost::currentFolderView(IFolderView2** view) const noexcept
    {
        if (view == nullptr)
        {
            return E_POINTER;
        }
        *view = nullptr;
        return browser_ != nullptr ? browser_->GetCurrentView(IID_PPV_ARGS(view)) : E_UNEXPECTED;
    }

    HRESULT ExplorerBrowserHost::createSearchFolder(std::wstring_view query, IShellItem** item) noexcept
    {
        if (item == nullptr || currentFolder_ == nullptr || query.empty())
        {
            return E_INVALIDARG;
        }
        *item = nullptr;
        try
        {
            const std::wstring& scope = cachedEditingName_;
            const std::wstring encodedQuery = urlEncode(query);
            const std::wstring encodedScope = urlEncode(scope);
            if (encodedQuery.empty() || encodedScope.empty())
            {
                return E_INVALIDARG;
            }

            // The location crumb lets Windows Search traverse non-indexed local or UNC folders itself.
            const std::wstring uri = L"search-ms:query=" + encodedQuery +
                L"&crumb=location:" + encodedScope + L",include,recursive";
            return SHCreateItemFromParsingName(uri.c_str(), nullptr, IID_PPV_ARGS(item));
        }
        catch (...)
        {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT ExplorerBrowserHost::search(std::wstring_view query) noexcept
    {
        searchNavigationFailed_ = false;
        if (query.empty())
        {
            if (searchScope_ == nullptr)
            {
                return S_FALSE;
            }
            IShellItem* scope{};
            HRESULT status = SHCreateItemFromIDList(searchScope_, IID_PPV_ARGS(&scope));
            if (SUCCEEDED(status))
            {
                status = browseObject(scope, SBSP_ABSOLUTE, true);
                scope->Release();
            }
            return status;
        }

        if (searchScope_ == nullptr)
        {
            searchScope_ = ILCloneFull(currentFolder_);
            if (searchScope_ == nullptr)
            {
                return E_OUTOFMEMORY;
            }
        }
        else
        {
            PIDLIST_ABSOLUTE scope = ILCloneFull(searchScope_);
            if (scope == nullptr)
            {
                return E_OUTOFMEMORY;
            }
            clearCurrentFolder();
            currentFolder_ = scope;
            refreshCurrentFolderNames();
        }

        IShellItem* searchFolder{};
        const HRESULT status = createSearchFolder(query, &searchFolder);
        if (FAILED(status))
        {
            return status;
        }
        searchNavigationPending_ = true;
        const HRESULT browseStatus = browseObject(searchFolder, SBSP_ABSOLUTE, false);
        if (FAILED(browseStatus))
        {
            searchNavigationPending_ = false;
        }
        searchFolder->Release();
        return browseStatus;
    }

    bool ExplorerBrowserHost::consumeSearchNavigationFailure() noexcept
    {
        const bool failed = searchNavigationFailed_;
        searchNavigationFailed_ = false;
        return failed;
    }

    HRESULT ExplorerBrowserHost::beginResults(std::wstring_view displayName) noexcept
    {
        cancelResults();
        if (browser_ == nullptr || displayName.empty() || displayName.size() >= 32767)
            return E_INVALIDARG;
        try
        {
            resultDisplayName_.assign(displayName);
            resultRestorePath_ = restorableName();
        }
        catch (...)
        {
            cancelResults();
            return E_OUTOFMEMORY;
        }
        HRESULT status = browser_->FillFromObject(nullptr, EBF_NODROPTARGET);
        if (SUCCEEDED(status)) status = browser_->GetCurrentView(IID_PPV_ARGS(&resultView_));
        if (SUCCEEDED(status)) status = resultView_->GetFolder(IID_PPV_ARGS(&resultsFolder_));
        if (FAILED(status)) cancelResults();
        return status;
    }

    HRESULT ExplorerBrowserHost::addResult(std::wstring_view path) noexcept
    {
        if (resultsFolder_ == nullptr || path.empty() || path.size() >= 32767)
            return E_UNEXPECTED;
        IShellItem* item{};
        HRESULT status{};
        try
        {
            const std::wstring parsingName(path);
            status = SHCreateItemFromParsingName(parsingName.c_str(), nullptr,
                IID_PPV_ARGS(&item));
        }
        catch (...)
        {
            status = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(status)) status = resultsFolder_->AddItem(item);
        if (item != nullptr) item->Release();
        if (status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return S_FALSE;
        return status;
    }

    HRESULT ExplorerBrowserHost::finishResults() noexcept
    {
        if (resultView_ == nullptr || resultsFolder_ == nullptr) return E_UNEXPECTED;
        IColumnManager* columns{};
        if (SUCCEEDED(resultView_->QueryInterface(IID_PPV_ARGS(&columns))))
        {
            const PROPERTYKEY keys[]{PKEY_ItemNameDisplay, PKEY_ItemFolderPathDisplay,
                PKEY_Keywords, PKEY_DateModified, PKEY_Size};
            columns->SetColumns(keys, static_cast<UINT>(std::size(keys)));
            columns->Release();
        }
        searchNavigationPending_ = false;
        clearSearchScope();
        clearCurrentFolder();
        customResultName_ = std::move(resultDisplayName_);
        customResultRestorePath_ = std::move(resultRestorePath_);
        resultsFolder_->Release();
        resultsFolder_ = nullptr;
        resultView_->Release();
        resultView_ = nullptr;
        notify(navigationMessage_);
        return S_OK;
    }

    void ExplorerBrowserHost::cancelResults() noexcept
    {
        if (resultsFolder_ != nullptr)
        {
            resultsFolder_->Release();
            resultsFolder_ = nullptr;
        }
        if (resultView_ != nullptr)
        {
            resultView_->Release();
            resultView_ = nullptr;
        }
        resultDisplayName_.clear();
        resultRestorePath_.clear();
    }

    void ExplorerBrowserHost::abortResults() noexcept
    {
        std::wstring restore = std::move(resultRestorePath_);
        cancelResults();
        if (!restore.empty()) (void)browsePath(restore);
    }

    void ExplorerBrowserHost::setGitDecorations(std::wstring statusOutput,
        std::wstring_view workingDirectory) noexcept
    {
        ++gitBuildGeneration_;
        gitDecorationBuilder_.cancel();
        pendingGitDecorationRoot_.clear();
        if (statusOutput.empty() || workingDirectory.empty())
        {
            gitDecorations_.clear();
            gitDecorationRoot_.clear();
            if (overlay_ != nullptr)
            {
                ShowWindow(overlay_, SW_HIDE);
                scheduleGitDecorationLayout();
            }
            return;
        }
        try
        {
            pendingGitDecorationRoot_ = core::normalizedGitPath(workingDirectory);
            if (pendingGitDecorationRoot_ != gitDecorationRoot_)
            {
                gitDecorations_.clear();
                gitDecorationRoot_.clear();
            }
            if (!gitDecorationBuilder_.start(
                    pendingGitDecorationRoot_, std::move(statusOutput)))
                pendingGitDecorationRoot_.clear();
        }
        catch (...)
        {
            gitDecorationBuilder_.cancel();
            pendingGitDecorationRoot_.clear();
            gitDecorations_.clear();
            gitDecorationRoot_.clear();
        }
        if (overlay_ != nullptr)
        {
            if (gitDecorationBuilder_.active())
                postGitDecorationBuild();
            else
            {
                ShowWindow(overlay_, gitDecorations_.empty() ? SW_HIDE : SW_SHOWNOACTIVATE);
                scheduleGitDecorationLayout();
            }
        }
    }

    HRESULT ExplorerBrowserHost::renameSelection() noexcept
    {
        IFolderView2* view{};
        const HRESULT status = currentFolderView(&view);
        if (FAILED(status))
        {
            return status;
        }
        const HRESULT renameStatus = view->DoRename();
        view->Release();
        return renameStatus;
    }

    HRESULT ExplorerBrowserHost::clearSelection() noexcept
    {
        IFolderView2* view{};
        const HRESULT status = currentFolderView(&view);
        if (FAILED(status)) return status;
        const HRESULT clearStatus = view->SelectItem(-1, SVSI_DESELECTOTHERS);
        view->Release();
        return clearStatus;
    }

    HRESULT ExplorerBrowserHost::invertSelection() noexcept
    {
        IFolderView2* view{};
        HRESULT status = currentFolderView(&view);
        int itemCount{};
        if (SUCCEEDED(status)) status = view->ItemCount(SVGIO_ALLVIEW, &itemCount);
        if (view != nullptr) view->Release();
        if (SUCCEEDED(status) && !selectionInversion_.beginSnapshot(itemCount))
            status = E_INVALIDARG;
        selectionInversionSnapshotCurrent_ = -1;
        const UINT message = selectionInversionMessage();
        if (SUCCEEDED(status) && selectionInversion_.snapshotActive() &&
            (shellViewWindow_ == nullptr || message == 0))
        {
            status = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        }
        else if (SUCCEEDED(status) && selectionInversion_.snapshotActive() &&
            !PostMessageW(shellViewWindow_, message, 0, 0))
        {
            const DWORD failure = GetLastError();
            status = HRESULT_FROM_WIN32(failure == ERROR_SUCCESS ?
                ERROR_INVALID_WINDOW_HANDLE : failure);
        }
        if (FAILED(status))
        {
            selectionInversion_.cancel();
            selectionInversionSnapshotCurrent_ = -1;
        }
        return status;
    }

    HRESULT ExplorerBrowserHost::executeOleCommand(DWORD command) noexcept
    {
        if (browser_ == nullptr)
        {
            return E_UNEXPECTED;
        }
        IOleCommandTarget* target{};
        const HRESULT status = browser_->GetCurrentView(IID_PPV_ARGS(&target));
        if (FAILED(status))
        {
            return status;
        }
        const HRESULT executeStatus = target->Exec(nullptr, command, OLECMDEXECOPT_DODEFAULT, nullptr, nullptr);
        target->Release();
        return executeStatus;
    }

    HRESULT ExplorerBrowserHost::invokeSelectionVerb(const char* verb) noexcept
    {
        IFolderView2* view{};
        const HRESULT status = currentFolderView(&view);
        if (FAILED(status))
        {
            return status;
        }
        const HRESULT invokeStatus = view->InvokeVerbOnSelection(verb);
        view->Release();
        return invokeStatus;
    }

    HRESULT ExplorerBrowserHost::invokeBackgroundVerb(const char* verb) noexcept
    {
        if (verb == nullptr || *verb == '\0') return E_INVALIDARG;
        IFolderView2* view{};
        IShellView* shellView{};
        IShellFolder* folder{};
        IContextMenu* contextMenu{};
        HWND viewWindow{};
        HRESULT status = currentFolderView(&view);
        if (SUCCEEDED(status)) status = view->QueryInterface(IID_PPV_ARGS(&shellView));
        if (SUCCEEDED(status)) status = shellView->GetWindow(&viewWindow);
        if (SUCCEEDED(status)) status = view->GetFolder(IID_PPV_ARGS(&folder));
        if (SUCCEEDED(status)) status = folder->CreateViewObject(viewWindow,
            IID_PPV_ARGS(&contextMenu));
        HMENU menu = SUCCEEDED(status) ? CreatePopupMenu() : nullptr;
        if (SUCCEEDED(status) && menu == nullptr) status = HRESULT_FROM_WIN32(GetLastError());
        constexpr UINT firstCommand = 1;
        constexpr UINT lastCommand = 0x7fff;
        if (SUCCEEDED(status))
        {
            // ponytail: Ask the Shell for its own folder-background verb so clipboard link
            // formats, provider locations, collisions, and elevation keep Explorer semantics.
            status = contextMenu->QueryContextMenu(menu, 0, firstCommand, lastCommand,
                CMF_NORMAL | CMF_OPTIMIZEFORINVOKE);
        }
        UINT selectedCommand{};
        if (SUCCEEDED(status))
        {
            const int count = GetMenuItemCount(menu);
            for (int index = 0; index < count; ++index)
            {
                const UINT command = GetMenuItemID(menu, index);
                if (command < firstCommand || command > lastCommand) continue;
                char canonical[64]{};
                if (SUCCEEDED(contextMenu->GetCommandString(command - firstCommand,
                        GCS_VERBA, nullptr, canonical, static_cast<UINT>(std::size(canonical)))) &&
                    lstrcmpiA(canonical, verb) == 0)
                {
                    selectedCommand = command;
                    break;
                }
            }
            if (selectedCommand == 0) status = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (SUCCEEDED(status))
        {
            CMINVOKECOMMANDINFOEX invocation{};
            invocation.cbSize = sizeof(invocation);
            invocation.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK;
            invocation.hwnd = viewWindow;
            invocation.lpVerb = MAKEINTRESOURCEA(selectedCommand - firstCommand);
            invocation.lpVerbW = MAKEINTRESOURCEW(selectedCommand - firstCommand);
            invocation.nShow = SW_SHOWNORMAL;
            status = contextMenu->InvokeCommand(
                reinterpret_cast<CMINVOKECOMMANDINFO*>(&invocation));
        }
        if (menu != nullptr) DestroyMenu(menu);
        if (contextMenu != nullptr) contextMenu->Release();
        if (folder != nullptr) folder->Release();
        if (shellView != nullptr) shellView->Release();
        if (view != nullptr) view->Release();
        return status;
    }

    HRESULT ExplorerBrowserHost::autoSizeColumns() noexcept
    {
        IFolderView2* view{};
        IColumnManager* columns{};
        HRESULT status = currentFolderView(&view);
        if (SUCCEEDED(status)) status = view->QueryInterface(IID_PPV_ARGS(&columns));
        UINT count{};
        if (SUCCEEDED(status)) status = columns->GetColumnCount(CM_ENUM_VISIBLE, &count);
        constexpr UINT maximumColumns = 256;
        if (SUCCEEDED(status) && (count == 0 || count > maximumColumns)) status = E_INVALIDARG;
        std::vector<PROPERTYKEY> keys;
        if (SUCCEEDED(status))
        {
            try
            {
                keys.resize(count);
            }
            catch (...)
            {
                status = E_OUTOFMEMORY;
            }
        }
        if (SUCCEEDED(status)) status = columns->GetColumns(CM_ENUM_VISIBLE, keys.data(), count);
        CM_COLUMNINFO information{};
        information.cbSize = sizeof(information);
        information.dwMask = CM_MASK_WIDTH;
        information.uWidth = static_cast<UINT>(CM_WIDTH_AUTOSIZE);
        for (const auto& key : keys)
        {
            if (SUCCEEDED(status)) status = columns->SetColumnInfo(key, &information);
        }
        if (columns != nullptr) columns->Release();
        if (view != nullptr) view->Release();
        return status;
    }

    HRESULT ExplorerBrowserHost::setView(FOLDERVIEWMODE mode, int iconSize) noexcept
    {
        IFolderView2* view{};
        const HRESULT status = currentFolderView(&view);
        if (FAILED(status))
        {
            return status;
        }
        const HRESULT setStatus = view->SetViewModeAndIconSize(mode, iconSize);
        view->Release();
        return setStatus;
    }

    void ExplorerBrowserHost::setInitialView(std::uint32_t view) noexcept
    {
        initialView_ = std::min(view, 5U);
        initialViewPending_ = true;
    }

    void ExplorerBrowserHost::togglePreviewPane() noexcept
    {
        previewPaneVisible_ = !previewPaneVisible_;
        refreshPaneState();
    }

    void ExplorerBrowserHost::toggleDetailsPane() noexcept
    {
        detailsPaneVisible_ = !detailsPaneVisible_;
        refreshPaneState();
    }

    std::wstring ExplorerBrowserHost::editingName() const
    {
        if (!customResultName_.empty()) return customResultName_;
        return cachedEditingName_;
    }

    std::wstring ExplorerBrowserHost::restorableName() const
    {
        if (!customResultName_.empty()) return customResultRestorePath_;
        if (searchScope_ != nullptr)
        {
            std::wstring result = nameFromPidl(searchScope_, SIGDN_DESKTOPABSOLUTEEDITING);
            if (result.empty())
            {
                result = nameFromPidl(searchScope_, SIGDN_DESKTOPABSOLUTEPARSING);
            }
            return result;
        }
        return editingName();
    }

    std::wstring ExplorerBrowserHost::filesystemPath() const
    {
        if (!customResultName_.empty()) return {};
        return cachedFileSystemPath_;
    }

    std::wstring ExplorerBrowserHost::parsingName() const
    {
        if (!customResultName_.empty()) return {};
        return cachedParsingName_;
    }

    std::wstring ExplorerBrowserHost::displayName() const
    {
        if (!customResultName_.empty()) return customResultName_;
        return cachedDisplayName_;
    }

    void ExplorerBrowserHost::itemCounts(int& total, int& selected) const noexcept
    {
        total = 0;
        selected = 0;
        IFolderView2* view{};
        if (FAILED(currentFolderView(&view))) return;
        view->ItemCount(SVGIO_ALLVIEW, &total);
        view->ItemCount(SVGIO_SELECTION, &selected);
        view->Release();
    }

    int ExplorerBrowserHost::selectedCount() const noexcept
    {
        IFolderView2* view{};
        if (FAILED(currentFolderView(&view))) return 0;
        int count{};
        view->ItemCount(SVGIO_SELECTION, &count);
        view->Release();
        return count;
    }

    HRESULT ExplorerBrowserHost::selectedItems(IShellItemArray** items) const noexcept
    {
        if (items == nullptr)
        {
            return E_POINTER;
        }
        *items = nullptr;
        IFolderView2* view{};
        const HRESULT status = currentFolderView(&view);
        if (FAILED(status))
        {
            return status;
        }
        const HRESULT selectionStatus = view->GetSelection(FALSE, items);
        view->Release();
        return selectionStatus;
    }

    HRESULT ExplorerBrowserHost::QueryInterface(REFIID interfaceId, void** result) noexcept
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }
        *result = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IServiceProvider)
        {
            *result = static_cast<IServiceProvider*>(this);
        }
        else if (interfaceId == IID_ICommDlgBrowser || interfaceId == IID_ICommDlgBrowser2)
        {
            *result = static_cast<ICommDlgBrowser2*>(this);
        }
        else if (interfaceId == IID_IExplorerBrowserEvents)
        {
            *result = static_cast<IExplorerBrowserEvents*>(this);
        }
        else if (interfaceId == IID_IExplorerPaneVisibility)
        {
            *result = static_cast<IExplorerPaneVisibility*>(this);
        }
        else
        {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG ExplorerBrowserHost::AddRef() noexcept
    {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG ExplorerBrowserHost::Release() noexcept
    {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    HRESULT ExplorerBrowserHost::QueryService(REFGUID serviceId, REFIID interfaceId, void** result) noexcept
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }
        *result = nullptr;
        if (serviceId == SID_SExplorerBrowserFrame || serviceId == SID_ExplorerPaneVisibility)
        {
            return QueryInterface(interfaceId, result);
        }
        return E_NOINTERFACE;
    }

    HRESULT ExplorerBrowserHost::OnDefaultCommand(IShellView*) noexcept
    {
        return S_FALSE;
    }

    HRESULT ExplorerBrowserHost::OnStateChange(IShellView*, ULONG change) noexcept
    {
        if (change == CDBOSC_SELCHANGE)
        {
            if (selectionInversion_.snapshotActive() || selectionInversion_.active())
                selectionInversionNotificationDeferred_ = true;
            else
                notify(selectionMessage_);
        }
        return S_OK;
    }

    HRESULT ExplorerBrowserHost::IncludeObject(IShellView*, PCUITEMID_CHILD) noexcept
    {
        return S_OK;
    }

    HRESULT ExplorerBrowserHost::Notify(IShellView*, DWORD) noexcept
    {
        return S_OK;
    }

    HRESULT ExplorerBrowserHost::GetDefaultMenuText(IShellView*, PWSTR, int) noexcept
    {
        return E_NOTIMPL;
    }

    HRESULT ExplorerBrowserHost::GetViewFlags(DWORD* flags) noexcept
    {
        if (flags == nullptr)
        {
            return E_POINTER;
        }
        *flags = CDB2GVF_NOINCLUDEITEM;
        return S_OK;
    }

    HRESULT ExplorerBrowserHost::OnNavigationPending(PCIDLIST_ABSOLUTE folder) noexcept
    {
        if (pendingSelection_ != nullptr && !ILIsParent(folder, pendingSelection_, TRUE))
            clearPendingSelection();
        selectionInversion_.cancel();
        selectionInversionSnapshotCurrent_ = -1;
        selectionInversionNotificationDeferred_ = false;
        if (overlay_ != nullptr) ShowWindow(overlay_, SW_HIDE);
        return S_OK;
    }

    HRESULT ExplorerBrowserHost::OnViewCreated(IShellView* shellView) noexcept
    {
        selectionInversion_.cancel();
        selectionInversionSnapshotCurrent_ = -1;
        selectionInversionNotificationDeferred_ = false;
        if (shellViewWindow_ != nullptr)
            RemoveWindowSubclass(shellViewWindow_, &ExplorerBrowserHost::shellViewProcedure, 1);
        shellViewWindow_ = nullptr;
        if (shellView != nullptr && SUCCEEDED(shellView->GetWindow(&shellViewWindow_)) &&
            shellViewWindow_ != nullptr)
        {
            SetWindowSubclass(shellViewWindow_, &ExplorerBrowserHost::shellViewProcedure, 1,
                reinterpret_cast<DWORD_PTR>(this));
        }
        if (overlay_ == nullptr && container_ != nullptr)
        {
            RECT client{};
            GetClientRect(container_, &client);
            overlay_ = CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                L"STATIC", nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                0, 0, client.right, client.bottom, container_, nullptr,
                reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(container_, GWLP_HINSTANCE)), nullptr);
            if (overlay_ != nullptr)
            {
                SetWindowSubclass(overlay_, &ExplorerBrowserHost::overlayProcedure, 1,
                    reinterpret_cast<DWORD_PTR>(this));
                // ponytail: Shell scroll messages drive repaint; the slow timer only recovers
                // from provider-specific views that do not emit the standard scroll messages.
                SetTimer(overlay_, 1, 2000, nullptr);
                SetWindowPos(overlay_, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (!gitDecorations_.empty()) ShowWindow(overlay_, SW_SHOWNOACTIVATE);
                postGitDecorationBuild();
                scheduleGitDecorationLayout();
            }
        }
        if (!visible_ && container_ != nullptr)
        {
            ShowWindow(container_, SW_HIDE);
        }
        if (initialViewPending_)
        {
            initialViewPending_ = false;
            switch (initialView_)
            {
            case 1: setView(FVM_LIST); break;
            case 2: setView(FVM_ICON, 48); break;
            case 3: setView(FVM_ICON, 96); break;
            case 4: setView(FVM_TILE); break;
            case 5: setView(FVM_CONTENT); break;
            default: setView(FVM_DETAILS); break;
            }
        }
        return S_OK;
    }

    HRESULT ExplorerBrowserHost::OnNavigationComplete(PCIDLIST_ABSOLUTE folder) noexcept
    {
        PIDLIST_ABSOLUTE replacement = ILCloneFull(folder);
        if (replacement == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        clearCurrentFolder();
        currentFolder_ = replacement;
        refreshCurrentFolderNames();
        if (overlay_ != nullptr)
        {
            const std::wstring current = core::normalizedGitPath(
                cachedFileSystemPath_);
            ShowWindow(overlay_, !gitDecorations_.empty() && current == gitDecorationRoot_ ?
                SW_SHOWNOACTIVATE : SW_HIDE);
            scheduleGitDecorationLayout();
        }
        if (pendingTravelDelta_ != 0)
        {
            const auto next = static_cast<std::ptrdiff_t>(historyIndex_) + pendingTravelDelta_;
            if (next >= 0 && static_cast<std::size_t>(next) < history_.size())
            {
                historyIndex_ = static_cast<std::size_t>(next);
            }
            pendingTravelDelta_ = 0;
        }
        else
        {
            const std::wstring& location = cachedEditingName_;
            if (!location.empty() && (history_.empty() ||
                _wcsicmp(history_[historyIndex_].c_str(), location.c_str()) != 0))
            {
                if (!history_.empty() && historyIndex_ + 1 < history_.size())
                {
                    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(historyIndex_ + 1), history_.end());
                }
                history_.push_back(location);
                historyIndex_ = history_.size() - 1;
            }
        }
        if (searchNavigationPending_)
        {
            searchNavigationPending_ = false;
            searchNavigationFailed_ = false;
        }
        else
        {
            clearSearchScope();
        }
        if (pendingSelection_ != nullptr)
        {
            if (ILIsParent(folder, pendingSelection_, TRUE))
            {
                IShellView* view{};
                if (SUCCEEDED(browser_->GetCurrentView(IID_PPV_ARGS(&view))))
                {
                    view->SelectItem(ILFindLastID(pendingSelection_), static_cast<SVSIF>(
                        SVSI_SELECT | SVSI_FOCUSED | SVSI_ENSUREVISIBLE | SVSI_DESELECTOTHERS));
                    view->Release();
                }
            }
            clearPendingSelection();
        }
        notify(navigationMessage_);
        return S_OK;
    }

    HRESULT ExplorerBrowserHost::OnNavigationFailed(PCIDLIST_ABSOLUTE) noexcept
    {
        searchNavigationFailed_ = searchNavigationPending_;
        searchNavigationPending_ = false;
        pendingTravelDelta_ = 0;
        clearPendingSelection();
        notify(navigationMessage_);
        return S_OK;
    }

    HRESULT ExplorerBrowserHost::GetPaneState(REFEXPLORERPANE pane, EXPLORERPANESTATE* state) noexcept
    {
        if (state == nullptr)
        {
            return E_POINTER;
        }
        if (IsEqualGUID(pane, EP_PreviewPane))
        {
            *state = static_cast<EXPLORERPANESTATE>(EPS_FORCE |
                (previewPaneVisible_ ? EPS_DEFAULT_ON : EPS_DEFAULT_OFF));
        }
        else if (IsEqualGUID(pane, EP_DetailsPane))
        {
            *state = static_cast<EXPLORERPANESTATE>(EPS_FORCE |
                (detailsPaneVisible_ ? EPS_DEFAULT_ON : EPS_DEFAULT_OFF));
        }
        else
        {
            *state = static_cast<EXPLORERPANESTATE>(EPS_FORCE | EPS_DEFAULT_OFF);
        }
        return S_OK;
    }

    void ExplorerBrowserHost::clearCurrentFolder() noexcept
    {
        if (currentFolder_ != nullptr)
        {
            ILFree(currentFolder_);
            currentFolder_ = nullptr;
        }
        cachedEditingName_.clear();
        cachedFileSystemPath_.clear();
        cachedParsingName_.clear();
        cachedDisplayName_.clear();
    }

    void ExplorerBrowserHost::clearPendingSelection() noexcept
    {
        if (pendingSelection_ != nullptr)
        {
            ILFree(pendingSelection_);
            pendingSelection_ = nullptr;
        }
    }

    void ExplorerBrowserHost::refreshCurrentFolderNames() noexcept
    {
        cachedEditingName_.clear();
        cachedFileSystemPath_.clear();
        cachedParsingName_.clear();
        cachedDisplayName_.clear();
        if (currentFolder_ == nullptr) return;
        try
        {
            cachedParsingName_ = nameFromPidl(currentFolder_, SIGDN_DESKTOPABSOLUTEPARSING);
            cachedEditingName_ = nameFromPidl(currentFolder_, SIGDN_DESKTOPABSOLUTEEDITING);
            if (cachedEditingName_.empty()) cachedEditingName_ = cachedParsingName_;
            cachedFileSystemPath_ = nameFromPidl(currentFolder_, SIGDN_FILESYSPATH);
            cachedDisplayName_ = nameFromPidl(currentFolder_, SIGDN_NORMALDISPLAY);
        }
        catch (...)
        {
            cachedEditingName_.clear();
            cachedFileSystemPath_.clear();
            cachedParsingName_.clear();
            cachedDisplayName_.clear();
        }
    }

    void ExplorerBrowserHost::clearSearchScope() noexcept
    {
        if (searchScope_ != nullptr)
        {
            ILFree(searchScope_);
            searchScope_ = nullptr;
        }
    }

    void ExplorerBrowserHost::notify(UINT message) noexcept
    {
        if (notificationWindow_ == nullptr || message == 0) return;
        core::CoalescingGate* gate{};
        if (message == navigationMessage_) gate = &navigationNotificationGate_;
        else if (message == selectionMessage_) gate = &selectionNotificationGate_;
        if (gate != nullptr && !gate->request()) return;
        if (!PostMessageW(notificationWindow_, message, reinterpret_cast<WPARAM>(this), 0) &&
            gate != nullptr)
            gate->consume();
    }

    void ExplorerBrowserHost::notificationDelivered(UINT message) noexcept
    {
        if (message == navigationMessage_)
        {
            navigationNotificationGate_.consume();
        }
        else if (message == selectionMessage_)
        {
            selectionNotificationGate_.consume();
        }
    }

    void ExplorerBrowserHost::refreshPaneState() noexcept
    {
        if (browser_ == nullptr)
        {
            return;
        }
        // ponytail: reapplying the Shell frame option is the smallest supported pane refresh hook.
        browser_->SetOptions(EBO_NONE);
        browser_->SetOptions(EBO_SHOWFRAMES);
    }

    LRESULT CALLBACK ExplorerBrowserHost::overlayProcedure(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR referenceData)
    {
        auto* self = reinterpret_cast<ExplorerBrowserHost*>(referenceData);
        switch (message)
        {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_TIMER:
            if (wParam == 1 && self != nullptr && IsWindowVisible(window) &&
                !self->gitDecorations_.empty())
                self->scheduleGitDecorationLayout();
            return 0;
        case gitDecorationLayoutMessage:
            if (self != nullptr)
                self->processGitDecorationLayout(static_cast<std::uint32_t>(wParam));
            return 0;
        case gitDecorationBuildMessage:
            if (self != nullptr)
                self->processGitDecorationBuild(static_cast<std::uint32_t>(wParam));
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(window, &paint);
            if (self != nullptr) self->paintGitDecorations(deviceContext);
            EndPaint(window, &paint);
            return 0;
        }
        default:
            break;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    LRESULT CALLBACK ExplorerBrowserHost::shellViewProcedure(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR referenceData)
    {
        auto* self = reinterpret_cast<ExplorerBrowserHost*>(referenceData);
        if (self != nullptr && message == selectionInversionMessage())
        {
            self->processSelectionInversion();
            return 0;
        }
        if (message == WM_NCDESTROY && self != nullptr && self->shellViewWindow_ == window)
        {
            self->shellViewWindow_ = nullptr;
            self->selectionInversion_.cancel();
            self->selectionInversionSnapshotCurrent_ = -1;
            self->selectionInversionNotificationDeferred_ = false;
        }
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (self != nullptr && self->overlay_ != nullptr)
        {
            switch (message)
            {
            case WM_VSCROLL:
            case WM_HSCROLL:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_SIZE:
            case WM_WINDOWPOSCHANGED:
                self->scheduleGitDecorationLayout();
                break;
            default:
                break;
            }
        }
        return result;
    }

    void ExplorerBrowserHost::processSelectionInversion() noexcept
    {
        if (!selectionInversion_.snapshotActive() && !selectionInversion_.active()) return;
        const auto postNext = [this]() noexcept
        {
            const UINT message = selectionInversionMessage();
            if (message != 0 && shellViewWindow_ != nullptr &&
                PostMessageW(shellViewWindow_, message, 0, 0))
                return true;
            selectionInversion_.cancel();
            selectionInversionSnapshotCurrent_ = -1;
            flushSelectionInversionNotification();
            return false;
        };

        if (selectionInversion_.snapshotActive())
        {
            IFolderView2* view{};
            HRESULT status = currentFolderView(&view);
            constexpr std::size_t selectedItemsPerDispatch = 32;
            const ULONGLONG deadline = GetTickCount64() + 6;
            for (std::size_t index = 0; SUCCEEDED(status) &&
                index < selectedItemsPerDispatch; ++index)
            {
                int item{};
                const HRESULT next = view->GetSelectedItem(
                    selectionInversionSnapshotCurrent_, &item);
                if (next == S_FALSE)
                {
                    if (!selectionInversion_.finishSnapshot()) status = E_UNEXPECTED;
                    break;
                }
                if (FAILED(next) || item <= selectionInversionSnapshotCurrent_)
                {
                    status = FAILED(next) ? next : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    break;
                }
                try
                {
                    if (!selectionInversion_.addSelected(item))
                        status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
                catch (...)
                {
                    status = E_OUTOFMEMORY;
                }
                selectionInversionSnapshotCurrent_ = item;
                if (GetTickCount64() >= deadline) break;
            }
            if (view != nullptr) view->Release();
            if (FAILED(status))
            {
                selectionInversion_.cancel();
                selectionInversionSnapshotCurrent_ = -1;
                flushSelectionInversionNotification();
                return;
            }
            if (selectionInversion_.snapshotActive() || selectionInversion_.active())
                (void)postNext();
            else
                selectionInversionSnapshotCurrent_ = -1;
            return;
        }

        std::vector<core::SelectionChange> changes;
        try
        {
            // A smaller apply quantum limits Shell selection messages and provider latency.
            changes = selectionInversion_.next(16);
        }
        catch (...)
        {
            selectionInversion_.cancel();
            selectionInversionSnapshotCurrent_ = -1;
            flushSelectionInversionNotification();
            return;
        }
        IFolderView2* view{};
        HRESULT status = currentFolderView(&view);
        for (const auto& change : changes)
        {
            if (FAILED(status)) break;
            status = view->SelectItem(change.index, change.select ? SVSI_SELECT : SVSI_DESELECT);
        }
        if (view != nullptr) view->Release();
        if (FAILED(status))
        {
            selectionInversion_.cancel();
            selectionInversionSnapshotCurrent_ = -1;
            flushSelectionInversionNotification();
            return;
        }
        if (selectionInversion_.active())
        {
            (void)postNext();
        }
        else
        {
            selectionInversionSnapshotCurrent_ = -1;
            flushSelectionInversionNotification();
        }
    }

    void ExplorerBrowserHost::flushSelectionInversionNotification() noexcept
    {
        if (!selectionInversionNotificationDeferred_) return;
        selectionInversionNotificationDeferred_ = false;
        notify(selectionMessage_);
    }

    void ExplorerBrowserHost::postGitDecorationBuild() noexcept
    {
        if (overlay_ == nullptr || !gitDecorationBuilder_.active()) return;
        if (!PostMessageW(overlay_, gitDecorationBuildMessage, gitBuildGeneration_, 0))
        {
            gitDecorationBuilder_.cancel();
            pendingGitDecorationRoot_.clear();
            if (gitDecorations_.empty()) ShowWindow(overlay_, SW_HIDE);
        }
    }

    void ExplorerBrowserHost::processGitDecorationBuild(std::uint32_t generation) noexcept
    {
        if (generation != gitBuildGeneration_ || !gitDecorationBuilder_.active()) return;
        try
        {
            // A work unit is either one porcelain record or one ancestor merge. Deep paths
            // therefore cannot escape the same work cap as a large repository. The wall-clock
            // deadline additionally protects input when an individual map allocation is costly.
            constexpr std::size_t maximumWork = 64;
            constexpr std::size_t workQuantum = 2;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(6);
            std::size_t processed{};
            do
            {
                gitDecorationBuilder_.next(std::min(workQuantum, maximumWork - processed));
                processed += std::min(workQuantum, maximumWork - processed);
            } while (gitDecorationBuilder_.active() && processed < maximumWork &&
                std::chrono::steady_clock::now() < deadline);
        }
        catch (...)
        {
            gitDecorationBuilder_.cancel();
            pendingGitDecorationRoot_.clear();
            gitDecorations_.clear();
            gitDecorationRoot_.clear();
            if (overlay_ != nullptr)
            {
                ShowWindow(overlay_, SW_HIDE);
                scheduleGitDecorationLayout();
            }
            return;
        }
        if (gitDecorationBuilder_.active())
        {
            postGitDecorationBuild();
            return;
        }
        gitDecorations_ = gitDecorationBuilder_.take();
        gitDecorationRoot_ = std::move(pendingGitDecorationRoot_);
        pendingGitDecorationRoot_.clear();
        if (overlay_ == nullptr) return;
        bool matchesCurrent{};
        try
        {
            matchesCurrent = !gitDecorations_.empty() &&
                core::normalizedGitPath(cachedFileSystemPath_) ==
                    gitDecorationRoot_;
        }
        catch (...)
        {
            gitDecorations_.clear();
            gitDecorationRoot_.clear();
        }
        ShowWindow(overlay_, matchesCurrent ? SW_SHOWNOACTIVATE : SW_HIDE);
        scheduleGitDecorationLayout();
    }

    void ExplorerBrowserHost::scheduleGitDecorationLayout() noexcept
    {
        ++gitLayoutGeneration_;
        gitLayoutActive_ = false;
        gitLayoutCurrentItem_ = -1;
        gitLayoutExamined_ = 0;
        gitBadgeVisuals_.clear();
        pendingGitBadgeVisuals_.clear();
        if (overlay_ != nullptr) InvalidateRect(overlay_, nullptr, FALSE);
        if (overlay_ == nullptr || gitDecorations_.empty() || !IsWindowVisible(overlay_)) return;
        gitLayoutActive_ = true;
        if (!postGitDecorationLayout())
            gitLayoutActive_ = false;
    }

    bool ExplorerBrowserHost::postGitDecorationLayout() noexcept
    {
        if (overlay_ == nullptr || !gitLayoutActive_) return false;
        if (gitLayoutMessageGate_.pending()) return true;
        if (!gitLayoutMessageGate_.request()) return true;
        if (PostMessageW(overlay_, gitDecorationLayoutMessage, gitLayoutGeneration_, 0))
            return true;
        gitLayoutMessageGate_.consume();
        return false;
    }

    void ExplorerBrowserHost::processGitDecorationLayout(std::uint32_t generation) noexcept
    {
        gitLayoutMessageGate_.consume();
        if (!gitLayoutActive_ || overlay_ == nullptr) return;
        if (generation != gitLayoutGeneration_)
        {
            if (!postGitDecorationLayout()) gitLayoutActive_ = false;
            return;
        }
        IFolderView2* view{};
        if (FAILED(currentFolderView(&view)))
        {
            gitLayoutActive_ = false;
            return;
        }
        IShellView* shellView{};
        HWND viewWindow{};
        FOLDERVIEWMODE mode = FVM_DETAILS;
        int iconSize = 16;
        if (browser_ != nullptr && SUCCEEDED(browser_->GetCurrentView(IID_PPV_ARGS(&shellView))))
            shellView->GetWindow(&viewWindow);
        view->GetViewModeAndIconSize(&mode, &iconSize);
        RECT client{};
        GetClientRect(overlay_, &client);
        const int dpi = GetDpiForWindow(overlay_);
        const int badgeWidth = MulDiv(19, dpi, 96);
        const int badgeHeight = MulDiv(15, dpi, 96);
        const int inset = MulDiv(3, dpi, 96);
        bool exhausted{};
        bool failed{};
        constexpr std::size_t maximumVisibleItems = 1000;
        constexpr std::size_t itemsPerDispatch = 32;
        const ULONGLONG deadline = GetTickCount64() + 8;
        for (std::size_t batchIndex = 0; batchIndex < itemsPerDispatch &&
            gitLayoutExamined_ < maximumVisibleItems && GetTickCount64() < deadline; ++batchIndex)
        {
            int item{};
            if (view->GetVisibleItem(gitLayoutCurrentItem_, FALSE, &item) != S_OK ||
                item <= gitLayoutCurrentItem_)
            {
                exhausted = true;
                break;
            }
            gitLayoutCurrentItem_ = item;
            ++gitLayoutExamined_;
            IShellItem* shellItem{};
            PWSTR rawPath{};
            if (SUCCEEDED(view->GetItem(item, IID_PPV_ARGS(&shellItem))) && shellItem != nullptr)
                shellItem->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
            wchar_t label{};
            if (rawPath != nullptr)
            {
                try
                {
                    const auto decoration = gitDecorations_.find(core::normalizedGitPath(rawPath));
                    if (decoration != gitDecorations_.end()) label = decoration->second;
                }
                catch (...)
                {
                    failed = true;
                }
            }
            if (rawPath != nullptr) CoTaskMemFree(rawPath);
            if (shellItem != nullptr) shellItem->Release();
            if (failed) break;
            if (label == L'\0') continue;

            PITEMID_CHILD itemId{};
            POINT position{};
            if (FAILED(view->Item(item, &itemId)) || itemId == nullptr ||
                FAILED(view->GetItemPosition(itemId, &position)))
            {
                if (itemId != nullptr) CoTaskMemFree(itemId);
                continue;
            }
            CoTaskMemFree(itemId);
            if (viewWindow != nullptr) MapWindowPoints(viewWindow, overlay_, &position, 1);
            const bool iconView = mode != FVM_DETAILS && mode != FVM_CONTENT;
            const int left = iconView ? position.x + std::max(inset, iconSize - badgeWidth) :
                client.right - badgeWidth - MulDiv(9, dpi, 96);
            const int top = position.y + inset;
            RECT badge{left, top, left + badgeWidth, top + badgeHeight};
            if (badge.right < 0 || badge.left > client.right ||
                badge.bottom < 0 || badge.top > client.bottom)
                continue;
            COLORREF color = RGB(67, 112, 180);
            switch (label)
            {
            case L'A': color = RGB(53, 139, 75); break;
            case L'D': color = RGB(190, 52, 52); break;
            case L'M': color = RGB(197, 126, 28); break;
            case L'U': color = RGB(126, 74, 173); break;
            default: break;
            }
            try
            {
                pendingGitBadgeVisuals_.push_back(GitBadgeVisual{badge, color, label});
            }
            catch (...)
            {
                failed = true;
                break;
            }
        }
        if (shellView != nullptr) shellView->Release();
        view->Release();
        if (failed)
        {
            pendingGitBadgeVisuals_.clear();
            gitLayoutActive_ = false;
            return;
        }
        if (exhausted || gitLayoutExamined_ == maximumVisibleItems)
        {
            gitBadgeVisuals_.swap(pendingGitBadgeVisuals_);
            pendingGitBadgeVisuals_.clear();
            gitLayoutActive_ = false;
            InvalidateRect(overlay_, nullptr, FALSE);
            return;
        }
        if (!postGitDecorationLayout())
        {
            pendingGitBadgeVisuals_.clear();
            gitLayoutActive_ = false;
        }
    }

    void ExplorerBrowserHost::paintGitDecorations(HDC deviceContext) noexcept
    {
        if (deviceContext == nullptr || overlay_ == nullptr || gitBadgeVisuals_.empty()) return;
        const int dpi = GetDpiForWindow(overlay_);
        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST,
            sizeof(contrast), &contrast, 0) != FALSE && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        if (gitBadgeFontDpi_ != dpi)
        {
            if (gitBadgeFont_ != nullptr) DeleteObject(gitBadgeFont_);
            gitBadgeFont_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_BOLD,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            gitBadgeFontDpi_ = dpi;
        }
        HGDIOBJ oldFont = SelectObject(deviceContext,
            gitBadgeFont_ != nullptr ? gitBadgeFont_ : GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldBrush = SelectObject(deviceContext, GetStockObject(DC_BRUSH));
        HGDIOBJ oldPen = SelectObject(deviceContext, GetStockObject(DC_PEN));
        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext,
            highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(255, 255, 255));
        for (const auto& visual : gitBadgeVisuals_)
        {
            const COLORREF color = highContrast ? GetSysColor(COLOR_HIGHLIGHT) : visual.color;
            SetDCBrushColor(deviceContext, color);
            SetDCPenColor(deviceContext,
                highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : color);
            RoundRect(deviceContext, visual.bounds.left, visual.bounds.top,
                visual.bounds.right, visual.bounds.bottom, MulDiv(5, dpi, 96), MulDiv(5, dpi, 96));
            wchar_t label[]{visual.label, L'\0'};
            RECT bounds = visual.bounds;
            DrawTextW(deviceContext, label, 1, &bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(deviceContext, oldPen);
        SelectObject(deviceContext, oldBrush);
        SelectObject(deviceContext, oldFont);
    }
}
