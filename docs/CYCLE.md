# The improvement cycle

How quat-3d develops itself: an orchestrating Claude session repeatedly
picks work from the roadmap and the audit, hands it to parallel worker agents
in separate git worktrees, merges and verifies the results, audits what
changed, and records what it learned so the next cycle does better.

It runs in two places:

- **In a live session** (self-paced loop) while the owner is around.
- **As a scheduled task every 2 hours that resumes the live session**
  (`quat-3d improvement cycle (this session)`), so the loop survives the
  container being reclaimed. Each cycle opens one pull request, which the
  cycle merges once its CI is green. (A fresh-session-per-run variant was
  tried first and disabled: fresh sessions had no repository access.)

The surrounding issue → worker → PR → review → merge flow (hourly scanner,
per-PR reviewer sessions) is described in [`FACTORY.md`](FACTORY.md).

Every cycle starts by reading, in order: [`STATUS.md`](STATUS.md),
[`LEARNINGS.md`](LEARNINGS.md), [`../ROADMAP.md`](../ROADMAP.md),
[`audit/`](audit/), and the open pull requests.

## Settings (chosen by the owner)

| Setting | Value |
|---|---|
| Parallel worker agents per cycle | up to **8** (aggressive) |
| Scheduled cadence | every **2 hours** |
| How work lands | **one pull request per cycle**, merged into `master` by the cycle as soon as its CI is fully green (owner instruction) |

## The cycle

1. **Sync.** Fetch. Find the base branch (below). Read the files above. Check
   CI on the base branch and the open PRs (each open cycle PR lists the item
   IDs it claimed — never pick a claimed item).
2. **Triage.** If CI on the base is red, fixing it is the cycle's first item.
   Otherwise take, in order: confirmed audit findings by severity, then the
   roadmap's priority order. Unverified findings get verified first.
3. **Plan the batch.** Choose up to 8 independent items. Check which files
   each will touch; items that share files go to the same worker or to a
   later cycle. At most one worker may restructure `src/main.cpp` per cycle;
   others get "minimal hook calls only".
4. **Claim.** Push the cycle branch with an empty "cycle start" commit and
   open a **draft PR** whose description lists the claimed item IDs.
5. **Brief and spawn.** One worktree agent per item, using the brief
   template below — the brief is all the agent knows.
6. **Merge.** Merge from the pushed `origin/feat/<id>` branches (fetch
   first). For each finished branch: read the diff (don't trust the
   summary), re-run `scripts/check-compile.sh` on changed files, run the
   CPU tests that link locally, merge into the cycle branch, resolve
   conflicts. Reject or send back work that breaks a rule in CLAUDE.md.
   Review **structure**, not only behaviour: file sizes, prose embedded in
   source (CLAUDE.md "No prose in source"), lint level, reuse of standard
   libraries/crates instead of hand-rolled code, `main.cpp` growth.
7. **Verify.** Push; wait for CI (subscribe to the PR rather than polling);
   fix failures. Mark the PR ready for review only when CI is green.
8. **Audit.** Spawn 1–2 fresh reviewers on the cycle's diff (give them the
   code, not your conclusions). Re-verify their findings; append confirmed
   ones to `audit/` — they feed the next cycle.
9. **Learn.** Append to `LEARNINGS.md` anything that went wrong or cost
   time, with the rule that prevents it. Only the orchestrator edits it, in
   its own docs PRs; workers' lessons arrive in their PR bodies. When a lesson recurs, promote it
   into CLAUDE.md or the brief template.
10. **Hand off.** Update `STATUS.md` and the roadmap checkboxes in the PR;
    summarise the cycle (done / verified / open) in the PR description.

## Branches

- Work happens on the session's designated branch (cloud sessions are told
  which branch they may push to). Never push to `master`.
- **Base:** `master`, unless an earlier cycle PR is still unmerged — then
  base on the newest unmerged cycle branch so work stacks instead of
  conflicting, and target the PR at that branch. Say so in the PR.
- **Merge rule:** merge a cycle PR into `master` only when every CI job on
  its head commit is green (no skipped required jobs, no pending runs) and
  the PR is not a draft. Use a merge commit (no squash/rebase — workers'
  branches reference the commits). Never merge a red, pending or partly
  cancelled run, and never merge anything other than cycle PRs.
- Never force-push or rewrite published history.

## Worker brief template

Every brief contains:

1. **Goal** — one item, its ID, and why it matters.
2. **Base check** — "Run `git log --oneline -1`; it must be `<sha>` or later.
   If not: `git checkout -B <branch> <sha>`." (Worktrees are created from
   `master`, not from the cycle branch — see LEARNINGS.)
3. **Read first** — CLAUDE.md, DEVELOPMENT.md, LEARNINGS.md, and the
   specific files/findings for the item.
4. **What exists** — APIs to reuse (so the agent doesn't rebuild them).
5. **What to build** — scope boundaries, including what NOT to touch, and
   the quality rules that apply (CLAUDE.md file sizes, "No prose in source",
   standard libraries/crates over hand-rolled code).
6. **Verification** — `scripts/check-compile.sh` on every touched file (zero
   warnings); build and run CPU tests locally where they link; GPU tests
   named `Gpu*` for CI; shaders compiled with `slangc`.
7. **Git rules** — branch `feat/<id>`, commit titles `[<ID>] …`, the
   attribution trailer lines, and **push the branch to origin after every
   commit** (`git push -u origin feat/<id>`, retry with backoff; never
   force-push, never push to master or the cycle branch). Worktrees live only
   in the container, so unpushed work is lost if it is reclaimed. Don't edit
   STATUS.md / ROADMAP.md / LEARNINGS.md; put lessons in the PR body under
   "Lessons for LEARNINGS" for the orchestrator to carry over.
8. **Report** — branch, commits, verified locally vs CI-only, files touched
   outside the item's module, open issues.

## Stop conditions

Stop the cycle and report (in the PR and STATUS.md) when: CI on the base
fails for a reason two attempts can't fix; a change needs an owner decision
(new dependency with licensing questions, platform strategy, data loss); or
the environment blocks required access.
