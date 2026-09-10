---
name: Native route mapping
description: Durable UI and protocol boundary for selecting mono audio channels in Werfeed Herzback.
---

The renderer should enumerate input and output mono channels directly from every native device record, then store each route's input and output selection independently. Route cards should display the saved mapping; the active route's two selectors belong in the audio backend panel.

**Why:** Building choices only from pre-matched input/output interface pairs can produce an empty selector even when Windows reports usable endpoint records. Native configuration still requires an exact physical input/output device pair and backend, so that constraint belongs in the start-time validation rather than in discovery.

**How to apply:** Do not synthesize channel choices when native records provide no channel count. Preserve exact device names and channel indices through the configure command, expose native channel labels when available, and keep route/device changes disabled while audio is running.