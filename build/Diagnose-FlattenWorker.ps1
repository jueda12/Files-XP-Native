[CmdletBinding()]
param(
    [string]$Binary = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\build\vs2022-x64\src\app\Release\FilesXPNative.exe'),

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IgnoredArguments
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ('FilesXPNative-FlattenDiagnosis-' + [guid]::NewGuid())
$resultPath = Join-Path ([IO.Path]::GetTempPath()) ('FilesXPNative-FlattenDiagnosis-' + [guid]::NewGuid() + '.txt')
$eventName = 'Local\FilesXPNative-FlattenDiagnosis-' + [guid]::NewGuid()
$cancel = $null
$app = $null
$worker = $null
try {
    $first = Join-Path $root 'first'
    $second = Join-Path $root 'second\deep'
    New-Item -ItemType Directory -Path $first, $second -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'top.txt'), 'top')
    [IO.File]::WriteAllText((Join-Path $first 'first.txt'), 'first')
    [IO.File]::WriteAllText((Join-Path $second 'second.txt'), 'second')

    $app = Start-Process -FilePath $Binary -ArgumentList @('--new-window', ('"' + $root + '"')) -PassThru
    $ready = [Diagnostics.Stopwatch]::StartNew()
    while ($ready.ElapsedMilliseconds -lt 5000 -and $app.MainWindowHandle -eq 0) {
        if ($app.HasExited) { throw "Owner application exited with code $($app.ExitCode)." }
        Start-Sleep -Milliseconds 10
        $app.Refresh()
    }
    if ($app.MainWindowHandle -eq 0) { throw 'Owner application did not create a window.' }

    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    if (-not $created) { throw 'Could not create cancellation event.' }
    $arguments = @(
        '--flatten-worker', $app.MainWindowHandle.ToString(), '990001',
        ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $root + '"'))
    $started = [Diagnostics.Stopwatch]::StartNew()
    $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
    $finishedBeforeCancel = $worker.WaitForExit(5000)
    $worker.Refresh()
    $beforeCancel = [ordered]@{
        Finished = $finishedBeforeCancel
        ExitCode = $(if ($finishedBeforeCancel) { $worker.ExitCode } else { $null })
        ResultExists = Test-Path -LiteralPath $resultPath -PathType Leaf
        ResultLength = $(if (Test-Path -LiteralPath $resultPath -PathType Leaf) { (Get-Item -LiteralPath $resultPath).Length } else { 0 })
        ElapsedMilliseconds = [math]::Round($started.Elapsed.TotalMilliseconds, 1)
    }
    if (-not $finishedBeforeCancel) {
        $cancel.Set() | Out-Null
        if (-not $worker.WaitForExit(5000)) {
            throw 'Flatten worker did not finish its bounded three-file operation within 5 seconds.'
        }
    }
    $worker.Refresh()
    if ($worker.ExitCode -ne 0) { throw "Flatten worker failed with exit code $($worker.ExitCode)." }
    foreach ($name in @('top.txt', 'first.txt', 'second.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)) {
            throw "Flatten worker did not preserve $name in the selected root."
        }
    }
    if ((Test-Path -LiteralPath $first) -or
        (Test-Path -LiteralPath (Join-Path $root 'second'))) {
        throw 'Flatten worker left nested source folders behind.'
    }
    if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf) -or
        (Get-Item -LiteralPath $resultPath).Length -eq 0) {
        throw 'Flatten worker did not write a non-empty bounded result.'
    }
    [ordered]@{
        Input = @{ Files = 3; NestedDirectories = 2 }
        CompletedWithinFiveSeconds = $finishedBeforeCancel
        ExitCode = $worker.ExitCode
        ResultLength = (Get-Item -LiteralPath $resultPath).Length
        ElapsedMilliseconds = [math]::Round($started.Elapsed.TotalMilliseconds, 1)
    } | ConvertTo-Json -Depth 3
}
finally {
    if ($worker -and -not $worker.HasExited) { $worker.Kill(); $worker.WaitForExit() }
    if ($cancel) { $cancel.Dispose() }
    if ($app -and -not $app.HasExited) {
        $null = $app.CloseMainWindow()
        if (-not $app.WaitForExit(3000)) { $app.Kill(); $app.WaitForExit() }
    }
    if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
