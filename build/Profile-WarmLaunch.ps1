[CmdletBinding()]
param(
    [string]$Binary,
    [string]$OutputDirectory,
    [ValidateRange(3, 30)]
    [int]$Samples = 9
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $Binary) {
    $Binary = Join-Path $repositoryRoot 'out\build\vs2022-x64\src\app\Release\FilesXPNative.exe'
}
$Binary = (Resolve-Path -LiteralPath $Binary).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot 'artifacts\warm-launch-profile'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

Add-Type -AssemblyName UIAutomationClient

function Close-TestWindow($Process) {
    if ($Process -and -not $Process.HasExited) {
        $null = $Process.CloseMainWindow()
        if (-not $Process.WaitForExit(3000)) { $Process.Kill() }
    }
}

function Measure-Launch([string]$Folder, [int]$Sample) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $quotedFolder = '"' + $Folder + '"'
    $process = Start-Process -FilePath $Binary -ArgumentList @('--new-window', $quotedFolder) -PassThru
    try {
        $inputIdle = $process.WaitForInputIdle(5000)
        $inputIdleMilliseconds = [double]$timer.Elapsed.TotalMilliseconds
        $root = $null
        while ($timer.ElapsedMilliseconds -lt 5000) {
            $process.Refresh()
            if ($process.HasExited) { throw "Application exited early with code $($process.ExitCode)." }
            if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
                try {
                    $candidate = [Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
                    if ($candidate -and $candidate.Current.IsEnabled) {
                        $root = $candidate
                        break
                    }
                }
                catch {
                    # UI Automation may race HWND publication; retain the official gate's retry behavior.
                }
            }
            Start-Sleep -Milliseconds 2
        }
        if (-not $root) { throw 'The main window did not become UI Automation-ready within 5 seconds.' }
        $uiAutomationReadyMilliseconds = [double]$timer.Elapsed.TotalMilliseconds
        $viewportTimer = [Diagnostics.Stopwatch]::StartNew()
        $listItemCondition = [Windows.Automation.PropertyCondition]::new(
            [Windows.Automation.AutomationElement]::ControlTypeProperty,
            [Windows.Automation.ControlType]::ListItem)
        $firstItem = $null
        while ($viewportTimer.ElapsedMilliseconds -lt 5000 -and -not $firstItem) {
            $firstItem = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $listItemCondition)
            if (-not $firstItem) { Start-Sleep -Milliseconds 5 }
        }
        if (-not $firstItem) { throw 'The first Shell viewport item did not appear within 5 seconds.' }
        [pscustomobject]@{
            Sample = $Sample
            InputIdleMilliseconds = $inputIdleMilliseconds
            UiAutomationReadyMilliseconds = $uiAutomationReadyMilliseconds
            UiAutomationAfterInputIdleMilliseconds = $uiAutomationReadyMilliseconds - $inputIdleMilliseconds
            FirstViewportAfterUiAutomationMilliseconds = [double]$viewportTimer.Elapsed.TotalMilliseconds
            WaitForInputIdleReturned = $inputIdle
        }
    }
    finally {
        Close-TestWindow $process
    }
}

$testFolder = Join-Path ([IO.Path]::GetTempPath()) ('FilesXPNative-WarmProfile-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testFolder | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $testFolder 'viewport-probe.txt'), 'probe')
    for ($index = 0; $index -lt 256; $index++) {
        [IO.File]::WriteAllText((Join-Path $testFolder ('item-{0:D3}.txt' -f $index)), 'probe')
    }
    $results = [Collections.Generic.List[object]]::new()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $results.Add((Measure-Launch $testFolder $sample))
    }
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'warm-launch-samples.json') -Encoding UTF8
    $summary = [pscustomobject]@{
        Samples = $Samples
        UiAutomationReadyMinimumMilliseconds = [double](($results | Measure-Object UiAutomationReadyMilliseconds -Minimum).Minimum)
        UiAutomationReadyMedianMilliseconds = [double](($results | Sort-Object UiAutomationReadyMilliseconds)[[int][math]::Floor(($Samples - 1) / 2)].UiAutomationReadyMilliseconds)
        UiAutomationReadyMaximumMilliseconds = [double](($results | Measure-Object UiAutomationReadyMilliseconds -Maximum).Maximum)
        InputIdleMinimumMilliseconds = [double](($results | Measure-Object InputIdleMilliseconds -Minimum).Minimum)
        InputIdleMedianMilliseconds = [double](($results | Sort-Object InputIdleMilliseconds)[[int][math]::Floor(($Samples - 1) / 2)].InputIdleMilliseconds)
        InputIdleMaximumMilliseconds = [double](($results | Measure-Object InputIdleMilliseconds -Maximum).Maximum)
        FirstViewportAfterUiAutomationMinimumMilliseconds = [double](($results | Measure-Object FirstViewportAfterUiAutomationMilliseconds -Minimum).Minimum)
        FirstViewportAfterUiAutomationMedianMilliseconds = [double](($results | Sort-Object FirstViewportAfterUiAutomationMilliseconds)[[int][math]::Floor(($Samples - 1) / 2)].FirstViewportAfterUiAutomationMilliseconds)
        FirstViewportAfterUiAutomationMaximumMilliseconds = [double](($results | Measure-Object FirstViewportAfterUiAutomationMilliseconds -Maximum).Maximum)
    }
    $summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'warm-launch-summary.json') -Encoding UTF8
    $results | Format-Table -AutoSize
    $summary | Format-List
}
finally {
    if (Test-Path -LiteralPath $testFolder) { Remove-Item -LiteralPath $testFolder -Recurse -Force }
}
