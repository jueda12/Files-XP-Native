#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
with_test_engine=false
if [[ "${1:-}" == '--with-test-engine' ]]; then
  with_test_engine=true
  shift
fi
destination="${1:-$repo_root/build-local/third-party/7zip/x64}"
test_engine="$repo_root/build-local/third-party/7zip/linux/x64/7zz"
package_url='https://registry.npmjs.org/7zip-bin-full/-/7zip-bin-full-26.2.1.tgz'
package_hash='08e0d1ad863a27967b5b4ed6fe78cb305ed2b075e16e5f0f31504fb638f314f6'

valid_files() {
  [[ -f "$destination/7z.exe" && -f "$destination/7z.dll" && -f "$destination/License.txt" ]] || return 1
  [[ "$(sha256sum "$destination/7z.exe" | awk '{print $1}')" == '83967f1b02b43c4efeda302795722c809e0e81b8307de73558d10484d5676a7d' ]] || return 1
  [[ "$(sha256sum "$destination/7z.dll" | awk '{print $1}')" == '69fd4df057985c40e510e2fac182881c7f85e90aa13ec703f763a8fdb2ce61f8' ]] || return 1
  [[ "$(sha256sum "$destination/License.txt" | awk '{print $1}')" == '519ac0a4bded9c18ea02e0afb71f663d8c47373bd9facd3ac96a79f51d77765d' ]]
}

valid_test_engine() {
  [[ -f "$test_engine" ]] || return 1
  [[ "$(sha256sum "$test_engine" | awk '{print $1}')" == '1676a968815b92e865bc0ffeecee3fa284ba4402bf23dc2bec2412c4b502e922' ]]
}

if valid_files && { ! $with_test_engine || valid_test_engine; }; then
  echo "$destination"
  exit 0
fi

temporary="$(mktemp -d "$repo_root/build-local/7zip-acquire.XXXXXX")"
trap 'find "$temporary" -type f -delete; find "$temporary" -depth -type d -empty -delete' EXIT
archive="$temporary/7zip.tgz"
curl --fail --location --proto '=https' --tlsv1.2 "$package_url" --output "$archive"
echo "$package_hash  $archive" | sha256sum --check --status
members=(package/win/x64/7z.exe package/win/x64/7z.dll package/win/x64/License.txt)
if $with_test_engine; then
  members+=(package/linux/x64/7zz)
fi
tar -xzf "$archive" -C "$temporary" -- "${members[@]}"
mkdir -p "$destination"
cp "$temporary/package/win/x64/7z.exe" "$temporary/package/win/x64/7z.dll" \
  "$temporary/package/win/x64/License.txt" "$destination/"
valid_files
if $with_test_engine; then
  mkdir -p "$(dirname "$test_engine")"
  cp "$temporary/package/linux/x64/7zz" "$test_engine"
  chmod +x "$test_engine"
  valid_test_engine
fi
echo "$destination"
