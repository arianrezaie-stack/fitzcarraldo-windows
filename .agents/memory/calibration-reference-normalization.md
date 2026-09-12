---
name: Calibration reference normalization
description: Rules for converting measured room response into the calibration chart and detector profile.
---

Use the 200 Hz–10 kHz portion of a sweep only to estimate one broadband gain offset. Subtract that single scalar from every calibration bin across the full 20 Hz–20 kHz response.

**Why:** Physical playback and microphone gain can shift the whole measured curve without changing its frequency-relative shape; independently normalizing or trimming the outer bands would destroy useful response information.

**How to apply:** Keep the normalized response at a 0 dB in-band mean, retain a visible 0 dB flat reference in the result chart, and test that normalized-minus-raw is constant at every bin.