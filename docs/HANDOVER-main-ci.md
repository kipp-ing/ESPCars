# Handover — a clean `main`, and CI that actually gates it

2026-10-10. The goal: everything meant for `main` is merged, every stale branch
is gone, and no merge happens again without a green CI run. Delete this file
when §5 holds. Its durable findings belong in `CONVENTIONS.md`.

**Do the steps in order.** Merging first and fixing CI afterwards would land two
PRs that no CI has ever compiled (see §1).

## 1. Finding: CI has never run in this repo

All of these were checked on 2026-10-10 and can be re-checked with the commands
shown:

| Fact | Check |
|---|---|
| **No run has ever happened.** `total_count: 0`. | `gh api 'repos/kipp-ing/ESPCars/actions/runs?per_page=5' --jq .total_count` |
| **The workflow is registered.** `CI`, `.github/workflows/ci.yml`, `state: active`, since 2026-09-13. | `gh api repos/kipp-ing/ESPCars/actions/workflows` |
| **Actions is enabled for the repo.** `enabled: true, allowed_actions: all`. | `gh api repos/kipp-ing/ESPCars/actions/permissions` |
| **The events arrive, but no run follows.** `PushEvent` on `refs/heads/main` (2026-10-01) and `PullRequestEvent`s are recorded. | `gh api 'repos/kipp-ing/ESPCars/events?per_page=10'` |
| **No Actions check suite is created at all.** On `main` and on PR heads, the only suite is the `claude` app's. A workflow that failed to parse would at least show a failed run. | `gh api repos/kipp-ing/ESPCars/commits/<sha>/check-suites` |
| **The account is fine.** Other repos of the same account run Actions (`OwnFrame`, last run 2026-10-06). The old repo `ESPCars-old-archive` last ran on 2026-09-13 13:02Z. This repo was created 26 min later, under the name the old one had held. | `gh api 'repos/kipp-ing/<repo>/actions/runs?per_page=1'` |

**Narrowed down on 2026-10-10, later the same day (steps 1–4 below):**

- The web UI could not be checked: Chrome was not connected.
- **A manual trigger works.** PR #6 (`ci/gate-main`) adds `workflow_dispatch:`.
  `gh workflow run CI --ref ci/gate-main` created run 38061578603. That was the
  repo's first run ever.
- **After that, `pull_request` works too.** Opening PR #6 thirteen minutes after
  PR #5 created run 38061602853. Opening #5 had created nothing, with the same
  account and the same event type.
- **The first manual run woke the repo.** The push carrying this paragraph to
  #5 still had the old `ci.yml`, and it created run 38061664107. So the content
  of `ci.yml` did not matter. Why GitHub left push/PR triggers dormant in a
  recreated repo until the first manual run is not visible from outside, so it
  stays an observation, not a cause. `workflow_dispatch:` stays in `ci.yml`.
- **§2 is done** (2026-10-10 17:28): the ruleset requires `Required checks`,
  pinned to the GitHub Actions app (integration id 15368).

**The original plan for narrowing it down:**

1. **Check the settings in the web UI.** Open Settings → Actions → General for
   this repo and compare it with `OwnFrame`. The REST API shows only part of
   these settings.
2. **Try a manual trigger.** Add `workflow_dispatch:` to `on:` in `ci.yml`, then
   run `gh workflow run CI --ref <branch>`.
   - It runs: the trigger path for push/PR events is the fault (point 3).
   - It does not run, or creates nothing: GitHub is not running workflows for
     this repo at all. That is a repo- or account-side state, so file a GitHub
     support ticket and attach the facts above.
3. **Suspect, not verified:** the repo was recreated under the name of a repo
   that had just been renamed (`ESPCars` → `ESPCars-old-archive`). If point 2
   points that way, compare the two repos' Actions settings first.
4. **Prove it with a no-op PR.** The fix holds when a pushed branch with an open
   PR shows the CI checks in `gh pr checks <n>`.

## 2. Finding: `main` is not gated even once CI runs

The ruleset `main-protection` (id 23193030) has only these rules: `deletion`,
`non_fast_forward` and `pull_request` with 0 approvals. **It requires no status
check.** `ci.yml` already has the job built for this, `Required checks`, one
stable context over all jobs. The ruleset just doesn't require it.

**Fix:** add a `required_status_checks` rule for the context `Required checks`
to the ruleset. Do this only after §1 is fixed: a required check that never runs
blocks every merge.

```bash
gh api repos/kipp-ing/ESPCars/rulesets/23193030              # read first, then PUT the full rules list plus:
# {"type":"required_status_checks","parameters":{"strict_required_status_checks_policy":false,
#   "required_status_checks":[{"context":"Required checks"}]}}
```

## 3. Finding: CI checks less than `script/check.sh`

`check.sh` runs `script/check_no_private_data.py`, but `ci.yml` does not. So
the private-data guard (a hard rule in `CLAUDE.md`) protects only machines where
someone ran `check.sh` locally. That is exactly the gap the repo recreation was
supposed to close.

**Fix:** add a step `python script/check_no_private_data.py` to the `validate`
job, before pytest.

Other points to check on the first real run:

- **The compile matrix has never been built in CI.** That is 14 configs,
  `ccp` included. Expect a first-run failure caused by PlatformIO downloads and
  cache keys, not by code. The configs compile locally.
- **The Python versions differ:** CI uses 3.12, local `.venv` uses 3.13.
  `tests/sd_logger/test_fleet.py` needs `fcntl` (fine on Linux) and
  `script/sdlog_fleet.py` needs `tomllib` (≥ 3.11).
- **clang-format matches:** 13.0.1 locally and in CI.

## 4. Merge queue, after §1–§3

| # | What | State 2026-10-10 | Do |
|---|---|---|---|
| 1 | **PR #4** `feat/signal-patch-e2e`: Motorola decode, `set_signal`, `e2e` | `MERGEABLE`/`CLEAN`; main merged in (6d6a63a). **The owner decided to merge it without the ring gate.** That decision and the tests still owed are in its `HANDOVER.md` §1.0. | Wait for CI to pass, then merge (merge commit, as with #2/#3). Another session was working on it today in the worktree `../ESPCars-signal-patch`; coordinate first, so it is not merged twice. |
| 2 | **PR #5** `feat/sdlog-fleet`: collector exit contract, `sdlog_fleet.py`, and this file | Rebased on main; check.sh green locally. **No file overlaps with #4**, so the order is free. | Wait for CI to pass, then merge. Live test in the deployment, not in this repo. |

Merge method: the ruleset allows merge, squash and rebase; the history so far
uses merge commits.

### Branch cleanup, after both merges

| Branch | Where | Evidence | Do |
|---|---|---|---|
| `feat/ccp-master`, `feat/rig-restore` | remote + local | 0 commits ahead of main (PR #2, #3 merged) | delete |
| `feat/signal-patch-e2e`, `feat/sdlog-fleet` | remote + local | merged by §4 | delete; also remove the worktree `../ESPCars-signal-patch` |
| `claude/peaceful-noether-0uu618` | remote | 4 commits: #4's 3 commits plus a merge into rig-restore; 2 behind main | delete once #4 is merged |
| `feat/sd-logger-utc-origin` | remote + local | 1 commit 0127697, **patch-identical** to main's e918552 (`git range-diff 0127697^! e918552^!` → `=`); PR #1 closed | delete |
| `backup-old-main`, `rescue/sd-confine-wip`, `rescue/utc-origin-pre-rebase` | **local only** | pre-squash history (144–156 commits on a separate root). It may contain the data the recreation scrubbed. | **Never push.** Keep or delete: owner's call |

Not a branch, but open too: on GitHub `ESPCars-old-archive` is **not**
archived (`isArchived: false`). Archiving it is the owner's call.

### CD

ESPCars has no deploy target of its own. Its "delivery" is a ref that consumers
pin in `external_components:`. The open wish (ROADMAP Phase 2) is a **release
tag `v0.1`**, so consumers pin a tag instead of a SHA. Set it on the first
`main` commit that has a green CI run, and only then. The host tools
(`sdlog_fleet`) are deployed from the consuming deployment repo; see
`deploy/sdlog-fleet/`.

## 5. Done when

- [ ] `gh api 'repos/kipp-ing/ESPCars/actions/runs?per_page=1'` shows a run on the current `main` head, conclusion `success`
- [ ] the ruleset requires `Required checks` (§2), and a PR without a green run cannot be merged
- [ ] `ci.yml` runs the private-data guard (§3)
- [ ] `gh pr list` shows no open PRs that are meant for main
- [ ] `git ls-remote --heads origin` lists `main` only
- [ ] `script/check.sh` is green on a fresh clone of `main`
- [ ] this file is deleted, and the lasting rules (§2, §3) are folded into `CONVENTIONS.md`
