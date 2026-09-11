---
name: Persistent feedback cuts
description: Native ownership and explicit clearing for recurring and manually held feedback notches
---

Persistent feedback cuts belong in the native DSP processor and should be controlled through explicit atomic command state; the renderer alert is only the user-facing trigger.

**Why:** A renderer-only warning disappears during telemetry updates or engine restarts and cannot guarantee that the realtime filter remains engaged.

**How to apply:** Keep manual frequencies and persistent recurring-notch flags in the processor, protect them from ordinary release/replacement, and expose a dedicated clear operation for the UI's explicit dismissal control.

Automatic recurrence should count a same-frequency hotspot again only after a short quiet-to-engaged transition; continuous detector frames are not separate recurring events.

**Why:** Counting every detector frame would latch a sustained tone immediately, while counting only slot replacement misses a recurring tone that reuses its existing notch.

**How to apply:** Keep the recurrence counter native, expire it on a short bounded analysis window, and require a calibrated hotspot plus the route's latch capacity before setting the persistent flag.