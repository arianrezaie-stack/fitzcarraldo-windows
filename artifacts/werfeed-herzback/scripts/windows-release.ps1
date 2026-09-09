param(
    [string]$AsioSdkPath = "",
    [ValidateSet("x64")]
    [string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$AppRoot = Split-Path -Parent $PSScriptRoot
$EngineRoot = Join-Path $AppRoot "native-engine"
$BuildRoot = Join-Path $EngineRoot "build-windows"
$EvidenceRoot = Join-Path $AppRoot "windows-validation-output"
$StagingRoot = Join-Path $AppRoot "engine\win32-x64"
$BuildMode = if ($AsioSdkPath) { "asio" } else { "wasapi" }
$TranscriptPath = Join-Path $EvidenceRoot "build-transcript-$BuildMode.txt"
$EnumerationPath = Join-Path $EvidenceRoot "device-enumeration-$BuildMode.jsonl"

New-Item -ItemType Directory -Force -Path $EvidenceRoot, $StagingRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $TranscriptPath, $EnumerationPath
Start-Transcript -Path $TranscriptPath

try {
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
    } else {
        $CmakeArguments += "-DWERFEED_ENABLE_ASIO=OFF"
    }

    Write-Host "Configuring and building the native engine..."
    & cmake @CmakeArguments
    cmake --build $BuildRoot --config Release --parallel
    ctest --test-dir $BuildRoot -C Release --output-on-failure

    $BuiltEngine = Join-Path $BuildRoot "Release\werfeed-engine.exe"
    if (-not (Test-Path $BuiltEngine)) {
        throw "Native build did not produce $BuiltEngine"
    }

    Write-Host "Capturing real audio-device enumeration..."
    '{"type":"list_devices"}' |
        & $BuiltEngine |
        Tee-Object -FilePath $EnumerationPath
    if (-not (Select-String -Quiet -Path $EnumerationPath -Pattern '"type"\s*:\s*"devices"')) {
        throw "The native engine did not emit a devices event."
    }

    Copy-Item -Force $BuiltEngine (Join-Path $StagingRoot "werfeed-engine.exe")

    Write-Host "Building the engine-backed Electron portable executable..."
    Push-Location $AppRoot
    try {
        pnpm run desktop:win
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

    Get-FileHash -Algorithm SHA256 $BuiltEngine, $Portable.FullName |
        Format-Table Path, Hash -AutoSize |
        Out-String -Width 4096 |
        Set-Content (Join-Path $EvidenceRoot "sha256-$BuildMode.txt")

    @"
Complete these hardware checks before sign-off:
[ ] WASAPI device names appear in device-enumeration.jsonl.
[ ] ASIO device names appear after an ASIO-enabled build.
[ ] Mono pass-through works for 1, 2, 3, 4, 5, 6, 7, and 8 routes.
[ ] Tested at 48 kHz with 64, 128, and 256-sample buffers.
[ ] Each buffer setting ran for 30 minutes without unsafe output.
[ ] Device disconnect stops routing and never selects another output silently.
[ ] Record interface model, driver version, measured delay, CPU, and xruns below.

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
} finally {
    Stop-Transcript
}