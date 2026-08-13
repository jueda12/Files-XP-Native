# Validation status

## Reconstructed native Shell checkpoint

This tree reconstructs the six lost responsiveness slices after the verified source-archive base
`560ddacf7ba22b63fe71008da8991e429cb7933d`. It moves internal worker process creation to the
Windows thread pool, bounds external-preview churn to one in-flight and one latest pending
request, adds deterministic preview race tests, reduces the Git decoration quantum from four to
two, matches the archive result consumer to the worker's 64-KiB ceiling, and extends the Windows
runtime gate with address-navigation latency probes.

## Completed in the authoring environment

- portable C++20 core tests compiled with GCC 13.3 and passed;
- concurrent generation gate and bounded-queue invariants passed;
- deterministic natural ordering passed;
- latest 100,000-name natural-sort benchmark completed in approximately 184 ms, FTP validation plus natural sort in approximately 223 ms, cooperative FTP materialization stayed below 0.03 ms per dispatch, and Git decoration work stayed below 0.28 ms per dispatch on the authoring runner;
- archive adapter encrypted 7z/ZIP and TAR round trips passed against the pinned, hash-verified 7-Zip package;
- UndefinedBehaviorSanitizer passed the complete portable core suite;
- GCC `-fanalyzer` passed the isolated new preview queue. Whole-suite analyzer runs were not accepted as passing because GCC 13 emitted diagnostics inside `libstdc++` deque allocation/mutex internals;
- automatic Git refresh and every internal archive, flatten, mapped file-operation, fallback-search, tag, text-preview, FTP, and external-preview process creation path now queue through the Windows thread pool;
- application manifest passed XML validation;
- CMake preset JSON passed parsing;
- shell scripts passed Bash syntax validation;
- Windows-facing sources and PowerShell scripts passed CRLF checks;
- source passed targeted private-key and common-token pattern scans.
- the Windows runtime script now includes the FTP refusal/credential-zeroing probe and twenty alternating address navigations across two 257-entry folders with concurrent input probes; it remains pending until run on Windows 10.

## Still not available in the authoring environment

The current authoring environment is Linux and does not contain MSVC, MinGW-w64, CMake,
PowerShell, a Windows desktop session, or Windows performance tooling. LeakSanitizer also cannot
start because the container denies `/proc/*/task` inspection. This checkpoint does not claim:

- a successful Windows application compile or link for the reconstructed tree;
- successful launch on Windows 10;
- measured frame-time, input-latency, working-set, or ETW results;
- installer or signing validation.

Run `build\Build-Windows.ps1` on Windows 10 with the documented Visual Studio C++ workload. Treat any compiler diagnostic as a blocking issue, make the smallest correction, and retain the build log and binary log.
