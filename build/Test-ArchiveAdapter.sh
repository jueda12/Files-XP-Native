#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$repo_root/build/Acquire-7Zip.sh" --with-test-engine >/dev/null
seven_zip="$repo_root/build-local/third-party/7zip/linux/x64/7zz"
test_root="$(mktemp -d "$repo_root/build-local/archive-adapter.XXXXXX")"
trap 'find "$test_root" -type f -delete; find "$test_root" -depth -type d -empty -delete' EXIT

printf 'native archive adapter\n' > "$test_root/source.txt"
printf '%s\n' "$test_root/source.txt" > "$test_root/items.lst"
password='sëcret-密碼'
printf '%s\n%s\n' "$password" "$password" | "$seven_zip" a -t7z "$test_root/encrypted.7z" -p -mhe=on \
  -scsUTF-8 -sccUTF-8 @"$test_root/items.lst" -y -bso0 -bsp0 -bse0
"$seven_zip" t "-p$password" "$test_root/encrypted.7z" -bso0 -bsp0 -bse0
mkdir -p "$test_root/extracted"
printf '%s\n' "$password" | "$seven_zip" x "$test_root/encrypted.7z" \
  -o"$test_root/extracted" -aou -y -sccUTF-8 -bso0 -bsp0 -bse0
cmp "$test_root/source.txt" "$test_root/extracted/source.txt"

zip_password='zip-secret'
printf '%s\n%s\n' "$zip_password" "$zip_password" | "$seven_zip" a -tzip "$test_root/encrypted.zip" -p -mem=AES256 \
  -scsUTF-8 -sccUTF-8 @"$test_root/items.lst" -y -bso0 -bsp0 -bse0
"$seven_zip" t "-p$zip_password" "$test_root/encrypted.zip" -bso0 -bsp0 -bse0
mkdir -p "$test_root/extracted-zip"
printf '%s\n' "$zip_password" | "$seven_zip" x "$test_root/encrypted.zip" \
  -o"$test_root/extracted-zip" -aou -y -sccUTF-8 -bso0 -bsp0 -bse0
cmp "$test_root/source.txt" "$test_root/extracted-zip/source.txt"

"$seven_zip" a -ttar "$test_root/archive.tar" -scsUTF-8 @"$test_root/items.lst" \
  -y -bso0 -bsp0 -bse0
"$seven_zip" t "$test_root/archive.tar" -bso0 -bsp0 -bse0

printf 'archive adapter round-trip passed\n'
