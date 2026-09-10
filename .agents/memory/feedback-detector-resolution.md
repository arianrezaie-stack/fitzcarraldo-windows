---
name: Feedback detector resolution
description: Non-obvious constraints for reliable realtime feedback detection across the audio band.
---

Use a fixed-size, allocation-free FFT for feedback detection and rank distinct
spectral peaks per frame. Release each tracked notch once from its own
neighborhood rather than once for every quiet nearby bin.

**Why:** Sparse logarithmic oscillators left frequency blind spots, while a
single strongest-peak policy starved simultaneous tones. Per-bin release also
cancelled repeated engagement at high frequencies.

**How to apply:** Test off-grid tones across the full operating band,
simultaneous tones, achieved cut depth, hysteresis, and finite/click-free output.

For low-frequency feedback, combine a longer overlapping FFT window with local
spectral-maximum gating and clear each notch's per-analysis “seen” state before
scanning candidates. Sweep calibration must compare delayed recordings against
the original sweep phase in local time windows, not use a global DFT of the
logarithmic sweep.

**Why:** A 70 Hz tone can spread across adjacent bins and make several notches
collapse onto one estimated frequency; stale seen flags can also prevent release.
The global DFT of a chirp measures sweep energy leakage rather than the room
response.

**How to apply:** Keep explicit 70 Hz, silence-release, multi-tone, moving-pitch,
and reference-gain calibration regressions whenever detector resolution or
calibration changes.