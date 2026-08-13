[CmdletBinding()]
param(
    [string]$SevenZipDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $SevenZipDirectory) {
    $SevenZipDirectory = & (Join-Path $PSScriptRoot 'Acquire-7Zip.ps1')
}
$sevenZip = Join-Path $SevenZipDirectory '7z.exe'
$testRoot = Join-Path $repositoryRoot ("build-local\archive-adapter-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $source = Join-Path $testRoot 'source.txt'
    $list = Join-Path $testRoot 'items.lst'
    [IO.File]::WriteAllText($source, "native archive adapter`r`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($list, "$source`n", [Text.UTF8Encoding]::new($false))

    $archive7z = Join-Path $testRoot 'encrypted.7z'
    @('secret', 'secret') | & $sevenZip a -t7z $archive7z -p -mhe=on -scsUTF-8 "@$list" -y -bso0 -bsp0 -bse0
    if ($LASTEXITCODE -ne 0) { throw "Encrypted 7z creation failed with exit code $LASTEXITCODE." }
    & $sevenZip t -psecret $archive7z -bso0 -bsp0 -bse0
    if ($LASTEXITCODE -ne 0) { throw "Encrypted 7z test failed with exit code $LASTEXITCODE." }
    $extract7z = Join-Path $testRoot 'extracted-7z'
    @('secret') | & $sevenZip x $archive7z "-o$extract7z" -aou -y -bso0 -bsp0 -bse0
    if ($LASTEXITCODE -ne 0) { throw "Encrypted 7z extraction failed with exit code $LASTEXITCODE." }

    $archiveZip = Join-Path $testRoot 'encrypted.zip'
    @('secret', 'secret') | & $sevenZip a -tzip $archiveZip -p -mem=AES256 -scsUTF-8 "@$list" -y -bso0 -bsp0 -bse0
    if ($LASTEXITCODE -ne 0) { throw "Encrypted ZIP creation failed with exit code $LASTEXITCODE." }
    & $sevenZip t -psecret $archiveZip -bso0 -bsp0 -bse0
    if ($LASTEXITCODE -ne 0) { throw "Encrypted ZIP test failed with exit code $LASTEXITCODE." }

    $archiveTar = Join-Path $testRoot 'archive.tar'
    & $sevenZip a -ttar $archiveTar -scsUTF-8 "@$list" -y -bso0 -bsp0 -bse0
    if ($LASTEXITCODE -ne 0) { throw "TAR creation failed with exit code $LASTEXITCODE." }
    & $sevenZip t $archiveTar -bso0 -bsp0 -bse0
    if ($LASTEXITCODE -ne 0) { throw "TAR test failed with exit code $LASTEXITCODE." }

    $originalHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash
    $extractedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $extract7z 'source.txt')).Hash
    if ($originalHash -ne $extractedHash) { throw 'Archive extraction changed the payload.' }
    Write-Host 'Archive adapter round-trip passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
