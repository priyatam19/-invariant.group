# Productionization Plan — from research prototype to adopted tool

This is the cross-cutting plan for turning LPTA into something a stranger can
install, trust, contribute to, and (eventually) propose to the LLVM
community — as distinct from [ROADMAP.md](ROADMAP.md), which is the technical
plan for the analysis engine itself (Phases 1-5). Read both; this one is
about everything *around* the code: process, CI, governance, UI, and how
multiple independent agents (human or AI) can work on this without
colliding.

## 0. Current state, honestly

As of 2026-09-13:
- Phase 1 of the analysis engine works and is documented (README/RESEARCH/ROADMAP).
- A public GitHub repo exists: `github.com/priyatam19/-invariant.group`, with
  a stacked PR chain: `#1` baseline import, `#2` reproducible experiments and
  `#3` LLVM 23.1.1 migration (both stacked on `#1`), pushed by a different
  agent/session than the one that wrote this plan — that session's local
  checkout had a git repo with a remote configured but zero commits, i.e. it
  wasn't the working copy that produced those PRs. **This has since been
  reconciled** by fetching the actual remote branches directly (`git fetch`
  against an HTTPS remote added alongside the broken SSH `origin` — see the
  note on remotes below) rather than trusting either session's local state.
- A `/code-review` pass on `#1` found a real script-injection bug (unescaped
  `</script>` in the HTML report's embedded trace data) plus a report-layer
  unit-naming collision; both fixed in `#4` (stacked on `#1`, see its
  description for detail). This governance/CI scaffolding lands as its own
  PR stacked on `#1` too, deliberately not touching the same files as `#4`
  to avoid the two racing.
- **Git transport note for whoever picks this up next:** `origin` is an SSH
  URL (`git@github.com:...`) and this sandbox has no SSH key configured, so
  `git fetch`/`push origin` fails with "Permission denied (publickey)".
  Don't "fix" this by repointing `origin`'s URL — that's flagged as a
  sensitive action for good reason (silently redirecting where a repo's
  commits actually go is exactly the kind of thing that should require a
  human decision, not an agent's judgment call). The workaround used here:
  add a **second** remote on the HTTPS URL (`git remote add ghhttps
  https://github.com/...`) and use `gh auth setup-git` / gh's stored
  credentials for it — additive, doesn't touch `origin`, and `gh pr create`
  works from either.
- **Still open**: GitHub refuses to let one account file a formal "request
  changes" review on its own PR, and all PRs so far are authored under the
  same account this session is authenticated as. Cross-agent review
  currently means PR comments, not GitHub's native review status — see §6.
  A real second identity (a bot account, or a human's own review) is needed
  before "review" here carries GitHub's usual guarantees.

Everything below assumes that gets resolved first; don't add CI workflows or
branch protection on top of a repo whose local/remote relationship nobody has
verified.

## 1. Software Development Lifecycle — the baseline, from day one

Minimum viable SDLC hygiene, roughly in the order it should land:

| Artifact | Purpose | Notes for this project |
|---|---|---|
| `LICENSE` | Legal clarity for adoption | If any LLVM-community future is intended, use **Apache 2.0 with LLVM Exceptions** to match `llvm-project` itself — this matters a lot if code ever moves in-tree, and costs nothing now. |
| `CONTRIBUTING.md` | How to propose a change | Must cover: branch naming, commit message format, required local checks before opening a PR, and — specific to this project — **how autonomous agents should behave** (see §5). |
| `CODE_OF_CONDUCT.md` | Community norms | Mirror LLVM's (Contributor Covenant–based) if the long-term goal is LLVM-adjacent adoption; consistency reduces friction later. |
| `SECURITY.md` | Vulnerability reporting path | Low-stakes for a static-analysis dev tool, but still expected of a "real" project; also matters because it eventually parses/executes against untrusted IR input. |
| `CODEOWNERS` | Who's accountable for what | Map directly to the component boundaries in §5 — this is the file that actually enforces the multi-agent ownership split, not just documents it. |
| `CHANGELOG.md` | Human-readable history | [Keep a Changelog](https://keepachangelog.com) format; generate/verify in CI so it can't silently drift from merged PRs. |
| Issue + PR templates (`.github/ISSUE_TEMPLATE/`, `.github/pull_request_template.md`) | Consistent intake | PR template should require: which LLVM version(s) tested, whether the trace schema changed (see §4), and a link to the ROADMAP.md phase the change belongs to. |
| Versioning scheme | Predictable releases | SemVer for the tool itself, but **also publish an LLVM-compatibility matrix per release** (e.g. "v0.3.0: tested LLVM 17–20") since that's the axis users actually care about, not the tool's own version number. |

## 2. CI/CD — GitHub Actions, concretely

### 2.1 What LLVM itself does (verified, not assumed)

LLVM finished migrating pre-commit CI off Phabricator/Buildkite onto **native
GitHub Actions**: infra definitions live in `llvm-zorg`, build scripts in
`llvm-project/.ci/`, workflow definitions in `llvm-project/.github/workflows/`.
Every PR — including from project members — goes through GitHub PR review;
force-pushing/rebasing mid-review is discouraged because it breaks review
context. ([LLVM GitHub User Guide](https://llvm.org/docs/GitHub.html),
[google/llvm-premerge-checks](https://github.com/google/llvm-premerge-checks),
[RFC: premerge postcommit via GitHub Actions](https://discourse.llvm.org/t/rfc-running-premerge-postcommit-through-github-actions/86124))
Python formatting in LLVM's pre-commit CI specifically uses `darker` (format
only the changed lines) rather than a full `black` pass on the whole file —
worth copying verbatim for `tools/lpta_report.py` if this project ever wants
patches to feel familiar to LLVM reviewers.

**Practical takeaway for LPTA:** mirror the shape (GitHub Actions, PR-gated,
no direct pushes to `main`), not the scale — this project doesn't need
`llvm-zorg`-grade infrastructure, just the same *discipline*.

### 2.2 The workflow set to actually build

**Status**: `#3` already added `.github/workflows/llvm.yml` (matrix over
LLVM 18/23.1.1, CTest, `run_demo.sh`, artifact upload — item 1 below, done,
just under a different filename and with a 2-version matrix rather than the
4-version one sketched here). The governance PR adds `lint.yml` (item 2,
format-check only for now — see that file's own comment for why full
`ruff check` linting is deferred a cycle), `schema-contract.yml` (item 4),
and `codeql.yml` (item 5). `test.yml` (item 3, the actual unit-test pyramid)
is still open — it's gated on the `src/Diff.h`/`src/IRUnit.h` refactor in §3.

1. **`ci.yml`** (now `llvm.yml`) — the core gate, matrix over LLVM versions:
   ```yaml
   strategy:
     matrix:
       llvm: [17, 18, 19, 20]   # whatever's installable via apt.llvm.org at the time
   ```
   Steps: install `llvm-${{matrix.llvm}}-dev`, configure + build the plugin,
   run `test/run_demo.sh`, **validate the produced JSONL against the trace
   schema** (§4 — this is the check that would have caught the 336→365
   event-count drift from PR #3 automatically, same-day, instead of needing a
   manual migration PR to discover it), upload `report.html` and `trace.jsonl`
   as build artifacts so a reviewer can eyeball the actual output of a PR
   without pulling the branch locally.
2. **`lint.yml`** — `clang-format --dry-run --Werror` on `src/`, `darker`
   (LLVM's own convention, see above) or `ruff format --check` on `tools/`,
   `clang-tidy` on the plugin (LLVM's own clang-tidy-in-CI story is
   currently unsettled upstream per the Discourse thread above — pick a
   fixed, documented checks list rather than "whatever clang-tidy defaults
   to today").
3. **`test.yml`** — the actual test pyramid (§3), separate from the
   build-and-smoke-test in `ci.yml` so a broken unit test doesn't get
   confused with a broken LLVM build in the PR checks list.
4. **`schema-contract.yml`** — validates every fixture under
   `test/fixtures/*.jsonl` against `docs/trace-schema.json` on every push,
   independent of whether the C++ plugin even builds. This is the one that
   lets a UI-only contributor (§5) get a green check without ever touching
   LLVM.
5. **`codeql.yml`** — GitHub's built-in CodeQL scanning for the C++ and
   Python; free, low-effort, and expected by anyone evaluating whether to
   trust a compiler-adjacent tool.
6. **Scheduled `nightly.yml`** — rebuild against LLVM `main`/tip-of-tree on a
   cron. This is what actually gives early warning of the exact kind of
   drift PR #3 was created to fix manually (`Any`/`dyn_cast` API differences
   between installed-18 and tip-of-tree were already observed firsthand in
   this project's own research — see RESEARCH.md §2.1).

### 2.3 Repo configuration (the "latest Git/GitHub features" ask specifically)

- **Rulesets** (GitHub's newer replacement for classic branch protection) on
  `main`: require PRs, require the `ci`/`lint`/`test`/`schema-contract`
  status checks, require at least one approving review, block force-push,
  block direct pushes — including from admins, which matters a lot once
  multiple agents have push access.
- **Merge queue** — once there's more than one active contributor/agent,
  this serializes "green PR" → merge without everyone re-basing on each
  other manually; directly relevant given §0's two-agents-one-repo situation.
- **Auto-merge** (PR-level opt-in) combined with the merge queue, so an
  agent can mark a PR "merge when green + approved" and not need to poll.
- **Dependabot / Renovate** — for the GitHub Actions versions themselves and
  any Python deps `tools/` picks up later; low-effort, prevents silent
  supply-chain drift.
- **OpenSSF Scorecard action** — a free, automated "how trustworthy does
  this repo look" signal; worth turning on early because the score improves
  retroactively as the other items in this doc land, and it's a concrete,
  external, non-self-reported credibility signal for LLVM community
  reviewers who don't know the project yet.
- **GitHub Projects (beta)** board — one board, columns per ROADMAP.md phase,
  used as the shared task queue *all* agents read from before starting work
  (see §5) — this is cheaper than it sounds and solves a real problem: right
  now nothing prevents two agents from independently deciding to work on the
  same ROADMAP phase.

## 3. Testing pyramid, mapped to this codebase specifically

| Layer | What | Where |
|---|---|---|
| Unit | `diffLines`'s LCS math, `resolveUnit`'s Any-unwrapping, JSON record shape | Currently all three are `static`/anonymous-namespace functions in one `.cpp` — **first refactor needed**: pull pure logic into a small header (`src/Diff.h`, `src/IRUnit.h`) compiled into both the plugin and a `gtest`-based unit test binary, so they're testable without loading LLVM's pass pipeline at all. |
| Contract | JSONL output validates against `docs/trace-schema.json` | Runs on every trace-producing test, in CI and locally; this is what makes the C++ backend and any future UI safely independent (§4). |
| Integration | `opt -load-pass-plugin=...` against a small real-world corpus (not just `test/sample.c`) — e.g. a handful of files from `llvm-project`'s own `test/Transforms/*` or a small real C project | Add `test/corpus/` with 5-10 diverse files; assert on invariants, not exact output (`cfg_changed=true` whenever `before_bb_count != after_bb_count`, `seq` is monotonic, stack never underflows) rather than golden-file exact match, since exact IR text is version-fragile by design. |
| Golden/snapshot | Pin one input + one LLVM version, store the expected trace, diff on change | One per matrix leg in `ci.yml`; a diff here is *expected* to fire on LLVM version bumps (that's the whole point — it turns "PR #3 had to manually discover 336→365" into "CI told us on push"), so treat this as an informational diff, not a failing check, when it fires on `nightly.yml` against tip-of-tree. |
| Fuzz | Malformed/adversarial JSONL into `lpta_report.py` | It's a report generator parsing untrusted-shaped input; a `python-afl`/Atheris pass here is cheap insurance against a crash-on-load bug reaching a user's browser via the HTML report. |
| Cross-version matrix | The `ci.yml` matrix itself | This *is* the test that should have made PR #3 unnecessary as a discovery mechanism (it would still be needed as the actual migration work, just triggered automatically instead of manually). |

## 4. The trace schema as the product's real interface contract

This is the single highest-leverage thing to formalize, because it's what
lets §5's multi-agent split actually work:

- Write `docs/trace-schema.json` (JSON Schema, draft 2020-12) describing
  every field currently emitted by `LPTAInstrumentation.cpp` (`pass`,
  `unit_kind`, `ir_changed`, `dt_preserved`, `incremental_update_candidate`,
  ...), with a `schema_version` field added to every record.
- Version the schema independently of the tool (SemVer again): a new
  *optional* field is a minor bump, a renamed/removed field or changed
  semantics (like the Phase 2 fix to `incremental_update_candidate` planned
  in ROADMAP.md) is a major bump with both old and new documented.
- Check real fixture files (`test/fixtures/*.jsonl`, captured once from real
  runs, committed to the repo) into version control specifically so a UI
  contributor, or a different AI agent, can build and test against them
  **without an LLVM toolchain installed at all**. This is what makes the
  "independent of backend development" ask in the prompt actually true
  rather than aspirational.

## 5. UI-side work, decoupled from the backend

The current `tools/lpta_report.py` output (single static HTML file, data
inlined as a JS blob) is a fine Phase-1 artifact but not a product surface.
Concrete, backend-independent next steps, ordered by how little LLVM
knowledge they require:

1. **Richer static report** (still python-generated, still a single HTML
   file, zero new infra): a real pass-tree view exploiting the LIFO nesting
   the plugin already captures (currently flattened into a per-unit table),
   a flamegraph-style timeline, a side-by-side before/after IR diff viewer
   per event instead of just counts. Buildable and testable entirely against
   the fixture JSONL files from §4.
2. **Small standalone web app** (React/Vite or plain JS, doesn't matter):
   loads a `.jsonl` file (drag-and-drop or `fetch`), same fixture-driven
   testability, but now supports live filtering/search/multiple-trace
   comparison (feeds directly into ROADMAP.md Phase 5's cross-run diffing)
   without regenerating a fresh HTML file per view.
3. **VS Code extension**: this exact session already runs inside a VS Code
   extension host — a natural, low-friction distribution channel is a
   "LPTA Trace Viewer" extension that opens `.jsonl` files with a custom
   editor, reusing the same rendering code as (2) via a webview. This is
   the highest-adoption-per-effort UI investment given the audience
   (compiler engineers already living in editors, not web dashboards).
4. **Hosted/shareable viewer**: once (2) exists, a hosted version (could
   start as a Claude Artifact for quick internal sharing, or a proper static
   site on GitHub Pages) that a compiler engineer can paste a trace into
   without installing anything.

All four items share one property on purpose: **none of them require a
working LLVM build**, only the committed fixture files from §4. That's what
makes it possible to hand this whole track to a different agent (or a
different AI vendor entirely) with zero coordination overhead beyond "don't
change the schema without a version bump."

## 6. Making multi-agent contribution actually safe

This section exists because of §0. Concretely:

1. **Fix the immediate discrepancy first.** Don't let two agents each
   believe they have "the" working copy. Either: (a) this session does a
   clean `git fetch origin && git reset --hard origin/main`-equivalent
   *after confirming with the user* which local state is authoritative and
   that no uncommitted local work would be lost, or (b) the user designates
   one session as the git/PR owner going forward and other sessions
   (including this one) work from patches/branches that owner applies. This
   plan does not resolve that on its own — it's a decision for the user,
   flagged, not defaulted.
2. **One shared task board, not tribal knowledge.** The GitHub Projects
   board from §2.3, columns = ROADMAP.md phases. Before starting work, an
   agent claims a card. This is the cheapest fix for "two agents
   independently work the same phase."
3. **Branch naming convention** — formalize what's already emerged
   organically in the existing PRs (`bootstrap/...`, `test/...`,
   `build/...`): `<track>/<slug>`, where `<track>` is one of `engine`
   (ROADMAP.md phases), `ui` (§5), `ci` (§2), `docs`. Document it in
   `CONTRIBUTING.md` so any agent, regardless of vendor, can infer intent
   from `git branch -a` alone.
4. **No agent merges its own PR.** Require the "1 approving review" ruleset
   from §2.3 unconditionally. Practically, this means either a human
   approves every merge (recommended while the project is this young — the
   blast radius of a bad merge to a *public* repo under a real identity is
   not small), or a *different* agent/session reviews (this project already
   has the `code-review` skill and its `ultra` multi-agent cloud review mode
   available for exactly this — "cross-agent review" isn't hypothetical
   infrastructure, it's one slash command away).
5. **Decision log, not just a roadmap.** Add `docs/decisions/` (lightweight
   ADRs — one markdown file per non-obvious choice: "why LLVM 18 as the
   pinned dev target," "why JSONL over SQLite for the trace format," "why
   any_cast over dyn_cast<Any>"). RESEARCH.md already contains some of this
   reasoning; ADRs make future ones searchable and dated instead of buried
   in a growing prose doc. This matters specifically because different
   agents (different context windows, different vendors, no shared memory)
   will otherwise re-litigate the same settled questions repeatedly.
6. **CODEOWNERS mapped to §5's tracks**, even if every path currently
   "belongs" to the same human owner — it's the file that makes review
   routing automatic once there's more than one real contributor, and
   costs nothing to add now.

## 7. The LLVM-community adoption path specifically

Two very different bars, and it matters which one is actually being aimed
at:

- **"Useful standalone tool the LLVM community knows about and uses"**
  (like `llvm-optiler`, `llvm-opt-benchmark`, Compiler Explorer plugins):
  low bar. Needs: good docs (have it), a permissive matching license (§1),
  a post on [LLVM Discourse](https://discourse.llvm.org) under "Community"
  once Phase 2/3 of ROADMAP.md produce something demoable, maybe an LLVM Dev
  Meeting lightning talk (fitting, given this project's own origin as a
  dev-meeting-style proposal doc). No RFC, no code review by LLVM
  maintainers, no CLA-equivalent process — it lives in its own repo forever.
- **"Merged into `llvm-project`"** (e.g. as a new tool under `llvm/tools/`
  or contributed instrumentation under `llvm/lib/Passes/`): high bar. Needs
  an RFC on Discourse *first* (before code, per LLVM's Developer Policy),
  strict adherence to
  [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html),
  passing the actual `llvm-project` premerge CI (§2.1), review and sign-off
  from code owners of the areas touched (`Passes/`, `Analysis/`), and
  acceptance that `clang-format`/formatting conventions in-tree are
  authoritative over this project's own (currently unstated) style.
  Realistically a multi-month process gated on ROADMAP.md Phase 3/4 proving
  out the static-analysis + patch-and-verify idea against real upstream
  code, not something to design CI around today.

**Recommendation:** build for the first bar now (it's most of what §1-§5
already cover), keep the second bar's requirements in this doc so nobody's
surprised later, and don't gold-plate in-tree-style CI/process before Phase 3
of ROADMAP.md gives something worth proposing.
