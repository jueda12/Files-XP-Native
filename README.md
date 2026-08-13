# Files XP Native

Files XP Native is a clean-room C++20/Win32 reconstruction of the useful parts of Files with an original Windows XP-inspired desktop shell. It is designed for Windows 10 and uses the operating system's native Shell view instead of a managed per-item UI graph.

The current native-shell checkpoint provides:

- `CLSID_ExplorerBrowser` viewports with Windows-native namespace enumeration, virtualization, thumbnails, overlays, drag/drop, context menus, file associations, change notifications, and accessibility;
- local, removable, UNC, Quick Access, known-folder, This PC, Libraries, Network, WSL, Recycle Bin, ZIP namespace, and registered cloud-provider navigation;
- per-tab native travel logs, new/duplicate/close/reopen/reorder, crash-safe session restore, and independent horizontal or vertical dual panes;
- editable Shell addresses, Back, Forward, Up, Refresh, keyboard routing, AQS `search-ms` results, and an isolated cancellable filename fallback when Windows Search rejects a local/UNC scope;
- details, list, and 16/48/96/256-pixel icon views, Shell column/sort/group state, native preview
  and details panes, plus a debounced asynchronous 256-KiB viewport text/code/Markdown fallback;
- Shell/OLE copy, cut, paste, inline rename, cooperative provider-safe selection snapshots, and out-of-process cancellable create/copy/move/delete, bulk rename, and folder-from-selection operations with native undo; Properties, redo, and select-all/invert;
  new folder/file, create-folder-from-selection, Copy Path, and Copy Path with Quotes;
- XP-like two-row toolbar, places pane, compact tab strip, status bar, classic menus, per-monitor DPI, and high-contrast-compatible native controls;
- mouse tab reordering, bounded single-instance folder routing, explicit multi-window launch,
  cancellable out-of-process Git sync/status with a 256-KiB in-window layout cap, clone/branch commands,
  automatic per-item Git status badges with cooperative status aggregation and cached layout, and a cancellable 7z/ZIP/TAR worker with encrypted
  archives, collision policy, and progress;
- persisted layout/session/integration settings with reset, configurable toolbar buttons and shortcuts,
  selectable Windows/QuickLook/Seer/PowerToys preview providers, en-US/zh-Hant/zh-Hans UI,
  bounded external locale-pack overrides, and a bounded command palette;
- cancellable out-of-process tags with Shell keywords, move-stable sidecars, file-ID repair, tag colors, and memory-mapped/time-budgeted native result views up to 100k items,
  native network-drive dialogs, SHA-256, signature, and alternate-stream tools;
- bounded cancellable out-of-process folder flattening with reparse isolation, Shell collision handling, and undo;
- a TLS-by-default FTP/FTPS manager using the Windows inbox `curl.exe`, with credentials delivered only through an anonymous pipe, cancellable isolated transfers, traversal-safe listings, and collision-safe atomic downloads;
- isolated native Windows shortcut and Library creation through Shell COM APIs, with native properties editing;
- portable state/queue/order tests, a 100,000-item model benchmark, and a warning-as-error MinGW-w64 x64 PE cross-build gate;
- reproducible preview/source ZIP scripts with SHA-256 output and an SPDX 2.3 SBOM, including a
  pinned and hash-verified 7-Zip 26.02 Windows runtime and its complete upstream licence notice;
- an original multi-resolution app icon and Windows SDK MSIX packaging with optional explicit signing.

The exact acceptance contract is in `docs/PARITY_CONTRACT.md`, and the implementation checklist is in `docs/FEATURE_MATRIX.md`. The native P0 implementation, bounded fallback services, audited Files action surface, and dedicated FTP/FTPS adapter are implemented. The Windows 10 UIA/performance/signature gates still remain before a parity release can be claimed.

## Build on Windows 10

Requirements:

- Windows 10 version 1809 or later (the FTP/FTPS manager requires the inbox `curl.exe`);
- Visual Studio 2022 Build Tools or Visual Studio 2022;
- Desktop development with C++ workload;
- Windows 10 or 11 SDK;
- CMake 3.28 or later.

From a regular PowerShell prompt:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\build\Build-Windows.ps1 -Configuration Release -Architecture x64
```

The executable is written under `out\build\vs2022-x64\src\app\Release`. The script creates portable ZIP and unsigned MSIX packages, includes an SPDX 2.3 SBOM, writes a build log, and prints SHA-256 values. `build\Build-MSIX.ps1` signs only when you explicitly provide a matching certificate.

## Verification

On Linux, portable logic and the Windows PE cross-build can be checked independently:

```bash
./build/Test-PortableCore.sh
./build/Test-ModelBenchmark.sh
./build/Test-ArchiveAdapter.sh
MINGW_ROOT=/path/to/mingw64 ./build/Test-CrossCompile.sh
```

A successful cross-build proves Windows-header compilation and x64 PE linkage, not runtime behavior. Windows 10 Shell, UI Automation, WPR/WPA, and PresentMon checks remain mandatory release gates.
Run `build\Test-WindowsRuntime.ps1` on the reference Windows 10 machine; see
`docs\WINDOWS_RUNTIME_VALIDATION.md` for trace and signature options.

## Licence

The clean-room source is provided under the MIT licence. Binary packages also contain 7-Zip under
its upstream LGPL/BSD/unRAR terms; see `THIRD_PARTY_NOTICES.md` and the packaged
`7-Zip-License.txt`. The FTP/FTPS manager uses the operating system's inbox curl and does not
redistribute it. Windows and Windows XP are Microsoft product names. The visual treatment is
original and does not include Microsoft artwork.
