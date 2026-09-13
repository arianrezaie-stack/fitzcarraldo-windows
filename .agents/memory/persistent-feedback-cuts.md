---
name: Persistent feedback cuts
description: Native ownership and explicit clearing for recurring and manually held feedback notches
---

Explicit user manual cuts belong in the native DSP processor and should be controlled through explicit atomic command state; recurring-cut suggestions are renderer-only, need no calibration evidence, expire automatically, and must never auto-promote to a persistent cut.

**Why:** A recurring-cut suggestion is advisory, while a manual cut changes realtime behavior. Auto-promoting the suggestion made it hold indefinitely even when the user had not requested a manual cut.

**How to apply:** Keep explicitly requested manual frequencies and persistent recurring-notch flags in the processor, protect them from ordinary release/replacement, and let the renderer display and expire advisory suggestions independently.

Automatic recurrence should count a same-frequency hotspot again only after a short quiet-to-engaged transition; continuous detector frames are not separate recurring events.

**Why:** Counting every detector frame would latch a sustained tone immediately, while counting only slot replacement misses a recurring tone that reuses its existing notch.

**How to apply:** Keep the recurrence counter native, expire it on a short bounded analysis window, and require a calibrated hotspot plus the route's latch capacity before setting the persistent flag.