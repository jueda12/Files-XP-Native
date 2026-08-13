#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${MINGW_ROOT:?Set MINGW_ROOT to the MinGW-w64 toolchain root}"

version="$(sed -n 's/^project(FilesXPNative VERSION \([^ ]*\).*/\1/p' "$repo_root/CMakeLists.txt")"
commit="$(git -C "$repo_root" rev-parse HEAD)"
artifact_dir="$repo_root/artifacts"
mkdir -p "$artifact_dir"
stage="$(mktemp -d "$artifact_dir/.package.XXXXXX")"
trap 'find "$stage" -type f -delete; find "$stage" -depth -type d -empty -delete' EXIT

"$repo_root/build/Test-PortableCore.sh"
"$repo_root/build/Test-ModelBenchmark.sh"
"$repo_root/build/Test-CrossCompile.sh"
"$repo_root/build/Test-ArchiveAdapter.sh"

seven_zip_dir="$("$repo_root/build/Acquire-7Zip.sh")"

binary="$repo_root/build-local/cross/FilesXPNative-cross.exe"
binary_hash="$(sha256sum "$binary" | awk '{print $1}')"
seven_zip_exe_hash="$(sha256sum "$seven_zip_dir/7z.exe" | awk '{print $1}')"
seven_zip_dll_hash="$(sha256sum "$seven_zip_dir/7z.dll" | awk '{print $1}')"
compiler_version="$($MINGW_ROOT/bin/x86_64-w64-mingw32-g++ -dumpfullversion)"
sbom="$stage/FilesXPNative.spdx.json"
jq -n \
  --arg version "$version" \
  --arg commit "$commit" \
  --arg compiler "$compiler_version" \
  --arg binaryHash "$binary_hash" \
  --arg sevenZipExeHash "$seven_zip_exe_hash" \
  --arg sevenZipDllHash "$seven_zip_dll_hash" \
  '{
    spdxVersion: "SPDX-2.3",
    dataLicense: "CC0-1.0",
    SPDXID: "SPDXRef-DOCUMENT",
    name: ("Files-XP-Native-" + $version),
    documentNamespace: ("https://filesxp.invalid/spdx/" + $commit),
    creationInfo: {
      created: "1970-01-01T00:00:00Z",
      creators: ["Tool: build/Package-Cross.sh", ("Tool: MinGW-w64-GCC-" + $compiler)]
    },
    packages: [{
      name: "Files XP Native",
      SPDXID: "SPDXRef-Package-FilesXPNative",
      versionInfo: $version,
      downloadLocation: "NOASSERTION",
      filesAnalyzed: false,
      licenseConcluded: "NOASSERTION",
      licenseDeclared: "MIT",
      supplier: "NOASSERTION",
      primaryPackagePurpose: "APPLICATION"
    }, {
      name: "7-Zip",
      SPDXID: "SPDXRef-Package-7Zip",
      versionInfo: "26.02",
      downloadLocation: "https://registry.npmjs.org/7zip-bin-full/-/7zip-bin-full-26.2.1.tgz",
      filesAnalyzed: false,
      licenseConcluded: "NOASSERTION",
      licenseDeclared: "NOASSERTION",
      supplier: "Person: Igor Pavlov"
    }, {
      name: "Windows inbox curl",
      SPDXID: "SPDXRef-Package-WindowsCurl",
      versionInfo: "Windows-provided",
      downloadLocation: "NOASSERTION",
      filesAnalyzed: false,
      licenseConcluded: "NOASSERTION",
      licenseDeclared: "curl",
      supplier: "Organization: Microsoft Corporation"
    }],
    files: [{
      fileName: "FilesXPNative.exe",
      SPDXID: "SPDXRef-File-Executable",
      checksums: [{algorithm: "SHA256", checksumValue: $binaryHash}],
      licenseConcluded: "MIT",
      copyrightText: "NOASSERTION"
    }, {
      fileName: "7z.exe",
      SPDXID: "SPDXRef-File-7ZipExe",
      checksums: [{algorithm: "SHA256", checksumValue: $sevenZipExeHash}],
      licenseConcluded: "LGPL-2.1-or-later",
      copyrightText: "Copyright (C) 1999-2026 Igor Pavlov"
    }, {
      fileName: "7z.dll",
      SPDXID: "SPDXRef-File-7ZipDll",
      checksums: [{algorithm: "SHA256", checksumValue: $sevenZipDllHash}],
      licenseConcluded: "NOASSERTION",
      copyrightText: "Copyright (C) 1999-2026 Igor Pavlov"
    }],
    relationships: [{
      spdxElementId: "SPDXRef-Package-FilesXPNative",
      relationshipType: "CONTAINS",
      relatedSpdxElement: "SPDXRef-File-Executable"
    }, {
      spdxElementId: "SPDXRef-Package-FilesXPNative",
      relationshipType: "DEPENDS_ON",
      relatedSpdxElement: "SPDXRef-Package-7Zip"
    }, {
      spdxElementId: "SPDXRef-Package-FilesXPNative",
      relationshipType: "DEPENDS_ON",
      relatedSpdxElement: "SPDXRef-Package-WindowsCurl"
    }, {
      spdxElementId: "SPDXRef-Package-7Zip",
      relationshipType: "CONTAINS",
      relatedSpdxElement: "SPDXRef-File-7ZipExe"
    }, {
      spdxElementId: "SPDXRef-Package-7Zip",
      relationshipType: "CONTAINS",
      relatedSpdxElement: "SPDXRef-File-7ZipDll"
    }]
  }' > "$sbom"

cp "$binary" "$stage/FilesXPNative.exe"
cp "$seven_zip_dir/7z.exe" "$seven_zip_dir/7z.dll" "$stage/"
cp "$seven_zip_dir/License.txt" "$stage/7-Zip-License.txt"
cp "$repo_root/README.md" "$repo_root/LICENSE" "$repo_root/THIRD_PARTY_NOTICES.md" "$stage/"
cp -R "$repo_root/Locales" "$stage/"
portable="$artifact_dir/Files-XP-Native-$version-windows10-x64-preview.zip"
find "$stage" -exec touch -t 198001010000 -- {} +
rm -f -- "$portable"
(cd "$stage" && zip -X -q "$portable" FilesXPNative.exe 7z.exe 7z.dll 7-Zip-License.txt \
  FilesXPNative.spdx.json README.md LICENSE THIRD_PARTY_NOTICES.md Locales/README.md \
  Locales/zh-Hant.lang.example)

source_zip="$artifact_dir/Files-XP-Native-$version-source.zip"
rm -f -- "$source_zip"
git -C "$repo_root" archive --format=zip --output="$source_zip" HEAD
sha256sum "$portable" "$source_zip" > "$artifact_dir/SHA256SUMS.txt"
sha256sum "$portable" "$source_zip"
