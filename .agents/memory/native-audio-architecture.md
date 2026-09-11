---
name: Native audio architecture
description: Durable native-audio architecture and Windows driver constraint for Werfeed Herzback.
---

Use a separate GPLv3 C++20/JUCE sidecar for realtime audio. Electron/React must remain a control and telemetry client and must never simulate successful engine state.

**Why:** Audio capture, backend-specific routing, calibration, and adaptive DSP require deterministic native realtime processing; Chromium and Node are not appropriate audio callback hosts. The user chose explicit multi-backend selection rather than silently coercing devices between APIs.

**How to apply:** Keep allocations, locks, IPC, logging, and device discovery off the audio callback. Build and validate on Windows. Enumerate every compiled backend, label the selected backend, and configure exactly that backend. Prefer WASAPI Exclusive Mode when available.

The sidecar's stdout is a strict newline-delimited JSON protocol; route JUCE and driver diagnostics to stderr before opening or configuring audio devices.

**Why:** Device initialization can emit framework diagnostics at exactly the point the UI sends its first commands. A single plain-text stdout line makes Electron reject subsequent engine events and appear to have disabled controls.

**How to apply:** Install a JUCE logger that writes to stderr, and keep Electron's parser diagnostic-tolerant without treating non-protocol lines as engine events.

Serialize every protocol event with JUCE's `allOnOneLine=true`; the Electron reader frames messages by newline.

**Why:** Pretty-printed JSON is valid as a document but invalid as a newline-delimited protocol message, causing the device event and command responses to be discarded line by line.

**How to apply:** Treat one stdout line as exactly one complete JSON object and test the framing separately from the DSP.

For background feedback analysis, use a single-producer/single-consumer sample handoff from the audio callback and publish notch targets as coherent per-slot snapshots. The callback may only pull bounded atomic state and run filters; processor reset/configuration must share a non-realtime mutex with the analyzer.

**Why:** Moving FFT work off the callback removes its largest timing spikes, but independently published frequency/depth/Q values can tear across threads, and reset can race the analyzer during device reconfiguration.

**How to apply:** Keep FFT/detection state background-owned, keep biquad state callback-owned, guard lifecycle/reset against the analyzer outside the callback, and validate both block-level DSP behavior and concurrent producer/consumer behavior.

Uncalibrated mapped routes must use a virtual flat frequency baseline and remain fully eligible for basic suppression. Calibration is an optional enhancement that adds measured-room peak bias and longer hotspot treatment.

**Why:** Users need immediate protection from any valid armed route without first emitting an audible calibration sweep; calibration should improve detection weighting, not unlock suppression.

**How to apply:** Initialize every prepared processor with the flat baseline, replace it only when a valid route-specific calibration exists, and never gate suppression on calibration state.