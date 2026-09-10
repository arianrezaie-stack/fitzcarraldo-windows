param(
    [string]$AsioSdkPath = "",
    [ValidateSet("x64")]
    [string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true
Set-StrictMode -Version Latest

$AppRoot = Split-Path -Parent $PSScriptRoot
$EngineRoot = Join-Path $AppRoot "native-engine"
$BuildRoot = Join-Path $EngineRoot "build-windows"
$EvidenceRoot = Join-Path $AppRoot "windows-validation-output"
$StagingRoot = Join-Path $AppRoot "engine\win32-x64"
$BuildMode = if ($AsioSdkPath) { "asio" } else { "multi-backend" }
$TranscriptPath = Join-Path $EvidenceRoot "build-transcript-$BuildMode.txt"
$EnumerationPath = Join-Path $EvidenceRoot "device-enumeration-$BuildMode.jsonl"
$NativeLogPath = Join-Path $EvidenceRoot "native-commands-$BuildMode.log"
$PortableValidationPath = Join-Path $EvidenceRoot "portable-startup-$BuildMode.jsonl"
$PortableEngineOutputPath = Join-Path $EvidenceRoot "portable-engine-output-$BuildMode.jsonl"
$BuildIdentifierPath = Join-Path $EvidenceRoot "portable-build-identifier-$BuildMode.json"
$Phase = "initialization"
$TranscriptStarted = $false

function Invoke-NativeLogged {
    param(
        [Parameter(Mandatory)]
        [string]$Executable,
        [Parameter(Mandatory)]
        [string[]]$Arguments,
        [Parameter(Mandatory)]
        [string]$LogPath
    )

    $PreviousPreference = $PSNativeCommandUseErrorActionPreference
    try {
        $PSNativeCommandUseErrorActionPreference = $false
        & $Executable @Arguments 2>&1 | Tee-Object -FilePath $LogPath -Append
        $ExitCode = $LASTEXITCODE
    } finally {
        $PSNativeCommandUseErrorActionPreference = $PreviousPreference
    }
    if ($ExitCode -ne 0) {
        throw "$Executable exited with code $ExitCode."
    }
}

New-Item -ItemType Directory -Force -Path $EvidenceRoot, $StagingRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue `
    $TranscriptPath, $EnumerationPath, $NativeLogPath, $PortableValidationPath,
    $PortableEngineOutputPath, $BuildIdentifierPath
Start-Transcript -Path $TranscriptPath
$TranscriptStarted = $true

try {
    $Phase = "toolchain inspection"
    Write-Host "Recording Windows and toolchain information..."
    Get-ComputerInfo |
        Select-Object WindowsProductName, WindowsVersion, OsBuildNumber, OsArchitecture |
        Format-List
    cmake --version
    pnpm --version

    $CmakeArguments = @(
        "-S", $EngineRoot,
        "-B", $BuildRoot,
        "-G", "Visual Studio 17 2022",
        "-A", $Architecture,
        "-DBUILD_TESTING=ON"
    )
    if ($AsioSdkPath) {
        $AsioHeader = Join-Path $AsioSdkPath "common\asio.h"
        if (-not (Test-Path $AsioHeader)) {
            throw "ASIO SDK is invalid: expected $AsioHeader"
        }
        $CmakeArguments += "-DWERFEED_ENABLE_ASIO=ON"
        $CmakeArguments += "-DWERFEED_ASIO_SDK_PATH=$AsioSdkPath"
    }

    $Phase = "native engine configuration"
    Write-Host "Configuring and building the native engine..."
    Invoke-NativeLogged -Executable "cmake" -Arguments $CmakeArguments -LogPath $NativeLogPath
    $Phase = "native engine compilation"
    Invoke-NativeLogged -Executable "cmake" `
        -Arguments @("--build", $BuildRoot, "--config", "Release", "--parallel", "--verbose") `
        -LogPath $NativeLogPath
    $Phase = "native engine tests"
    Invoke-NativeLogged -Executable "ctest" `
        -Arguments @("--test-dir", $BuildRoot, "-C", "Release", "--output-on-failure") `
        -LogPath $NativeLogPath

    $BuiltEngine = Join-Path $BuildRoot "Release\werfeed-engine.exe"
    if (-not (Test-Path $BuiltEngine)) {
        throw "Native build did not produce $BuiltEngine"
    }

    $Phase = "audio device enumeration"
    Write-Host "Capturing real audio-device enumeration..."
    '{"type":"list_devices"}' |
        & $BuiltEngine |
        Tee-Object -FilePath $EnumerationPath
    if (-not (Select-String -Quiet -Path $EnumerationPath -Pattern '"type"\s*:\s*"devices"')) {
        throw "The native engine did not emit a devices event."
    }

    Copy-Item -Force $BuiltEngine (Join-Path $StagingRoot "werfeed-engine.exe")

    $Phase = "Electron portable packaging"
    Write-Host "Building the engine-backed Electron portable executable..."
    Push-Location $AppRoot
    try {
        Invoke-NativeLogged -Executable "pnpm" `
            -Arguments @("run", "desktop:win") `
            -LogPath $NativeLogPath
    } finally {
        Pop-Location
    }

    $Portable = Get-ChildItem (Join-Path $AppRoot "desktop-dist") `
        -Filter "Werfeed-Herzback-*-x64.exe" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $Portable) {
        throw "Electron packaging did not produce an x64 portable executable."
    }

    $Phase = "packaged portable startup validation"
    Write-Host "Launching the fresh portable executable and validating the renderer device selector..."
    $env:WERFEED_VALIDATION_OUTPUT = $PortableValidationPath
    $env:WERFEED_VALIDATION_ENGINE_OUTPUT = $PortableEngineOutputPath
    try {
        $ValidationProcess = Start-Process -FilePath $Portable.FullName -PassThru -WindowStyle Hidden
        if (-not $ValidationProcess.WaitForExit(45000)) {
            $ValidationProcess.Kill()
            throw "Portable startup validation timed out after 45 seconds."
        }
        if ($ValidationProcess.ExitCode -ne 0) {
            throw "Portable startup validation exited with code $($ValidationProcess.ExitCode)."
        }
    } finally {
        Remove-Item Env:WERFEED_VALIDATION_OUTPUT -ErrorAction SilentlyContinue
        Remove-Item Env:WERFEED_VALIDATION_ENGINE_OUTPUT -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path $PortableValidationPath)) {
        throw "Portable startup validation did not produce $PortableValidationPath."
    }
    $ValidationRecords = @(Get-Content -Path $PortableValidationPath | ForEach-Object { $_ | ConvertFrom-Json })
    if (@($ValidationRecords | Where-Object { $_.type -eq "engine_protocol_error" }).Count -gt 0) {
        throw "Portable startup emitted ENGINE_PROTOCOL_ERROR. See $PortableValidationPath."
    }
    $ValidationResult = $ValidationRecords | Where-Object { $_.type -eq "validation_result" } | Select-Object -Last 1
    if (-not $ValidationResult -or $ValidationResult.pass -ne $true) {
        throw "Portable startup validation did not pass. See $PortableValidationPath."
    }
    $RendererDevices = $ValidationRecords | Where-Object { $_.type -eq "renderer_devices" } | Select-Object -Last 1
    if (-not $RendererDevices) {
        throw "The packaged renderer did not report a devices event."
    }
    if ([int]$RendererDevices.pairCount -lt 1) {
        Write-Warning "The validation runner reported no compatible audio device pair; hardware validation must run on the target Windows machine."
    }

    $PortableHash = (Get-FileHash -Algorithm SHA256 $Portable.FullName).Hash
    $EngineHash = (Get-FileHash -Algorithm SHA256 $BuiltEngine).Hash
    [ordered]@{
        packageVersion = (Get-Content (Join-Path $AppRoot "package.json") | ConvertFrom-Json).version
        portableArtifact = $Portable.Name
        portablePath = $Portable.FullName
        portableBytes = $Portable.Length
        portableSha256 = $PortableHash
        engineArtifact = "werfeed-engine.exe"
        engineSha256 = $EngineHash
        rendererDevicePairs = [int]$RendererDevices.pairCount
        hardwareAvailable = ([int]$RendererDevices.pairCount -gt 0)
        validationEvidence = (Split-Path -Leaf $PortableValidationPath)
        exactEngineOutput = (Split-Path -Leaf $PortableEngineOutputPath)
    } | ConvertTo-Json | Set-Content $BuildIdentifierPath

    Get-FileHash -Algorithm SHA256 $BuiltEngine, $Portable.FullName |
        Format-Table Path, Hash -AutoSize |
        Out-String -Width 4096 |
        Set-Content (Join-Path $EvidenceRoot "sha256-$BuildMode.txt")

    @"
Complete these hardware checks before sign-off:
[ ] Portable executable starts and displays the control surface.
[ ] Requested WASAPI input/output names appear in hardware-device-enumeration-*.jsonl.
[ ] Target capture includes a USB endpoint plus a built-in or virtual endpoint in audio-endpoint-inventory-*.json.
[ ] Every emitted device record has direction, positive channels, channel labels when available, exact name, and USB/Ethernet transport metadata.
[ ] route-stability-*.jsonl keeps disabled route slots at their original indices.
[ ] Mono pass-through works for 1, 2, 3, 4, 5, 6, 7, and 8 routes.
[ ] Tested at 48 kHz with 64, 128, and 256-sample buffers.
[ ] Each buffer setting ran for 30 minutes without unsafe output.
[ ] Disconnect test emits device_stopped and never selects another output silently.
[ ] hardware-matrix-*.csv records actual rate, buffer, callback CPU, and xruns.
[ ] hardware-session-*-results.txt records measured delay and tester notes.
[ ] acoustic-stimulus-*-results.txt records speech/music one- and two-tone telemetry, release, and tester notes.

Interface:
Driver/mode:
Driver version:
Measured delay:
Callback CPU:
Xruns:
Tester notes:
"@ | Set-Content (Join-Path $EvidenceRoot "hardware-checklist.txt")

    Write-Host ""
    Write-Host "Build succeeded: $($Portable.FullName)"
    Write-Host "Upload windows-validation-output and the portable executable for final sign-off."
} catch {
    $Failure = $_
    if ($TranscriptStarted) {
        Stop-Transcript | Out-Null
        $TranscriptStarted = $false
    }
    $TranscriptTail = if (Test-Path $TranscriptPath) {
        (Get-Content -Path $TranscriptPath -Tail 120 | Out-String)
    } else {
        "Build transcript was not created."
    }
    $NativeTail = if (Test-Path $NativeLogPath) {
        (Get-Content -Path $NativeLogPath -Tail 35 | Out-String)
    } else {
        "Native command log was not created."
    }
    $NativeErrors = if (Test-Path $NativeLogPath) {
        (Get-Content -Path $NativeLogPath |
            Select-String -Pattern "CMake Error|fatal error|error C[0-9]+|error LNK[0-9]+|error MSB[0-9]+" |
            Select-Object -Last 50 |
            ForEach-Object { $_.Line } |
            Out-String)
    } else {
        ""
    }
    $Annotation = "Windows release failed during ${Phase}: $($Failure.Exception.Message)`n$NativeErrors`n$NativeTail`n$TranscriptTail"
    $Annotation = $Annotation.Replace("%", "%25").Replace("`r", "%0D").Replace("`n", "%0A")
    Write-Host "::error file=artifacts/werfeed-herzback/scripts/windows-release.ps1::$Annotation"
    throw $Failure
} finally {
    if ($TranscriptStarted) {
        Stop-Transcript | Out-Null
    }
}
