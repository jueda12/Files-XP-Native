# Files 4.2 parity contract

This document defines what “Files feature equivalent” means for Files XP Native. It is based on a source audit of Files Community 4.2.5 (`68c68a58d4d6a5f7197e07c8fff85cfd4279c78b`, 2026-08-09), not on screenshots or assumptions. A capability is complete only when its acceptance check passes on Windows 10 22H2 x64.

The implementation is intentionally not a line-by-line port. Windows Shell services are the compatibility layer for native namespace, file-operation, thumbnail, drag/drop, context-menu, association, cloud-placeholder, accessibility, and change-notification semantics. Files-specific capabilities are implemented above that layer.

## P0: everyday file-manager parity

| Capability | Native implementation | Acceptance check |
| --- | --- | --- |
| Local and UNC navigation | `IExplorerBrowser` and Shell PIDLs | Browse local, removable, and `\\server\share` paths without blocking the frame |
| Shell locations | Known-folder PIDLs | Browse Home, Desktop, Downloads, Documents, Pictures, Music, Videos, This PC, Network, Libraries, and Recycle Bin |
| History and address entry | Explorer travel log plus editable parsing names | Back, Forward, Up, Refresh, typed paths, `shell:` names, and environment-expanded paths work per tab |
| Tabs and session restore | One Shell browser host per tab; compact persisted session | New, close, duplicate, reorder, reopen, restore, next/previous, and close-others survive restart |
| Dual pane | One tab strip with two independent browser hosts in a split tab | Horizontal/vertical panes navigate and select independently |
| Views | `IFolderView2` | Details, list, small/medium/large/extra-large icons, tiles, and content views switch without rebuilding item models |
| Sort, group, and columns | Shell folder view/property system | Header sorting, grouping, auto-size, and per-folder column state persist |
| Thumbnails and overlays | Shell view thumbnail/icon pipeline | Image/video/PDF/provider thumbnails, shortcut arrows, sync state, and registered overlays appear asynchronously |
| Selection | Shell folder view | Multi-select, select all/invert, keyboard range selection, and item counts work in large folders |
| Open and Open With | Shell verbs and associations | Files, folders, shortcuts, apps, and registered protocols launch with system semantics |
| Clipboard | Shell view commands and `IDataObject` | Copy, cut, paste, paste shortcut, and cross-tab/pane operations preserve Shell formats |
| Create and rename | `IFileOperation::NewItem` and `IFolderView2::DoRename` | New folder/file, inline rename, extension warnings, collision UI, and invalid-name handling work |
| Copy, move, delete, recycle | Shell view and `IFileOperation` | Progress, cancellation, elevation, collision policy, Recycle Bin, permanent delete, and undo/redo follow Windows semantics |
| Drag and drop | Shell view OLE drop targets | Internal, cross-pane, Explorer, desktop, mail, browser, and cloud-provider drag/drop work |
| Context menus and properties | Shell default view | Registered classic verbs, Properties, sharing/provider verbs, and background New menu appear |
| Live refresh | Shell change notifications | Create/rename/delete updates the view without full manual reload |
| Search | Windows Structured Query and Search Folder APIs with a bounded isolated filename fallback | Indexed AQS search works; when Windows Search rejects a local/UNC scope, recursive filename fallback is cancellable and never traverses reparse-point directories |

## P1: Files differentiators

| Capability | Native implementation | Acceptance check |
| --- | --- | --- |
| Preview and details panes | registered Shell preview handlers and property system | Common text, code, Markdown, image, media, PDF, rich text, HTML, and provider previews load on demand and cancel on selection change |
| Quick preview | registered QuickLook/Seer/PowerToys Peek integration | Space opens the configured provider without blocking navigation |
| Archives | Shell ZIP plus 7-Zip-compatible worker adapter | Create/extract ZIP and supported 7-Zip formats with password, progress, cancellation, collision, and skipped-item reporting |
| Git | background `git.exe` adapter, repository-scoped cache | Status decorations plus init, clone, fetch, pull, push, sync, and branch actions never run on the UI thread |
| Network and remote | Shell network namespace, registered protocol handlers, and an isolated TLS-by-default FTP/FTPS adapter | Discover shares, map/unmap, credentials, UNC, and FTP navigation and transfers work |
| Cloud providers | Shell namespace and Cloud Files API surfaces | OneDrive and registered providers expose hydration, pin/free-space, quota/status, and provider context verbs |
| WSL | `\\wsl$`/`\\wsl.localhost` namespace | Installed distributions appear and browse through native Shell paths |
| Tags | NTFS property/sidecar database with move reconciliation | Add/edit/remove tags, tag widget, filtering, and missing-item repair work |
| Libraries and shortcuts | Shell library and link APIs | Create/edit libraries and `.lnk` targets, arguments, working directories, and icons |
| Shelf | process-local `IDataObject` shelf | Stage copy/cut items across folders and panes, then paste or remove safely |
| Bulk utilities | background operation jobs | Bulk rename, flatten folder, create folder from selection, hashes, signatures, and alternate streams handle partial failures |

## P2: product completeness

| Capability | Acceptance check |
| --- | --- |
| XP-like appearance | Classic two-row toolbar, places pane, compact metrics, original XP-inspired assets, high DPI, light/high-contrast support |
| Settings | Appearance, layout, folders, toolbar, actions, tags, advanced, and privacy settings persist atomically and can reset |
| Localization | en-US, zh-Hant, and zh-Hans ship complete; resource loading supports further locale packs without code changes |
| Keyboard and command palette | Files-compatible core shortcuts, discoverable command search, conflict validation, and remapping |
| Accessibility | Keyboard-only operation, visible focus, screen-reader names/states, 200% text, high contrast, and Windows UIA smoke tests |
| Reliability | Single-instance routing, multi-window launch, crash-safe session/settings writes, unavailable-location recovery, and no credential logging |
| Packaging | Portable ZIP and MSIX x64 packages build reproducibly; hashes and SBOM are emitted; signing is optional and never faked |
| Performance | Warm launch ≤ 300 ms to interactive on reference hardware; first viewport ≤ 100 ms for local folders; input p95 ≤ 50 ms; no UI-thread filesystem, archive, Git, search, or database work |

## Source-audit map

The contract was checked against the current upstream feature surfaces, including:

- `src/Files.App/Actions/*` for navigation, file operations, Git, display, panes, shelf, themes, tabs, undo, and redo;
- `src/Files.App/Views/Layouts/*` for column, columns, details, and grid layouts;
- `src/Files.App/UserControls/FilePreviews/*` for basic, code, folder, HTML, image, Markdown, media, PDF, rich-text, Shell, and text previews;
- `src/Files.App/Services/Storage/*` for archives, devices, network, security, and trash;
- `src/Files.App/Utils/{Cloud,FileTags,Git,Library,Shell}/*` for provider, tag, repository, library, context-menu, preview-handler, and file-operation integration;
- `src/Files.App/Views/Settings/*` and `src/Files.App/Views/Properties/*` for settings and property surfaces.

“Parity” does not include upstream telemetry, branding, Store commerce, update marketing, or implementation-internal architecture. Those are not file-manager capabilities.
