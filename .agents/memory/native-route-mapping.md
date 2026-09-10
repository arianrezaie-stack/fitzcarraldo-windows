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