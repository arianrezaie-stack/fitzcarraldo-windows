---
name: Windows DSP test stack limits
description: Windows release tests use a much smaller default stack than Linux, so large realtime state must not be embedded in stack-heavy test objects.
---

Keep large DSP handoff storage off the `FeedbackProcessor` object stack, and retain a size guard in the DSP regression tests.

**Why:** The Windows release test executable can segfault while the same optimized test passes on Linux when several processors with large embedded buffers coexist in one test function. The compiler reports a successful build, so this appears only during test execution.

**How to apply:** Treat Windows runtime stack failures separately from compiler failures; heap-own large ring buffers while preserving the no-allocation realtime callback contract, then run the native Windows test workflow.