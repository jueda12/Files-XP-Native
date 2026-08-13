[CmdletBinding()]
param(
    [string]$Destination
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $Destination) {
    $Destination = Join-Path $repositoryRoot 'build-local\third-party\7zip\x64'
}

$packageUrl = 'https://registry.npmjs.org/7zip-bin-full/-/7zip-bin-full-26.2.1.tgz'
$packageHash = '08E0D1AD863A27967B5B4ED6FE78CB305ED2B075E16E5F0F31504FB638F314F6'
$fileHashes = @{
    '7z.exe' = '83967F1B02B43C4EFEDA302795722C809E0E81B8307DE73558D10484D5676A7D'
    '7z.dll' = '69FD4DF057985C40E510E2FAC182881C7F85E90AA13EC703F763A8FDB2CE61F8'
    'License.txt' = '519AC0A4BDED9C18EA02E0AFB71F663D8C47373BD9FACD3AC96A79F51D77765D'
}

function Test-AcquiredFiles {
    foreach ($entry in $fileHashes.GetEnumerator()) {
        $path = Join-Path $Destination $entry.Key
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $entry.Value) { return $false }
    }
    return $true
}

if (Test-AcquiredFiles) {
    $Destination
    return
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) ("FilesXPNative-7Zip-" + [guid]::NewGuid())
$archive = Join-Path $temporary '7zip.tgz'
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    Invoke-WebRequest -UseBasicParsing -Uri $packageUrl -OutFile $archive
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $packageHash) {
        throw 'The pinned 7-Zip package failed SHA-256 verification.'
    }
    $members = @('package/win/x64/7z.exe', 'package/win/x64/7z.dll', 'package/win/x64/License.txt')
    & tar.exe -xzf $archive -C $temporary -- @members
    if ($LASTEXITCODE -ne 0) { throw "tar.exe failed with exit code $LASTEXITCODE." }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($name in $fileHashes.Keys) {
        Copy-Item -LiteralPath (Join-Path $temporary "package\win\x64\$name") -Destination $Destination -Force
    }
    if (-not (Test-AcquiredFiles)) { throw 'The extracted 7-Zip files failed SHA-256 verification.' }
    $Destination
}
finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
