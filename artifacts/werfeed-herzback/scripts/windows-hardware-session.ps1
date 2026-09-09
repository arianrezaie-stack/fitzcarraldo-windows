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
    [int]$SecondsPerRouteCount = 10
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$AppRoot = Split-Path -Parent $PSScriptRoot
$Engine = Join-Path $AppRoot "engine\win32-x64\werfeed-engine.exe"
$EvidenceRoot = Join-Path $AppRoot "windows-validation-output"
$SafeDeviceType = $DeviceType -replace '[^A-Za-z0-9_.-]', '_'
$SessionLog = Join-Path $EvidenceRoot "routes-$SafeDeviceType-$BufferSize.jsonl"
$Results = Join-Path $EvidenceRoot "routes-$SafeDeviceType-$BufferSize-results.txt"

if (-not (Test-Path $Engine)) {
    throw "Run .\scripts\windows-release.ps1 first; staged engine not found at $Engine"
}

New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $SessionLog, $Results

Write-Host "SAFETY: Set output gain to minimum and keep a physical mute within reach."
$SafetyReady = Read-Host "Type READY when the test area is safe"
if ($SafetyReady -ne "READY") {
    throw "Hardware session cancelled because safety was not confirmed."
}

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

    Write-Host "Testing $RouteCount mono route(s) for $SecondsPerRouteCount seconds..."
    Start-Sleep -Seconds $SecondsPerRouteCount
    $Process.StandardInput.WriteLine('{"type":"stop"}')
    $Process.StandardInput.Close()

    if (-not $Process.WaitForExit(10000)) {
        $Process.Kill()
        throw "Native engine did not exit after route test $RouteCount."
    }

    $Stdout = $StdoutTask.Result
    $Stderr = $StderrTask.Result
    Add-Content $SessionLog "# route-count=$RouteCount"
    Add-Content $SessionLog $Stdout.TrimEnd()
    if ($Stderr) {
        Add-Content $SessionLog "# stderr: $($Stderr.TrimEnd())"
    }

    if ($Stdout -match '"type"\s*:\s*"error"') {
        Add-Content $Results "$RouteCount routes: ENGINE ERROR"
        Write-Warning "The engine reported an error. See $SessionLog"
        continue
    }

    $AudibleResult = Read-Host "Did all $RouteCount route(s) pass clean audio on the matching outputs? (yes/no)"
    Add-Content $Results "$RouteCount routes: $AudibleResult"
}

Write-Host "Hardware route session complete."
Write-Host "Upload $SessionLog and $Results with the other validation evidence."