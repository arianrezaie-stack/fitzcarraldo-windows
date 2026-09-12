---
name: GitHub Actions connector limits
description: GitHub OAuth and proxy constraints encountered while publishing and diagnosing Actions workflows.
---

GitHub OAuth may allow ordinary repository source commits while still refusing changes under `.github/workflows` because workflow-write permission is separate.

**Why:** Reauthorizing did not add workflow-write access, and proxy filtering also blocked workflow-file REST routes plus Actions log and artifact archive downloads. Ordinary source commits, check-run annotations, run metadata, and artifact metadata remained accessible.

**How to apply:** Use the Git Data API for normal source publication. If a workflow file itself must change, ask the user to commit that edit in GitHub. For private CI failures, emit concise diagnostic annotations and read them through check runs; use artifact metadata to verify uploads when archive downloads are blocked.

For exact source-tree synchronization, do not build blob payloads from shell stdout in the durable sandbox. That transport can normalize CRLF and can truncate oversized combined output, producing a valid-looking but incomplete blob. Read each file directly and create the Git blob with UTF-8 content, then compare the complete tree SHA.

**Why:** A native header upload lost its leading bytes during a shell/base64 transfer, causing Windows compile errors even though the local header was correct; per-file direct reads preserved the expected Git blob hash.

**How to apply:** Compare paths and blob hashes before writing, upload bounded per-file payloads, use a fast-forward Git Data API commit, and require the remote tree SHA to equal the local `HEAD^{tree}` afterward.