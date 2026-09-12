---
name: GitHub tree synchronization
description: Keep GitHub commits aligned with the complete local source tree when using API-based sync.
---

When syncing through the GitHub API, compare the complete local tree against the remote tree and upload every differing tracked file. Do not rely only on `git diff --name-only HEAD`, because auto-committed local changes can already be clean while still missing remotely.

**Why:** A native header was omitted while dependent source files were uploaded, so the Windows build compiled against an older struct and failed only in CI.

**How to apply:** Before a release-triggering commit, compare local blob SHAs to the remote recursive tree; treat attached logs and other untracked evidence separately from source synchronization.