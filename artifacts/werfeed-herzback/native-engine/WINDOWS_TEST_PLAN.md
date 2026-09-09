# Windows hardware and acoustic stability test

Use the Release build on the target Windows machine. Test WASAPI Exclusive Mode
first, then repeat in WASAPI Shared Mode as the compatibility fallback.

## Safety

- Put a physical mute or amplifier power control within reach.
- Begin with amplifier/output gain at minimum and keep listeners away from the
  loudspeaker during calibration.
- Use one microphone/loudspeaker route first. Never exceed the engine's 0.08
  calibration level limit.

## Hardware matrix

For each supported interface, record WASAPI mode, driver version, sample rate,
buffer size, measured delay, callback CPU, and xrun count. Use one physical
interface for input and output unless the devices are externally clocked.

1. Run at 48 kHz with 64, 128, and 256-sample buffers for 30 minutes each.
   Record the smallest stable setting rather than assuming every endpoint
   accepts the requested buffer.
2. Verify routing stops cleanly on device disconnect and does not reconnect to
   a different output silently.
3. Confirm calibration detects the impulse, measures a plausible stable delay
   (three runs within two samples), covers 20 Hz through the device Nyquist
   limit, and reloads the same baseline after restarting the app.
4. Confirm silence/noise below the correlation threshold fails calibration and
   does not overwrite the last valid baseline.

## Acoustic stability

Test speech and music presets separately with representative program material.

1. Raise loop gain slowly until a stable tone begins. Confirm engagement is
   visible in analyzer telemetry and is followed by one bounded notch.
2. Hold for 10 minutes. Confirm no more than six cuts, no cut deeper than
   -12 dB for speech or -9 dB for music, finite output, and no callback xruns.
3. Remove the feedback source. Confirm hysteresis prevents rapid on/off chatter
   and the notch releases gradually without an audible click.
4. Sweep a sine tone and play sustained music. Confirm broadband/program peaks
   do not accumulate permanent notches.
5. Repeat with two simultaneous feedback tones and on every enabled mono route.

Stop and log a failure for any audible discontinuity, non-finite sample, missed
deadline, unsafe output, unstable delay, false persistent cut, or unbounded cut.