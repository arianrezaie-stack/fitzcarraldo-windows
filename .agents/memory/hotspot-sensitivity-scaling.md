---
name: Calibration hotspot sensitivity scaling
description: How native detector sensitivity should track measured calibration peaks.
---

Calibrated hotspot sensitivity must scale with the measured amplitude excess above the whole-spectrum calibration baseline, not use only a fixed hotspot lift. Keep a bounded minimum lift for the smallest accepted hotspot and a bounded maximum for very strong peaks.

**Why:** A fixed sensitivity bonus treats barely elevated calibration peaks and severe room resonances the same, even though the measured amplitude difference is the strongest available indicator of how aggressively the detector should respond.

**How to apply:** Convert the stored calibration bias back to the measured excess before calculating hotspot sensitivity. Preserve the ordinary frequency-shaped gate, baseline-relative gate, and lower clamp; only the hotspot lift should vary with the measured excess.