---
name: Dynamic notch pool
description: Why feedback-notch capacity is pooled per configured stable route and redistributed only among armed routes.
---

Provision six notch slots for every configured stable route, then divide that complete pool evenly among routes that are currently armed and validly mapped. Four configured routes therefore receive 6, 8, 12, or 24 slots each as the active count changes from four to one. An eight-route configuration must still retain six slots per route when all eight are armed.

**Why:** Dynamic redistribution should improve protection when routes are disarmed without regressing the original capacity of configurations that use more than the four visible routes.

**How to apply:** Any future slot-allocation, telemetry, UI, or Windows validation work should derive capacity from the configured stable-route count, exclude disabled or invalid routes from the active divisor, and preserve stable route indices.