#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${MINGW_ROOT:?Set MINGW_ROOT to a MinGW-w64 toolchain root}"

compiler="$MINGW_ROOT/bin/x86_64-w64-mingw32-g++"
resource_compiler="$MINGW_ROOT/bin/x86_64-w64-mingw32-windres"
object_dump="$MINGW_ROOT/bin/x86_64-w64-mingw32-objdump"
for tool in "$compiler" "$resource_compiler" "$object_dump"; do
  [[ -x "$tool" ]] || { echo "Missing tool: $tool" >&2; exit 2; }
done

output_dir="$repo_root/build-local/cross"
mkdir -p "$output_dir"
run_dir="$(mktemp -d "$output_dir/run.XXXXXX")"
compiler_temp_dir="$run_dir/compiler-temp"
mkdir -p "$compiler_temp_dir"
cleanup() { rm -rf -- "$run_dir"; }
trap cleanup EXIT
export TMPDIR="$compiler_temp_dir"
export LD_LIBRARY_PATH="$MINGW_ROOT/libexec${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SOURCE_DATE_EPOCH="$(git -C "$repo_root" show -s --format=%ct HEAD)"

common=(
  -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
  -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX
)
sources=(
  "$repo_root/src/app/archive_worker.cpp"
  "$repo_root/src/app/bulk_rename_worker.cpp"
  "$repo_root/src/app/main.cpp"
  "$repo_root/src/app/app_window.cpp"
  "$repo_root/src/app/explorer_browser_host.cpp"
  "$repo_root/src/app/flatten_worker.cpp"
  "$repo_root/src/app/folder_selection_worker.cpp"
  "$repo_root/src/app/ftp_worker.cpp"
  "$repo_root/src/app/git_worker.cpp"
  "$repo_root/src/app/localization.cpp"
  "$repo_root/src/app/preview_worker.cpp"
  "$repo_root/src/app/search_worker.cpp"
  "$repo_root/src/app/session_store.cpp"
  "$repo_root/src/app/settings_store.cpp"
  "$repo_root/src/app/shell_operation_worker.cpp"
  "$repo_root/src/app/shell_artifact_worker.cpp"
  "$repo_root/src/app/tag_worker.cpp"
  "$repo_root/src/app/xp_theme.cpp"
)

(
  cd "$output_dir"
  "$compiler" "${common[@]}" -fsyntax-only "${sources[@]}"
  "$resource_compiler" -DUNICODE -D_UNICODE -I "$repo_root/src/app" \
    "$repo_root/src/app/app.rc" -O coff -o "$run_dir/app.res"
  "$compiler" "${common[@]}" -municode -mwindows -static-libgcc -static-libstdc++ \
    -Wl,--no-insert-timestamp \
    "${sources[@]}" "$run_dir/app.res" -o "$run_dir/FilesXPNative-cross.exe" \
    -lcomctl32 -lole32 -lshell32 -lshlwapi -lmsimg32 -loleaut32 -lpropsys -lmpr -loleacc -ladvapi32 -luuid
)

"$object_dump" -p "$run_dir/FilesXPNative-cross.exe" > "$run_dir/pe-headers.txt"
grep -q 'Subsystem.*Windows GUI' "$run_dir/pe-headers.txt"
for protection in HIGH_ENTROPY_VA DYNAMIC_BASE NX_COMPAT; do
  grep -q "$protection" "$run_dir/pe-headers.txt"
done
mv -f "$run_dir/FilesXPNative-cross.exe" "$output_dir/FilesXPNative-cross.exe"
sha256sum "$output_dir/FilesXPNative-cross.exe"
