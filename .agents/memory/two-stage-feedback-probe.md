---
name: Two-stage feedback probe
description: Feedback candidates need a shallow native probe and raw-source reassessment before full suppression
---

Feedback protection must first apply half of the requested notch depth, then reassess the raw route signal after several overlapping analysis frames. Promote only when the peak stays at the same frequency and its source amplitude falls by roughly the probe amount; otherwise release the probe and cool down that frequency.

**Why:** A normal sustained tone is attenuated by the output notch but does not change at the source. Measuring post-notch output makes ordinary tones look like feedback and defeats the distinction.

**How to apply:** Keep probe state, reference level, expected attenuation, and rejection cooldown in native DSP state. Keep the full slider depth reserved for confirmed feedback and leave the renderer as telemetry/control only.

Music mode should require a longer qualifying-peak persistence before starting the probe, while Speech should retain the fastest candidate initiation.

**Why:** Music contains more sustained tonal material that can resemble a feedback peak; delaying the first intervention reduces audible false positives without slowing speech protection.

**How to apply:** Tune the pre-probe candidate persistence by preset, not the confirmation response or full-depth suppression path. Keep Speech at its fast path when changing Music thresholds.