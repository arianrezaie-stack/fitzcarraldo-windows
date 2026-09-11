---
name: Native route mapping
description: Durable UI and protocol boundary for selecting mono audio channels in Werfeed Herzback.
---

The renderer should enumerate input and output mono channels directly from every native device record, then store each route's input and output selection independently. Route cards should display the saved mapping; the active route's two selectors belong in the audio backend panel.

**Why:** Building choices only from pre-matched input/output interface pairs can produce an empty selector even when Windows reports usable endpoint records. Native configuration still requires an exact physical input/output device pair and backend, so that constraint belongs in the start-time validation rather than in discovery.

**How to apply:** Do not synthesize channel choices when native records provide no channel count. Preserve exact device names and channel indices through the configure command, expose native channel labels when available, and keep route/device changes disabled while audio is running.

JUCE Windows endpoints can report a valid active-channel mask while returning an empty channel-name array. Use the mask for the count and JUCE-compatible index labels as the fallback so usable mono channels are not discarded.

**Why:** The renderer cannot populate mappings when native discovery drops an endpoint solely because its driver omitted human-readable channel names.

**How to apply:** Keep the native channel index authoritative; use reported names when present and `Input channel N` / `Output channel N` only when the driver omits labels.

Native discovery scans every active endpoint exposed by the selected JUCE backend; transport classification is descriptive metadata rather than a discovery gate.

**Why:** Users need to map any connected active interface, including endpoints whose Windows names do not contain a recognizable USB or Ethernet token.

**How to apply:** Retain transport and eligibility metadata for diagnostics, but do not discard an endpoint before channel enumeration solely because its transport cannot be inferred from the name.

Named endpoints must also survive discovery when an unopened JUCE device reports no channel names or active-channel mask. Expose a mono Channel 1 fallback and let the configure/open operation validate the mapping.

**Why:** Some Windows WASAPI and ASIO drivers defer channel metadata until the endpoint is opened; using pre-open channel metadata as an enumeration gate leaves both selectors empty.

**How to apply:** Treat `getDeviceNames()` as the discovery authority. Use channel names and masks when available, but never require them to emit the endpoint record.

The device-name-first discovery fallback and native ASIO build were confirmed working on physical Windows hardware.

**Why:** A real portable-app run populated the separate input/output mappings, opened ASIO successfully, and improved live DSP behavior.

**How to apply:** Preserve this discovery/open boundary during future mapping changes; treat the named-endpoint fallback as the known-good baseline rather than restoring pre-open channel gates.