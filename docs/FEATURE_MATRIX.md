# Feature matrix

A checked item is implemented in the current source and passes the available compile/test gate. It is not a substitute for the Windows 10 runtime gates in `PARITY_CONTRACT.md`.

## Shell and navigation

- [x] Native Win32 process, window, XP-like toolbar, places pane, tabs, and status bar
- [x] Local, UNC, Shell namespace, This PC, Libraries, Network, WSL, Recycle Bin, and provider locations
- [x] Address, Back, Forward, Up, Refresh, keyboard, environment-expanded paths, and `shell:` names
- [x] Multiple tabs, duplicate, close, reopen, keyboard reorder, next/previous, and session restore
- [x] Independent horizontal and vertical dual panes
- [x] Mouse tab drag/reorder and explicit multi-window launch with folder routing
- [x] Open selected folders in a new tab/window/other pane and reveal a selected result in its parent
- [x] Bounded single-instance launch routing with a non-blocking hung-window timeout

## File viewport

- [x] Native virtualized Shell view with live refresh
- [x] Async system thumbnails, icons, overlays, and registered provider decorations
- [x] Details, list, small/medium/large/extra-large icons, tiles, and content views
- [x] Shell columns, grouping, sorting, header actions, and property-bag persistence
- [x] Multi-selection, cooperative large-folder inversion, item counts, native preview pane, and native details pane
- [x] Native hidden-item and file-extension Shell switches plus select-all/clear/invert actions
- [x] Space-key Quick Preview through registered native preview handlers
- [x] Debounced out-of-process text/code/Markdown viewport preview with a 256-KiB UI-layout cap and explicit truncation notice

## Operations

- [x] Open/Open With and registered Shell verbs
- [x] New folder, new text file, inline rename, and Properties
- [x] Shell/OLE cut/paste and drag/drop plus cooperative provider-safe snapshots and isolated cancellable create/copy/move/Recycle Bin/permanent-delete jobs, undo, and redo
- [x] Native collision, elevation, progress, cancellation, and change notifications
- [x] Copy Path and Copy Path with Quotes
- [x] Shell-identity Shelf for staged copy/move across tabs and panes
- [x] Native Recycle Bin empty and selected-item restore actions
- [x] SHA-256, Authenticode inspection, and alternate-stream listing through isolated system tools
- [x] Cancellable out-of-process collision-safe bulk rename and create-folder-from-selection with native undo
- [x] Bounded cancellable out-of-process reparse-safe flatten with native undo and isolated alternate-stream editing

## Extended functionality

- [x] Scoped Windows `search-ms`/AQS search views
- [x] Isolated cancellable filename fallback when Windows Search rejects a local/UNC scope
- [x] Shell ZIP navigation and registered archive context handlers
- [x] Network shares, WSL, Shell-registered FTP/protocol handlers, and registered cloud placeholders/context verbs
- [x] Dedicated credential-isolated FTP/FTPS adapter with TLS-by-default browsing, upload/download, folder actions, cancellation, and no credentials in argv or disk configuration
- [x] Native credential-aware map/disconnect network-drive dialogs
- [x] Out-of-process Git init/status/fetch/pull/push/sync commands with a 256-KiB in-window layout cap and cancellation
- [x] Pinned and hash-verified 7-Zip 26.02 runtime with complete upstream licence notice
- [x] Bounded 7z/ZIP/TAR create and extract worker with password, progress, cancellation, collision, and skipped-item output
- [x] Safely resolved Git executable with clone, create/switch branch, and console progress
- [x] Automatic repository-scoped Git status cache with cooperative parsing/ancestor aggregation and mouse-transparent cached visible-item badges
- [x] Bounded editable `System.Keywords` tags with out-of-process writes and move-stable sidecar records
- [x] Cancellable registry scan plus memory-mapped, 8-ms-budgeted sidecar-backed native Shell tag result view up to 100k items
- [x] Volume/file-ID move repair without recursive disk scans
- [x] Persisted bounded tag-color management and high-contrast-safe result chip
- [x] Isolated native `.lnk` and Windows Library creation plus Shell properties editing
- [x] Non-blocking Automatic/Windows/QuickLook/Seer/PowerToys preview provider selection with one in-flight handshake and latest-selection coalescing
- [x] Bounded settings for locale, layout, start/session behavior, deletion, Git, archives, and preview with reset
- [x] Localized en-US, zh-Hant, and zh-Hans chrome/menu/settings/status surfaces
- [x] Localized bounded command palette with fuzzy matching
- [x] Persisted toolbar-button customization and conflict-checked shortcut remapping
- [x] Bounded UTF-8 external locale-pack overrides with all-or-nothing validation and built-in fallback
- [x] Portable/source ZIP generation with SHA-256 and SPDX 2.3 SBOM
- [x] Original multi-resolution app icon and unsigned MSIX build path; optional certificate signing
- [x] High-contrast system colors, explicit focus cues, and accessible names for custom chrome
- [x] Bounded native Shelf item manager with incremental display-name loading, per-item removal, and reordering
- [x] Bounded cancellable Restore-all Recycle Bin worker using the native Shell restore verb
- [x] Audited upstream action coverage through native commands, Shell verbs, equivalent surfaces, and explicit product-scope exclusions
- [ ] Run the automated UIA/WPR/PresentMon/signature gate on reference Windows 10 hardware
