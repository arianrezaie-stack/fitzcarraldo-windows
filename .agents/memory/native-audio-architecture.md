---
name: Native audio architecture
description: Durable native-audio architecture and Windows driver constraint for Werfeed Herzback.
---

Use a separate GPLv3 C++20/JUCE sidecar for realtime audio. Electron/React must remain a control and telemetry client and must never simulate successful engine state.

**Why:** Audio capture, WASAPI routing, calibration, and adaptive DSP require deterministic native realtime processing; Chromium and Node are not appropriate audio callback hosts. The user chose to drop ASIO and optimize exclusively for WASAPI.

**How to apply:** Keep allocations, locks, IPC, logging, and device discovery off the audio callback. Build and validate on Windows. Prefer WASAPI Exclusive Mode, retain Shared Mode as fallback, and use one physical interface for input/output.