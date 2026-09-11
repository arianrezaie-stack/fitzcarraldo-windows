# Windows hardware and acoustic stability test

Use the Release build on the target Windows machine. Test every backend that
the build exposes: WASAPI Exclusive, WASAPI Shared, DirectSound, and ASIO when
the separately supplied ASIO SDK is used.

## Safety

- Put a physical mute or amplifier power control within reach.
- Begin with amplifier/output gain at minimum and keep listeners away from the
  loudspeaker during calibration.
- Use one microphone/loudspeaker route first. Never exceed the engine's 0.08
  calibration level limit.

## Hardware matrix

For each supported backend/interface combination, record backend, driver
version, sample rate, buffer size, measured delay, callback CPU, and xrun count.
Use one physical interface for input and output unless the devices are
externally clocked.

## Device filtering and route-slot capture

Run the route session on a Windows machine with at least one connected USB
interface and at least one available built-in, HDMI, Bluetooth, or virtual
endpoint:

```powershell
.\scripts\windows-hardware-session.ps1 `
  -DeviceType "Windows Audio (Exclusive Mode)" `
  -InputDevice "Exact USB input name" `
  -OutputDevice "Exact USB output name" `
  -InterfaceModel "Interface model" `
  -DriverVersion "Driver version" `
  -SecondsPerRouteCount 5
```

Use the default 1800-second route hold for sign-off; the shorter value above is
only a smoke test. The session saves both the Windows endpoint inventory and
the native `devices` event. The evidence validator requires every emitted
record to have an exact device name, input/output direction, a positive channel
count, channel labels when supplied by the driver, `hardwareEligible: true`, and
`USB` or `Ethernet audio` transport metadata. It also fails if blocked
built-in, HDMI/DisplayPort, Bluetooth, or virtual endpoint text reaches the
renderer, or if the live inventory does not contain USB plus a built-in or
virtual endpoint to exercise the filter.

The same session runs four mono route slots on the selected exact input/output
device pair with enabled states `true, false, true, false`. The saved
`route-stability-*.jsonl` evidence must show four stable route indices in every
telemetry event, including the disabled slots. This is separate from the
1-through-8 route matrix so a compacted route list cannot hide an indexing
regression.

The session also keeps one four-route native process running while it exercises
route arming changes. The saved `notch-redistribution-*.jsonl` evidence must
show capacities of `6,6,6,6`, then `8,8,8,0`, `12,12,0,0`, and
`24,0,0,0`, followed by `12,12,0,0`, `8,8,8,0`, and `6,6,6,6` again.
Each step must retain four route records, report `running: true`, keep active
cuts within capacity, and report zero active cuts for disarmed routes. The
validator also rejects engine errors, stop events, missing health telemetry,
and non-finite callback CPU, xrun, peak, or clock readings.

1. Run at 48 kHz with 64, 128, and 256-sample buffers for 30 minutes each.
   Record the smallest stable setting rather than assuming every endpoint
   accepts the requested buffer.
2. Verify routing stops cleanly on device disconnect and does not reconnect to
   a different output silently.
3. Confirm calibration detects the impulse, measures a plausible stable delay
   (three runs within two samples), covers 20 Hz through the device Nyquist
   limit, and reloads the same baseline after restarting the app. The packaged
   app restart sequence below is the required route-specific evidence; do not
   substitute the native persistence unit test for this physical check.
4. Confirm silence/noise below the correlation threshold fails calibration and
   does not overwrite the last valid baseline.

### Route reset and packaged-app restart

The hardware session script pauses for this UI/device-lifecycle check after
the packaged app startup check. It writes
`route-reset-restart-<backend>.json` and the evidence validator requires this
file to report `pass`.

1. In the open packaged app, select the exact shared input/output interface
   and map two mono routes. Calibrate both routes. Verify both route cards say
   `baseline saved`.
2. In the calibration panel, select only one of those routes and choose
   `Reset Route N`. Verify that route says `needs calibration` and the other
   route still says `baseline saved`.
3. Close the packaged app completely. Confirm the process has exited before
   allowing the script to reopen it.
4. After the app reopens, select the same mapped routes if needed. Verify the
   reset route still says `needs calibration` and the other route still says
   `baseline saved`.
5. Recalibrate only the reset route. Verify it returns to `baseline saved` and
   the other route remains `baseline saved` with its prior measurement.

Answer every prompt in `windows-hardware-session.ps1` from the visible route
cards, not from the calibration JSON file. A `pass` requires the reset route
and retained route numbers to be different and every checkpoint to be `yes`.

## Acoustic stability

Test speech and music presets separately with representative program material.
Run the controlled session from the app directory after the portable release
artifact and route matrix are complete:

```powershell
.\scripts\windows-acoustic-stimulus.ps1 `
  -DeviceType "Windows Audio (Exclusive Mode)" `
  -InputDevice "Exact input name" `
  -OutputDevice "Exact output name" `
  -InterfaceModel "Interface model" `
  -DriverVersion "Driver version"
```

Replace `-DeviceType` with the exact backend name emitted by `list_devices`,
such as `Windows Audio (Exclusive Mode)`, `DirectSound`, or `ASIO`.

The default `-SecondsPerPhase 600` is the sign-off hold time. The script runs four phases for
each preset: program-only baseline, one feedback tone, two simultaneous
feedback tones at different frequencies, and source removal/release. It saves
the raw JSONL telemetry plus a results file under `windows-validation-output`.
The two tones must be introduced acoustically through the intended
microphone/loudspeaker path; do not inject them into the engine's stdin.

1. Raise loop gain slowly until a stable tone begins. Confirm engagement is
   visible in analyzer telemetry and is followed by one bounded notch.
2. Hold for 10 minutes. Confirm no more than six cuts, no cut deeper than
   -12 dB for speech or -9 dB for music, finite output, and no callback xruns.
3. Remove the feedback source. Confirm hysteresis prevents rapid on/off chatter
   and the notch releases gradually without an audible click.
4. Sweep a sine tone and play sustained music. Confirm broadband/program peaks
   do not accumulate permanent notches.
5. Repeat with two simultaneous feedback tones and on every enabled mono route.

For each preset, attach these observations to the results file:

- Interface model, backend, driver version, sample rate, actual buffer,
  input/output gain, and the normalized operating level.
- The captured audio endpoint inventory, emitted eligible device records, and
  route-stability evidence must be attached to the Windows session output.
- Room layout, microphone and loudspeaker positions, program material, and
  whether the same physical interface supplied input and output.
- Each tone's approximate frequency, the deepest reported cut, maximum active
  notch count, and whether both tones were tracked at once.
- Whether release was gradual after source removal, whether chatter occurred,
  and whether any click, audible discontinuity, unsafe output, or missed
  callback deadline was heard.

The engine telemetry is cumulative for each run. `xruns` must remain zero,
`nonFiniteOutputSamples` must remain zero, `activeNotches` must never exceed
`maximumAllowedNotches` (six), and `maximumCutDb` must remain no deeper than
-12 dB for speech or -9 dB for music. A passing automated summary does not
override a failed audible observation.

Stop and log a failure for any audible discontinuity, non-finite sample, missed
deadline, unsafe output, unstable delay, false persistent cut, or unbounded cut.