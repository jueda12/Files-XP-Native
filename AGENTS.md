# Working agreement

- The supported target is Windows 10 version 1809 or later, x64 first.
- Use C++20, Win32, COM, and Windows Shell APIs. Keep the hot file viewport free of XAML and per-item heap-owned view models.
- Never perform file-system, thumbnail, Git, archive, or database work on the UI thread.
- Every asynchronous result carries a navigation generation. Stale generations never update UI state.
- Bound queues and caches. Prefer viewport-driven work and backpressure over speculative loading.
- Preserve CRLF in Windows-facing source and PowerShell scripts.
- Keep every commit buildable. Add functionality as complete vertical slices.
- Do not add third-party dependencies without documenting the version, licence, and measured reason.
- Do not commit credentials, signing certificates, generated binaries, `out`, or `artifacts`.

