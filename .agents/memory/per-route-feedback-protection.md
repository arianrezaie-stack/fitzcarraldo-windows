---
name: Per-route feedback protection
description: Durable constraints for multi-route feedback suppression and detector tuning.
---

Route indices must remain stable from the renderer through native configuration, calibration persistence, telemetry, and DSP state. A disabled route should stay represented rather than causing later routes to shift positions.

**Why:** Calibration and live telemetry are route-specific. Compacting only enabled routes made Route 2 calibration or live feedback data attach to the wrong processor.

**How to apply:** Send a fixed route array with explicit enabled state, keep per-route suppression and calibration response data, and let the renderer select which route's telemetry to view while all armed processors continue running.

Speech and music protection need different detector timing. A faster speech gate is useful for real feedback, but music needs longer persistence and a sharper tonal threshold to avoid cutting short notes.

**Why:** Reducing persistence globally made transient musical notes engage shallow notches even after the detector had improved its frequency resolution.

**How to apply:** Tune reaction time, tonal threshold, and release reporting by preset; use parabolic FFT peak interpolation when shortening the FFT window so low-frequency off-grid tones still receive accurate notch placement.