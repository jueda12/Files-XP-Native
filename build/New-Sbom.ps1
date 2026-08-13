[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Binary,

    [Parameter(Mandatory)]
    [string]$SevenZipDirectory,

    [Parameter(Mandatory)]
    [string]$OutputPath,

    [string]$Version = '0.1.0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$resolvedBinary = (Resolve-Path -LiteralPath $Binary).Path
$resolvedSevenZip = (Resolve-Path -LiteralPath $SevenZipDirectory).Path
$sevenZipExe = (Resolve-Path -LiteralPath (Join-Path $resolvedSevenZip '7z.exe')).Path
$sevenZipDll = (Resolve-Path -LiteralPath (Join-Path $resolvedSevenZip '7z.dll')).Path

function Get-LowerSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

$binaryHash = Get-LowerSha256 $resolvedBinary
$sevenZipExeHash = Get-LowerSha256 $sevenZipExe
$sevenZipDllHash = Get-LowerSha256 $sevenZipDll
$commit = 'NOASSERTION'
$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) { $git = Get-Command git -ErrorAction SilentlyContinue }
if ($git) {
    $candidate = (& $git.Source -C $repositoryRoot rev-parse HEAD 2>$null | Select-Object -First 1)
    if ($LASTEXITCODE -eq 0 -and $candidate -match '^[0-9a-fA-F]{40}$') {
        $commit = $candidate.ToLowerInvariant()
    }
}
$namespaceId = if ($commit -eq 'NOASSERTION') { $binaryHash } else { $commit }

$document = [ordered]@{
    spdxVersion = 'SPDX-2.3'
    dataLicense = 'CC0-1.0'
    SPDXID = 'SPDXRef-DOCUMENT'
    name = "Files-XP-Native-$Version"
    documentNamespace = "https://filesxp.invalid/spdx/$namespaceId"
    creationInfo = [ordered]@{
        created = '1970-01-01T00:00:00Z'
        creators = @('Tool: build/New-Sbom.ps1')
    }
    packages = @(
        [ordered]@{
            name = 'Files XP Native'
            SPDXID = 'SPDXRef-Package-FilesXPNative'
            versionInfo = $Version
            downloadLocation = 'NOASSERTION'
            filesAnalyzed = $false
            licenseConcluded = 'NOASSERTION'
            licenseDeclared = 'MIT'
            supplier = 'NOASSERTION'
            primaryPackagePurpose = 'APPLICATION'
        },
        [ordered]@{
            name = '7-Zip'
            SPDXID = 'SPDXRef-Package-7Zip'
            versionInfo = '26.02'
            downloadLocation = 'https://registry.npmjs.org/7zip-bin-full/-/7zip-bin-full-26.2.1.tgz'
            filesAnalyzed = $false
            licenseConcluded = 'NOASSERTION'
            licenseDeclared = 'NOASSERTION'
            supplier = 'Person: Igor Pavlov'
        },
        [ordered]@{
            name = 'Windows inbox curl'
            SPDXID = 'SPDXRef-Package-WindowsCurl'
            versionInfo = 'Windows-provided'
            downloadLocation = 'NOASSERTION'
            filesAnalyzed = $false
            licenseConcluded = 'NOASSERTION'
            licenseDeclared = 'curl'
            supplier = 'Organization: Microsoft Corporation'
        }
    )
    files = @(
        [ordered]@{
            fileName = 'FilesXPNative.exe'
            SPDXID = 'SPDXRef-File-Executable'
            checksums = @([ordered]@{ algorithm = 'SHA256'; checksumValue = $binaryHash })
            licenseConcluded = 'MIT'
            copyrightText = 'NOASSERTION'
        },
        [ordered]@{
            fileName = '7z.exe'
            SPDXID = 'SPDXRef-File-7ZipExe'
            checksums = @([ordered]@{ algorithm = 'SHA256'; checksumValue = $sevenZipExeHash })
            licenseConcluded = 'LGPL-2.1-or-later'
            copyrightText = 'Copyright (C) 1999-2026 Igor Pavlov'
        },
        [ordered]@{
            fileName = '7z.dll'
            SPDXID = 'SPDXRef-File-7ZipDll'
            checksums = @([ordered]@{ algorithm = 'SHA256'; checksumValue = $sevenZipDllHash })
            licenseConcluded = 'NOASSERTION'
            copyrightText = 'Copyright (C) 1999-2026 Igor Pavlov'
        }
    )
    relationships = @(
        [ordered]@{ spdxElementId = 'SPDXRef-Package-FilesXPNative'; relationshipType = 'CONTAINS'; relatedSpdxElement = 'SPDXRef-File-Executable' },
        [ordered]@{ spdxElementId = 'SPDXRef-Package-FilesXPNative'; relationshipType = 'DEPENDS_ON'; relatedSpdxElement = 'SPDXRef-Package-7Zip' },
        [ordered]@{ spdxElementId = 'SPDXRef-Package-FilesXPNative'; relationshipType = 'DEPENDS_ON'; relatedSpdxElement = 'SPDXRef-Package-WindowsCurl' },
        [ordered]@{ spdxElementId = 'SPDXRef-Package-7Zip'; relationshipType = 'CONTAINS'; relatedSpdxElement = 'SPDXRef-File-7ZipExe' },
        [ordered]@{ spdxElementId = 'SPDXRef-Package-7Zip'; relationshipType = 'CONTAINS'; relatedSpdxElement = 'SPDXRef-File-7ZipDll' }
    )
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) { New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null }
$document | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath
