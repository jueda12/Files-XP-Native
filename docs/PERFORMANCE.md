# Performance verification

Source-level architecture can remove obvious stalls, but "silky" is a measurement target rather than a visual claim.

## Test folders

Generate controlled datasets in an expendable directory:

```powershell
.\build\Generate-TestTree.ps1 -Root C:\PerfData\FilesXP -Count 100000
```

The script refuses to use a drive root, profile root, or an existing nonempty directory unless `-Force` is supplied.

## Required traces

For each 1k, 10k, and 100k folder, record cold and warm runs:

- launch to first frame;
- address navigation to first viewport;
- five seconds of continuous wheel scrolling;
- rapid Back/Forward navigation twenty times;
- resize while scrolling;
- idle for sixty seconds after enumeration.

Capture UI Delays, CPU Usage, Disk I/O, File I/O, DWM Core, and Present events using Windows Performance Recorder. PresentMon may be used for frame-time percentiles.

## Phase-one gates

- no filesystem calls on the UI thread during enumeration;
- first 32 entries are deliverable before full enumeration finishes;
- realized ListView rows stay proportional to the viewport, not directory count;
- stale navigation generations commit zero rows;
- p95 input-to-present below 50 ms;
- p95 scroll frame time below 16.7 ms and p99 below 33.3 ms on the declared test machine;
- memory reaches a stable plateau after final snapshot and idle cleanup.
- FTP/FTPS 100k-entry validation and natural sorting stay out of the UI process; result reads yield after two 64-KiB chunks or 6 ms, while dialog revalidation/materialization/insertion yields after 64 lines or 8 ms.
- Automatic Git status coalesces activation/tab/navigation bursts for 200 ms and creates its isolated worker on the Windows thread pool; a busy foreground task retains one bounded refresh retry per second instead of dropping the newest folder.
- Archive, flatten, mapped file operations, fallback search, tags, text preview, FTP, Git, and external-preview handshakes create their worker processes on the Windows thread pool. External-preview selection churn retains only the latest pending path behind one in-flight request.

Record hardware, Windows build, storage medium, power mode, dataset, raw traces, and exact commit with every result.
