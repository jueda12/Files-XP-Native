# Upstream action audit

This audit covers the 172 action classes under `src/Files.App/Actions` at the
Files Community source revision pinned by `PARITY_CONTRACT.md`. It records the
user capability, not a class-for-class port: inherited base classes and the
Home/sidebar variants of one action map to one native surface.

| Upstream action family | Files XP Native surface | Disposition |
| --- | --- | --- |
| Navigation, tabs, panes, history | File/Go/View menus, shortcuts, command palette, native Shell travel log | Direct |
| Copy, cut, paste, paste shortcut, rename, delete, restore, undo/redo | Shell OLE commands plus isolated `IFileOperation` workers | Direct |
| Create file/folder/shortcut/library, bulk folder actions, flatten | Native dialogs and isolated Shell workers | Direct |
| Selection, views, columns, sort, group, refresh | `IFolderView2`, Shell header and view menus | Native Shell |
| Open, Open With, Properties, sharing, provider actions | Native Shell view default command and context menu | Native Shell |
| Run as administrator/different user; run PowerShell | Explicit Actions commands using canonical Shell verbs or trusted `powershell.exe -File` arguments | Direct |
| Play selected; rotate left/right | Explicit Actions commands delegated to the selected items' canonical Shell verbs | Direct/native Shell |
| Certificate, font and INF install | Explicit Install/Install Certificate commands plus the selected items' Shell context menu | Direct/native Shell |
| Desktop wallpaper and slideshow | `IDesktopWallpaper::SetWallpaper` and `SetSlideshow` | Direct |
| Lock-screen wallpaper | Windows lock-screen personalization surface | Equivalent OS surface |
| Format/eject/drive cleanup | This PC Shell context menu; explicit Storage Sense URI | Native Shell/direct |
| Quick Access and sidebar pinning | `pintohome`/`unpinfromhome`, Places pane | Direct |
| Start pinning | Applicable items' Windows 10 Shell context menu | Native Shell |
| Archives, Git, tags, preview, hashes, signatures, alternate streams | Isolated workers and native dialogs documented in `FEATURE_MATRIX.md` | Direct |
| Hidden/dot files, extensions, filter, toolbar/sidebar | Persistent search field, native visibility flags, layout settings and View commands | Equivalent |
| Light/dark/app-background themes and compact overlay | Fixed XP-like chrome with system high-contrast support and ordinary Win32 minimize/maximize/full-screen | Product-scope substitution |
| Help, release notes, log/settings-file, repository/IDE shortcuts | Not a file-management capability of this product; registered editors and terminals remain available | Excluded |

Canonical verbs are deliberately resolved by the Windows Shell for the actual
selection. They are not reimplemented or assumed to exist for every file type;
an unsupported verb fails without changing the selected item. This preserves
registered provider behavior and avoids a parallel extension/association model.
