[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string]$Root,

    [ValidateRange(1, 1000000)]
    [int]$Count = 10000,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$fullRoot = [System.IO.Path]::GetFullPath($Root)
$driveRoot = [System.IO.Path]::GetPathRoot($fullRoot)
$profileRoot = [System.IO.Path]::GetFullPath($env:USERPROFILE)
if ($fullRoot.TrimEnd('\') -eq $driveRoot.TrimEnd('\')) {
    throw 'Refusing to use a drive root as benchmark output.'
}
if ($fullRoot.TrimEnd('\') -eq $profileRoot.TrimEnd('\')) {
    throw 'Refusing to use the user profile root as benchmark output.'
}

if (Test-Path -LiteralPath $fullRoot) {
    $hasContent = Get-ChildItem -LiteralPath $fullRoot -Force -ErrorAction Stop | Select-Object -First 1
    if ($hasContent -and -not $Force) {
        throw 'The target directory is not empty. Choose an empty path or pass -Force explicitly.'
    }
} elseif ($PSCmdlet.ShouldProcess($fullRoot, 'Create benchmark directory')) {
    New-Item -ItemType Directory -Path $fullRoot -Force | Out-Null
}

if (-not $PSCmdlet.ShouldProcess($fullRoot, "Create $Count benchmark entries")) {
    return
}

$extensions = @('.txt', '.log', '.jpg', '.png', '.mp3', '.zip', '.cpp', '.md')
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
for ($index = 0; $index -lt $Count; $index++) {
    if (($index % 20) -eq 0) {
        $folder = Join-Path $fullRoot ('Folder-{0:D6}' -f $index)
        [System.IO.Directory]::CreateDirectory($folder) | Out-Null
    } else {
        $extension = $extensions[$index % $extensions.Count]
        $name = 'File-{0:D7}{1}' -f $index, $extension
        $path = Join-Path $fullRoot $name
        [System.IO.File]::WriteAllText($path, "benchmark item $index`r`n", $utf8NoBom)
    }
}
$stopwatch.Stop()

Write-Host "Created $Count entries in $($stopwatch.ElapsedMilliseconds) ms"
Write-Host "Dataset: $fullRoot"

