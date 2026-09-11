---
name: Windows live-capture tooling
description: Environment boundary for validating Werfeed's native Windows device and PowerShell evidence flows.
---

The development workspace is Linux and may not provide CMake or PowerShell. Native Windows endpoint enumeration, driver-backed channel inspection, and PowerShell session validation must be run on a target Windows machine rather than represented as completed by Linux checks.

Portable C++ evidence-replay tests can still run in this workspace when the JUCE
Linux build dependencies are available; their pass only validates deterministic
protocol and telemetry contracts, not physical endpoint behavior.

**Why:** These checks depend on Windows audio endpoints and Windows-only tooling; a Linux compile or static inspection cannot prove the live filtering and routing behavior.

**How to apply:** Keep Windows session scripts self-validating and save raw endpoint, native event, and route telemetry evidence. Report the Windows run separately from portable C++ or TypeScript checks.