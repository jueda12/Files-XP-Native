# Warm launch profiling report

## Scope and preserved evidence

Baseline commit: `581033a`.
Profiler commit: `41141bd` (`build/Profile-WarmLaunch.ps1`).
All command transcripts are additive under `raw-logs/70-*` through `raw-logs/82-*`; existing raw logs and artifacts were not removed or overwritten.

The bounded profiler launches the same Release binary with `--new-window` into one generated local fixture (one viewport probe plus 256 files), waits for input idle, then uses the same enabled-root UIA condition as the official gate. The second run also waits for the official gate's ListItem condition. Every sample closes its process. It does not alter the official script, gate, threshold, warmup behavior, or sample count.

## Measurement mechanics

`Test-WindowsRuntime.ps1` starts a stopwatch before `Start-Process`, calls `WaitForInputIdle(5000)`, then polls until the process main HWND has an enabled UIA root. Its warm metric is that total elapsed time. It does one unmeasured warmup process, then one measured process. The warm assertion runs before the viewport assertion, so a warm failure intentionally leaves no official viewport value.

## Controlled distribution

| Binary / experiment | samples | UIA-ready min | median | max | input-idle min | median | max | UIA after input-idle median |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 581033a baseline | 9 | 295.951 ms | 335.477 ms | 432.991 ms | 252.958 ms | 284.106 ms | 328.197 ms | 48.573 ms |
| rejected startup de-dup experiment | 9 | 328.330 ms | 354.437 ms | 556.107 ms | 277.134 ms | 298.579 ms | 456.206 ms | 49.452 ms |

The independent five-sample viewport baseline measured first Shell ListItem *after* UIA-ready at 185.050–228.960 ms (median 210.131 ms). Therefore deferring the synchronous ExplorerBrowser creation/browse until after UIA-ready would hide latency from the warm metric and make the official 100 ms viewport gate fail; that option was rejected.

## App-side cost evidence and decision

The dominant measured segment is process start through input idle (baseline median 284.106 ms), not UIA readiness after input idle (median 48.573 ms). Source inspection identifies the initial critical path as `WM_CREATE` control/menu construction followed synchronously by `AppWindow::addTab` → `ensureTabBrowser` → `ExplorerBrowserHost::create` → `CoCreateInstance(CLSID_ExplorerBrowser)` / `IExplorerBrowser::Initialize` → `SHCreateItemFromParsingName` / `BrowseToObject`.

A least-invasive trial removed only duplicate initial `createMainMenu()` and `populatePlaces()` calls. It compiled under the existing strict Release project flags but regressed the controlled median from 335.477 to 354.437 ms. It was reverted; no native product-code change is included. This rules out that duplicate UI chrome work as a positive optimization under this sample protocol.

No evidence shows startup git workers, thumbnail workers, database/session work, or external process start on this launch path. Session restore is bypassed by `--new-window <fixture>`. Git refresh is scheduled after tab activation, while the first browser initialization and browse are synchronous.

## Official rerun outcome

After restoring the baseline source and rebuilding the Release `/W4`/`/WX` generated project, the unmodified official script was invoked with explicit existing binary/probe/output paths (required only to avoid its pre-existing optional-probe default-path binding failure). It stopped at the real performance gate:

`Warm launch exceeded 300 ms: 462.3248 ms.`

No produced `runtime-results.json` exists because the official script writes results only after all assertions pass. Consequently the full runtime gate did not continue, and no pass result is claimed.

## Tooling gaps

`wpr.exe` is present at `C:\Windows\System32\wpr.exe`, but no trace was captured: the requested starting premise was that WPR was absent, and this bounded profiling work did not start an external tracing session. `PresentMon.exe` was not found in the inspected standard installation paths. The remaining evidence gap is an ETW/WPR or equivalent CPU-stack trace that attributes the approximately 284 ms input-idle segment inside ExplorerBrowser/Shell initialization versus loader/OS work. That trace should be captured externally without changing gates or rerunning selectively for a pass.
