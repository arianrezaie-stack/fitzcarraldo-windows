---
name: Windows CI audio hardware
description: The hosted Windows release runner does not expose physical audio endpoints.
---

Hosted Windows builds can compile, launch, enumerate successfully, and still report an empty device list because no physical input/output hardware is attached.

**Why:** The release workflow runs on a Windows Server runner, not the target machine with the Audient interface. Requiring a compatible pair during packaging makes an otherwise valid portable artifact unavailable.

**How to apply:** Treat the renderer/native protocol handshake as the CI gate. Record hardware availability separately, and perform device, routing, calibration, and acoustic checks on the target Windows machine.