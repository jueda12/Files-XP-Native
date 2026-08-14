[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$InputPath,

    [Parameter(Mandatory)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $InputPath))
$localHeaders = 0
$centralHeaders = 0

for ($index = 0; $index -le $bytes.Length - 4; $index++) {
    if ($bytes[$index] -ne 0x50 -or $bytes[$index + 1] -ne 0x4b) { continue }
    if ($bytes[$index + 2] -eq 0x03 -and $bytes[$index + 3] -eq 0x04) {
        [Array]::Clear($bytes, $index + 10, 4)
        $localHeaders++
    } elseif ($bytes[$index + 2] -eq 0x01 -and $bytes[$index + 3] -eq 0x02) {
        [Array]::Clear($bytes, $index + 12, 4)
        $centralHeaders++
    }
}

if ($localHeaders -eq 0 -or $centralHeaders -eq 0) {
    throw 'No ZIP local/central directory headers were found.'
}

[IO.File]::WriteAllBytes($OutputPath, $bytes)
Write-Host "ZIP timestamps normalized: local=$localHeaders central=$centralHeaders"
