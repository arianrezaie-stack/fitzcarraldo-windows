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