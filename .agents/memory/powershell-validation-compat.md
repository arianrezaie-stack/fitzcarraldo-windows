---
name: PowerShell validation compatibility
description: Runtime compatibility constraints discovered while validating Windows evidence scripts.
---

Use ordinary arrays for small PowerShell validator collections and write diagnostics with simple host output before a terminating failure.

**Why:** The available Linux PowerShell runtime hung when mutating generic `List[...]` collections or when a script emitted `Write-Error`/`throw` and was launched with `-File`; Windows sign-off still needs the terminating failure behavior.

**How to apply:** Keep the production script's `throw` for nonzero Windows execution, but avoid generic collection types and `Write-Error` in cross-platform smoke tests. Validate syntax and passing fixtures in Linux, then exercise the unsafe exit on Windows PowerShell.