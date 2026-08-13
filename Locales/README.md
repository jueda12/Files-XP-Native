# External locale packs

Files XP Native loads an optional UTF-8 locale pack from this directory after selecting the
matching built-in language:

- `en-US.lang`
- `zh-Hant.lang`
- `zh-Hans.lang`

The first meaningful line must be `FXL1`. Remaining entries use `Text` enum indices from
`src/app/localization.h`:

```text
FXL1
# index=translation
1=Previous
9=Search here
```

Blank lines and lines beginning with `#` or `;` are ignored. Use `\n`, `\t`, `\\`, and `\=` for
escaped newlines, tabs, backslashes, and equals signs. Packs may override only the entries they
contain. Invalid UTF-8, duplicate or out-of-range indices, empty values, entries over 512 UTF-16
characters, and files over 256 KiB cause the whole pack to be ignored safely.

Indices are an append-only compatibility contract for the `FXL1` format. A future incompatible
string-table change will use a new format marker instead of silently remapping an existing pack.
