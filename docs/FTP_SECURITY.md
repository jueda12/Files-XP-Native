# FTP/FTPS security contract

The dedicated adapter is independent of registered Shell protocol handlers. Its trust boundary is the Files XP Native process, its short-lived worker process, the Windows inbox `System32\curl.exe`, and the selected remote server.

## Credential handling

- The UI encodes one bounded request into a pagefile-backed named mapping with an unpredictable name.
- The worker opens, validates, decodes, and immediately zeroes that mapping.
- The worker launches the exact `System32\curl.exe` path with only `-q --config -` in its command line. `-q` is first, so a per-user curl configuration is not loaded.
- Username and password are emitted only into a quoted curl configuration sent through an anonymous stdin pipe. They are not placed in argv, an environment variable, a URL, a log, or a disk configuration file.
- Request, UTF-8 credential, quoted-value, and full configuration buffers are explicitly zeroed before release.

This does not claim protection from malicious code already running as the same Windows user; such a process can normally inspect this process's memory. The contract prevents ordinary process-list, command-history, log, and temporary-file disclosure.

## Transport and input policy

- Only `ftp://` and `ftps://` folder URLs are accepted. Embedded URL credentials, query strings, fragments, backslashes, controls, raw spaces, and malformed percent escapes are rejected.
- `ftp://` uses explicit TLS via curl's `ssl-reqd` by default. `ftps://` uses implicit TLS. Certificate validation stays enabled; the adapter never supplies an insecure-certificate override.
- Plain FTP requires the user to clear **Require TLS** and accept a warning. This is an explicit per-connection downgrade.
- Remote names reject controls, `/`, `\`, `.`, and `..`. Each selected remote name is UTF-8 percent-encoded as one URL path segment.

## Bounded and atomic work

- Requests are capped at 96 KiB, captured server output at 4 MiB, names at 1,024 characters, and listings at 100,000 entries.
- Listing validation and natural sorting happen in the isolated worker. Completion output is read in at most two 64-KiB chunks or 6 ms per UI dispatch. The dialog then revalidates, materializes, and inserts at most 64 remote lines or 8 ms of work per dispatch.
- Downloads first target an unpredictable file in the chosen destination directory. A successful transfer is committed with a write-through rename and never overwrites an existing item; collision suffixes are bounded at 9,999.
- Upload sources must be existing regular files. Download destinations must be absolute paths whose parent directory already exists.
- Cancellation terminates the isolated curl process and removes an uncommitted download temporary file.
- Curl is assigned while suspended to a `KILL_ON_JOB_CLOSE` Windows Job before it can read credentials or start network I/O, so a worker failure cannot leave an orphan transfer running.
