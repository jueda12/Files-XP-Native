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

## 2026-08-13 elevated WPR follow-up: ETW evidence boundary

The approved elevated capture completed after the earlier report. This section is additive: no pre-existing log, ETL, JSON, or artifact was modified or removed.

### Capture inventory and integrity

- ETL: `artifacts/wpr-warm-launch-20260813-1525/FilesXPNative-warm-launch-9samples.etl`
  - exists; `804,257,792` bytes; SHA-256
    `5508DCE3A7BAC3941AB6616D5C3F3BCA908821411EB9F4F44535F3D3FA915096`.
- Capture transcript: `raw-logs/85-wpr-warm-launch-capture.txt`.
  It records the Release binary SHA-256
  `BA8FBEE750704A42A2499CCCDB1707E4F11D6456B04618A0BF55E079CC9731B9`,
  `WPR_START_SUCCEEDED=1`, `PROFILE_EXIT=0`, and
  `WPR_STATUS_AFTER_EXIT=0` (WPR is not recording).
- The earlier `raw-logs/87-wpr-capture-postcondition.txt` predates the successful
  capture and says `ETL_EXISTS=False`; it is retained as historical evidence and
  is not the postcondition for the completed 16:52 capture. The authoritative
  postcondition is the final `WPR is not recording` / exit code 0 in `85-*`.
- The nine profiler samples are preserved in
  `artifacts/wpr-warm-launch-20260813-1525/warm-launch-samples.json`; every
  `WaitForInputIdleReturned` value is `true`. The companion summary reports:

  | metric | min | median | max |
  |---|---:|---:|---:|
  | input idle | 402.4261 ms | 450.4007 ms | 552.4043 ms |
  | UIA root ready | 459.1808 ms | 496.2647 ms | 785.7951 ms |
  | first viewport after UIA | 207.7348 ms | 358.7639 ms | 405.3488 ms |

### ETW parsing result

`C:\Windows\System32\tracerpt.exe` (the only locally available official ETW
analysis tool; `wpa.exe` and `xperf.exe` were not installed or on `PATH`) parsed
the ETL successfully. Its unmodified command output is retained below the ignored
capture directory in `analysis-work/`:

```text
tracerpt.exe <ETL> -summary analysis-work/tracerpt-summary.txt \
  -o analysis-work/tracerpt-events.csv -of CSV
The command completed successfully.
```

The generated `tracerpt-summary.txt` reports 767 buffers, 5,557,401 events,
204,198 `PerfInfo/SampleProf` events, and 1,936,580 `StackWalk/Stack` events.
It also reports **48,827 lost events**, matching the WPR stop warning in `85-*`:
`This trace has dropped 48827 events. Please record this trace again.` The CSV is
an address-only event dump (5,557,402 lines) rather than a symbolized CPU usage
call tree: it contains instruction pointers and raw stack addresses but no resolved
function/module attribution or sampled-stack association suitable for a top-
contributor ranking. `tracerpt` therefore establishes that stack data was captured
and the ETL is parseable, but cannot establish a concrete app-owned CPU top stack.

The export does independently show the expected FilesXPNative launches and
associated Shell activity (for example, icon-cache accesses) and antimalware
provider activity around those launches. Those are observed ETW events, **not CPU
attribution**; neither is presented as a causal contributor or optimization target.

### Decision and regression status

No C++ change was made. The ETW capture is incomplete (48,827 lost events) and the
available official parser cannot symbolicate or aggregate it into a CPU call tree.
The only source-side synchronous candidate already identified remains
`AppWindow::ensureTabBrowser` → `ExplorerBrowserHost::create` →
`IExplorerBrowser::Initialize` / initial `BrowseToObject`. It is necessary to
preserve the native Shell first viewport; deferring it past UIA would violate the
existing viewport contract. The previous minimal duplicate chrome trial was also a
measured regression. Therefore there is no evidence-supported redundant app-owned
synchronous action and no safe native fix to commit.

Consequently no new Release build, profiler rerun, or official runtime rerun was
performed: doing so would manufacture a new baseline without an evidence-supported
change. The prior unmodified official runtime outcome remains a failure at the real
warm gate (`462.3248 ms`; `raw-logs/82-official-runtime-after-warm-launch-profile-explicit-paths.txt`),
so downstream runtime gates did not run and no pass is claimed.

### Reproducibility / tooling status

The capture script, profiler (`build/Profile-WarmLaunch.ps1`), binary hash, exact
sample count (9), and raw WPR transcript make this capture reproducible. A valid
attribution retry requires a machine with WPA/xperf available, symbols configured,
and a WPR profile/buffer configuration that completes without dropped events;
then filter the nine FilesXPNative PIDs and aggregate CPU Usage (Sampled) from
process start through input idle. `PresentMon.exe` was not found locally, no
PresentMon data was captured, and signing was not requested or verified in this
follow-up.

The only new derived artifacts are the ignored,
non-source `artifacts/wpr-warm-launch-20260813-1525/analysis-work/` files:
`tracerpt-console.txt`, `tracerpt-summary.txt`, and `tracerpt-events.csv`.
They are derived from the immutable ETL; their creation did not overwrite the ETL,
JSON samples, script, or `raw-logs`.

## Official rerun outcome

After restoring the baseline source and rebuilding the Release `/W4`/`/WX` generated project, the unmodified official script was invoked with explicit existing binary/probe/output paths (required only to avoid its pre-existing optional-probe default-path binding failure). It stopped at the real performance gate:

`Warm launch exceeded 300 ms: 462.3248 ms.`

No produced `runtime-results.json` exists because the official script writes results only after all assertions pass. Consequently the full runtime gate did not continue, and no pass result is claimed.

## Tooling gaps

`wpr.exe` is present at `C:\Windows\System32\wpr.exe`, but no trace was captured: the requested starting premise was that WPR was absent, and this bounded profiling work did not start an external tracing session. `PresentMon.exe` was not found in the inspected standard installation paths. The remaining evidence gap is an ETW/WPR or equivalent CPU-stack trace that attributes the approximately 284 ms input-idle segment inside ExplorerBrowser/Shell initialization versus loader/OS work. That trace should be captured externally without changing gates or rerunning selectively for a pass.
