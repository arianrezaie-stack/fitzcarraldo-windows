---
name: Coupled-notch consolidation
description: Prevent expanded notch capacity from stacking several narrow cuts on one low-mid room mode.
---

When three or more active cuts below 1.2 kHz fall within one third octave, consolidate them into a weighted center-frequency cut. Broaden the band by lowering Q, add only a small amount of depth, and never exceed the route's existing suppression limit.

**Why:** Extra capacity from disarmed routes can let a drifting low-frequency feedback mode collect several adjacent narrow cuts. A single broader central cut is more stable and avoids over-processing one room resonance.

**How to apply:** Keep consolidation on the background analysis state, with fixed-size storage and no allocation on the realtime callback. Do not merge fewer than three cuts, high-frequency cuts, or clusters wider than one third octave. Preserve route-local capacity and normal release behavior.