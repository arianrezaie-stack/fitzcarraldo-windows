---
name: Persistent feedback cuts
description: Native ownership and explicit clearing for recurring and manually held feedback notches
---

Persistent feedback cuts belong in the native DSP processor and should be controlled through explicit atomic command state; the renderer alert is only the user-facing trigger.

**Why:** A renderer-only warning disappears during telemetry updates or engine restarts and cannot guarantee that the realtime filter remains engaged.

**How to apply:** Keep manual frequencies and persistent recurring-notch flags in the processor, protect them from ordinary release/replacement, and expose a dedicated clear operation for the UI's explicit dismissal control.