# Agent working guide

Before changing the engine, read [ENGINE_CONTRACT.md](docs/ENGINE_CONTRACT.md).
This guide describes the SLRR contract, subsystem responsibilities, and
compatibility pitfalls. Then read the specialized documentation for your task:

- VM, threads, events, and dialogs: [SCRIPT_RUNTIME.md](docs/SCRIPT_RUNTIME.md).
- Host threads, the worker pool and how to add parallel work:
  [KNOWLEDGE_BASE.md §5.10](docs/KNOWLEDGE_BASE.md#510-threads-and-the-worker-pool)
  and [ENGINE_CONTRACT.md §6](docs/ENGINE_CONTRACT.md#host-threads-and-the-worker-pool).
- Object lifetime: [VM_GC.md](docs/VM_GC.md).
- Script compilation: [SCRIPT_COMPILER.md](docs/SCRIPT_COMPILER.md).
- Bindings, no-ops, and limitations: [NATIVE_AUDIT.md](docs/NATIVE_AUDIT.md).
- Building and running: [README.md](README.md).
- Verified code map, known doc drift, open risks and issue entry points:
  [KNOWLEDGE_BASE.md](docs/KNOWLEDGE_BASE.md).
- Formats, mechanics and lessons from SLRR_rev and draft PR #3:
  [EXTERNAL_RESEARCH.md](docs/EXTERNAL_RESEARCH.md).
- Proposed roadmap: [MILESTONES.md](docs/MILESTONES.md).

SLRR_rev (https://github.com/nedobylskiy/SLRR_rev) implements most game
mechanics. Before implementing a missing native or mechanic, check whether
SLRR_rev already has it and migrate it, following
[EXTERNAL_RESEARCH.md §7](docs/EXTERNAL_RESEARCH.md#7-migrating-mechanics-from-slrr_rev).

After changing code that the knowledge base cites, run
`python scripts/check_knowledge_refs.py`.

Check the active code: some audits and comments describe earlier implementations.
Preserve changes already present in the working tree. Fix general primitives
rather than adding VM exceptions for a specific car or screen. Java changes
must remain within the scope authorized by the user and must be included in
the distributed patch when needed.

Update the relevant documentation whenever a contract or responsibility changes.
Report compilation and in-game validation separately; neither result alone
establishes complete compatibility.

## Branches and pull requests

- Name branches `{type}/{issue_number}-short-desc`, where `type` is `feature`,
  `bug`, `hotfix`, or `spike`. The issue number is required. For example:
  `bug/44-remaining-limitations-fix`.
- Start the PR title with `#<issue_number>` and make its wording match the
  branch's short description, for example: `#44 Remaining limitations fix`.
- Keep the PR description concise and direct. Include a `Not fixed / limitations`
  section listing remaining work so a crawler can add it to issue #44; write
  `None` when nothing remains. Do not omit known gaps.
- For UI changes, include labelled **Before** and **After** screenshots in the
  PR description.
- Always request review from someone other than the PR author. PRs created by
  `antomys` must request `EliasFD3S`; PRs created by `EliasFD3S` must request
  `antomys`.
