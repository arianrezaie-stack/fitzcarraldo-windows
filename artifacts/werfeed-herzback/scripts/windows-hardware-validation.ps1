param(
    [Parameter(Mandatory = $true)]
    [string]$DeviceType,
    [string]$InputDevice = "",
    [string]$OutputDevice = "",
    [Alias("ExpectedBufferSizes")]
    [ValidateSet(64, 128, 256)]
    [int[]]$BufferSizes = @(64, 128, 256),
    [int]$ExpectedSampleRate = 48000,
    [double]$MaximumCallbackCpu = 1.0,
    [string]$EvidenceRoot = "",
    [switch]$RequireUsbInterface
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$AppRoot = Split-Path -Parent $PSScriptRoot
if (-not $EvidenceRoot) {
    $EvidenceRoot = Join-Path $AppRoot "windows-validation-output"
}

$SafeDeviceType = $DeviceType -replace '[^A-Za-z0-9_.-]', '_'
$SessionLogPath = Join-Path $EvidenceRoot "hardware-session-$SafeDeviceType.jsonl"
$ResultsPath = Join-Path $EvidenceRoot "hardware-session-$SafeDeviceType-results.txt"
$MatrixPath = Join-Path $EvidenceRoot "hardware-matrix-$SafeDeviceType.csv"
$MetadataPath = Join-Path $EvidenceRoot "hardware-session-$SafeDeviceType-metadata.json"
$EnumerationPath = Join-Path $EvidenceRoot "hardware-device-enumeration-$SafeDeviceType.jsonl"
$EndpointInventoryPath = Join-Path $EvidenceRoot "audio-endpoint-inventory-$SafeDeviceType.json"
$RouteStabilityPath = Join-Path $EvidenceRoot "route-stability-$SafeDeviceType.jsonl"
$NotchRedistributionPath = Join-Path $EvidenceRoot "notch-redistribution-$SafeDeviceType.jsonl"
$RouteResetPath = Join-Path $EvidenceRoot "route-reset-restart-$SafeDeviceType.json"
$ReportPath = Join-Path $EvidenceRoot "hardware-validation-$SafeDeviceType.txt"

$Failures = @()
$Warnings = @()
$Devices = @()

function Add-Failure {
    param([string]$Message)
    [void]($script:Failures += $Message)
}

function Add-Warning {
    param([string]$Message)
    [void]($script:Warnings += $Message)
}

function Get-PropertyValue {
    param(
        [object]$Object,
        [string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }
    $Property = $Object.PSObject.Properties[$Name]
    if ($null -eq $Property) {
        return $null
    }
    return $Property.Value
}

function Has-Value {
    param(
        [object]$Object,
        [string]$Name
    )

    $Value = Get-PropertyValue $Object $Name
    return $null -ne $Value -and -not [string]::IsNullOrWhiteSpace([string]$Value)
}

function Convert-ToDouble {
    param(
        [object]$Value,
        [string]$FieldName,
        [string]$Context,
        [ref]$Number
    )

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) {
        Add-Failure "$Context is missing $FieldName."
        return $false
    }

    try {
        $Number.Value = [double]::Parse(
            [string]$Value,
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture
        )
    } catch {
        Add-Failure "$Context has a non-numeric $FieldName value '$Value'."
        return $false
    }

    if ([double]::IsNaN($Number.Value) -or [double]::IsInfinity($Number.Value)) {
        Add-Failure "$Context has a non-finite $FieldName value '$Value'."
        return $false
    }
    return $true
}

function Convert-JsonLines {
    param(
        [string]$Path,
        [string]$Label
    )

    $Events = @()
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Failure "$Label is missing: $Path"
        return $Events
    }

    $LineNumber = 0
    foreach ($Line in @(Get-Content -LiteralPath $Path)) {
        $LineNumber++
        $Trimmed = $Line.Trim()
        if (-not $Trimmed -or $Trimmed.StartsWith("#")) {
            continue
        }
        try {
            [void]($Events += ($Trimmed | ConvertFrom-Json))
        } catch {
            Add-Failure "$Label contains invalid JSON at line $LineNumber."
        }
    }
    if ($Events.Count -eq 0) {
        Add-Failure "$Label contains no JSON events."
    }
    return $Events
}

function Get-CaseKey {
    param(
        [int]$BufferSize,
        [int]$RouteCount
    )
    return "$BufferSize/$RouteCount"
}

function Test-BooleanTrue {
    param(
        [object]$Value,
        [string]$FieldName,
        [string]$Context
    )

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) {
        Add-Failure "$Context is missing $FieldName."
        return $false
    }
    if ([string]$Value -notmatch '^(?i:true|yes)$') {
        Add-Failure "$Context has unsafe $FieldName value '$Value'; expected true/yes."
        return $false
    }
    return $true
}

New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null

# Metadata identifies the requested matrix and the people/devices involved.
$Metadata = $null
if (-not (Test-Path -LiteralPath $MetadataPath -PathType Leaf)) {
    Add-Failure "Session metadata is missing: $MetadataPath"
} else {
    try {
        $Metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "Session metadata is not valid JSON: $MetadataPath"
    }
}

if ($null -ne $Metadata) {
    foreach ($RequiredField in @(
        "capturedAt", "deviceType", "inputDevice", "outputDevice",
        "interfaceModel", "driverVersion", "tester", "sampleRateRequested",
        "bufferSizes", "portableStartupConfirmed", "routeResetRestartConfirmed"
    )) {
        if (-not (Has-Value $Metadata $RequiredField)) {
            Add-Failure "Session metadata is missing $RequiredField."
        }
    }

    $MetadataDeviceType = [string](Get-PropertyValue $Metadata "deviceType")
    if ($MetadataDeviceType -and $MetadataDeviceType -ne $DeviceType) {
        Add-Failure "Session metadata deviceType '$MetadataDeviceType' does not match '$DeviceType'."
    }
    $MetadataRate = 0.0
    if (Convert-ToDouble (Get-PropertyValue $Metadata "sampleRateRequested") `
            "sampleRateRequested" "Session metadata" ([ref]$MetadataRate)) {
        if ($MetadataRate -ne $ExpectedSampleRate) {
            Add-Failure "Session metadata requests $MetadataRate Hz, expected $ExpectedSampleRate Hz."
        }
    }
    $MetadataBuffers = @(
        Get-PropertyValue $Metadata "bufferSizes" |
            ForEach-Object {
                try { [int]$_ } catch { $null }
            }
    )
    foreach ($ExpectedBuffer in $BufferSizes) {
        if ($MetadataBuffers -notcontains $ExpectedBuffer) {
            Add-Failure "Session metadata does not include requested $ExpectedBuffer-sample buffer."
        }
    }
    if (-not (Test-BooleanTrue (Get-PropertyValue $Metadata "portableStartupConfirmed") `
            "portableStartupConfirmed" "Session metadata")) {
        # The helper already recorded the actionable failure.
    }
    if (-not (Test-BooleanTrue (Get-PropertyValue $Metadata "routeResetRestartConfirmed") `
            "routeResetRestartConfirmed" "Session metadata")) {
        # The helper already recorded the actionable failure.
    }

    if (-not $InputDevice) {
        $InputDevice = [string](Get-PropertyValue $Metadata "inputDevice")
    }
    if (-not $OutputDevice) {
        $OutputDevice = [string](Get-PropertyValue $Metadata "outputDevice")
    }
}

if (-not $InputDevice) {
    Add-Failure "No requested input device was supplied or recorded in session metadata."
}
if (-not $OutputDevice) {
    Add-Failure "No requested output device was supplied or recorded in session metadata."
}

# The enumeration must be parseable and contain the exact requested endpoints.
$EnumerationEvents = Convert-JsonLines $EnumerationPath "Device enumeration"
$DeviceEvent = @($EnumerationEvents | Where-Object {
    [string](Get-PropertyValue $_ "type") -eq "devices"
} | Select-Object -Last 1)
if ($DeviceEvent.Count -eq 0) {
    Add-Failure "Device enumeration does not contain a devices event."
} else {
    $Devices = @(Get-PropertyValue $DeviceEvent[0] "devices")
    if ($Devices.Count -eq 0) {
        Add-Failure "Device enumeration devices event contains no devices."
    }
    $HasUsbRecord = $false
    $BlockedOutputTokens = @(
        "bluetooth", "hdmi", "displayport", "display audio", "stereo mix",
        "voicemeeter", "vb-audio", "asio4all", "fl studio asio", "loopback",
        "virtual audio", "virtual cable", "blackhole", "realtek high definition",
        "high definition audio", "onboard audio", "built-in audio", "built in audio"
    )
    foreach ($Device in $Devices) {
        $Context = "Emitted device record"
        $DeviceTypeValue = [string](Get-PropertyValue $Device "deviceType")
        $NameValue = [string](Get-PropertyValue $Device "name")
        $DirectionValue = [string](Get-PropertyValue $Device "direction")
        $TransportValue = [string](Get-PropertyValue $Device "transport")
        $InterfaceValue = [string](Get-PropertyValue $Device "interfaceName")
        $ChannelsValue = 0.0
        $NameLower = "$DeviceTypeValue $NameValue".ToLowerInvariant()

        if ([string]::IsNullOrWhiteSpace($DeviceTypeValue)) {
            Add-Failure "$Context is missing deviceType."
        }
        if ([string]::IsNullOrWhiteSpace($NameValue) -or $NameValue -ne $NameValue.Trim()) {
            Add-Failure "$Context has a missing or non-exact device name '$NameValue'."
        }
        if ($DirectionValue -notin @("input", "output")) {
            Add-Failure "$Context has unusable direction '$DirectionValue'."
        }
        if ([string]::IsNullOrWhiteSpace($InterfaceValue)) {
            Add-Failure "$Context is missing interfaceName."
        }
        if (-not (Test-BooleanTrue (Get-PropertyValue $Device "hardwareEligible") "hardwareEligible" $Context)) {
            # The helper already recorded the actionable failure.
        }
        if (-not (Convert-ToDouble (Get-PropertyValue $Device "channels") `
                "channels" $Context ([ref]$ChannelsValue))) {
            continue
        }
        if ($ChannelsValue -le 0 -or $ChannelsValue -ne [math]::Truncate($ChannelsValue)) {
            Add-Failure "$Context reports unusable channel count '$ChannelsValue'."
        }
        $ChannelNamesValue = Get-PropertyValue $Device "channelNames"
        if ($null -eq $ChannelNamesValue) {
            Add-Failure "$Context is missing channelNames."
        } else {
            $ChannelNames = @($ChannelNamesValue)
            foreach ($ChannelName in $ChannelNames) {
                if ($null -ne $ChannelName -and $ChannelName -isnot [string]) {
                    Add-Failure "$Context contains a non-text channel label."
                }
            }
            if ($ChannelNames.Count -gt [int]$ChannelsValue) {
                Add-Failure "$Context contains more channel labels than channels."
            }
        }
        if ($TransportValue -notin @("USB", "Ethernet audio")) {
            Add-Failure "$Context has unsupported transport '$TransportValue'."
        } elseif ($TransportValue -eq "USB") {
            $HasUsbRecord = $true
            if ($NameLower -notmatch '\busb\b') {
                Add-Failure "$Context is labeled USB but its native type/name has no USB marker."
            }
        } else {
            if ($NameLower -notmatch 'dante|sound\s*grid|aes67|ravenna|avb|audio over ethernet') {
                Add-Failure "$Context is labeled Ethernet audio without an approved transport marker."
            }
        }
        foreach ($BlockedToken in $BlockedOutputTokens) {
            if ($NameLower.Contains($BlockedToken)) {
                Add-Failure "$Context leaked blocked endpoint text '$BlockedToken'."
            }
        }
    }
    if ($RequireUsbInterface -and -not $HasUsbRecord) {
        Add-Failure "Live device enumeration contains no eligible USB record."
    }
    if (@($Devices | Where-Object { [string](Get-PropertyValue $_ "direction") -eq "input" }).Count -eq 0) {
        Add-Failure "Device enumeration contains no eligible input record."
    }
    if (@($Devices | Where-Object { [string](Get-PropertyValue $_ "direction") -eq "output" }).Count -eq 0) {
        Add-Failure "Device enumeration contains no eligible output record."
    }
    $MatchingInputs = @($Devices | Where-Object {
        [string](Get-PropertyValue $_ "direction") -eq "input" -and
        [string](Get-PropertyValue $_ "name") -eq $InputDevice
    })
    if ($InputDevice -and $MatchingInputs.Count -eq 0) {
        Add-Failure "Device enumeration does not contain requested input '$InputDevice'."
    }
    $MatchingOutputs = @($Devices | Where-Object {
        [string](Get-PropertyValue $_ "direction") -eq "output" -and
        [string](Get-PropertyValue $_ "name") -eq $OutputDevice
    })
    if ($OutputDevice -and $MatchingOutputs.Count -eq 0) {
        Add-Failure "Device enumeration does not contain requested output '$OutputDevice'."
    }
}

# The native event intentionally contains only eligible records. Capture the
# Windows endpoint inventory separately so a live run proves that filtering was
# exercised against a machine with both a USB interface and a built-in or
# virtual endpoint available to the operating system.
$EndpointInventory = @()
if (-not (Test-Path -LiteralPath $EndpointInventoryPath -PathType Leaf)) {
    Add-Failure "Audio endpoint inventory is missing: $EndpointInventoryPath"
} else {
    try {
        $EndpointInventory = @(Get-Content -LiteralPath $EndpointInventoryPath -Raw | ConvertFrom-Json)
    } catch {
        Add-Failure "Audio endpoint inventory is not valid JSON: $EndpointInventoryPath"
    }
    if ($EndpointInventory.Count -eq 0) {
        Add-Failure "Audio endpoint inventory contains no Windows audio endpoints."
    }
}
if ($EndpointInventory.Count -gt 0) {
    $InventoryText = ($EndpointInventory | ForEach-Object {
        "$(Get-PropertyValue $_ 'friendlyName') $(Get-PropertyValue $_ 'instanceId')"
    }) -join " | "
    $InventoryLower = $InventoryText.ToLowerInvariant()
    if ($InventoryLower -notmatch '\busb\b') {
        Add-Failure "Audio endpoint inventory contains no USB endpoint."
    }
    if ($InventoryLower -notmatch 'realtek|high definition|built[- ]in|onboard|bluetooth|hdmi|displayport|virtual|loopback|cable|voicemeeter') {
        Add-Failure "Audio endpoint inventory contains no built-in or virtual endpoint to exercise exclusion filtering."
    }
}

# The packaged-app check is intentionally manual: calibration requires a real
# microphone/loudspeaker path and its UI/device lifecycle cannot be proven by
# the native protocol-only route matrix.
$RouteResetEvidence = $null
if (-not (Test-Path -LiteralPath $RouteResetPath -PathType Leaf)) {
    Add-Failure "Route reset/restart evidence is missing: $RouteResetPath"
} else {
    try {
        $RouteResetEvidence = Get-Content -LiteralPath $RouteResetPath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "Route reset/restart evidence is not valid JSON: $RouteResetPath"
    }
}
if ($null -ne $RouteResetEvidence) {
    foreach ($RequiredRouteResetField in @(
        "capturedAt", "tester", "resetRoute", "retainedRoute",
        "twoMappedRoutesBaselineSaved", "resetRouteNeedsCalibration",
        "retainedRouteRemainedCalibratedAfterReset", "appClosedBeforeRestart", "appRestarted",
        "resetRouteNeedsCalibrationAfterRestart", "resetRouteRecalibrated",
        "retainedRouteRemainedCalibratedAfterRestart",
        "retainedRouteUnchangedAfterRecalibration"
    )) {
        if ($null -eq $RouteResetEvidence.PSObject.Properties[$RequiredRouteResetField]) {
            Add-Failure "Route reset/restart evidence is missing $RequiredRouteResetField."
        }
    }
    if ([string](Get-PropertyValue $RouteResetEvidence "status") -ne "pass") {
        Add-Failure "Route reset/restart evidence status is '$($RouteResetEvidence.status)'; expected pass."
    }
    $ResetRouteValue = 0.0
    $RetainedRouteValue = 0.0
    if (Convert-ToDouble (Get-PropertyValue $RouteResetEvidence "resetRoute") `
            "resetRoute" "Route reset/restart evidence" ([ref]$ResetRouteValue) -and
        Convert-ToDouble (Get-PropertyValue $RouteResetEvidence "retainedRoute") `
            "retainedRoute" "Route reset/restart evidence" ([ref]$RetainedRouteValue)) {
        if ($ResetRouteValue -notin @(1, 2, 3, 4) -or $RetainedRouteValue -notin @(1, 2, 3, 4) -or
            $ResetRouteValue -eq $RetainedRouteValue) {
            Add-Failure "Route reset/restart evidence must name two different routes from 1 through 4."
        }
    }
    foreach ($RouteResetField in @(
        "twoMappedRoutesBaselineSaved", "resetRouteNeedsCalibration",
        "retainedRouteRemainedCalibratedAfterReset", "appClosedBeforeRestart", "appRestarted",
        "resetRouteNeedsCalibrationAfterRestart", "resetRouteRecalibrated",
        "retainedRouteRemainedCalibratedAfterRestart",
        "retainedRouteUnchangedAfterRecalibration"
    )) {
        [void](Test-BooleanTrue (Get-PropertyValue $RouteResetEvidence $RouteResetField) `
            $RouteResetField "Route reset/restart evidence")
    }
}

# Build the exact expected route/buffer matrix before examining CSV rows.
$ExpectedCases = @{}
foreach ($BufferSize in $BufferSizes) {
    foreach ($RouteCount in 1..8) {
        $ExpectedCases[(Get-CaseKey $BufferSize $RouteCount)] = $true
    }
}

$Rows = @()
if (-not (Test-Path -LiteralPath $MatrixPath -PathType Leaf)) {
    Add-Failure "Hardware matrix CSV is missing: $MatrixPath"
} else {
    try {
        $Rows = @(Import-Csv -LiteralPath $MatrixPath)
    } catch {
        Add-Failure "Hardware matrix CSV could not be parsed: $MatrixPath"
    }
    if ($Rows.Count -eq 0) {
        Add-Failure "Hardware matrix CSV contains no rows."
    }
}

$SeenCases = @{}
foreach ($Row in $Rows) {
    $Context = "Matrix row"
    $RequestedBuffer = 0.0
    $RouteCountValue = 0.0
    $HasRequestedBuffer = Convert-ToDouble (Get-PropertyValue $Row "bufferSizeRequested") `
        "bufferSizeRequested" $Context ([ref]$RequestedBuffer)
    $HasRouteCount = Convert-ToDouble (Get-PropertyValue $Row "routeCount") `
        "routeCount" $Context ([ref]$RouteCountValue)
    if (-not $HasRequestedBuffer -or -not $HasRouteCount) {
        continue
    }
    if ($RequestedBuffer -ne [math]::Truncate($RequestedBuffer) -or
        $RouteCountValue -ne [math]::Truncate($RouteCountValue)) {
        Add-Failure "$Context must use whole-number bufferSizeRequested and routeCount values."
        continue
    }

    $RequestedBufferInt = [int]$RequestedBuffer
    $RouteCount = [int]$RouteCountValue
    $CaseKey = Get-CaseKey $RequestedBufferInt $RouteCount
    $Context = "Matrix case $CaseKey"
    if (-not $ExpectedCases.ContainsKey($CaseKey)) {
        Add-Failure "$Context is not one of the requested 64/128/256-sample, 1-8-route cases."
        continue
    }
    if ($SeenCases.ContainsKey($CaseKey)) {
        Add-Failure "$Context appears more than once in the hardware matrix CSV."
        continue
    }
    $SeenCases[$CaseKey] = $true

    $ActualRate = 0.0
    $ActualBuffer = 0.0
    $MaximumCpu = 0.0
    $MaximumXruns = 0.0
    [void](Convert-ToDouble (Get-PropertyValue $Row "sampleRate") `
        "sampleRate" $Context ([ref]$ActualRate))
    [void](Convert-ToDouble (Get-PropertyValue $Row "bufferSizeActual") `
        "bufferSizeActual" $Context ([ref]$ActualBuffer))
    [void](Convert-ToDouble (Get-PropertyValue $Row "callbackCpuMaximum") `
        "callbackCpuMaximum" $Context ([ref]$MaximumCpu))
    [void](Convert-ToDouble (Get-PropertyValue $Row "xrunsMaximum") `
        "xrunsMaximum" $Context ([ref]$MaximumXruns))
    $ExecutionMs = 0.0
    $ExecutionPeakMs = 0.0
    $JitterMs = 0.0
    $JitterPeakMs = 0.0
    $DeadlineMisses = 0.0
    $DriverXruns = 0.0
    $NonFiniteInput = 0.0
    $NonFiniteOutput = 0.0
    [void](Convert-ToDouble (Get-PropertyValue $Row "callbackExecutionMsMaximum") `
        "callbackExecutionMsMaximum" $Context ([ref]$ExecutionMs))
    [void](Convert-ToDouble (Get-PropertyValue $Row "callbackExecutionPeakMsMaximum") `
        "callbackExecutionPeakMsMaximum" $Context ([ref]$ExecutionPeakMs))
    [void](Convert-ToDouble (Get-PropertyValue $Row "callbackJitterMsMaximum") `
        "callbackJitterMsMaximum" $Context ([ref]$JitterMs))
    [void](Convert-ToDouble (Get-PropertyValue $Row "callbackJitterPeakMsMaximum") `
        "callbackJitterPeakMsMaximum" $Context ([ref]$JitterPeakMs))
    [void](Convert-ToDouble (Get-PropertyValue $Row "callbackDeadlineMissesMaximum") `
        "callbackDeadlineMissesMaximum" $Context ([ref]$DeadlineMisses))
    [void](Convert-ToDouble (Get-PropertyValue $Row "driverXrunsMaximum") `
        "driverXrunsMaximum" $Context ([ref]$DriverXruns))
    [void](Convert-ToDouble (Get-PropertyValue $Row "nonFiniteInputSamplesMaximum") `
        "nonFiniteInputSamplesMaximum" $Context ([ref]$NonFiniteInput))
    [void](Convert-ToDouble (Get-PropertyValue $Row "nonFiniteOutputSamplesMaximum") `
        "nonFiniteOutputSamplesMaximum" $Context ([ref]$NonFiniteOutput))

    if ($ActualRate -ne $ExpectedSampleRate) {
        Add-Failure "$Context reports actual sample rate $ActualRate Hz; expected $ExpectedSampleRate Hz."
    }
    if ($ActualBuffer -le 0) {
        Add-Failure "$Context reports an invalid actual buffer size '$ActualBuffer'."
    } elseif ($ActualBuffer -ne [math]::Truncate($ActualBuffer)) {
        Add-Failure "$Context reports a non-integer actual buffer size '$ActualBuffer'."
    } elseif ($ActualBuffer -ne $RequestedBufferInt) {
        Add-Warning "$Context used actual buffer $ActualBuffer instead of requested $RequestedBufferInt; review endpoint support."
    }
    if ($MaximumCpu -lt 0 -or $MaximumCpu -gt $MaximumCallbackCpu) {
        Add-Failure "$Context reports callback CPU $MaximumCpu, outside the safe 0-$MaximumCallbackCpu ratio."
    }
    foreach ($Metric in @(
        @{ name = "callbackExecutionMsMaximum"; value = $ExecutionMs },
        @{ name = "callbackExecutionPeakMsMaximum"; value = $ExecutionPeakMs },
        @{ name = "callbackJitterMsMaximum"; value = $JitterMs },
        @{ name = "callbackJitterPeakMsMaximum"; value = $JitterPeakMs }
    )) {
        if ($Metric.value -lt 0) {
            Add-Failure "$Context reports a negative $($Metric.name) value $($Metric.value)."
        }
    }
    if ($ExecutionPeakMs -lt $ExecutionMs) {
        Add-Failure "$Context reports callback peak execution $ExecutionPeakMs ms below maximum execution $ExecutionMs ms."
    }
    if ($MaximumXruns -ne 0) {
        Add-Failure "$Context reports $MaximumXruns xrun(s); expected zero."
    }
    if ($DeadlineMisses -ne 0) {
        Add-Failure "$Context reports $DeadlineMisses callback deadline miss(es); expected zero."
    }
    if ($DriverXruns -ne 0) {
        Add-Failure "$Context reports $DriverXruns driver xrun(s); expected zero."
    }
    if ($NonFiniteInput -ne 0 -or $NonFiniteOutput -ne 0) {
        Add-Failure "$Context reports non-finite samples (input=$NonFiniteInput, output=$NonFiniteOutput); expected zero."
    }
    if (-not (Has-Value $Row "engineErrors")) {
        Add-Failure "$Context is missing engineErrors."
    } else {
        $EngineErrors = 0.0
        $HasEngineErrorsValue = Convert-ToDouble (Get-PropertyValue $Row "engineErrors") `
            "engineErrors" $Context ([ref]$EngineErrors)
        if ($HasEngineErrorsValue -and $EngineErrors -ne 0) {
            Add-Failure "$Context reports $EngineErrors engine error(s)."
        }
    }
    [void](Test-BooleanTrue (Get-PropertyValue $Row "audiblePass") "audiblePass" $Context)
    if (-not (Has-Value $Row "result")) {
        Add-Failure "$Context is missing result."
    } elseif ([string](Get-PropertyValue $Row "result") -ne "PASS") {
        Add-Failure "$Context has result '$($Row.result)' instead of PASS."
    }
}

foreach ($ExpectedCase in $ExpectedCases.Keys) {
    if (-not $SeenCases.ContainsKey($ExpectedCase)) {
        Add-Failure "Hardware matrix is missing route/buffer case $ExpectedCase."
    }
}
if ($Rows.Count -ne $ExpectedCases.Count) {
    Add-Failure "Hardware matrix contains $($Rows.Count) row(s); expected exactly $($ExpectedCases.Count)."
}

# Confirm each matrix case has raw telemetry in the session log, not just a CSV row.
$TelemetryCases = @{}
$SessionEvents = @()
if (Test-Path -LiteralPath $SessionLogPath -PathType Leaf) {
    $CurrentCase = ""
    $LineNumber = 0
    foreach ($Line in @(Get-Content -LiteralPath $SessionLogPath)) {
        $LineNumber++
        $Trimmed = $Line.Trim()
        if ($Trimmed -match '^# buffer-size=(\d+) route-count=(\d+)$') {
            $CurrentCase = Get-CaseKey ([int]$Matches[1]) ([int]$Matches[2])
            continue
        }
        if (-not $Trimmed -or $Trimmed.StartsWith("#")) {
            continue
        }
        try {
            $Event = $Trimmed | ConvertFrom-Json
            [void]($SessionEvents += $Event)
        } catch {
            Add-Failure "Hardware session telemetry contains invalid JSON at line $LineNumber."
            continue
        }
        if ($CurrentCase -and [string](Get-PropertyValue $Event "type") -eq "telemetry") {
            if (-not $TelemetryCases.ContainsKey($CurrentCase)) {
                $TelemetryCases[$CurrentCase] = $true
            }
            foreach ($TelemetryField in @(
                "sampleRate", "bufferSize", "callbackCpu", "callbackExecutionMs",
                "callbackExecutionPeakMs", "callbackJitterMs", "callbackJitterPeakMs",
                "xruns", "callbackDeadlineMisses", "driverXruns",
                "nonFiniteInputSamples", "nonFiniteOutputSamples"
            )) {
                if (-not (Has-Value $Event $TelemetryField)) {
                    Add-Failure "Telemetry for matrix case $CurrentCase is missing $TelemetryField."
                } else {
                    $TelemetryNumber = 0.0
                    [void](Convert-ToDouble (Get-PropertyValue $Event $TelemetryField) `
                        $TelemetryField "Telemetry for matrix case $CurrentCase" ([ref]$TelemetryNumber))
                }
            }
        }
    }
} else {
    Add-Failure "Hardware session telemetry log is missing: $SessionLogPath"
}
foreach ($ExpectedCase in $ExpectedCases.Keys) {
    if (-not $TelemetryCases.ContainsKey($ExpectedCase)) {
        Add-Failure "Hardware session telemetry has no telemetry event for matrix case $ExpectedCase."
    }
}

# A dedicated four-slot run proves that disabled route positions do not shift
# when multiple mono routes share the exact configured device pair.
$StabilityEvents = @()
if (-not (Test-Path -LiteralPath $RouteStabilityPath -PathType Leaf)) {
    Add-Failure "Route stability evidence is missing: $RouteStabilityPath"
} else {
    $StabilityLineNumber = 0
    foreach ($Line in @(Get-Content -LiteralPath $RouteStabilityPath)) {
        $StabilityLineNumber++
        $Trimmed = $Line.Trim()
        if (-not $Trimmed -or $Trimmed.StartsWith("#")) {
            continue
        }
        try {
            [void]($StabilityEvents += ($Trimmed | ConvertFrom-Json))
        } catch {
            Add-Failure "Route stability evidence contains invalid JSON at line $StabilityLineNumber."
        }
    }
}
$StabilityTelemetry = @($StabilityEvents | Where-Object {
    [string](Get-PropertyValue $_ "type") -eq "telemetry"
})
if ($StabilityTelemetry.Count -eq 0) {
    Add-Failure "Route stability evidence contains no telemetry events."
} else {
    foreach ($TelemetryEvent in $StabilityTelemetry) {
        $RouteTelemetry = @(Get-PropertyValue $TelemetryEvent "routeTelemetry")
        if ($RouteTelemetry.Count -ne 4) {
            Add-Failure "Route stability telemetry must contain exactly four route slots."
            continue
        }
        foreach ($ExpectedRoute in @(
            @{ route = 1; enabled = $true },
            @{ route = 2; enabled = $false },
            @{ route = 3; enabled = $true },
            @{ route = 4; enabled = $false }
        )) {
            $ObservedRoute = $RouteTelemetry | Where-Object {
                [int](Get-PropertyValue $_ "route") -eq $ExpectedRoute.route
            } | Select-Object -First 1
            if ($null -eq $ObservedRoute) {
                Add-Failure "Route stability telemetry is missing route $($ExpectedRoute.route)."
                continue
            }
            $ObservedEnabled = Get-PropertyValue $ObservedRoute "enabled"
            if ($null -eq $ObservedEnabled) {
                Add-Failure "Route $($ExpectedRoute.route) is missing its enabled state."
            } elseif ([bool]$ObservedEnabled -ne $ExpectedRoute.enabled) {
                Add-Failure "Route $($ExpectedRoute.route) changed enabled state; expected $($ExpectedRoute.enabled)."
            }
        }
    }
}

# The redistribution run keeps one audio process alive while four mapped routes
# move through 8, 11/10, 16, and 32 slots. Every step must have live telemetry,
# stable route indices, finite health readings, and no stale cuts after reset.
$RedistributionEvents = Convert-JsonLines $NotchRedistributionPath "Notch redistribution evidence"
$ExpectedRedistribution = [ordered]@{
    "notch-four-active" = @(@{ route = 1; enabled = $true; capacity = 8 }, @{ route = 2; enabled = $true; capacity = 8 }, @{ route = 3; enabled = $true; capacity = 8 }, @{ route = 4; enabled = $true; capacity = 8 })
    "notch-three-active" = @(@{ route = 1; enabled = $true; capacity = 11 }, @{ route = 2; enabled = $true; capacity = 11 }, @{ route = 3; enabled = $true; capacity = 10 }, @{ route = 4; enabled = $false; capacity = 0 })
    "notch-two-active" = @(@{ route = 1; enabled = $true; capacity = 16 }, @{ route = 2; enabled = $true; capacity = 16 }, @{ route = 3; enabled = $false; capacity = 0 }, @{ route = 4; enabled = $false; capacity = 0 })
    "notch-one-active" = @(@{ route = 1; enabled = $true; capacity = 32 }, @{ route = 2; enabled = $false; capacity = 0 }, @{ route = 3; enabled = $false; capacity = 0 }, @{ route = 4; enabled = $false; capacity = 0 })
    "notch-two-active-restored" = @(@{ route = 1; enabled = $true; capacity = 16 }, @{ route = 2; enabled = $true; capacity = 16 }, @{ route = 3; enabled = $false; capacity = 0 }, @{ route = 4; enabled = $false; capacity = 0 })
    "notch-three-active-restored" = @(@{ route = 1; enabled = $true; capacity = 11 }, @{ route = 2; enabled = $true; capacity = 11 }, @{ route = 3; enabled = $true; capacity = 10 }, @{ route = 4; enabled = $false; capacity = 0 })
    "notch-four-active-restored" = @(@{ route = 1; enabled = $true; capacity = 8 }, @{ route = 2; enabled = $true; capacity = 8 }, @{ route = 3; enabled = $true; capacity = 8 }, @{ route = 4; enabled = $true; capacity = 8 })
}
$RedistributionCurrentStep = ""
$RedistributionStepTelemetry = @{}
$RedistributionErrors = 0
foreach ($Event in $RedistributionEvents) {
    $EventType = [string](Get-PropertyValue $Event "type")
    if ($EventType -eq "test_marker") {
        $MarkerName = [string](Get-PropertyValue $Event "name")
        if ($ExpectedRedistribution.Contains($MarkerName)) {
            $RedistributionCurrentStep = $MarkerName
        }
        continue
    }
    if ($EventType -eq "error") {
        $RedistributionErrors++
        continue
    }
    if ($EventType -eq "state" -and [string](Get-PropertyValue $Event "phase") -in @("stopped", "device_stopped")) {
        Add-Failure "Notch redistribution audio stopped during $RedistributionCurrentStep."
        continue
    }
    if ($EventType -ne "telemetry" -or -not $RedistributionCurrentStep) {
        continue
    }
    if (-not $RedistributionStepTelemetry.ContainsKey($RedistributionCurrentStep)) {
        $RedistributionStepTelemetry[$RedistributionCurrentStep] = $Event
    }
}
if ($RedistributionErrors -gt 0) {
    Add-Failure "Notch redistribution emitted $RedistributionErrors engine error event(s)."
}
foreach ($ExpectedStep in $ExpectedRedistribution.Keys) {
    if (-not $RedistributionStepTelemetry.ContainsKey($ExpectedStep)) {
        Add-Failure "Notch redistribution has no live telemetry for $ExpectedStep."
        continue
    }
    $TelemetryEvent = $RedistributionStepTelemetry[$ExpectedStep]
    foreach ($Field in @(
        "running", "callbackCpu", "callbackExecutionMs", "callbackExecutionPeakMs",
        "callbackJitterMs", "callbackJitterPeakMs", "xruns", "callbackDeadlineMisses",
        "driverXruns", "nonFiniteInputSamples", "nonFiniteOutputSamples",
        "deviceClockDriftPpm", "deviceClockReady", "deviceClockAgeMs",
        "inputPeak", "outputPeak"
    )) {
        if (-not (Has-Value $TelemetryEvent $Field)) {
            Add-Failure "Notch redistribution step $ExpectedStep is missing $Field telemetry."
        }
    }
    if ([bool](Get-PropertyValue $TelemetryEvent "running") -ne $true) {
        Add-Failure "Notch redistribution step $ExpectedStep was not running."
    }
    $CallbackCpu = 0.0
    if (Convert-ToDouble (Get-PropertyValue $TelemetryEvent "callbackCpu") `
            "callbackCpu" "Notch redistribution step $ExpectedStep" ([ref]$CallbackCpu) -and
        ($CallbackCpu -lt 0 -or $CallbackCpu -gt $MaximumCallbackCpu)) {
        Add-Failure "Notch redistribution step $ExpectedStep reports callback CPU $CallbackCpu outside the safe 0-$MaximumCallbackCpu ratio."
    }
    $Xruns = 0.0
    if (Convert-ToDouble (Get-PropertyValue $TelemetryEvent "xruns") `
            "xruns" "Notch redistribution step $ExpectedStep" ([ref]$Xruns) -and $Xruns -ne 0) {
        Add-Failure "Notch redistribution step $ExpectedStep reports $Xruns xrun(s); expected zero."
    }
    $DriverXruns = 0.0
    if (Convert-ToDouble (Get-PropertyValue $TelemetryEvent "driverXruns") `
            "driverXruns" "Notch redistribution step $ExpectedStep" ([ref]$DriverXruns) -and
        $DriverXruns -ne 0) {
        Add-Failure "Notch redistribution step $ExpectedStep reports $DriverXruns driver xrun(s); expected zero."
    }
    foreach ($CounterField in @("callbackDeadlineMisses", "nonFiniteInputSamples", "nonFiniteOutputSamples")) {
        $Counter = 0.0
        if (Convert-ToDouble (Get-PropertyValue $TelemetryEvent $CounterField) `
                $CounterField "Notch redistribution step $ExpectedStep" ([ref]$Counter) -and
            $Counter -ne 0) {
            Add-Failure "Notch redistribution step $ExpectedStep reports $Counter $CounterField; expected zero."
        }
    }
    $ClockDrift = 0.0
    if (Convert-ToDouble (Get-PropertyValue $TelemetryEvent "deviceClockDriftPpm") `
            "deviceClockDriftPpm" "Notch redistribution step $ExpectedStep" ([ref]$ClockDrift) -and
        [double]::IsNaN($ClockDrift)) {
        Add-Failure "Notch redistribution step $ExpectedStep reports invalid clock drift $ClockDrift."
    }
    foreach ($PeakField in @("inputPeak", "outputPeak")) {
        $Peak = 0.0
        if (Convert-ToDouble (Get-PropertyValue $TelemetryEvent $PeakField) `
                $PeakField "Notch redistribution step $ExpectedStep" ([ref]$Peak) -and
            ($Peak -lt 0 -or $Peak -gt 2)) {
            Add-Failure "Notch redistribution step $ExpectedStep reports invalid $PeakField $Peak."
        }
    }
    $RouteTelemetry = @(Get-PropertyValue $TelemetryEvent "routeTelemetry")
    if ($RouteTelemetry.Count -ne 4) {
        Add-Failure "Notch redistribution step $ExpectedStep must contain exactly four route slots."
        continue
    }
    foreach ($ExpectedRoute in $ExpectedRedistribution[$ExpectedStep]) {
        $ObservedRoute = $RouteTelemetry | Where-Object {
            [int](Get-PropertyValue $_ "route") -eq $ExpectedRoute.route
        } | Select-Object -First 1
        if ($null -eq $ObservedRoute) {
            Add-Failure "Notch redistribution step $ExpectedStep is missing route $($ExpectedRoute.route)."
            continue
        }
        if ([bool](Get-PropertyValue $ObservedRoute "enabled") -ne $ExpectedRoute.enabled) {
            Add-Failure "Notch redistribution step $ExpectedStep has unexpected enabled state for route $($ExpectedRoute.route)."
        }
        $Capacity = 0.0
        if (-not (Convert-ToDouble (Get-PropertyValue $ObservedRoute "maximumAllowedNotches") `
                "maximumAllowedNotches" "Notch redistribution step $ExpectedStep route $($ExpectedRoute.route)" ([ref]$Capacity))) {
            continue
        }
        if ($Capacity -ne $ExpectedRoute.capacity) {
            Add-Failure "Notch redistribution step $ExpectedStep route $($ExpectedRoute.route) reports $Capacity slots; expected $($ExpectedRoute.capacity)."
        }
        $ActiveNotches = 0.0
        if (Convert-ToDouble (Get-PropertyValue $ObservedRoute "activeNotches") `
                "activeNotches" "Notch redistribution step $ExpectedStep route $($ExpectedRoute.route)" ([ref]$ActiveNotches) -and
            $ActiveNotches -gt $Capacity) {
            Add-Failure "Notch redistribution step $ExpectedStep route $($ExpectedRoute.route) has $ActiveNotches active cuts above its $Capacity-slot capacity."
        }
        if (-not $ExpectedRoute.enabled -and $ActiveNotches -ne 0) {
            Add-Failure "Disarmed route $($ExpectedRoute.route) retained $ActiveNotches active notch(es) at $ExpectedStep."
        }
    }
}

# Results must preserve measured delay and both sides of the disconnect check.
$ResultsText = ""
if (-not (Test-Path -LiteralPath $ResultsPath -PathType Leaf)) {
    Add-Failure "Hardware session results are missing: $ResultsPath"
} else {
    $ResultsText = Get-Content -LiteralPath $ResultsPath -Raw
    if ($ResultsText -notmatch '(?im)^\s*Route reset/restart:\s*PASS\b') {
        Add-Failure "Hardware session results do not confirm the route reset/restart check."
    }
    if ($ResultsText -notmatch '(?im)^\s*Measured delay:\s*\S+') {
        Add-Failure "Hardware session results are missing a measured delay value."
    }
    if ($ResultsText -notmatch '(?im)^\s*Disconnect event observed:\s*PASS\b') {
        Add-Failure "Hardware session results do not confirm the engine emitted device_stopped."
    }
    $DisconnectBehavior = [regex]::Match(
        $ResultsText,
        '(?im)^\s*Disconnect behavior:\s*(?<value>\S+)'
    )
    if (-not $DisconnectBehavior.Success) {
        Add-Failure "Hardware session results are missing Disconnect behavior."
    } elseif ($DisconnectBehavior.Groups["value"].Value -notmatch '^(?i:yes)$') {
        Add-Failure "Disconnect behavior is '$($DisconnectBehavior.Groups["value"].Value)'; expected yes (no silent output switch)."
    }
    if ($ResultsText -notmatch '(?im)^\s*Tester notes:') {
        Add-Failure "Hardware session results are missing Tester notes."
    }
}

# The raw disconnect event is independently checked so a hand-edited results file
# cannot turn a missing device stop into a pass.
$DisconnectState = @($SessionEvents | Where-Object {
    [string](Get-PropertyValue $_ "type") -eq "state" -and
    [string](Get-PropertyValue $_ "phase") -eq "device_stopped"
})
if ($DisconnectState.Count -eq 0) {
    Add-Failure "Hardware session telemetry does not contain a device_stopped state event."
}

$ReportLines = @()
if ($Failures.Count -eq 0) {
    $ReportLines += "Windows hardware evidence validation: PASS"
} else {
    $ReportLines += "Windows hardware evidence validation: FAIL"
}
$ReportLines += "Device type: $DeviceType"
$ReportLines += "Expected sample rate: $ExpectedSampleRate Hz"
$ReportLines += "Expected matrix cases: $($ExpectedCases.Count)"
$ReportLines += "Recorded matrix rows: $($Rows.Count)"
$ReportLines += "Eligible device records checked: $(@($Devices).Count)"
$ReportLines += "Route stability telemetry events: $($StabilityTelemetry.Count)"
if ($Warnings.Count -gt 0) {
    $ReportLines += ""
    $ReportLines += "Warnings:"
    foreach ($Warning in $Warnings) {
        $ReportLines += "- $Warning"
    }
}
if ($Failures.Count -gt 0) {
    $ReportLines += ""
    $ReportLines += "Failures:"
    foreach ($Failure in $Failures) {
        $ReportLines += "- $Failure"
    }
}
$ReportLines | Set-Content -LiteralPath $ReportPath

if ($Failures.Count -gt 0) {
    Write-Host `
        "Windows hardware evidence validation failed with $($Failures.Count) issue(s). See $ReportPath"
    foreach ($Failure in $Failures) {
        Write-Host "  - $Failure"
    }
    throw "Hardware evidence is incomplete or unsafe; sign-off is blocked."
}

Write-Host "Windows hardware evidence validation: PASS"
if ($Warnings.Count -gt 0) {
    foreach ($Warning in $Warnings) {
        Write-Warning $Warning
    }
}
Write-Host "Validation report: $ReportPath"
