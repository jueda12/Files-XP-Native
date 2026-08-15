# Windows 10 runtime validation
Run this gate on the Windows 10 reference machine after a Release build:
```powershell
.\build\Test-WindowsRuntime.ps1 `
  -Binary .\out\build\vs2022-x64\src\app\Release\FilesXPNative.exe
```
It performs a warm-launch and first-viewport measurement, isolated nested-folder flatten,
collision-safe bulk-rename, folder-from-selection, and native create/copy/move/permanent-delete
round trips, native shortcut creation, a 512-KiB-to-256-KiB text-preview bound check, twenty
alternating address navigations across two 257-entry folders with concurrent responsiveness
probes, plus 100 additional bounded responsiveness
probes. The FTP refusal probe also verifies that its pagefile-backed credential request is zeroed
and that worker output does not contain the test username or password.
The gate preserves UI Automation traversal, names, and `ValuePattern` navigation checks. It also
builds and invokes `FilesXPNativeAccessibilityProbe.exe`, a C++20 `/W4 /WX` OleAcc helper that
calls documented `AccessibleObjectFromWindow` after apartment-threaded COM initialization. If an
HWND-backed address edit, Places list, or Tabs control reports `ControlType.Pane` through UIA,
the gate requires its exact MSAA role instead (42, 33, or 60 respectively). Pane is never accepted
alone; a non-Pane UIA type must remain its expected type, and a mismatched or unavailable native
role fails the gate. `runtime-results.json` records the UIA type and, where Pane fallback was
required, HWND, MSAA role, and bounded name under `HwndBackedControls`.
It fails above the contract limits of 300 ms warm launch, 100 ms first viewport, 100 ms
address-navigation p95, or 50 ms input p95. Results are written to
`artifacts\runtime-validation\runtime-results.json`.

## Release disposition: 2026-08-15

The current release is complete with a documented warm-launch exception. Release and profile
builds succeed, UI Automation and runtime behavior remain intact, and five strict WPR captures
completed without dropped events. Repeated measurements placed warm launch near the 300 ms
contract (best repeatable 15-sample UIA-ready median: 325.9863 ms; runtime gate observation:
321.9658 ms). Profiling and controlled A/B changes isolated the remaining cost to the first
`IExplorerBrowser::BrowseToObject` navigation, which Microsoft documents as synchronous.

The 300 ms assertion remains enabled so a future Shell, toolchain, or implementation improvement
is visible rather than silently redefining the contract. For this release only, its failure is an
accepted platform-performance exception; accessibility, first-viewport, navigation, input,
functional, packaging, and build failures are not waived.
Optional release evidence:
```powershell
.\build\Test-WindowsRuntime.ps1 `
  -Binary .\artifacts\signed\FilesXPNative.exe `
  -RequireSignature `
  -CaptureWpr `
  -PresentMonPath C:\Tools\PresentMon.exe
```
This also writes a WPR ETL and a five-second PresentMon CSV. WPR file-mode traces can contain
paths and other sensitive data; review them before sharing. The script cancels only the WPR or
PresentMon process that it started when a gate fails.
References:
- [Microsoft WPR command-line options](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)
- [Intel PresentMon console options](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md)
