---
name: Feedback detector resolution
description: Non-obvious constraints for reliable realtime feedback detection across the audio band.
---

Use a fixed-size, allocation-free FFT for feedback detection and rank distinct
spectral peaks per frame. Use log-magnitude interpolation around the winning
bin when the notch frequency must track an off-grid tone.

**Why:** Sparse logarithmic oscillators left frequency blind spots, while a
single strongest-peak policy starved simultaneous tones. Linear bin selection
can also leave a persistent notch audibly offset from the actual tone.

**How to apply:** Test off-grid tones across the full operating band,
simultaneous tones, achieved cut depth, hysteresis, and finite/click-free output.
Release each tracked notch once from its own neighborhood rather than once for
every quiet nearby bin.

For low-frequency feedback, combine a longer overlapping FFT window with local
spectral-maximum gating and clear each notch's per-analysis “seen” state before
scanning candidates. Sweep calibration must compare delayed recordings against
the original sweep phase in local time windows, not use a global DFT of the
logarithmic sweep.

**Why:** Low-frequency tones can spread across adjacent bins and make several
notches collapse onto one estimated frequency; stale seen flags can also prevent
release. Lowering the baseline gate alone is insufficient when a second “strong
peak” floor remains higher.
The global DFT of a chirp measures sweep energy leakage rather than the room
response.

**How to apply:** Keep Speech detection fast with a lower calibrated gate, but
retain a higher gate and longer persistence for Music so ordinary sustained
notes do not become notches. Test generic low-frequency release at multiple
representative tones, plus silence-release, multi-tone, moving-pitch, lower-level
feedback, calibration-peak sensitivity, and reference-gain regressions whenever
detector resolution or calibration changes.