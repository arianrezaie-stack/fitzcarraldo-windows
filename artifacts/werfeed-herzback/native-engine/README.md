# Werfeed Native Engine

GPLv3 C++20 console sidecar for direct, low-latency mono routing, acoustic
calibration, and bounded adaptive feedback suppression.
It communicates using one JSON object per stdin line and one event per stdout
line; stdout is protocol-only. JUCE and device-driver diagnostics are redirected
to stderr so they cannot corrupt the Electron protocol.

## Build

Requires Windows, CMake 3.22+, MSVC with C++20 support, and Git/network access
for JUCE FetchContent. From this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

The default Windows build includes WASAPI and DirectSound. WASAPI Exclusive
Mode is preferred for the lowest practical latency; WASAPI Shared Mode remains
available as a compatibility fallback. Use the same physical interface for
capture and playback because independent devices can drift unless their clocks
are synchronized externally.

ASIO is optional because the Steinberg SDK must be supplied separately:

```powershell
cmake -S . -B build -DWERFEED_ENABLE_ASIO=ON -DWERFEED_ASIO_SDK_PATH=C:\sdk\asiosdk
```

The selected device type is the selected backend. Werfeed does not relabel an
ASIO or DirectSound device as WASAPI.

For a complete MSVC build, test, device-enumeration capture, and Electron
portable package, run this from the app directory in PowerShell:

```powershell
.\scripts\windows-release.ps1 -AsioSdkPath C:\sdk\asiosdk
```

The script writes evidence to `windows-validation-output` and the distributable
to `desktop-dist`. Omit `-AsioSdkPath` for the initial WASAPI-only pass.

After checking the captured WASAPI enumeration for the exact names, exercise
the physical mono routes:

```powershell
.\scripts\windows-hardware-session.ps1 `
  -DeviceType "Windows Audio" `
  -InputDevice "Exact input name" `
  -OutputDevice "Exact output name" `
  -InterfaceModel "Interface model" `
  -DriverVersion "Driver version"
```

The session launches the latest portable executable, confirms startup, then
runs 1 through 8 one-to-one mono routes at 48 kHz for 64, 128, and 256-sample
buffers. It records the actual device rate and buffer, maximum callback CPU,
maximum local xrun estimate, engine errors, audible pass/fail, and tester
metadata in `windows-validation-output`. Use a smaller
`-SecondsPerRouteCount` only for a smoke test; the sign-off matrix uses the
30-minute default. Keep output gain low and a physical mute within reach.

For live protection sign-off, run the acoustic stimulus session after the
route matrix:

```powershell
.\scripts\windows-acoustic-stimulus.ps1 `
  -DeviceType "Windows Audio (Exclusive Mode)" `
  -InputDevice "Exact input name" `
  -OutputDevice "Exact output name" `
  -InterfaceModel "Interface model" `
  -DriverVersion "Driver version"
```

It exercises speech and music program material through program-only, one-tone,
two-tone, and source-removal phases, then records raw telemetry and tester
notes. The default `-SecondsPerPhase 600` provides the required 10-minute
hold for each phase; use a smaller value only for a smoke test.

## Protocol

`{"type":"list_devices"}` reports input/output device records, including the
available channel count for each endpoint. Configure before start, for example:

```json
{"type":"configure","deviceType":"Windows Audio (Exclusive Mode)","inputDevice":"Input","outputDevice":"Output","sampleRate":48000,"bufferSize":128,"inputChannels":4,"outputChannels":4,"routes":[{"input":0,"output":0,"enabled":true,"suppression":0.75},{"input":1,"output":1,"enabled":true,"suppression":0.75},{"input":2,"output":2,"enabled":true,"suppression":0.75},{"input":3,"output":3,"enabled":true,"suppression":0.75}]}
```

Commands are `list_devices`, `configure`, `start`, `stop`, `set_protection`,
`start_calibration`, and the validation-only `test_marker`. Protection accepts
`enabled` and a `speech` or `music` preset. A route-specific suppression amount
is set with `{"route":0,"suppression":0.75}` and remains in the range 0–1.
Calibration accepts a zero-based route and a safe normalized level no higher
than 0.08. The Electron desktop bridge adds the bundled
`calibration-announcement.mp3` path to this command. The native engine plays
that announcement on the selected route output, normalizes only peaks above
the calibration level, waits one second in silence, and then emits the impulse
and sweep. Events use `type`:
`hello`, `devices`, `state`, `telemetry`, `calibration`, `test_marker`, or
`error`.
Routes are ordered, independent mono channel maps, summed when sharing an
output, and limited to eight. The physical input/output device pair and
backend are shared by the engine so all routes use one stable device clock;
each route can independently select its input channel and output channel.
While running, status events are capped at 10 Hz and expose actual device rate,
buffer size, callback CPU fraction, local deadline overruns, cumulative
non-finite input/output sample counts, and input/output peaks. Each telemetry
event also has a monotonically increasing sequence number. Device
configuration and route changes are deliberately rejected while running to keep
callback memory immutable.
All device types compiled into the native engine are enumerated and accepted.

Calibration emits a bounded impulse followed by a two-second 20 Hz–20 kHz
logarithmic sweep, finds loop delay with normalized cross-correlation, and
persists the sweep-reference 256-bin response per device route under the user's
application-data directory. Telemetry exposes `routeTelemetry` for every
configured route, including its live spectrum, active notches, suppression
amount, calibration response, and measured delay. The detector uses the
calibration response as a priority baseline, a fast stable/rising-peak speech
gate, a more conservative music gate, and at most six smoothly-ramped notch
filters. Analyzer updates use overlapping 2048-sample FFT windows with 256
display bins. The route suppression amount scales the maximum cut from 0 to
-12 dB in both protection modes. Below 300 Hz, notch Q decreases progressively
to widen the protection band for low-frequency room feedback.

`xruns` is a local callback-deadline estimate, not a driver reported glitch
counter. Hardware and acoustic tests must be performed on Windows with the
intended microphones, outputs, room, and gain structure. Start at low gain,
keep a physical mute available, and never calibrate with listeners near a
loudspeaker. Audio callbacks do no allocation, locking, console IO, or JSON
work; the command and reporting threads may allocate/lock.
