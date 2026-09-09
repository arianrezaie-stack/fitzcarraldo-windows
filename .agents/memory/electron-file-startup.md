---
name: Electron file startup
description: Desktop packaging constraint learned from a blank packaged renderer.
---

The single-screen desktop app should render directly rather than depend on browser-path routing when loaded from a packaged `file://` URL.

**Why:** A desktop build could compile and work in the development server while opening as a blank Electron window because its router base did not match the packaged file path.

**How to apply:** After desktop-facing frontend changes, build with relative assets and test the generated HTML directly through `file://` before distributing the Windows executable.