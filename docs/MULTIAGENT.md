# Multi-agent Harness workflow

## When to isolate

Read-only reviews may run concurrently in the main checkout. Any concurrent coding tasks use one Git worktree and branch per agent. Sequential small edits can share the main checkout only when the coordinator is not editing it.

## Task packet

Give every Harness subagent a self-contained prompt containing:

- **Objective:** one observable result, not a broad subsystem cleanup.
- **Base:** repository path and base commit.
- **Writable scope:** exact files/directories owned by this agent.
- **Read-only context:** reference files and related modules it may inspect.
- **Constraints:** compatibility, no unrelated reformat, no game-data edits, no dependency changes without approval.
- **Acceptance:** exact build/regression/behavior checks.
- **Report:** changed files, evidence, unresolved issues, and integration concerns.

Start independent Harness `subagent` calls together. Continue coordinator work while they run; do not poll or duplicate their assignments. Use `send_message` for scope corrections and `interrupt_agent` for obsolete work.

## Create an agent worktree

From the main repository:

```powershell
# Creates ..\worktrees\vm-array-bounds and branch agent/vm-array-bounds.
./scripts/New-SlrrAgentWorktree.ps1 -Name vm-array-bounds

# Bootstrap shared pinned tools/dependency in that worktree and optionally build.
& .\scripts\Initialize-SlrrWorkspace.ps1 -RepositoryDirectory ..\worktrees\vm-array-bounds -Build
```

Then start the coding subagent with `cwd = C:\openslrr\worktrees\vm-array-bounds`. Each worktree gets its own `build-bgfx-x64`; the pinned bgfx source and verified local CMake are shared read-only from the main checkout.

The selected base commit must already contain `AGENTS.md`, `CMakePresets.json`, and `scripts/Initialize-SlrrWorkspace.ps1`; the worktree script checks this and stops with a clear error otherwise. Until the initial Harness setup is committed/integrated, use the main checkout for read-only agents or sequential coordinator edits—do not copy untracked setup files into agent branches.

Branches/worktrees are isolation mechanics, not permission to commit or merge autonomously. The coordinator decides when a task may commit and performs all integration.

## Integration gate

For every completed task:

1. Collect the agent result and inspect `git status` plus the complete diff.
2. Verify that only assigned files changed and no generated/build/reference files entered the diff.
3. Confirm new parser/VM/physics regressions have focused tests and exact validation output.
4. Rebase or merge centrally; agents do not integrate into `main` themselves.
5. Build the combined tree, run all applicable headless checks, and only then assign interactive checks.
6. Update `NATIVE_AUDIT.md`, `docs/SOURCE_REVIEW.md`, or `docs/ROADMAP.md` when behavior or priorities change.
7. Remove worktrees only after their changes are integrated and verified, using an explicit reviewed path.

Recommended order for colliding work: foundational safety/parser fixes → VM/runtime → resources/natives → physics → render/UI/platform → docs/coverage regeneration.

## Agent report template

```text
Objective:
Base commit:
Changed files:
Implementation summary:
Validation (exact commands + pass/fail):
Behavioral evidence and limitations:
Unresolved findings:
Cross-subsystem/integration notes:
```

## Coordinator checklist

- No two agents own the same writable file.
- No background build shares a build directory with another command.
- No agent edits `open-SL/` or user saves.
- No agent promotes an executable or changes global machine configuration.
- No claim of interactive correctness based solely on compilation or a headless probe.
- All agents and background jobs are collected before the final report.
