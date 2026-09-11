param(
    [Parameter(Mandatory = $true)]
    [string]$DeviceType,
    [Parameter(Mandatory = $true)]
    [string]$InputDevice,
    [Parameter(Mandatory = $true)]
    [string]$OutputDevice,
    [ValidateSet(64, 128, 256)]
    [int]$BufferSize = 128,
    [ValidateRange(5, 1800)]
    [int]$SecondsPerPhase = 600,
    [string]$Engine = "",
    [string]$InterfaceModel = "",
    [string]$DriverVersion = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$AppRoot = Split-Path -Parent $PSScriptRoot
if (-not $Engine) {
    $Engine = Join-Path $AppRoot "engine\win32-x64\werfeed-engine.exe"
}
$EvidenceRoot = Join-Path $AppRoot "windows-validation-output"
$SafeDeviceType = $DeviceType -replace '[^A-Za-z0-9_.-]', '_'
$SessionLog = Join-Path $EvidenceRoot "acoustic-stimulus-$SafeDeviceType.jsonl"
$Results = Join-Path $EvidenceRoot "acoustic-stimulus-$SafeDeviceType-results.txt"
$MetadataPath = Join-Path $EvidenceRoot "acoustic-stimulus-$SafeDeviceType-metadata.json"

if (-not (Test-Path $Engine)) {
    throw "Run .\scripts\windows-release.ps1 first; staged engine not found at $Engine"
}

New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $SessionLog, $Results, $MetadataPath

Write-Host "SAFETY: Put a physical mute or amplifier control within reach."
Write-Host "SAFETY: Start with amplifier/output gain at minimum and keep listeners away from the loudspeaker."
$SafetyReady = Read-Host "Type READY when the test area is safe"
if ($SafetyReady -ne "READY") {
    throw "Acoustic session cancelled because safety was not confirmed."
}

if (-not $InterfaceModel) { $InterfaceModel = Read-Host "Interface model" }
if (-not $DriverVersion) { $DriverVersion = Read-Host "Driver version" }
$Tester = Read-Host "Tester name or initials"
$GainStructure = Read-Host "Gain structure (interface gain, output gain, normalized operating level)"
$RoomSetup = Read-Host "Room and loudspeaker/microphone setup"
$PhaseNames = @("program-only", "one-feedback-tone", "two-feedback-tones", "feedback-removed")

$Metadata = [ordered]@{
    capturedAt = (Get-Date).ToString("o")
    deviceType = $DeviceType
    inputDevice = $InputDevice
    outputDevice = $OutputDevice
    interfaceModel = $InterfaceModel
    driverVersion = $DriverVersion
    sampleRateRequested = 48000
    bufferSizeRequested = $BufferSize
    secondsPerPhase = $SecondsPerPhase
    tester = $Tester
    gainStructure = $GainStructure
    roomSetup = $RoomSetup
    phaseOrder = $PhaseNames
    safetyLevelLimit = 0.08
}
$Metadata | ConvertTo-Json -Depth 5 | Set-Content $MetadataPath

@(
    "Acoustic stimulus session: $($Metadata.capturedAt)"
    "Interface: $InterfaceModel"
    "Driver version: $DriverVersion"
    "WASAPI mode: $DeviceType"
    "Input: $InputDevice"
    "Output: $OutputDevice"
    "Requested sample rate: 48000 Hz"
    "Requested buffer: $BufferSize samples"
    "Seconds per phase: $SecondsPerPhase"
    "Tester: $Tester"
    "Gain structure: $GainStructure"
    "Room/setup: $RoomSetup"
    ""
) | Set-Content $Results

function Convert-EngineEvents([string]$Stdout) {
    @(
        $Stdout -split '\r?\n' |
            Where-Object { $_.Trim() } |
            ForEach-Object {
                try { $_ | ConvertFrom-Json } catch { $null }
            } |
            Where-Object { $_ -ne $null }
    )
}

function Start-AcousticRun([string]$Preset) {
    $Configure = @{
        type = "configure"
        deviceType = $DeviceType
        inputDevice = $InputDevice
        outputDevice = $OutputDevice
        sampleRate = 48000
        bufferSize = $BufferSize
        inputChannels = 1
        outputChannels = 1
        routes = @(@{ input = 0; output = 0 })
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
    if (-not $Process.Start()) { throw "Unable to start the native engine for the $Preset run." }

    $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
    $StderrTask = $Process.StandardError.ReadToEndAsync()
    $Process.StandardInput.WriteLine($Configure)
    $Process.StandardInput.WriteLine('{"type":"start"}')
    $Process.StandardInput.WriteLine((@{
        type = "set_protection"
        enabled = $true
        preset = $Preset
    } | ConvertTo-Json -Compress))
    $Process.StandardInput.Flush()

    Write-Host ""
    Write-Host "[$Preset] Start representative $Preset program material at the normal test level."
    [void](Read-Host "Press Enter to begin the program-only phase")
    $Process.StandardInput.WriteLine((@{ type = "test_marker"; name = "${Preset}:program-only" } | ConvertTo-Json -Compress))
    $Process.StandardInput.Flush()
    Start-Sleep -Seconds $SecondsPerPhase

    Write-Host "[$Preset] Introduce ONE controlled feedback tone and hold it stable."
    [void](Read-Host "Press Enter when the one-tone stimulus is present")
    $Process.StandardInput.WriteLine((@{ type = "test_marker"; name = "${Preset}:one-feedback-tone" } | ConvertTo-Json -Compress))
    $Process.StandardInput.Flush()
    Start-Sleep -Seconds $SecondsPerPhase

    Write-Host "[$Preset] Add a SECOND simultaneous feedback tone at a different frequency."
    [void](Read-Host "Press Enter when both tones are present")
    $Process.StandardInput.WriteLine((@{ type = "test_marker"; name = "${Preset}:two-feedback-tones" } | ConvertTo-Json -Compress))
    $Process.StandardInput.Flush()
    Start-Sleep -Seconds $SecondsPerPhase

    Write-Host "[$Preset] Remove both feedback sources while continuing program material."
    [void](Read-Host "Press Enter when both tones have been removed")
    $Process.StandardInput.WriteLine((@{ type = "test_marker"; name = "${Preset}:feedback-removed" } | ConvertTo-Json -Compress))
    $Process.StandardInput.Flush()
    Start-Sleep -Seconds $SecondsPerPhase

    $Process.StandardInput.WriteLine('{"type":"stop"}')
    $Process.StandardInput.Close()
    if (-not $Process.WaitForExit(10000)) {
        $Process.Kill()
        throw "Native engine did not exit after the $Preset acoustic run."
    }
    [pscustomobject]@{
        preset = $Preset
        stdout = $StdoutTask.Result
        stderr = $StderrTask.Result
    }
}

$RunSummaries = @()
foreach ($Preset in @("speech", "music")) {
    $Run = Start-AcousticRun $Preset
    Add-Content $SessionLog "# preset=$Preset phases=$($PhaseNames -join ',')"
    Add-Content $SessionLog $Run.stdout.TrimEnd()
    if ($Run.stderr) { Add-Content $SessionLog "# stderr: $($Run.stderr.TrimEnd())" }

    $Events = @(Convert-EngineEvents $Run.stdout)
    $Telemetry = @($Events | Where-Object { $_.type -eq "telemetry" })
    $Errors = @($Events | Where-Object { $_.type -eq "error" })
    $LastTelemetry = $Telemetry | Select-Object -Last 1
    $MaximumCpu = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property callbackCpu -Maximum).Maximum
    } else { $null }
    $MaximumXruns = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property xruns -Maximum).Maximum
    } else { $null }
    $MaximumDriverXruns = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property driverXruns -Maximum).Maximum
    } else { $null }
    $MaximumDeadlineMisses = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property callbackDeadlineMisses -Maximum).Maximum
    } else { $null }
    $MaximumExecutionMs = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property callbackExecutionMs -Maximum).Maximum
    } else { $null }
    $MaximumExecutionPeakMs = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property callbackExecutionPeakMs -Maximum).Maximum
    } else { $null }
    $MaximumJitterMs = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property callbackJitterMs -Maximum).Maximum
    } else { $null }
    $MaximumJitterPeakMs = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property callbackJitterPeakMs -Maximum).Maximum
    } else { $null }
    $MaximumNonFiniteInput = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property nonFiniteInputSamples -Maximum).Maximum
    } else { $null }
    $MaximumNonFiniteOutput = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property nonFiniteOutputSamples -Maximum).Maximum
    } else { $null }
    $MaximumActiveNotches = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property activeNotches -Maximum).Maximum
    } else { $null }
    $DeepestCut = if ($Telemetry.Count -gt 0) {
        ($Telemetry | Measure-Object -Property maximumCutDb -Minimum).Minimum
    } else { $null }
    $MarkerEvents = @($Events | Where-Object { $_.type -eq "test_marker" })
    $ReleaseMarker = $MarkerEvents | Where-Object { $_.name -eq "${Preset}:feedback-removed" } | Select-Object -Last 1
    $ReleaseTelemetry = if ($ReleaseMarker) {
        @($Telemetry | Where-Object { $_.telemetrySequence -gt $ReleaseMarker.telemetrySequence })
    } else { @() }
    $FinalReleaseNotches = if ($ReleaseTelemetry.Count -gt 0) {
        ($ReleaseTelemetry | Select-Object -Last 1).activeNotches
    } else { $null }
    $TelemetryReleaseObserved = $ReleaseTelemetry.Count -gt 0 -and
        $MaximumActiveNotches -ge 1 -and $FinalReleaseNotches -lt $MaximumActiveNotches
    $CutLimit = if ($Preset -eq "speech") { -12.1 } else { -9.1 }
    $AutomatedPass = $Telemetry.Count -gt 0 -and
        $Errors.Count -eq 0 -and
        $MaximumXruns -eq 0 -and
        $MaximumNonFiniteOutput -eq 0 -and
        $MaximumActiveNotches -ge 2 -and
        $MaximumActiveNotches -le 6 -and
        $TelemetryReleaseObserved -and
        $MaximumDriverXruns -eq 0 -and
        $MaximumDeadlineMisses -eq 0 -and
        $MaximumNonFiniteInput -eq 0 -and
        $DeepestCut -ge $CutLimit

    Write-Host ""
    Write-Host "[$Preset] Automated telemetry: $($Telemetry.Count) samples, max notches=$MaximumActiveNotches, deepest cut=$DeepestCut dB, max xruns=$MaximumXruns, max non-finite output=$MaximumNonFiniteOutput."
    $ReleaseObserved = Read-Host "[$Preset] Did both notches release gradually after source removal without rapid chatter? (yes/no)"
    $ChatterObserved = Read-Host "[$Preset] Was rapid on/off chatter observed? (yes/no)"
    $AudibleDiscontinuity = Read-Host "[$Preset] Any audible click, discontinuity, unsafe output, or missed deadline? (yes/no)"
    $TransparentProgram = Read-Host "[$Preset] Did the clean program-only phase remain transparent without feedback suppression? (yes/no)"
    $FrequencyTracked = Read-Host "[$Preset] Did each reported notch stay centered on its injected feedback tone rather than the program material? (yes/no)"
    $DepthRampObserved = Read-Host "[$Preset] Did each notch ramp into depth without a step or click before release? (yes/no)"
    $ToneFrequencies = Read-Host "[$Preset] Approximate one-tone and two-tone frequencies in Hz"
    $CutDepthNotes = Read-Host "[$Preset] Tester notes on achieved cut depth and audible behavior"
    $ManualPass = $ReleaseObserved -match '^(?i:yes)$' -and
        $ChatterObserved -match '^(?i:no)$' -and
        $AudibleDiscontinuity -match '^(?i:no)$' -and
        $TransparentProgram -match '^(?i:yes)$' -and
        $FrequencyTracked -match '^(?i:yes)$' -and
        $DepthRampObserved -match '^(?i:yes)$'
    $Result = if ($AutomatedPass -and $ManualPass) { "PASS" } else { "FAIL" }
    @(
        "$Preset result: $Result"
        "$Preset telemetry samples: $($Telemetry.Count)"
        "$Preset actual sample rate: $(if ($LastTelemetry) { $LastTelemetry.sampleRate } else { 'N/A' })"
        "$Preset actual buffer: $(if ($LastTelemetry) { $LastTelemetry.bufferSize } else { 'N/A' })"
        "$Preset maximum callback CPU: $MaximumCpu"
        "$Preset maximum callback execution: $MaximumExecutionMs ms"
        "$Preset maximum callback peak execution: $MaximumExecutionPeakMs ms"
        "$Preset maximum callback jitter: $MaximumJitterMs ms"
        "$Preset maximum callback peak jitter: $MaximumJitterPeakMs ms"
        "$Preset maximum xruns: $MaximumXruns"
        "$Preset maximum callback deadline misses: $MaximumDeadlineMisses"
        "$Preset maximum driver xruns: $MaximumDriverXruns"
        "$Preset maximum non-finite input samples: $MaximumNonFiniteInput"
        "$Preset maximum non-finite output samples: $MaximumNonFiniteOutput"
        "$Preset maximum active notches: $MaximumActiveNotches / 6"
        "$Preset deepest reported cut: $DeepestCut dB (limit $CutLimit dB)"
        "$Preset final active notches after source removal: $FinalReleaseNotches"
        "$Preset telemetry release observed: $TelemetryReleaseObserved"
        "$Preset release/chatter observation: $ReleaseObserved"
        "$Preset chatter observation: $ChatterObserved"
        "$Preset audible discontinuity observation: $AudibleDiscontinuity"
        "$Preset clean program transparency observation: $TransparentProgram"
        "$Preset notch frequency tracking observation: $FrequencyTracked"
        "$Preset notch depth ramp observation: $DepthRampObserved"
        "$Preset tone frequencies: $ToneFrequencies"
        "$Preset cut-depth/audible notes: $CutDepthNotes"
        ""
    ) | Add-Content $Results
    $RunSummaries += [pscustomobject][ordered]@{
        preset = $Preset
        telemetrySamples = $Telemetry.Count
        callbackCpuMaximum = $MaximumCpu
        callbackExecutionMsMaximum = $MaximumExecutionMs
        callbackExecutionPeakMsMaximum = $MaximumExecutionPeakMs
        callbackJitterMsMaximum = $MaximumJitterMs
        callbackJitterPeakMsMaximum = $MaximumJitterPeakMs
        xrunsMaximum = $MaximumXruns
        callbackDeadlineMissesMaximum = $MaximumDeadlineMisses
        driverXrunsMaximum = $MaximumDriverXruns
        nonFiniteInputMaximum = $MaximumNonFiniteInput
        nonFiniteOutputMaximum = $MaximumNonFiniteOutput
        activeNotchesMaximum = $MaximumActiveNotches
        deepestCutDb = $DeepestCut
        finalReleaseNotches = $FinalReleaseNotches
        telemetryReleaseObserved = $TelemetryReleaseObserved
        transparentProgram = $TransparentProgram
        frequencyTracked = $FrequencyTracked
        depthRampObserved = $DepthRampObserved
        automatedPass = $AutomatedPass
        releaseAndChatterPass = $ManualPass
        result = $Result
    }
}

if (@($RunSummaries | Where-Object { $_.result -ne "PASS" }).Count -gt 0) {
    throw "Acoustic stimulus validation failed. Review $Results and $SessionLog."
}

@(
    "Overall acoustic result: PASS"
    "Detailed telemetry: $SessionLog"
    "Tester summary: $Results"
    "Metadata: $MetadataPath"
) | Add-Content $Results

Write-Host "Acoustic stimulus validation complete."
Write-Host "Evidence written to $EvidenceRoot"