[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64')]
    [string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$artifactDirectory = Join-Path $repositoryRoot 'artifacts'
$logDirectory = Join-Path $artifactDirectory 'logs'
$preset = 'vs2022-x64'
$buildPreset = if ($Configuration -eq 'Release') { 'release-x64' } else { 'debug-x64' }
$cmakeProject = Get-Content -LiteralPath (Join-Path $repositoryRoot 'CMakeLists.txt') -Raw
if ($cmakeProject -notmatch 'project\(FilesXPNative VERSION ([0-9]+(?:\.[0-9]+)+)') {
    throw 'Could not read the project version from CMakeLists.txt.'
}
$version = $Matches[1]

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw 'CMake 3.28 or later is required and must be available on PATH.'
}

$cmakeVersionLine = (& cmake.exe --version | Select-Object -First 1)
if ($cmakeVersionLine -notmatch '(\d+)\.(\d+)\.(\d+)') {
    throw "Unable to determine CMake version from: $cmakeVersionLine"
}
if ([version]$Matches[0] -lt [version]'3.28.0') {
    throw "CMake 3.28 or later is required. Found $($Matches[0])."
}

New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$sevenZipDirectory = & (Join-Path $PSScriptRoot 'Acquire-7Zip.ps1')
& (Join-Path $PSScriptRoot 'Test-ArchiveAdapter.ps1') -SevenZipDirectory $sevenZipDirectory
$configureLog = Join-Path $logDirectory 'configure.log'
$buildLog = Join-Path $logDirectory 'build.log'

Push-Location $repositoryRoot
try {
    & cmake.exe --preset $preset 2>&1 | Tee-Object -FilePath $configureLog
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE."
    }

    & cmake.exe --build --preset $buildPreset -- /m /bl:"$logDirectory\build.binlog" 2>&1 |
        Tee-Object -FilePath $buildLog
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    $binary = Join-Path $repositoryRoot "out\build\$preset\src\app\$Configuration\FilesXPNative.exe"
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Build reported success but the executable was not found at $binary."
    }

    $staging = Join-Path $artifactDirectory "Files-XP-Native-$version-$Architecture"
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
    New-Item -ItemType Directory -Path $staging | Out-Null
    Copy-Item -LiteralPath $binary -Destination $staging
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.md') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'THIRD_PARTY_NOTICES.md') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'Locales') -Destination $staging -Recurse
    Copy-Item -LiteralPath (Join-Path $sevenZipDirectory '7z.exe') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $sevenZipDirectory '7z.dll') -Destination $staging
    Copy-Item -LiteralPath (Join-Path $sevenZipDirectory 'License.txt') -Destination (Join-Path $staging '7-Zip-License.txt')
    $sbomPath = Join-Path $staging 'FilesXPNative.spdx.json'
    & (Join-Path $PSScriptRoot 'New-Sbom.ps1') -Binary $binary `
        -SevenZipDirectory $sevenZipDirectory -OutputPath $sbomPath -Version $version
    if (-not (Test-Path -LiteralPath $sbomPath -PathType Leaf)) {
        throw 'SPDX SBOM generation did not produce an output file.'
    }

    $zipPath = Join-Path $artifactDirectory "Files-XP-Native-$version-$Architecture.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zipPath -CompressionLevel Optimal

    $msixPath = Join-Path $artifactDirectory "Files-XP-Native-$version-$Architecture.msix"
    & (Join-Path $PSScriptRoot 'Build-MSIX.ps1') -Binary $binary -OutputPath $msixPath `
        -ThirdPartyDirectory $sevenZipDirectory -SbomPath $sbomPath
    if ($LASTEXITCODE -ne 0) {
        throw "MSIX packaging failed with exit code $LASTEXITCODE."
    }

    Write-Host ''
    Write-Host 'BUILD RESULT: SUCCESS'
    Get-FileHash -Algorithm SHA256 -LiteralPath $binary, $zipPath, $msixPath |
        Format-Table Path, Hash -AutoSize
    Write-Host "Build log: $buildLog"
    Write-Host "Binary log: $(Join-Path $logDirectory 'build.binlog')"
}
finally {
    Pop-Location
}

