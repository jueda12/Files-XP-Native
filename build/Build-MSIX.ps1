[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Binary,

    [string]$OutputPath,

    [string]$ThirdPartyDirectory,

    [string]$SbomPath,

    [string]$CertificatePath,

    [securestring]$CertificatePassword
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputPath) {
    $OutputPath = Join-Path $repositoryRoot 'artifacts\Files-XP-Native-0.1.0-x64.msix'
}

function Find-WindowsSdkTool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $candidate = Get-ChildItem -LiteralPath $kitsRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$Name" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $candidate) { throw "$Name was not found in the Windows 10 SDK." }
    return $candidate
}

$resolvedBinary = (Resolve-Path -LiteralPath $Binary).Path
$makeAppx = Find-WindowsSdkTool 'makeappx.exe'
$stage = Join-Path $repositoryRoot 'artifacts\msix-stage'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'Assets') | Out-Null
Copy-Item -LiteralPath $resolvedBinary -Destination (Join-Path $stage 'FilesXPNative.exe')
if ($ThirdPartyDirectory) {
    $resolvedThirdParty = (Resolve-Path -LiteralPath $ThirdPartyDirectory).Path
    Copy-Item -LiteralPath (Join-Path $resolvedThirdParty '7z.exe') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $resolvedThirdParty '7z.dll') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $resolvedThirdParty 'License.txt') -Destination (Join-Path $stage '7-Zip-License.txt')
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'THIRD_PARTY_NOTICES.md') -Destination $stage
}
if ($SbomPath) {
    $resolvedSbom = (Resolve-Path -LiteralPath $SbomPath).Path
    Copy-Item -LiteralPath $resolvedSbom -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'packaging\Package.appxmanifest') -Destination (Join-Path $stage 'AppxManifest.xml')
Copy-Item -Path (Join-Path $repositoryRoot 'src\app\assets\*.png') -Destination (Join-Path $stage 'Assets')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'Locales') -Destination $stage -Recurse

$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
if (Test-Path -LiteralPath $OutputPath) { Remove-Item -LiteralPath $OutputPath -Force }
& $makeAppx pack /d $stage /p $OutputPath /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed with exit code $LASTEXITCODE." }
& (Join-Path $PSScriptRoot 'Normalize-ZipDosTimes.ps1') `
    -InputPath $OutputPath -OutputPath $OutputPath

if ($CertificatePath) {
    $signTool = Find-WindowsSdkTool 'signtool.exe'
    $arguments = @('sign', '/fd', 'SHA256', '/f', (Resolve-Path -LiteralPath $CertificatePath).Path)
    if ($CertificatePassword) {
        $credential = [System.Net.NetworkCredential]::new('', $CertificatePassword)
        $arguments += @('/p', $credential.Password)
    }
    $arguments += $OutputPath
    & $signTool @arguments
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed with exit code $LASTEXITCODE." }
}

Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath
