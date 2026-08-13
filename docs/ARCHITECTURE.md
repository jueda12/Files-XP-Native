# Native architecture

## Why C++ and Win32

The target is Windows 10 and the primary requirement is predictable interaction latency. C++20 and Win32 provide direct access to the Windows Shell, common controls, overlapped I/O, ETW, Direct2D, DirectWrite, and Composition without a managed-object graph for every file.

The production viewport hosts the operating system's `CLSID_ExplorerBrowser`. This is the smallest reliable way to obtain the same namespace, virtualized folder view, thumbnails, drag/drop, context menus, file associations, provider overlays, accessibility, and change notifications as Windows Explorer. The phase-one `LVS_OWNERDATA` viewport remains a benchmarkable fallback and a proof that the surrounding navigation pipeline is not tied to a managed UI framework.

This does not mean that C++ is automatically fast. The following contracts, rather than the language alone, are the performance design.

## Contracts

1. The UI thread owns only windows, active pane/tab state, cheap command routing, and Shell COM objects that require the apartment.
2. `IExplorerBrowser` owns enumeration, virtualization, thumbnails, overlays, selection, drag/drop, and Shell namespace navigation.
3. The host implements `ICommDlgBrowser2::GetViewFlags(CDB2GVF_NOINCLUDEITEM)` so Search Folder filtering is not forced through a per-item UI-thread callback.
4. Each tab owns one browser host; inactive tabs hide their child host instead of destroying and reconstructing item models.
5. Copy, move, rename, delete, recycle, collision, elevation, and undo use Shell commands or `IFileOperation`.
6. Git, non-Shell archives, tags, bulk file utilities, fallback filename search, dedicated FTP/FTPS, text preview, and external-preview handshakes use isolated workers with bounded result delivery. Internal worker process creation runs on the Windows thread pool, outside navigation and selection dispatches; external preview retains one in-flight request and only the latest pending selection. Hash/signature tools run as trusted system child processes. The FTP worker drives the Windows inbox `curl.exe` through an anonymous stdin configuration pipe, so credentials never enter argv or a disk configuration file.
7. No background result may update a tab after its operation generation or lifetime token is stale.
8. Settings and session state use compact, crash-safe writes; no per-item state is serialized.
9. Shutdown cancels workers first, disconnects Shell event sinks, then destroys browser windows.
10. The phase-one owner-data enumerator remains available only as a diagnostic fallback until the native Shell host passes the parity gates.

## Data flow

```text
XP shell / tabs / panes -> IExplorerBrowser -> native IShellView
                                               |       |       |
                                         namespace  thumbnails  OLE/Shell verbs

Files-only services -> cancellable worker -> bounded messages -> active tab/pane
```

## Migration roadmap

| Stage | Deliverable | Gate |
| --- | --- | --- |
| 1 | Local-folder browsing vertical slice | Build, core tests, 100k virtualization check |
| 2 | Native Explorer Browser viewport and Shell command routing | Namespace, thumbnail, drag/drop, context-menu, and search smoke tests |
| 3 | Tabs, dual pane, session/settings, and XP shell | Navigation and crash-recovery integration tests |
| 4 | Archives, Git, tags, preview, shelf, fallback filename search, and dedicated FTP/FTPS | Each worker stays within the regression budget |
| 5 | Localization, accessibility, installer, and performance closure | UIA audit, crash tests, WPR/PresentMon gates, release packages |
