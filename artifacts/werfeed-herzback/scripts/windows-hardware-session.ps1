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
$ValidationReportPath = Join-Path $EvidenceRoot "hardware-validation-$SafeDeviceType.txt"

if (-not (Test-Path $Engine)) {
    throw "Run .\scripts\windows-release.ps1 first; staged engine not found at $Engine"
}

New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue `
    $SessionLog, $Results, $MatrixPath, $MetadataPath, $EnumerationPath, $ValidationReportPath

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
Write-Host "Close the portable app, then press Enter to continue with the native hardware session."
[void](Read-Host)
if (-not $PortableProcess.HasExited) {
    Stop-Process -Id $PortableProcess.Id -Force
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
    tester = $Tester
}
$Metadata | ConvertTo-Json -Depth 5 | Set-Content $MetadataPath

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
    "Device enumeration: PASS ($EnumerationPath)"
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
    -EvidenceRoot $EvidenceRoot

Write-Host "Hardware route session complete."
Write-Host "Evidence written to $EvidenceRoot"
