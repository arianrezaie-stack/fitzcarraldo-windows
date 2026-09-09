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