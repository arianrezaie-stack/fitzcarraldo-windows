# Werfeed Native Engine

GPLv3 C++20 console sidecar for direct, low-latency mono routing, acoustic
calibration, and bounded adaptive feedback suppression.
It communicates using one JSON object per stdin line and one event per stdout
line; stdout is protocol-only (diagnostics should be captured outside it).

## Build

Requires Windows, CMake 3.22+, MSVC with C++20 support, and Git/network access
for JUCE FetchContent. From this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

Werfeed is WASAPI-only. JUCE's ASIO and DirectSound backends are disabled.
WASAPI Exclusive Mode is preferred for the lowest practical latency; WASAPI
Shared Mode remains available as a compatibility fallback. Use the same
physical interface for capture and playback because independent devices can
drift unless their clocks are synchronized externally.

For a complete MSVC build, test, device-enumeration capture, and Electron
portable package, run this from the app directory in PowerShell:

```powershell
.\scripts\windows-release.ps1 -AsioSdkPath C:\sdk\asiosdk
```

The script writes evidence to `windows-validation-output` and the distributable
to `desktop-dist`. Omit `-AsioSdkPath` for the initial WASAPI-only pass.

After checking `device-enumeration-wasapi.jsonl` and
`device-enumeration-asio.jsonl` for the exact names, exercise the
physical mono routes (repeat once for WASAPI and once for ASIO):

```powershell
.\scripts\windows-hardware-session.ps1 `
  -DeviceType "Windows Audio" `
  -InputDevice "Exact input name" `
  -OutputDevice "Exact output name"
```

The session runs 1 through 8 one-to-one mono routes and asks for an audible
pass/fail confirmation after each run. Keep output gain low and a physical mute
within reach.

## Protocol

`{"type":"list_devices"}` reports input/output device records. Configure before
start, for example:

```json
{"type":"configure","deviceType":"Windows Audio (Exclusive Mode)","inputDevice":"Input","outputDevice":"Output","sampleRate":48000,"bufferSize":128,"inputChannels":2,"outputChannels":2,"routes":[{"input":0,"output":0},{"input":1,"output":1}]}
```

Commands are `list_devices`, `configure`, `start`, `stop`, `set_protection`,
and `start_calibration`. Protection accepts `enabled` and a `speech` or `music`
preset. Calibration accepts a zero-based route and a safe normalized level no
higher than 0.08. Events use `type`: `hello`, `devices`, `state`, `telemetry`,
`calibration`, or `error`.
Routes are ordered, mono, summed when sharing an output, and limited to eight.
While running, status events are capped at 10 Hz and expose actual device rate,
buffer size, callback CPU fraction, local deadline overruns, and input/output
peaks. Device configuration and route changes are deliberately rejected while
running to keep callback memory immutable.
Only device types whose JUCE name starts with `Windows Audio` are accepted.

Calibration emits a bounded impulse followed by a two-second 20 Hz–20 kHz
logarithmic sweep, finds loop delay with normalized cross-correlation, and
persists the deconvolved 96-bin response per device route under the user's
application-data directory. The detector uses baseline-relative hysteresis and
at most six smoothly-ramped notch filters (speech: -12 dB, music: -9 dB).

`xruns` is a local callback-deadline estimate, not a driver reported glitch
counter. Hardware and acoustic tests must be performed on Windows with the
intended microphones, outputs, room, and gain structure. Start at low gain,
keep a physical mute available, and never calibrate with listeners near a
loudspeaker. Audio callbacks do no allocation, locking, console IO, or JSON
work; the command and reporting threads may allocate/lock.
