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

Device discovery is intentionally limited to live-sound transports. Records
must identify USB in the native endpoint or driver name, or identify an
approved audio-over-Ethernet protocol: Dante, Waves SoundGrid, AES67, RAVENNA,
or AVB. Built-in motherboard audio, HDMI/DisplayPort, Bluetooth, virtual
cables, loopback devices, and common virtual ASIO drivers are excluded before
they reach the renderer. Every emitted record includes `transport: "USB"` or
`transport: "Ethernet audio"` and `hardwareEligible: true`.

Windows release builds enable JUCE's native ASIO device type using the official
Steinberg ASIO SDK 2.3.3 archive, verified by SHA-256 before compilation.
WASAPI and DirectSound remain compiled alongside ASIO. ASIO drivers therefore
appear through the same independent mono input and output selectors as the
other Windows backends.

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
maximum callback execution/jitter values, callback deadline misses, both driver
and local xrun counts, non-finite input/output counters, engine errors, audible
pass/fail, and tester metadata in `windows-validation-output`. Use a smaller
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

`{"type":"list_devices"}` reports only eligible input/output device records,
including the transport label, available channel count, channel labels when the
backend provides them, and an `interfaceName` family label for each endpoint.
The renderer exposes all reported mono inputs and outputs in the audio backend
panel. Selecting a route loads that route's two channel choices; route cards
show the selected values without duplicating the controls. The endpoint `name`
values are retained for native configuration, and all armed routes must use the
exact same physical input/output device pair and backend. Configure before
start, for example:

```json
{"type":"configure","deviceType":"Windows Audio (Exclusive Mode)","inputDevice":"Input","outputDevice":"Output","sampleRate":48000,"bufferSize":128,"inputChannels":4,"outputChannels":4,"routes":[{"input":0,"output":0,"enabled":true,"depth":0.75,"sensitivity":0.75},{"input":1,"output":1,"enabled":true,"depth":0.75,"sensitivity":0.75},{"input":2,"output":2,"enabled":true,"depth":0.75,"sensitivity":0.75},{"input":3,"output":3,"enabled":true,"depth":0.75,"sensitivity":0.75}]}
```

Commands are `list_devices`, `configure`, `start`, `stop`, `set_protection`,
`start_calibration`, and the validation-only `test_marker`. Protection accepts
`enabled` and a `speech` or `music` preset. A route-specific cut depth is set
with `{"route":0,"depth":0.75}`, while detector sensitivity is set with
`{"route":0,"sensitivity":0.75}`; both remain in the range 0–1. Higher
sensitivity lowers the feedback detection threshold and admits quieter candidates.
The detector threshold spans 0 dBFS at 0% sensitivity to −70 dBFS at 100%.
Calibration removes the measured broadband gain offset using the average from
200 Hz through 10 kHz, preserving the relative frequency response around a 0 dB
reference.
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
buffer size, callback CPU fraction, callback execution time and peak execution
time, smoothed callback jitter and peak callback jitter, callback deadline
misses, the driver's native xrun count, and cumulative non-finite input/output
sample counts and input/output peaks. Input diagnostics are limited to
configured route channels; output sanitization and peak measurement are limited
to channels used by configured routes. Each telemetry event also includes a
long-window device-clock drift estimate in ppm, its measurement age, readiness,
and source label. For WASAPI and ASIO this first implementation estimates the
effective backend frame clock from delivered callback frames against the host's
high-resolution steady clock; it does not claim direct access to the already-open
backend's private IAudioClock or ASIO sample-position handle. Static notch
frequency terms are calculated on the analysis thread so the realtime callback
does not evaluate `sin()` or `cos()` while refreshing coefficients. Each
telemetry event also has a monotonically increasing sequence number. Device
configuration and route changes are deliberately rejected while running to keep
callback memory immutable.
All compiled backends are enumerated, but only eligible live-sound transports
are emitted as selectable device records.

Calibration emits a bounded impulse followed by a two-second 20 Hz–20 kHz
logarithmic sweep, finds loop delay with normalized cross-correlation, and
persists the sweep-reference 256-bin response per device route under the user's
application-data directory. Telemetry exposes `routeTelemetry` for every
configured route, including its live spectrum, active notches, cut depth,
detector sensitivity, calibration response, and measured delay. The detector uses the
calibration response as a priority baseline, a fast stable/rising-peak speech
gate, a more conservative music gate, and at most six smoothly-ramped notch
filters. Analyzer updates use overlapping 2048-sample FFT windows with 256
display bins. The route depth amount reaches -14 dB at 70%, -24 dB at
80%, and approximately -38.4 dB at 100%. Below 300 Hz, notch Q decreases progressively
to widen the protection band for low-frequency room feedback. Feedback probes
between 300 Hz and 800 Hz initially apply 75% of the slider-defined maximum
cut instead of the normal 50% probe cut, while confirmed protection remains
limited by the same slider-defined maximum. Automatically engaged Music-mode
notches use an 8% lower Q than Speech-mode notches for a slightly wider band;
manual notches retain their existing Q. Automatic persistent holds require six
same-frequency recurrence confirmations within roughly 2.2 seconds and an
initial probe of at least 12 dB; latch values at or above 80% relax that to
three confirmations, roughly 2.6–2.8 seconds, and a 10 dB initial probe.
At 0% latch, held automatic notches are released on the fastest path when the
current detector no longer sees a qualifying feedback peak; explicit manual
notches remain held until cleared.

`xruns` remains a backwards-compatible alias for callback deadline misses.
`driverXruns` is the JUCE/backend-reported xrun count and may not be available
for every driver. Hardware and acoustic tests must be performed on Windows with the
intended microphones, outputs, room, and gain structure. Start at low gain,
keep a physical mute available, and never calibrate with listeners near a
loudspeaker. Audio callbacks do no allocation, locking, console IO, or JSON
work; the command and reporting threads may allocate/lock.
