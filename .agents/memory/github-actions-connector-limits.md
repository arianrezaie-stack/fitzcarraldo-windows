---
name: GitHub Actions connector limits
description: GitHub OAuth and proxy constraints encountered while publishing and diagnosing Actions workflows.
---

GitHub OAuth may allow ordinary repository source commits while still refusing changes under `.github/workflows` because workflow-write permission is separate.

**Why:** Reauthorizing did not add workflow-write access, and proxy filtering also blocked workflow-file REST routes plus Actions log and artifact archive downloads. Ordinary source commits, check-run annotations, run metadata, and artifact metadata remained accessible.

**How to apply:** Use the Git Data API for normal source publication. If a workflow file itself must change, ask the user to commit that edit in GitHub. For private CI failures, emit concise diagnostic annotations and read them through check runs; use artifact metadata to verify uploads when archive downloads are blocked.