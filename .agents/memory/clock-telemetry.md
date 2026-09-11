---
name: Clock telemetry
description: Separation of callback scheduling, deadline, driver, and device-clock measurements for live audio diagnostics
---

Treat callback timing jitter, callback deadline misses, driver-reported xruns, and device-clock drift as separate telemetry concepts.

**Why:** A callback can arrive irregularly without the hardware sample clock drifting, and a driver xrun is not the same event as a local callback deadline miss. Combining them into one stability score obscures the cause of a 0.88 ms reading.

**How to apply:** Keep the initial portable measurement as a long-window backend frame-clock estimate when the public JUCE device API does not expose the already-open WASAPI `IAudioClock` or ASIO sample-position handle. Label that estimate explicitly and do not present it as direct native-clock evidence.

For low-latency operation, measure callback execution duration separately from callback-to-callback scheduling jitter, and scope diagnostic peak/non-finite scans to configured route channels while keeping output sanitization enabled.

**Why:** Full-device scans add work to the realtime callback even when most hardware channels are unused, while a late callback and a slow callback require different fixes. Removing safety sanitization would protect latency at the cost of transparent, valid output.

**How to apply:** Use execution time and peak execution time to identify DSP cost; use jitter to identify host scheduling. Preserve route input sanitization and route output non-finite protection, but do not scan inactive device channels in the hot path.