# Security policy

Files XP Native performs privileged-looking operations on user files and therefore treats Shell semantics, path handling, cancellation, and release provenance as security boundaries.

## Current checkpoint

Version 0.1.0 includes native Shell file operations (including Recycle Bin and permanent delete),
Git child workers, a pinned 7-Zip adapter, and an FTP/FTPS adapter driven by the Windows inbox
`curl.exe`. It has no updater, telemetry client, background service, bundled elevation helper, or
persistent credential store.

FTP credentials are carried in a bounded pagefile-backed mapping, consumed and zeroed by the
worker, and delivered to curl through anonymous stdin rather than argv or a disk configuration
file. Downloads use an unpredictable temporary file and a write-through, no-overwrite commit.
TLS verification is enabled by default; disabling TLS is an explicit per-connection choice.

Archive, Git, tag, preview, fallback-search, and Shell-operation workers accept bounded,
versioned requests. Result files and UI reads have explicit size ceilings. User-provided paths are
passed as individually quoted arguments without a command shell. External QuickLook, Seer, and
PowerToys preview integrations remain separate installed programs and inherit their own security
models.

The application is not sandboxed: a user-confirmed file operation has the same file-system rights
as the interactive user. An unsigned development package proves neither publisher identity nor
release provenance.

## Reporting

Report path traversal, arbitrary execution, unsafe file operations, privilege-boundary, installer, or dependency issues privately to the project maintainer before public disclosure.

## Release requirements

- build from a clean commit;
- run portable, Windows compile/link, UI Automation, worker, and runtime performance gates;
- publish compiler and SDK versions;
- retain build logs and hashes;
- scan source and artifacts for secrets and unexpected executables;
- sign binaries only in a protected release environment;
- never distribute self-extracting or obfuscated installers.
