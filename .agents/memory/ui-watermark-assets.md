---
name: Watermark asset blending
description: Asset-loading and compositing guidance for subtle image watermarks in the web app.
---

Use Vite asset imports for attached image assets rather than assuming a public URL will resolve through every artifact preview path. For monochrome watermarks where white must have no visual effect, place the image in a low-opacity layer with `mix-blend-mode: multiply`.

**Why:** Artifact previews may inject a base path that makes hand-written public asset URLs resolve to the app shell, and multiply compositing preserves the intended white-neutral behavior without preprocessing the source image.

**How to apply:** Import the image through the configured asset alias, pass the resolved URL into the component or a CSS custom property, and keep the watermark below interactive content with pointer events disabled.