param(
    [Parameter(Mandatory = $true)]
    [string]$DeviceType,
    [Parameter(Mandatory = $true)]
    [string]$InputDevice,
    [Parameter(Mandatory = $true)]
    [string]$OutputDevice,
    [ValidateSet(64, 128, 256)]
    [int[]]$BufferSizes = @(64, 128, 256),
    [ValidateRange(5, 1800)]
    [int]$SecondsPerRouteCount = 1800,
    [string]$PortableExecutable = "",
    [string]$InterfaceModel = "",
    [string]$DriverVersion = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$AppRoot = Split-Path -Parent $PSScriptRoot
$Engine = Join-Path $AppRoot "engine\win32-x64\werfeed-engine.exe"
$EvidenceRoot = Join-Path $AppRoot "windows-validation-output"
$SafeDeviceType = $DeviceType -replace '[^A-Za-z0-9_.-]', '_'
$SessionLog = Join-Path $EvidenceRoot "hardware-session-$SafeDeviceType.jsonl"
$Results = Join-Path $EvidenceRoot "hardware-session-$SafeDeviceType-results.txt"
$MatrixPath = Join-Path $EvidenceRoot "hardware-matrix-$SafeDeviceType.csv"
$MetadataPath = Join-Path $EvidenceRoot "hardware-session-$SafeDeviceType-metadata.json"
$EnumerationPath = Join-Path $EvidenceRoot "hardware-device-enumeration-$SafeDeviceType.jsonl"
$EndpointInventoryPath = Join-Path $EvidenceRoot "audio-endpoint-inventory-$SafeDeviceType.json"
$RouteStabilityPath = Join-Path $EvidenceRoot "route-stability-$SafeDeviceType.jsonl"
$RouteResetPath = Join-Path $EvidenceRoot "route-reset-restart-$SafeDeviceType.json"
$ValidationReportPath = Join-Path $EvidenceRoot "hardware-validation-$SafeDeviceType.txt"

if (-not (Test-Path $Engine)) {
    throw "Run .\scripts\windows-release.ps1 first; staged engine not found at $Engine"
}

New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue `
    $SessionLog, $Results, $MatrixPath, $MetadataPath, $EnumerationPath,
    $EndpointInventoryPath, $RouteStabilityPath, $RouteResetPath, $ValidationReportPath

Write-Host "SAFETY: Set output gain to minimum and keep a physical mute within reach."
$SafetyReady = Read-Host "Type READY when the test area is safe"
if ($SafetyReady -ne "READY") {
    throw "Hardware session cancelled because safety was not confirmed."
}

if (-not $PortableExecutable) {
    $PortableCandidate = Get-ChildItem (Join-Path $AppRoot "desktop-dist") `
        -Filter "Werfeed-Herzback-*-x64.exe" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($PortableCandidate) {
        $PortableExecutable = $PortableCandidate.FullName
    }
}
if (-not $PortableExecutable -or -not (Test-Path $PortableExecutable)) {
    throw "Portable Windows artifact was not found. Pass -PortableExecutable with the downloaded executable."
}

Write-Host "Launching portable artifact: $PortableExecutable"
$PortableProcess = Start-Process -FilePath $PortableExecutable -PassThru
Start-Sleep -Seconds 5
if ($PortableProcess.HasExited) {
    throw "Portable artifact exited during startup with code $($PortableProcess.ExitCode)."
}
$PortableStartup = Read-Host "Confirm the packaged app opened and displayed its control surface (yes/no)"
if ($PortableStartup -notmatch '^(?i:yes)$') {
    if (-not $PortableProcess.HasExited) { Stop-Process -Id $PortableProcess.Id -Force }
    throw "Portable artifact startup was not confirmed."
}

$Tester = Read-Host "Tester name or initials"
$RouteResetEvidence = [ordered]@{
    schemaVersion = 1
    test = "route-reset-restart"
    capturedAt = (Get-Date).ToString("o")
    portableExecutable = (Resolve-Path $PortableExecutable).Path
    tester = $Tester
    status = "incomplete"
}
$RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath

$ResetRoute = Read-Host "Enter the route number that will be reset (1-4)"
$RetainedRoute = Read-Host "Enter the other calibrated route number (1-4)"
if ($ResetRoute -notmatch '^[1-4]$' -or $RetainedRoute -notmatch '^[1-4]$' -or $ResetRoute -eq $RetainedRoute) {
    $RouteResetEvidence.status = "fail"
    $RouteResetEvidence.failure = "Reset and retained routes must be different route numbers from 1 through 4."
    $RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
    throw "Route reset evidence requires two different route numbers from 1 through 4."
}
$RouteResetEvidence.resetRoute = [int]$ResetRoute
$RouteResetEvidence.retainedRoute = [int]$RetainedRoute

$RouteResetEvidence.twoMappedRoutesBaselineSaved = (Read-Host `
    "In the packaged app, map and calibrate both routes. Do both route cards show baseline saved? (yes/no)") `
    -match '^(?i:yes)$'
$RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
if (-not $RouteResetEvidence.twoMappedRoutesBaselineSaved) {
    $RouteResetEvidence.status = "fail"
    $RouteResetEvidence.failure = "Both mapped routes did not show baseline saved before reset."
    $RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
    throw "Route reset evidence failed before the reset step. See $RouteResetPath"
}

$RouteResetEvidence.resetRouteNeedsCalibration = (Read-Host `
    "Reset only Route $ResetRoute. Does it show needs calibration? (yes/no)") `
    -match '^(?i:yes)$'
$RouteResetEvidence.retainedRouteRemainedCalibratedAfterReset = (Read-Host `
    "After that reset, does Route $RetainedRoute remain baseline saved? (yes/no)") `
    -match '^(?i:yes)$'
$RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
if (-not $RouteResetEvidence.resetRouteNeedsCalibration -or `
    -not $RouteResetEvidence.retainedRouteRemainedCalibratedAfterReset) {
    $RouteResetEvidence.status = "fail"
    $RouteResetEvidence.failure = "Reset did not leave only the selected route needing calibration."
    $RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
    throw "Route reset evidence failed after reset. See $RouteResetPath"
}

Write-Host "Close the packaged app now. The restart check requires the app process to exit before it is reopened."
[void](Read-Host "Press Enter after closing the packaged app")
Start-Sleep -Seconds 2
$PortableProcess.Refresh()
if (-not $PortableProcess.HasExited) {
    $RouteResetEvidence.status = "fail"
    $RouteResetEvidence.failure = "The packaged app was still running when the restart was requested."
    $RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
    throw "Packaged app did not exit before restart. Close it and rerun the session."
}
$RouteResetEvidence.appClosedBeforeRestart = $true
$RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath

Write-Host "Reopening portable artifact: $PortableExecutable"
$PortableRestartProcess = Start-Process -FilePath $PortableExecutable -PassThru
Start-Sleep -Seconds 5
if ($PortableRestartProcess.HasExited) {
    $RouteResetEvidence.status = "fail"
    $RouteResetEvidence.failure = "The packaged app exited during the restart check with code $($PortableRestartProcess.ExitCode)."
    $RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
    throw "Packaged artifact exited during restart with code $($PortableRestartProcess.ExitCode)."
}
$RouteResetEvidence.appRestarted = $true
$RouteResetEvidence.resetRouteNeedsCalibrationAfterRestart = (Read-Host `
    "After reopening, does Route $ResetRoute still show needs calibration? (yes/no)") `
    -match '^(?i:yes)$'
$RouteResetEvidence.retainedRouteRemainedCalibratedAfterRestart = (Read-Host `
    "After reopening, does Route $RetainedRoute still show baseline saved? (yes/no)") `
    -match '^(?i:yes)$'
$RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
if (-not $RouteResetEvidence.resetRouteNeedsCalibrationAfterRestart -or `
    -not $RouteResetEvidence.retainedRouteRemainedCalibratedAfterRestart) {
    $RouteResetEvidence.status = "fail"
    $RouteResetEvidence.failure = "Calibration state did not survive the packaged app restart as expected."
    $RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
    throw "Route reset restart evidence failed after reopening the app. See $RouteResetPath"
}

$RouteResetEvidence.resetRouteRecalibrated = (Read-Host `
    "Recalibrate Route $ResetRoute. Does it show baseline saved again? (yes/no)") `
    -match '^(?i:yes)$'
$RouteResetEvidence.retainedRouteUnchangedAfterRecalibration = (Read-Host `
    "After recalibration, does Route $RetainedRoute remain baseline saved and unchanged? (yes/no)") `
    -match '^(?i:yes)$'
$RouteResetEvidence.status = if ($RouteResetEvidence.resetRouteRecalibrated -and `
    $RouteResetEvidence.retainedRouteUnchangedAfterRecalibration) { "pass" } else { "fail" }
if ($RouteResetEvidence.status -ne "pass") {
    $RouteResetEvidence.failure = "The reset route was not restored without changing the retained route."
}
$RouteResetEvidence | ConvertTo-Json -Depth 5 | Set-Content $RouteResetPath
if ($RouteResetEvidence.status -ne "pass") {
    throw "Route reset restart evidence failed during recalibration. See $RouteResetPath"
}

Write-Host "Close the reopened packaged app, then press Enter to continue with the native hardware session."
[void](Read-Host)
if (-not $PortableRestartProcess.HasExited) {
    Stop-Process -Id $PortableRestartProcess.Id -Force
}

if (-not $InterfaceModel) {
    $InterfaceModel = Read-Host "Interface model"
}
if (-not $DriverVersion) {
    $DriverVersion = Read-Host "Driver version"
}
$Tester = Read-Host "Tester name or initials"
$Metadata = [ordered]@{
    capturedAt = (Get-Date).ToString("o")
    deviceType = $DeviceType
    inputDevice = $InputDevice
    outputDevice = $OutputDevice
    interfaceModel = $InterfaceModel
    driverVersion = $DriverVersion
    sampleRateRequested = 48000
    bufferSizes = $BufferSizes
    secondsPerRouteCount = $SecondsPerRouteCount
    portableExecutable = (Resolve-Path $PortableExecutable).Path
    portableStartupConfirmed = $true
    routeResetRestartConfirmed = $true
    routeResetEvidence = (Split-Path -Leaf $RouteResetPath)
    tester = $Tester
    endpointInventory = (Split-Path -Leaf $EndpointInventoryPath)
}
$Metadata | ConvertTo-Json -Depth 5 | Set-Content $MetadataPath

Write-Host "Capturing the Windows audio endpoint inventory used to exercise filtering..."
$EndpointInventory = @()
try {
    $EndpointInventory = @(
        Get-PnpDevice -Class AudioEndpoint -ErrorAction Stop |
            ForEach-Object {
                [pscustomobject][ordered]@{
                    friendlyName = [string]$_.FriendlyName
                    instanceId = [string]$_.InstanceId
                    status = [string]$_.Status
                    class = [string]$_.Class
                }
            }
    )
} catch {
    try {
        $EndpointInventory = @(
            Get-CimInstance Win32_SoundDevice -ErrorAction Stop |
                ForEach-Object {
                    [pscustomobject][ordered]@{
                        friendlyName = [string]$_.Name
                        instanceId = [string]$_.PNPDeviceID
                        status = [string]$_.Status
                        class = "Win32_SoundDevice"
                    }
                }
        )
    } catch {
        throw "Unable to capture Windows audio endpoint inventory: $($_.Exception.Message)"
    }
}
if ($EndpointInventory.Count -eq 0) {
    throw "Windows reported no audio endpoints; USB and built-in/virtual filtering cannot be confirmed."
}
$EndpointInventory | ConvertTo-Json -Depth 5 | Set-Content $EndpointInventoryPath

$EnumerationStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
$EnumerationStartInfo.FileName = $Engine
$EnumerationStartInfo.UseShellExecute = $false
$EnumerationStartInfo.CreateNoWindow = $true
$EnumerationStartInfo.RedirectStandardInput = $true
$EnumerationStartInfo.RedirectStandardOutput = $true
$EnumerationStartInfo.RedirectStandardError = $true
$EnumerationProcess = [System.Diagnostics.Process]::new()
$EnumerationProcess.StartInfo = $EnumerationStartInfo
if (-not $EnumerationProcess.Start()) {
    throw "Unable to start the native engine for device enumeration."
}
$EnumerationStdoutTask = $EnumerationProcess.StandardOutput.ReadToEndAsync()
$EnumerationStderrTask = $EnumerationProcess.StandardError.ReadToEndAsync()
$EnumerationProcess.StandardInput.WriteLine('{"type":"list_devices"}')
$EnumerationProcess.StandardInput.Close()
if (-not $EnumerationProcess.WaitForExit(10000)) {
    $EnumerationProcess.Kill()
    throw "Native engine did not exit after device enumeration."
}
$EnumerationStdout = $EnumerationStdoutTask.Result
$EnumerationStderr = $EnumerationStderrTask.Result
Set-Content $EnumerationPath $EnumerationStdout.TrimEnd()
if ($EnumerationStderr) {
    Add-Content $EnumerationPath "# stderr: $($EnumerationStderr.TrimEnd())"
}
$EnumerationEvents = @(
    $EnumerationStdout -split '\r?\n' |
        Where-Object { $_.Trim() } |
        ForEach-Object {
            try { $_ | ConvertFrom-Json } catch { $null }
        } |
        Where-Object { $_ -ne $null }
)
$DeviceEvent = $EnumerationEvents | Where-Object { $_.type -eq "devices" } | Select-Object -Last 1
if (-not $DeviceEvent) {
    throw "The native engine did not emit a WASAPI devices event. See $EnumerationPath"
}
$MatchingInput = @($DeviceEvent.devices | Where-Object {
    $_.direction -eq "input" -and $_.name -eq $InputDevice
})
$MatchingOutput = @($DeviceEvent.devices | Where-Object {
    $_.direction -eq "output" -and $_.name -eq $OutputDevice
})
if ($MatchingInput.Count -eq 0 -or $MatchingOutput.Count -eq 0) {
    throw "The requested input/output device was not present in the captured WASAPI enumeration. See $EnumerationPath"
}

@(
    "Hardware session: $($Metadata.capturedAt)"
    "Interface: $InterfaceModel"
    "Driver version: $DriverVersion"
    "WASAPI mode: $DeviceType"
    "Input: $InputDevice"
    "Output: $OutputDevice"
    "Requested sample rate: 48000 Hz"
    "Requested buffers: $($BufferSizes -join ', ') samples"
    "Tester: $Tester"
    "Portable startup: PASS"
    "Route reset/restart: PASS ($RouteResetPath)"
    "Device enumeration: PASS ($EnumerationPath)"
    "Audio endpoint inventory: $EndpointInventoryPath"
    ""
) | Set-Content $Results

$SummaryRows = @()
foreach ($BufferSize in $BufferSizes) {
  for ($RouteCount = 1; $RouteCount -le 8; $RouteCount++) {
    $Routes = @()
    for ($Channel = 0; $Channel -lt $RouteCount; $Channel++) {
        $Routes += @{ input = $Channel; output = $Channel }
    }

    $Configure = @{
        type = "configure"
        deviceType = $DeviceType
        inputDevice = $InputDevice
        outputDevice = $OutputDevice
        sampleRate = 48000
        bufferSize = $BufferSize
        inputChannels = $RouteCount
        outputChannels = $RouteCount
        routes = $Routes
    } | ConvertTo-Json -Compress -Depth 5

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $Engine
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardInput = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true

    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    if (-not $Process.Start()) {
        throw "Unable to start the native engine."
    }

    $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
    $StderrTask = $Process.StandardError.ReadToEndAsync()
    $Process.StandardInput.WriteLine($Configure)
    $Process.StandardInput.WriteLine('{"type":"start"}')
    $Process.StandardInput.Flush()

    Write-Host "Testing $RouteCount mono route(s) at $BufferSize samples for $SecondsPerRouteCount seconds..."
    Start-Sleep -Seconds $SecondsPerRouteCount
    $Process.StandardInput.WriteLine('{"type":"stop"}')
    $Process.StandardInput.Close()

    if (-not $Process.WaitForExit(10000)) {
        $Process.Kill()
        throw "Native engine did not exit after route test $RouteCount."
    }

    $Stdout = $StdoutTask.Result
    $Stderr = $StderrTask.Result
    Add-Content $SessionLog "# buffer-size=$BufferSize route-count=$RouteCount"
    Add-Content $SessionLog $Stdout.TrimEnd()
    if ($Stderr) {
        Add-Content $SessionLog "# stderr: $($Stderr.TrimEnd())"
    }

    $Events = @(
        $Stdout -split '\r?\n' |
            Where-Object { $_.Trim() } |
            ForEach-Object {
                try { $_ | ConvertFrom-Json } catch { $null }
            } |
            Where-Object { $_ -ne $null }
    )
    $Telemetry = @($Events | Where-Object { $_.type -eq "telemetry" })
    $Errors = @($Events | Where-Object { $_.type -eq "error" })
    $LastTelemetry = $Telemetry | Select-Object -Last 1
    $MaximumCpu = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property callbackCpu -Maximum).Maximum
    } else { $null }
    $MaximumXruns = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property xruns -Maximum).Maximum
    } else { $null }
    $ActualRate = if ($LastTelemetry) { $LastTelemetry.sampleRate } else { $null }
    $ActualBuffer = if ($LastTelemetry) { $LastTelemetry.bufferSize } else { $null }
    $EngineResult = if ($Errors.Count -gt 0) { "ENGINE ERROR" } else { "PASS" }
    if ($Errors.Count -gt 0) {
        Add-Content $Results "$BufferSize samples / $RouteCount routes: ENGINE ERROR"
        Write-Warning "The engine reported an error. See $SessionLog"
    }

    $AudibleResult = Read-Host "Did all $RouteCount route(s) at $BufferSize samples pass clean audio on matching outputs? (yes/no)"
    $AudiblePass = $AudibleResult -match '^(?i:yes)$'
    if ($Errors.Count -eq 0) {
        $EngineResult = if ($AudiblePass) { "PASS" } else { "AUDIBLE FAIL" }
        Add-Content $Results "$BufferSize samples / $RouteCount routes: $EngineResult"
    }
    $SummaryRow = [pscustomobject][ordered]@{
        bufferSizeRequested = $BufferSize
        routeCount = $RouteCount
        sampleRate = $ActualRate
        bufferSizeActual = $ActualBuffer
        callbackCpuMaximum = $MaximumCpu
        xrunsMaximum = $MaximumXruns
        engineErrors = $Errors.Count
        audiblePass = $AudiblePass
        result = $EngineResult
    }
    $SummaryRows += $SummaryRow
  }
}

$SummaryRows | Export-Csv -Path $MatrixPath -NoTypeInformation

# Exercise multiple mono routes on the same exact device pair with disabled
# slots between enabled slots. The native telemetry must preserve those slot
# indices rather than compacting the route list.
$StabilityConfigure = @{
    type = "configure"
    deviceType = $DeviceType
    inputDevice = $InputDevice
    outputDevice = $OutputDevice
    sampleRate = 48000
    bufferSize = 128
    inputChannels = 2
    outputChannels = 2
    routes = @(
        @{ input = 0; output = 0; enabled = $true; suppression = 0.75 }
        @{ input = -1; output = -1; enabled = $false; suppression = 0.75 }
        @{ input = 1; output = 1; enabled = $true; suppression = 0.75 }
        @{ input = -1; output = -1; enabled = $false; suppression = 0.75 }
    )
} | ConvertTo-Json -Compress -Depth 5
$StabilityStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
$StabilityStartInfo.FileName = $Engine
$StabilityStartInfo.UseShellExecute = $false
$StabilityStartInfo.CreateNoWindow = $true
$StabilityStartInfo.RedirectStandardInput = $true
$StabilityStartInfo.RedirectStandardOutput = $true
$StabilityStartInfo.RedirectStandardError = $true
$StabilityProcess = [System.Diagnostics.Process]::new()
$StabilityProcess.StartInfo = $StabilityStartInfo
if (-not $StabilityProcess.Start()) {
    throw "Unable to start the native engine for route stability validation."
}
$StabilityStdoutTask = $StabilityProcess.StandardOutput.ReadToEndAsync()
$StabilityStderrTask = $StabilityProcess.StandardError.ReadToEndAsync()
$StabilityProcess.StandardInput.WriteLine($StabilityConfigure)
$StabilityProcess.StandardInput.WriteLine('{"type":"start"}')
$StabilityProcess.StandardInput.Flush()
Start-Sleep -Seconds 3
$StabilityProcess.StandardInput.WriteLine('{"type":"stop"}')
$StabilityProcess.StandardInput.Close()
if (-not $StabilityProcess.WaitForExit(10000)) {
    $StabilityProcess.Kill()
    throw "Native engine did not exit after route stability validation."
}
$StabilityStdout = $StabilityStdoutTask.Result
$StabilityStderr = $StabilityStderrTask.Result
Set-Content $RouteStabilityPath $StabilityStdout.TrimEnd()
if ($StabilityStderr) {
    Add-Content $RouteStabilityPath "# stderr: $($StabilityStderr.TrimEnd())"
}
$StabilityEvents = @(
    $StabilityStdout -split '\r?\n' |
        Where-Object { $_.Trim() } |
        ForEach-Object {
            try { $_ | ConvertFrom-Json } catch { $null }
        } |
        Where-Object { $_ -ne $null }
)
$StabilityTelemetry = @($StabilityEvents | Where-Object { $_.type -eq "telemetry" })
if ($StabilityTelemetry.Count -eq 0) {
    throw "Route stability validation produced no telemetry. See $RouteStabilityPath"
}
$ExpectedRouteStates = @{
    1 = $true
    2 = $false
    3 = $true
    4 = $false
}
foreach ($TelemetryEvent in $StabilityTelemetry) {
    $RouteTelemetry = @($TelemetryEvent.routeTelemetry)
    if ($RouteTelemetry.Count -ne 4) {
        throw "Route stability telemetry did not contain exactly four route slots. See $RouteStabilityPath"
    }
    foreach ($ExpectedRoute in $ExpectedRouteStates.Keys) {
        $ObservedRoute = $RouteTelemetry | Where-Object { [int]$_.route -eq $ExpectedRoute } | Select-Object -First 1
        if ($null -eq $ObservedRoute -or [bool]$ObservedRoute.enabled -ne $ExpectedRouteStates[$ExpectedRoute]) {
            throw "Route $ExpectedRoute changed enabled state during stability validation. See $RouteStabilityPath"
        }
    }
}
Add-Content $Results "Route stability: PASS ($RouteStabilityPath)"

$Delay = Read-Host "Measured round-trip delay in milliseconds (enter N/A if calibration was not run)"

$DisconnectConfigure = @{
    type = "configure"
    deviceType = $DeviceType
    inputDevice = $InputDevice
    outputDevice = $OutputDevice
    sampleRate = 48000
    bufferSize = 128
    inputChannels = 1
    outputChannels = 1
    routes = @(@{ input = 0; output = 0 })
} | ConvertTo-Json -Compress -Depth 5
$DisconnectStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
$DisconnectStartInfo.FileName = $Engine
$DisconnectStartInfo.UseShellExecute = $false
$DisconnectStartInfo.CreateNoWindow = $true
$DisconnectStartInfo.RedirectStandardInput = $true
$DisconnectStartInfo.RedirectStandardOutput = $true
$DisconnectStartInfo.RedirectStandardError = $true
$DisconnectProcess = [System.Diagnostics.Process]::new()
$DisconnectProcess.StartInfo = $DisconnectStartInfo
if (-not $DisconnectProcess.Start()) {
    throw "Unable to start the native engine for the disconnect test."
}
$DisconnectStdoutTask = $DisconnectProcess.StandardOutput.ReadToEndAsync()
$DisconnectStderrTask = $DisconnectProcess.StandardError.ReadToEndAsync()
$DisconnectProcess.StandardInput.WriteLine($DisconnectConfigure)
$DisconnectProcess.StandardInput.WriteLine('{"type":"start"}')
$DisconnectProcess.StandardInput.Flush()
Start-Sleep -Seconds 2
Write-Host "Disconnect test: physically disconnect the active input or output device now."
[void](Read-Host "Press Enter after the device is disconnected")
Start-Sleep -Seconds 10
$DisconnectProcess.StandardInput.WriteLine('{"type":"stop"}')
$DisconnectProcess.StandardInput.Close()
if (-not $DisconnectProcess.WaitForExit(10000)) {
    $DisconnectProcess.Kill()
    throw "Native engine did not exit after the disconnect test."
}
$DisconnectStdout = $DisconnectStdoutTask.Result
$DisconnectStderr = $DisconnectStderrTask.Result
Add-Content $SessionLog "# disconnect-test"
Add-Content $SessionLog $DisconnectStdout.TrimEnd()
if ($DisconnectStderr) {
    Add-Content $SessionLog "# disconnect stderr: $($DisconnectStderr.TrimEnd())"
}
$DisconnectEvents = @(
    $DisconnectStdout -split '\r?\n' |
        Where-Object { $_.Trim() } |
        ForEach-Object {
            try { $_ | ConvertFrom-Json } catch { $null }
        } |
        Where-Object { $_ -ne $null }
)
$DisconnectObserved = @($DisconnectEvents | Where-Object {
    $_.type -eq "state" -and $_.phase -eq "device_stopped"
}).Count -gt 0
$Disconnect = if ($DisconnectObserved) {
    "PASS (engine emitted device_stopped)"
} else {
    "NOT OBSERVED"
}
Add-Content $Results "Disconnect event observed: $Disconnect"
$DisconnectTesterResult = Read-Host "Did routing stop without selecting another output? (yes/no/N/A)"
$Notes = Read-Host "Tester notes (use a separate file for additional detail)"
@(
    ""
    "Measured delay: $Delay ms"
    "Disconnect behavior: $DisconnectTesterResult"
    "Tester notes: $Notes"
    ""
    "Detailed telemetry: $SessionLog"
    "Matrix summary: $MatrixPath"
) | Add-Content $Results

Write-Host "Running automated hardware evidence validation..."
$Validator = Join-Path $PSScriptRoot "windows-hardware-validation.ps1"
& $Validator `
    -DeviceType $DeviceType `
    -InputDevice $InputDevice `
    -OutputDevice $OutputDevice `
    -BufferSizes $BufferSizes `
    -ExpectedSampleRate 48000 `
    -RequireUsbInterface `
    -EvidenceRoot $EvidenceRoot

Write-Host "Hardware route session complete."
Write-Host "Evidence written to $EvidenceRoot"
