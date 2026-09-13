---
name: High-shelf floor visualization
description: The native and renderer treatment for deep speech-mode high-frequency detection shelves.
---

Deep high-frequency threshold offsets must be applied after clamping the flat engage gate; otherwise a large negative shelf collapses to the clamp and is barely visible or effective.

**Why:** A deep shelf can fall below the FFT leakage produced by strong low-frequency program material. Without a separate high-band signal gate, ordinary tones can create false notches at unrelated high frequencies.

**How to apply:** Keep the renderer’s detection curve on the same raw analyzer dB scale as the live FFT trace, and use a targeted high-shelf signal gate to reject leakage without flattening the displayed shelf.