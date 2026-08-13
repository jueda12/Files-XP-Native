#pragma once

#include "../core/localization_policy.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <string>

namespace filesxp::app
{
    enum class Text : std::size_t
    {
        title, back, forward, up, refresh, newFolder, view, address, go, searchHint, places,
        quickAccess, desktop, documents, downloads, pictures, music, videos, thisPC, libraries,
        network, wsl, recycleBin, fileMenu, editMenu, viewMenu, goMenu, toolsMenu, newTab,
        newWindow, duplicateTab, reopenClosedTab, closeTab, closeOtherTabs, moveTabLeft,
        moveTabRight, newTextFile, createShortcut, createLibrary, exitApp, undo, redo, cut, copy, paste, copyPath,
        copyPathQuoted, rename, bulkRename, folderFromSelection, bulkRenameInstructions,
        folderFromSelectionInstructions, invalidName, flattenFolder, flattenWarning,
        editAlternateStream, alternateStreamInstructions, deleteItem, permanentDelete, properties,
        editShortcut, editLibrary, shortcutName, shortcutTarget, shortcutArguments,
        shortcutWorkingDirectory, shortcutIcon, shortcutInvalid, libraryInstructions,
        selectAll, shelfCopy,
        shelfMove, shelfPaste, shelfClear, details, list, smallIcons, mediumIcons, largeIcons,
        extraLargeIcons, tiles, content, quickPreview, previewLoading, previewUnavailable,
        detailsPane, splitVertical, splitHorizontal,
        focusOtherPane, closePane, autoSizeColumns, addressCommand, searchCommand, settings,
        commandPalette, keyboardShortcuts, shortcutInstructions, shortcutConflict, runCommand,
        editTags, filterByTag, manageTagColor, tagColor, tagInstructions, invalidTags,
        colorNone, colorRed, colorOrange, colorYellow, colorGreen, colorBlue, colorPurple, colorGray,
        mapNetworkDrive, disconnectNetworkDrive, sha256, alternateStreams, verifySignature,
        compress, extract, archiveName, destinationFolder, archiveFormat, password, existingItems,
        renameExisting, overwriteExisting, skipExisting, archiveInvalid,
        gitInit, gitClone, gitCreateBranch, gitSwitchBranch,
        gitRepositoryInstructions, gitBranchInstructions, gitStatus, gitFetch, gitPull, gitPush, gitSync,
        gitRunning, gitCompleted, gitCanceled, gitFailed, close,
        language, defaultView, newTabLocation, restoreTabs, showPlaces, compactToolbar, toolbarButtons,
        confirmDelete, enableGit, enableArchives, spacePreview, previewProvider,
        previewAutomatic, previewWindows, previewQuickLook, previewSeer, previewPowerToys,
        reset, ok, cancel,
        systemLanguage, english, traditionalChinese, simplifiedChinese, object, objects,
        selected, searchResults, shelf, toMove, toCopy,
        tabsLabel, statusLabel, taskOutputLabel, invertSelection, pasteShortcut,
        closeTabsLeft, closeTabsRight, closeAllTabs, openTerminal, openTerminalAdmin,
        toggleSidebar, fullScreen, emptyRecycleBin, goHome, pinQuickAccess,
        unpinQuickAccess, pasteIntoFolder, showHiddenItems, showFileExtensions,
        clearSelection, restoreRecycleBin, openInNewTab, openInNewWindow,
        openInOtherPane, openCurrentFolderOtherPane, openFileLocation,
        restoreAllRecycleBin, manageShelf, removeShelfItem, moveUp, moveDown,
        unavailableItem, actionsMenu, playSelection, runAsAdministrator,
        runAsDifferentUser, runWithPowerShell, rotateLeft, rotateRight, installSelection,
        installCertificate, setDesktopWallpaper, setDesktopSlideshow,
        openStorageSense, ftpManager, ftpServerUrl, ftpUsername, ftpRequireTls,
        ftpConnect, ftpOpenFolder, ftpDownloadHere, ftpUploadFile, ftpDeleteFile,
        ftpDeleteFolder, ftpCancelTransfer, ftpReady, ftpLoading, ftpPlainWarning,
        ftpInvalid, count
    };

    class Localizer final
    {
    public:
        explicit Localizer(core::Locale locale = core::Locale::system) noexcept
        {
            setLocale(locale);
        }

        void setLocale(core::Locale locale) noexcept;

        [[nodiscard]] const wchar_t* operator()(Text text) const noexcept;
        [[nodiscard]] core::Locale locale() const noexcept { return locale_; }

    private:
        core::Locale locale_{core::Locale::english};
        std::array<std::wstring, static_cast<std::size_t>(Text::count)> overrides_{};
    };
}
