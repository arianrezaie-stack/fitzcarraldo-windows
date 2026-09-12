---
name: Calibration reference normalization
description: Rules for converting measured room response into the calibration chart and detector profile.
---

Use the complete 20 Hz–20 kHz measured spectrum to calculate one median amplitude. Apply one scalar offset that places that median around −3 dB, then shift every calibration bin by that same amount.

**Why:** Physical playback and microphone gain can shift the whole measured curve without changing its frequency-relative shape; a whole-spectrum median gives a stable reference without trimming the low or high bands.

**How to apply:** Keep the normalized response at a −3 dB whole-spectrum median, retain a visible 0 dB flat reference in the result chart, and test that normalized-minus-raw is constant at every bin.