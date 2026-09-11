---
name: Clock telemetry
description: Separation of callback scheduling, deadline, driver, and device-clock measurements for live audio diagnostics
---

Treat callback timing jitter, callback deadline misses, driver-reported xruns, and device-clock drift as separate telemetry concepts.

**Why:** A callback can arrive irregularly without the hardware sample clock drifting, and a driver xrun is not the same event as a local callback deadline miss. Combining them into one stability score obscures the cause of a 0.88 ms reading.

**How to apply:** Keep the initial portable measurement as a long-window backend frame-clock estimate when the public JUCE device API does not expose the already-open WASAPI `IAudioClock` or ASIO sample-position handle. Label that estimate explicitly and do not present it as direct native-clock evidence.