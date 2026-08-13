[CmdletBinding()]
param(
    [string]$Binary,

    [string]$OutputDirectory,



    [string]$AccessibilityProbe,



    [switch]$CaptureWpr,

    [string]$PresentMonPath,

    [switch]$RequireSignature
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $Binary) {
    $Binary = Join-Path $repositoryRoot 'out\build\vs2022-x64\src\app\Release\FilesXPNative.exe'
}
$Binary = (Resolve-Path -LiteralPath $Binary).Path

if (-not $AccessibilityProbe) {

    $AccessibilityProbe = Join-Path $repositoryRoot `

        'out\build\vs2022-x64\tests\Release\FilesXPNativeAccessibilityProbe.exe'

}

$AccessibilityProbe = (Resolve-Path -LiteralPath $AccessibilityProbe).Path

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot 'artifacts\runtime-validation'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

if ($RequireSignature) {
    $signature = Get-AuthenticodeSignature -FilePath $Binary
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "Authenticode validation failed: $($signature.Status) $($signature.StatusMessage)"
    }
}

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class FilesXpRuntimeNative {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SendMessageTimeout(
        IntPtr hWnd, uint msg, UIntPtr wParam, IntPtr lParam,
        uint flags, uint timeout, out UIntPtr result);
}
'@

function Start-MeasuredWindow([string]$Folder) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $quotedFolder = '"' + $Folder + '"'
    $process = Start-Process -FilePath $Binary -ArgumentList @('--new-window', $quotedFolder) -PassThru
    try { $null = $process.WaitForInputIdle(5000) } catch { }
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
            } catch { }
        }
        Start-Sleep -Milliseconds 10
    }
    if (-not $root) {
        $process.Kill()
        throw 'The main window did not become UI Automation-ready within 5 seconds.'
    }
    [pscustomobject]@{
        Process = $process
        Root = $root
        LaunchMilliseconds = [double]$timer.Elapsed.TotalMilliseconds
    }
}

function Close-TestWindow($Process) {
    if ($Process -and -not $Process.HasExited) {
        $null = $Process.CloseMainWindow()
        if (-not $Process.WaitForExit(3000)) { $Process.Kill() }
    }
}

function Get-P95([Collections.Generic.List[double]]$Values) {
    if ($Values.Count -eq 0) { throw 'Cannot calculate a percentile from an empty sample.' }
    $ordered = $Values | Sort-Object
    return [double]$ordered[[Math]::Min(
        $ordered.Count - 1, [Math]::Ceiling($ordered.Count * 0.95) - 1)]
}

function Get-NativeAccessibility([IntPtr]$Handle, [string]$ExpectedRole, [string]$Label) {
    if ($Handle -eq [IntPtr]::Zero) {
        throw "$Label has no native HWND for MSAA role validation."
    }
    $probeOutput = & $AccessibilityProbe ('0x{0:X}' -f $Handle.ToInt64())
    if ($LASTEXITCODE -ne 0) {
        throw "$Label native accessibility probe failed with exit code ${LASTEXITCODE}: $probeOutput"
    }
    try {
        $probe = $probeOutput | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "$Label native accessibility probe returned invalid JSON: $probeOutput"
    }
    if (-not $probe.ok -or $probe.role -ne $ExpectedRole) {
        throw "$Label native MSAA role mismatch: expected $ExpectedRole, got $($probe.role)."
    }
    return [ordered]@{
        Hwnd = $probe.hwnd
        Role = [int]$probe.role
        Name = [string]$probe.name
    }
}

function Test-HwndBackedControlRole($Element, $ExpectedControlType, [string]$ExpectedRole,
        [string]$Label) {
    $uiaType = [string]$Element.Current.ControlType.ProgrammaticName
    $handle = [IntPtr]$Element.Current.NativeWindowHandle
    if ($Element.Current.ControlType -eq $ExpectedControlType) {
        return [ordered]@{ UiAutomationType = $uiaType; Native = $null }
    }
    if ($uiaType -ne 'ControlType.Pane') {
        throw "$Label UIA ControlType mismatch: expected $($ExpectedControlType.ProgrammaticName) or ControlType.Pane, got $uiaType."
    }
    return [ordered]@{
        UiAutomationType = $uiaType
        Native = Get-NativeAccessibility $handle $ExpectedRole $Label
    }
}

function Find-HwndBackedControl($Root, [string]$Name, $ExpectedControlType,
        [string]$ExpectedRole, [string]$Label) {
    $all = $Root.FindAll([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    foreach ($candidate in $all) {
        if ($candidate.Current.ControlType -eq $ExpectedControlType -and
            $candidate.Current.Name -eq $Name) {
            return [pscustomobject]@{ Element = $candidate; Native = $null }
        }
    }
    foreach ($candidate in $all) {
        if ([string]$candidate.Current.ControlType.ProgrammaticName -ne 'ControlType.Pane') {
            continue
        }
        $handle = [IntPtr]$candidate.Current.NativeWindowHandle
        if ($handle -eq [IntPtr]::Zero) { continue }
        try {
            $native = Get-NativeAccessibility $handle $ExpectedRole $Label
            return [pscustomobject]@{ Element = $candidate; Native = $native }
        } catch {
            if ($_.Exception.Message -notlike '*native MSAA role mismatch*') { throw }
        }
    }
    throw "Could not resolve the $Label HWND-backed control through UI Automation and MSAA."
}

function Find-AddressEdit($Root, [string]$ExpectedValue) {
    $all = $Root.FindAll([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    foreach ($element in $all) {
        try {
            $pattern = $element.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern)
            if ([string]::Equals($pattern.Current.Value, $ExpectedValue,
                    [StringComparison]::OrdinalIgnoreCase)) {
                return [pscustomobject]@{
                    Element = $element
                    Pattern = $pattern
                    Handle = [IntPtr]$element.Current.NativeWindowHandle
                    Native = $null
                }
            }
        } catch { }
    }
    $address = Find-HwndBackedControl $Root 'Address' ([Windows.Automation.ControlType]::Edit) `
        '42' 'Address edit'
    try {
        $pattern = $address.Element.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern)
        return [pscustomobject]@{
            Element = $address.Element
            Pattern = $pattern
            Handle = [IntPtr]$address.Element.Current.NativeWindowHandle
            Native = $address.Native
        }
    } catch {
        throw 'Address edit has the required native MSAA role but does not expose the required UIA ValuePattern.'
    }
}

function Invoke-MeasuredAddressNavigation(
        $Measured, $Address, [string]$Target, [string]$Marker,
        [Collections.Generic.List[double]]$InputLatencies) {
    $Address.Pattern.SetValue($Target)
    $navigationTimer = [Diagnostics.Stopwatch]::StartNew()
    $nativeResult = [UIntPtr]::Zero
    $sent = [FilesXpRuntimeNative]::SendMessageTimeout(
        $Address.Handle, 0x0100, [UIntPtr]([uint32]0x0d), [IntPtr]::Zero,
        2, 50, [ref]$nativeResult)
    if ($sent -eq [IntPtr]::Zero) { throw "Address navigation to $Target timed out on Enter." }

    $condition = [Windows.Automation.PropertyCondition]::new(
        [Windows.Automation.AutomationElement]::NameProperty, $Marker)
    $found = $null
    while ($navigationTimer.ElapsedMilliseconds -lt 5000 -and -not $found) {
        $probeTimer = [Diagnostics.Stopwatch]::StartNew()
        $probeResult = [UIntPtr]::Zero
        $responsive = [FilesXpRuntimeNative]::SendMessageTimeout(
            $Measured.Process.MainWindowHandle, 0, [UIntPtr]::Zero, [IntPtr]::Zero,
            2, 50, [ref]$probeResult)
        $probeTimer.Stop()
        if ($responsive -eq [IntPtr]::Zero) {
            throw "Window stopped responding during address navigation to $Target."
        }
        $InputLatencies.Add($probeTimer.Elapsed.TotalMilliseconds)
        $found = $Measured.Root.FindFirst(
            [Windows.Automation.TreeScope]::Descendants, $condition)
        if (-not $found) { Start-Sleep -Milliseconds 2 }
    }
    $navigationTimer.Stop()
    if (-not $found) { throw "Address navigation to $Target did not show $Marker within 5 seconds." }
    return [double]$navigationTimer.Elapsed.TotalMilliseconds
}

function Test-FlattenWorker([IntPtr]$OwnerWindow, [string]$ParentFolder) {
    $root = Join-Path $ParentFolder 'flatten-worker-probe'
    $first = Join-Path $root 'first'
    $second = Join-Path $root 'second\deep'
    New-Item -ItemType Directory -Path $first -Force | Out-Null
    New-Item -ItemType Directory -Path $second -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'top.txt'), 'top')
    [IO.File]::WriteAllText((Join-Path $first 'first.txt'), 'first')
    [IO.File]::WriteAllText((Join-Path $second 'second.txt'), 'second')
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-FlattenResult-' + [guid]::NewGuid() + '.txt')
    $eventName = 'Local\FilesXPNative-FlattenTest-' + [guid]::NewGuid()
    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    try {
        if (-not $created) { throw 'Could not create the flatten cancellation event.' }
        $arguments = @(
            '--flatten-worker', $OwnerWindow.ToInt64().ToString(), '900001',
            ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $root + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $cancel.Set() | Out-Null
            if (-not $worker.WaitForExit(5000)) { $worker.Kill() }
            throw 'Flatten worker did not finish within 30 seconds.'
        }
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
            throw 'Flatten worker did not write its bounded result.'
        }
        return $true
    }
    finally {
        $cancel.Dispose()
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

function Add-RequestUInt32([Collections.Generic.List[byte]]$Buffer, [uint32]$Value) {
    for ($shift = 0; $shift -lt 32; $shift += 8) {
        $Buffer.Add([byte](($Value -shr $shift) -band 0xff))
    }
}

function Add-RequestString([Collections.Generic.List[byte]]$Buffer, [string]$Value) {
    Add-RequestUInt32 $Buffer ([uint32]$Value.Length)
    $Buffer.AddRange([Text.Encoding]::Unicode.GetBytes($Value))
}

function Test-BulkRenameWorker([IntPtr]$OwnerWindow, [string]$ParentFolder) {
    $root = Join-Path $ParentFolder 'bulk-rename-worker-probe'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $first = Join-Path $root 'one.txt'
    $second = Join-Path $root 'two.txt'
    [IO.File]::WriteAllText($first, 'one')
    [IO.File]::WriteAllText($second, 'two')
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-RequestUInt32 $bytes 0x52425846
    Add-RequestUInt32 $bytes 1
    Add-RequestUInt32 $bytes 0
    Add-RequestUInt32 $bytes 2
    Add-RequestString $bytes 'Renamed'
    Add-RequestString $bytes $first
    Add-RequestString $bytes $second
    $total = [uint32]$bytes.Count
    for ($index = 0; $index -lt 4; $index++) {
        $bytes[8 + $index] = [byte](($total -shr ($index * 8)) -band 0xff)
    }
    $mappingName = 'Local\FilesXPNative-BulkRenameTest-' + [guid]::NewGuid()
    $eventName = 'Local\FilesXPNative-BulkRenameCancelTest-' + [guid]::NewGuid()
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-BulkRenameResult-' + [guid]::NewGuid() + '.txt')
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($mappingName, $bytes.Count)
    $accessor = $mapping.CreateViewAccessor()
    $payload = $bytes.ToArray()
    for ($index = 0; $index -lt $payload.Length; $index++) {
        $accessor.Write([long]$index, [byte]$payload[$index])
    }
    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    try {
        if (-not $created) { throw 'Could not create the bulk rename cancellation event.' }
        $arguments = @(
            '--bulk-rename-worker', $OwnerWindow.ToInt64().ToString(), '900002',
            ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $mappingName + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $cancel.Set() | Out-Null
            if (-not $worker.WaitForExit(5000)) { $worker.Kill() }
            throw 'Bulk rename worker did not finish within 30 seconds.'
        }
        if ($worker.ExitCode -ne 0) { throw "Bulk rename worker failed with exit code $($worker.ExitCode)." }
        $renamed = @(Get-ChildItem -LiteralPath $root -File)
        if ($renamed.Count -ne 2 -or
            @($renamed | Where-Object { $_.Name -notlike 'Renamed*.txt' }).Count -ne 0 -or
            (Test-Path -LiteralPath $first) -or (Test-Path -LiteralPath $second)) {
            throw 'Bulk rename worker did not produce two collision-safe target names.'
        }
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf) -or
            (Get-Item -LiteralPath $resultPath).Length -eq 0) {
            throw 'Bulk rename worker did not write its bounded result.'
        }
        return $true
    }
    finally {
        $cancel.Dispose()
        $accessor.Dispose()
        $mapping.Dispose()
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

function Test-FolderSelectionWorker([IntPtr]$OwnerWindow, [string]$ParentFolder) {
    $root = Join-Path $ParentFolder 'folder-selection-worker-probe'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $first = Join-Path $root 'one.txt'
    $second = Join-Path $root 'two.txt'
    [IO.File]::WriteAllText($first, 'one')
    [IO.File]::WriteAllText($second, 'two')
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-RequestUInt32 $bytes 0x53465846
    Add-RequestUInt32 $bytes 1
    Add-RequestUInt32 $bytes 0
    Add-RequestUInt32 $bytes 2
    Add-RequestString $bytes $root
    Add-RequestString $bytes 'Grouped'
    Add-RequestString $bytes $first
    Add-RequestString $bytes $second
    $total = [uint32]$bytes.Count
    for ($index = 0; $index -lt 4; $index++) {
        $bytes[8 + $index] = [byte](($total -shr ($index * 8)) -band 0xff)
    }
    $mappingName = 'Local\FilesXPNative-FolderSelectionTest-' + [guid]::NewGuid()
    $eventName = 'Local\FilesXPNative-FolderSelectionCancelTest-' + [guid]::NewGuid()
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-FolderSelectionResult-' + [guid]::NewGuid() + '.txt')
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($mappingName, $bytes.Count)
    $accessor = $mapping.CreateViewAccessor()
    $payload = $bytes.ToArray()
    for ($index = 0; $index -lt $payload.Length; $index++) {
        $accessor.Write([long]$index, [byte]$payload[$index])
    }
    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    try {
        if (-not $created) { throw 'Could not create the folder selection cancellation event.' }
        $arguments = @(
            '--folder-selection-worker', $OwnerWindow.ToInt64().ToString(), '900003',
            ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $mappingName + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $cancel.Set() | Out-Null
            if (-not $worker.WaitForExit(5000)) { $worker.Kill() }
            throw 'Folder selection worker did not finish within 30 seconds.'
        }
        if ($worker.ExitCode -ne 0) {
            throw "Folder selection worker failed with exit code $($worker.ExitCode)."
        }
        $grouped = Join-Path $root 'Grouped'
        if (-not (Test-Path -LiteralPath (Join-Path $grouped 'one.txt') -PathType Leaf) -or
            -not (Test-Path -LiteralPath (Join-Path $grouped 'two.txt') -PathType Leaf) -or
            (Test-Path -LiteralPath $first) -or (Test-Path -LiteralPath $second)) {
            throw 'Folder selection worker did not move both files into the new folder.'
        }
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf) -or
            (Get-Item -LiteralPath $resultPath).Length -eq 0) {
            throw 'Folder selection worker did not write its bounded result.'
        }
        return $true
    }
    finally {
        $cancel.Dispose()
        $accessor.Dispose()
        $mapping.Dispose()
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

function Test-TagSetWorker([IntPtr]$OwnerWindow, [string]$ParentFolder) {
    $root = Join-Path $ParentFolder 'tag-set-worker-probe'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $path = Join-Path $root 'tagged.txt'
    [IO.File]::WriteAllText($path, 'tagged')
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-RequestUInt32 $bytes 0x54525846
    Add-RequestUInt32 $bytes 1
    Add-RequestUInt32 $bytes 0
    Add-RequestUInt32 $bytes 1
    Add-RequestUInt32 $bytes 1
    Add-RequestString $bytes 'RuntimeTag'
    Add-RequestString $bytes $path
    $total = [uint32]$bytes.Count
    for ($index = 0; $index -lt 4; $index++) {
        $bytes[8 + $index] = [byte](($total -shr ($index * 8)) -band 0xff)
    }
    $mappingName = 'Local\FilesXPNative-TagSetTest-' + [guid]::NewGuid()
    $eventName = 'Local\FilesXPNative-TagSetCancelTest-' + [guid]::NewGuid()
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-TagSetResult-' + [guid]::NewGuid() + '.txt')
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($mappingName, $bytes.Count)
    $accessor = $mapping.CreateViewAccessor()
    $payload = $bytes.ToArray()
    for ($index = 0; $index -lt $payload.Length; $index++) {
        $accessor.Write([long]$index, [byte]$payload[$index])
    }
    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    $matchedKey = $null
    try {
        if (-not $created) { throw 'Could not create the tag-set cancellation event.' }
        $arguments = @(
            '--tag-set-worker', $OwnerWindow.ToInt64().ToString(), '900003',
            ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $mappingName + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $cancel.Set() | Out-Null
            if (-not $worker.WaitForExit(5000)) { $worker.Kill() }
            throw 'Tag-set worker did not finish within 30 seconds.'
        }
        if ($worker.ExitCode -ne 0) { throw "Tag-set worker failed with exit code $($worker.ExitCode)." }
        $tagRoot = 'HKCU:\Software\FilesXPNative\Tags'
        if (Test-Path -LiteralPath $tagRoot) {
            foreach ($candidate in Get-ChildItem -LiteralPath $tagRoot) {
                $value = Get-ItemProperty -LiteralPath $candidate.PSPath
                $pathProperty = $value.PSObject.Properties['Path']
                $tagsProperty = $value.PSObject.Properties['Tags']
                if ($pathProperty -and $pathProperty.Value -eq $path) {
                    $matchedKey = $candidate.PSPath
                    if (-not $tagsProperty -or @($tagsProperty.Value) -notcontains 'RuntimeTag') {
                        throw 'Tag-set worker sidecar did not contain the requested tag.'
                    }
                    break
                }
            }
        }
        if (-not $matchedKey) { throw 'Tag-set worker did not create a move-stable sidecar.' }
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf) -or
            (Get-Item -LiteralPath $resultPath).Length -eq 0) {
            throw 'Tag-set worker did not write its bounded result.'
        }
        return $true
    }
    finally {
        if ($matchedKey -and (Test-Path -LiteralPath $matchedKey)) {
            Remove-Item -LiteralPath $matchedKey -Recurse -Force
        }
        $cancel.Dispose()
        $accessor.Dispose()
        $mapping.Dispose()
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

function Invoke-ShellOperationWorker(
    [IntPtr]$OwnerWindow,
    [uint32]$Operation,
    [bool]$ConfirmPermanent,
    [string]$Destination,
    [string]$NewName,
    [string[]]$Items) {
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-RequestUInt32 $bytes 0x504f5846
    Add-RequestUInt32 $bytes 1
    Add-RequestUInt32 $bytes 0
    Add-RequestUInt32 $bytes $Operation
    Add-RequestUInt32 $bytes $(if ($ConfirmPermanent) { 1 } else { 0 })
    Add-RequestUInt32 $bytes ([uint32]$Items.Count)
    Add-RequestString $bytes $Destination
    Add-RequestString $bytes $NewName
    foreach ($item in $Items) { Add-RequestString $bytes $item }
    $total = [uint32]$bytes.Count
    for ($index = 0; $index -lt 4; $index++) {
        $bytes[8 + $index] = [byte](($total -shr ($index * 8)) -band 0xff)
    }
    $mappingName = 'Local\FilesXPNative-ShellOperationTest-' + [guid]::NewGuid()
    $eventName = 'Local\FilesXPNative-ShellOperationCancelTest-' + [guid]::NewGuid()
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-ShellOperationResult-' + [guid]::NewGuid() + '.txt')
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($mappingName, $bytes.Count)
    $accessor = $mapping.CreateViewAccessor()
    $payload = $bytes.ToArray()
    for ($index = 0; $index -lt $payload.Length; $index++) {
        $accessor.Write([long]$index, [byte]$payload[$index])
    }
    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    try {
        if (-not $created) { throw 'Could not create the Shell operation cancellation event.' }
        $generation = (910000 + [int]$Operation).ToString()
        $arguments = @(
            '--shell-operation-worker', $OwnerWindow.ToInt64().ToString(), $generation,
            ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $mappingName + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $cancel.Set() | Out-Null
            if (-not $worker.WaitForExit(5000)) { $worker.Kill() }
            throw "Shell operation $Operation did not finish within 30 seconds."
        }
        if ($worker.ExitCode -ne 0) {
            throw "Shell operation $Operation failed with exit code $($worker.ExitCode)."
        }
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf) -or
            (Get-Item -LiteralPath $resultPath).Length -eq 0) {
            throw "Shell operation $Operation did not write its bounded result."
        }
    }
    finally {
        $cancel.Dispose()
        $accessor.Dispose()
        $mapping.Dispose()
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

function Test-ShellOperationWorker([IntPtr]$OwnerWindow, [string]$ParentFolder) {
    $root = Join-Path $ParentFolder 'shell-operation-worker-probe'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    Invoke-ShellOperationWorker $OwnerWindow 1 $false $root 'Created' @()
    $created = Join-Path $root 'Created'
    if (-not (Test-Path -LiteralPath $created -PathType Container)) {
        throw 'Shell operation worker did not create its folder.'
    }
    Invoke-ShellOperationWorker $OwnerWindow 2 $false $root 'Worker document.txt' @()
    $source = Join-Path $root 'Worker document.txt'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw 'Shell operation worker did not create its file.'
    }
    $copyFolder = Join-Path $root 'Copy destination'
    $moveFolder = Join-Path $root 'Move destination'
    New-Item -ItemType Directory -Path $copyFolder, $moveFolder | Out-Null
    Invoke-ShellOperationWorker $OwnerWindow 5 $false $copyFolder '' @($source)
    $copied = Join-Path $copyFolder 'Worker document.txt'
    if (-not (Test-Path -LiteralPath $copied -PathType Leaf)) {
        throw 'Shell operation worker did not copy its file.'
    }
    Invoke-ShellOperationWorker $OwnerWindow 6 $false $moveFolder '' @($copied)
    $moved = Join-Path $moveFolder 'Worker document.txt'
    if (-not (Test-Path -LiteralPath $moved -PathType Leaf) -or
        (Test-Path -LiteralPath $copied)) {
        throw 'Shell operation worker did not move its file.'
    }
    Invoke-ShellOperationWorker $OwnerWindow 4 $false '' '' @($moved)
    if (Test-Path -LiteralPath $moved) {
        throw 'Shell operation worker did not permanently delete its file.'
    }
    return $true
}

function Test-ShellArtifactWorker([IntPtr]$OwnerWindow, [string]$ParentFolder) {
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-RequestUInt32 $bytes 0x54415846
    Add-RequestUInt32 $bytes 1
    Add-RequestUInt32 $bytes 0
    Add-RequestUInt32 $bytes 1
    Add-RequestString $bytes $ParentFolder
    Add-RequestString $bytes 'Runtime shortcut.lnk'
    Add-RequestString $bytes (Join-Path $env:SystemRoot 'System32\notepad.exe')
    Add-RequestString $bytes ''
    Add-RequestString $bytes $ParentFolder
    Add-RequestString $bytes ''
    $total = [uint32]$bytes.Count
    for ($index = 0; $index -lt 4; $index++) {
        $bytes[8 + $index] = [byte](($total -shr ($index * 8)) -band 0xff)
    }
    $mappingName = 'Local\FilesXPNative-ShellArtifactTest-' + [guid]::NewGuid()
    $eventName = 'Local\FilesXPNative-ShellArtifactCancelTest-' + [guid]::NewGuid()
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-ShellArtifactResult-' + [guid]::NewGuid() + '.txt')
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($mappingName, $bytes.Count)
    $accessor = $mapping.CreateViewAccessor()
    $payload = $bytes.ToArray()
    for ($index = 0; $index -lt $payload.Length; $index++) {
        $accessor.Write([long]$index, [byte]$payload[$index])
    }
    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    try {
        if (-not $created) { throw 'Could not create the Shell artifact cancellation event.' }
        $arguments = @(
            '--shell-artifact-worker', $OwnerWindow.ToInt64().ToString(), '930001',
            ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $mappingName + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $cancel.Set() | Out-Null
            if (-not $worker.WaitForExit(5000)) { $worker.Kill() }
            throw 'Shell artifact worker did not finish within 30 seconds.'
        }
        if ($worker.ExitCode -ne 0) {
            throw "Shell artifact worker failed with exit code $($worker.ExitCode)."
        }
        $shortcut = Join-Path $ParentFolder 'Runtime shortcut.lnk'
        if (-not (Test-Path -LiteralPath $shortcut -PathType Leaf) -or
            (Get-Item -LiteralPath $shortcut).Length -eq 0) {
            throw 'Shell artifact worker did not create a valid shortcut file.'
        }
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf) -or
            (Get-Item -LiteralPath $resultPath).Length -eq 0) {
            throw 'Shell artifact worker did not write its bounded result.'
        }
        return $true
    }
    finally {
        $cancel.Dispose()
        $accessor.Dispose()
        $mapping.Dispose()
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

function Test-TextPreviewWorker([IntPtr]$OwnerWindow, [string]$ParentFolder) {
    $inputPath = Join-Path $ParentFolder 'large-preview-probe.txt'
    [IO.File]::WriteAllText($inputPath, ('x' * (512 * 1024)), [Text.Encoding]::UTF8)
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-PreviewResult-' + [guid]::NewGuid() + '.txt')
    try {
        $arguments = @(
            '--preview-text', $OwnerWindow.ToInt64().ToString(), '920001',
            ('"' + $inputPath + '"'), ('"' + $resultPath + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $worker.Kill()
            throw 'Text preview worker did not finish within 30 seconds.'
        }
        if ($worker.ExitCode -ne 0) {
            throw "Text preview worker failed with exit code $($worker.ExitCode)."
        }
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
            throw 'Text preview worker did not produce a result.'
        }
        $maximumResultBytes = (256 * 1024 + 128) * 2
        if ((Get-Item -LiteralPath $resultPath).Length -gt $maximumResultBytes) {
            throw 'Text preview worker exceeded its UI-layout payload cap.'
        }
        $preview = [IO.File]::ReadAllText($resultPath, [Text.Encoding]::Unicode)
        if (-not $preview.Contains('Preview truncated at 256 KiB')) {
            throw 'Text preview worker did not mark its bounded viewport result.'
        }
        return $true
    }
    finally {
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

function Test-FallbackSearchWorker([IntPtr]$OwnerWindow, [string]$ParentFolder) {
    $root = Join-Path $ParentFolder 'fallback-search-worker-probe'
    $nested = Join-Path $root 'nested'
    New-Item -ItemType Directory -Path $nested -Force | Out-Null
    $first = Join-Path $root 'Quarterly Report.txt'
    $second = Join-Path $nested 'report-notes.md'
    $hidden = Join-Path $root 'hidden-report.txt'
    [IO.File]::WriteAllText($first, 'one')
    [IO.File]::WriteAllText($second, 'two')
    [IO.File]::WriteAllText($hidden, 'hidden')
    [IO.File]::SetAttributes($hidden, [IO.FileAttributes]::Hidden)
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-RequestUInt32 $bytes 0x53525846
    Add-RequestUInt32 $bytes 1
    Add-RequestUInt32 $bytes 0
    Add-RequestUInt32 $bytes 0
    Add-RequestString $bytes $root
    Add-RequestString $bytes 'report'
    $total = [uint32]$bytes.Count
    for ($index = 0; $index -lt 4; $index++) {
        $bytes[8 + $index] = [byte](($total -shr ($index * 8)) -band 0xff)
    }
    $mappingName = 'Local\FilesXPNative-SearchTest-' + [guid]::NewGuid()
    $eventName = 'Local\FilesXPNative-SearchCancelTest-' + [guid]::NewGuid()
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-SearchResult-' + [guid]::NewGuid() + '.txt')
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($mappingName, $bytes.Count)
    $accessor = $mapping.CreateViewAccessor()
    $payload = $bytes.ToArray()
    for ($index = 0; $index -lt $payload.Length; $index++) {
        $accessor.Write([long]$index, [byte]$payload[$index])
    }
    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    try {
        if (-not $created) { throw 'Could not create the fallback search cancellation event.' }
        $arguments = @(
            '--fallback-search', $OwnerWindow.ToInt64().ToString(), '940001',
            ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $mappingName + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $cancel.Set() | Out-Null
            if (-not $worker.WaitForExit(5000)) { $worker.Kill() }
            throw 'Fallback search worker did not finish within 30 seconds.'
        }
        if ($worker.ExitCode -ne 0) {
            throw "Fallback search worker failed with exit code $($worker.ExitCode)."
        }
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
            throw 'Fallback search worker did not produce a result.'
        }
        $resultBytes = [IO.File]::ReadAllBytes($resultPath)
        if ($resultBytes.Length -lt 12 -or
            [BitConverter]::ToUInt16($resultBytes, 0) -ne 0x4658 -or
            [BitConverter]::ToUInt16($resultBytes, 2) -ne 0x5254 -or
            [BitConverter]::ToUInt16($resultBytes, 4) -ne 1 -or
            [BitConverter]::ToUInt16($resultBytes, 6) -ne 2 -or
            [BitConverter]::ToUInt16($resultBytes, 8) -ne 0) {
            throw 'Fallback search worker wrote an invalid versioned result header.'
        }
        $result = [Text.Encoding]::Unicode.GetString($resultBytes)
        if (-not $result.Contains($first) -or -not $result.Contains($second) -or
            $result.Contains($hidden)) {
            throw 'Fallback search worker returned the wrong bounded result set.'
        }
        return $true
    }
    finally {
        $cancel.Dispose()
        $accessor.Dispose()
        $mapping.Dispose()
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

function Test-FtpCredentialWorker([IntPtr]$OwnerWindow) {
    $username = 'runtime-ftp-user'
    $password = 'RuntimeFtpSecret!'
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-RequestUInt32 $bytes 0x31465846
    $bytes.Add([byte]0)
    $bytes.Add([byte]1)
    $bytes.Add([byte]0)
    $bytes.Add([byte]0)
    Add-RequestUInt32 $bytes 0
    Add-RequestString $bytes 'ftp://127.0.0.1:1/'
    Add-RequestString $bytes $username
    Add-RequestString $bytes $password
    Add-RequestString $bytes ''
    Add-RequestString $bytes ''
    $total = [uint32]$bytes.Count
    for ($index = 0; $index -lt 4; $index++) {
        $bytes[8 + $index] = [byte](($total -shr ($index * 8)) -band 0xff)
    }
    $mappingName = 'Local\FilesXPNative-FtpCredentialTest-' + [guid]::NewGuid()
    $eventName = 'Local\FilesXPNative-FtpCredentialCancelTest-' + [guid]::NewGuid()
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) (
        'FilesXPNative-FtpCredentialResult-' + [guid]::NewGuid() + '.txt')
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew($mappingName, $bytes.Count)
    $accessor = $mapping.CreateViewAccessor()
    $payload = $bytes.ToArray()
    for ($index = 0; $index -lt $payload.Length; $index++) {
        $accessor.Write([long]$index, [byte]$payload[$index])
    }
    $created = $false
    $cancel = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName, [ref]$created)
    try {
        if (-not $created) { throw 'Could not create the FTP cancellation event.' }
        $arguments = @(
            '--ftp-worker', $OwnerWindow.ToInt64().ToString(), '950001',
            ('"' + $resultPath + '"'), ('"' + $eventName + '"'), ('"' + $mappingName + '"'))
        $worker = Start-Process -FilePath $Binary -ArgumentList $arguments -PassThru
        if (-not $worker.WaitForExit(30000)) {
            $cancel.Set() | Out-Null
            if (-not $worker.WaitForExit(5000)) { $worker.Kill() }
            throw 'FTP credential worker did not finish within 30 seconds.'
        }
        if ($worker.ExitCode -ne 4) {
            throw "FTP refusal probe returned unexpected exit code $($worker.ExitCode)."
        }
        for ($index = 0; $index -lt $payload.Length; $index++) {
            if ($accessor.ReadByte([long]$index) -ne 0) {
                throw 'FTP worker did not zero its pagefile-backed credential request.'
            }
        }
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
            throw 'FTP credential worker did not produce a bounded error result.'
        }
        $result = [IO.File]::ReadAllText($resultPath, [Text.Encoding]::UTF8)
        if ($result.Contains($username) -or $result.Contains($password)) {
            throw 'FTP worker disclosed credentials in its result output.'
        }
        return $true
    }
    finally {
        [Array]::Clear($payload, 0, $payload.Length)
        $cancel.Dispose()
        $accessor.Dispose()
        $mapping.Dispose()
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
    }
}

$testFolder = Join-Path ([IO.Path]::GetTempPath()) ("FilesXPNative-Runtime-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testFolder | Out-Null
[IO.File]::WriteAllText((Join-Path $testFolder 'viewport-probe.txt'), 'probe')
$navigationA = Join-Path $testFolder 'address-a'
$navigationB = Join-Path $testFolder 'address-b'
New-Item -ItemType Directory -Path $navigationA, $navigationB | Out-Null
[IO.File]::WriteAllText((Join-Path $navigationA 'address-a-marker.txt'), 'a')
[IO.File]::WriteAllText((Join-Path $navigationB 'address-b-marker.txt'), 'b')
for ($index = 0; $index -lt 256; $index++) {
    [IO.File]::WriteAllText((Join-Path $navigationA ("item-a-{0:D3}.txt" -f $index)), 'a')
    [IO.File]::WriteAllText((Join-Path $navigationB ("item-b-{0:D3}.txt" -f $index)), 'b')
}
$wprStarted = $false
$presentMon = $null
$measured = $null
try {
    $warmup = Start-MeasuredWindow $testFolder
    Close-TestWindow $warmup.Process

    if ($CaptureWpr) {
        $wpr = (Get-Command wpr.exe -ErrorAction Stop).Source
        & $wpr -start GeneralProfile -filemode
        if ($LASTEXITCODE -ne 0) { throw "WPR start failed with exit code $LASTEXITCODE." }
        $wprStarted = $true
    }
    $measured = Start-MeasuredWindow $testFolder
    if ($PresentMonPath) {
        $PresentMonPath = (Resolve-Path -LiteralPath $PresentMonPath).Path
        $presentCsv = Join-Path $OutputDirectory 'presentmon.csv'
        $quotedPresentCsv = '"' + $presentCsv + '"'
        $presentMon = Start-Process -FilePath $PresentMonPath -ArgumentList @(
            '--process_name', 'FilesXPNative.exe', '--timed', '5', '--terminate_after_timed',
            '--output_file', $quotedPresentCsv, '--no_console_stats') -PassThru
    }
    $viewportTimer = [Diagnostics.Stopwatch]::StartNew()
    $listItemCondition = [Windows.Automation.PropertyCondition]::new(
        [Windows.Automation.AutomationElement]::ControlTypeProperty,
        [Windows.Automation.ControlType]::ListItem)
    $firstItem = $null
    while ($viewportTimer.ElapsedMilliseconds -lt 5000 -and -not $firstItem) {
        $firstItem = $measured.Root.FindFirst(
            [Windows.Automation.TreeScope]::Descendants, $listItemCondition)
        if (-not $firstItem) { Start-Sleep -Milliseconds 5 }
    }
    if (-not $firstItem) { throw 'The first Shell viewport item did not appear within 5 seconds.' }

    $all = $measured.Root.FindAll(
        [Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    $counts = @{}
    $controlTypes = [ordered]@{
        Button = [Windows.Automation.ControlType]::Button
        Edit = [Windows.Automation.ControlType]::Edit
        List = [Windows.Automation.ControlType]::List
        Tab = [Windows.Automation.ControlType]::Tab
        TabItem = [Windows.Automation.ControlType]::TabItem
        StatusBar = [Windows.Automation.ControlType]::StatusBar
        ListItem = [Windows.Automation.ControlType]::ListItem
    }
    foreach ($typeName in $controlTypes.Keys) {
        $type = $controlTypes[$typeName]
        $count = 0
        $named = 0
        foreach ($element in $all) {
            if ($element.Current.ControlType -eq $type) {
                $count++
                if (-not [string]::IsNullOrWhiteSpace($element.Current.Name)) { $named++ }
            }
        }
        $counts[$typeName] = @{ Count = $count; Named = $named }
    }
    if ($counts['Button']['Count'] -lt 4 -or $counts['Button']['Named'] -lt 4 -or
        $counts['StatusBar']['Count'] -lt 1) {
        throw 'UI Automation surface is missing required controls or accessible names.'
    }

    $address = Find-AddressEdit $measured.Root $testFolder

    $nativeRoles = [ordered]@{}
    $nativeRoles['AddressEdit'] = Test-HwndBackedControlRole $address.Element `
        ([Windows.Automation.ControlType]::Edit) '42' 'Address edit'
    $placesList = Find-HwndBackedControl $measured.Root 'Places' `
        ([Windows.Automation.ControlType]::List) '33' 'Places list'
    $nativeRoles['PlacesList'] = Test-HwndBackedControlRole $placesList.Element `
        ([Windows.Automation.ControlType]::List) '33' 'Places list'
    $tabs = Find-HwndBackedControl $measured.Root 'Tabs' `
        ([Windows.Automation.ControlType]::Tab) '60' 'Tabs'
    $nativeRoles['Tabs'] = Test-HwndBackedControlRole $tabs.Element `
        ([Windows.Automation.ControlType]::Tab) '60' 'Tabs'

    $inputLatencies = [Collections.Generic.List[double]]::new()

    $navigationLatencies = [Collections.Generic.List[double]]::new()
    for ($iteration = 0; $iteration -lt 20; $iteration++) {
        if (($iteration % 2) -eq 0) {
            $navigationLatencies.Add((Invoke-MeasuredAddressNavigation `
                $measured $address $navigationA 'address-a-marker.txt' $inputLatencies))
        } else {
            $navigationLatencies.Add((Invoke-MeasuredAddressNavigation `
                $measured $address $navigationB 'address-b-marker.txt' $inputLatencies))
        }
    }

    $flattenWorkerPassed = Test-FlattenWorker $measured.Process.MainWindowHandle $testFolder
    $bulkRenameWorkerPassed = Test-BulkRenameWorker $measured.Process.MainWindowHandle $testFolder
    $folderSelectionWorkerPassed = Test-FolderSelectionWorker `
        $measured.Process.MainWindowHandle $testFolder
    $tagSetWorkerPassed = Test-TagSetWorker `
        $measured.Process.MainWindowHandle $testFolder
    $shellOperationWorkerPassed = Test-ShellOperationWorker `
        $measured.Process.MainWindowHandle $testFolder
    $shellArtifactWorkerPassed = Test-ShellArtifactWorker `
        $measured.Process.MainWindowHandle $testFolder
    $textPreviewWorkerPassed = Test-TextPreviewWorker `
        $measured.Process.MainWindowHandle $testFolder
    $fallbackSearchWorkerPassed = Test-FallbackSearchWorker `
        $measured.Process.MainWindowHandle $testFolder
    $ftpCredentialWorkerPassed = Test-FtpCredentialWorker `
        $measured.Process.MainWindowHandle

    for ($iteration = 0; $iteration -lt 100; $iteration++) {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $nativeResult = [UIntPtr]::Zero
        $sent = [FilesXpRuntimeNative]::SendMessageTimeout(
            $measured.Process.MainWindowHandle, 0, [UIntPtr]::Zero, [IntPtr]::Zero,
            2, 50, [ref]$nativeResult)
        $timer.Stop()
        if ($sent -eq [IntPtr]::Zero) { throw "Window responsiveness probe $iteration timed out." }
        $inputLatencies.Add($timer.Elapsed.TotalMilliseconds)
    }
    $navigationP95 = Get-P95 $navigationLatencies
    $inputP95 = Get-P95 $inputLatencies
    $results = [ordered]@{
        Binary = $Binary
        Timestamp = [DateTimeOffset]::Now.ToString('o')
        WarmLaunchMilliseconds = $measured.LaunchMilliseconds
        FirstViewportMilliseconds = [double]$viewportTimer.Elapsed.TotalMilliseconds
        AddressNavigationP95Milliseconds = $navigationP95
        InputP95Milliseconds = $inputP95
        UiAutomation = $counts
        HwndBackedControls = $nativeRoles
        FlattenWorker = $flattenWorkerPassed
        BulkRenameWorker = $bulkRenameWorkerPassed
        FolderSelectionWorker = $folderSelectionWorkerPassed
        TagSetWorker = $tagSetWorkerPassed
        ShellOperationWorker = $shellOperationWorkerPassed
        ShellArtifactWorker = $shellArtifactWorkerPassed
        TextPreviewWorker = $textPreviewWorkerPassed
        FallbackSearchWorker = $fallbackSearchWorkerPassed
        FtpCredentialWorker = $ftpCredentialWorkerPassed
        Authenticode = (Get-AuthenticodeSignature -FilePath $Binary).Status.ToString()
        WprTrace = $(if ($CaptureWpr) { Join-Path $OutputDirectory 'FilesXPNative.etl' } else { $null })
        PresentMonCsv = $(if ($PresentMonPath) { Join-Path $OutputDirectory 'presentmon.csv' } else { $null })
    }
    if ($results.WarmLaunchMilliseconds -gt 300) {
        throw "Warm launch exceeded 300 ms: $($results.WarmLaunchMilliseconds) ms."
    }
    if ($results.FirstViewportMilliseconds -gt 100) {
        throw "First viewport exceeded 100 ms: $($results.FirstViewportMilliseconds) ms."
    }
    if ($results.AddressNavigationP95Milliseconds -gt 100) {
        throw "Address navigation p95 exceeded 100 ms: $($results.AddressNavigationP95Milliseconds) ms."
    }
    if ($results.InputP95Milliseconds -gt 50) {
        throw "Input p95 exceeded 50 ms: $($results.InputP95Milliseconds) ms."
    }
    if ($presentMon) {
        $null = $presentMon.WaitForExit(15000)
        if (-not $presentMon.HasExited) { throw 'PresentMon did not finish its timed capture.' }
        if ($presentMon.ExitCode -ne 0) { throw "PresentMon failed with exit code $($presentMon.ExitCode)." }
        if (-not (Test-Path -LiteralPath $presentCsv -PathType Leaf) -or
            (Get-Item -LiteralPath $presentCsv).Length -eq 0) {
            throw 'PresentMon did not produce a non-empty CSV.'
        }
    }
    if ($wprStarted) {
        $wprTrace = Join-Path $OutputDirectory 'FilesXPNative.etl'
        & $wpr -stop $wprTrace 'Files XP Native runtime gate'
        if ($LASTEXITCODE -ne 0) { throw "WPR stop failed with exit code $LASTEXITCODE." }
        $wprStarted = $false
        if (-not (Test-Path -LiteralPath $wprTrace -PathType Leaf) -or
            (Get-Item -LiteralPath $wprTrace).Length -eq 0) {
            throw 'WPR did not produce a non-empty ETL.'
        }
    }
    $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
        Join-Path $OutputDirectory 'runtime-results.json') -Encoding UTF8
    Write-Host 'WINDOWS RUNTIME GATE: PASS'
    $results | Format-List
}
finally {
    if ($measured) { Close-TestWindow $measured.Process }
    if ($presentMon -and -not $presentMon.HasExited) { $presentMon.Kill() }
    if ($wprStarted) { & $wpr -cancel | Out-Null }
    if (Test-Path -LiteralPath $testFolder) { Remove-Item -LiteralPath $testFolder -Recurse -Force }
}
