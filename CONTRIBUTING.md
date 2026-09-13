# Contributing

This project accepts contributions from human and AI-agent contributors
alike — see [PRODUCTIONIZATION.md](PRODUCTIONIZATION.md) for the full
rationale. This file is the short, actionable version.

## Before opening a PR

1. Build and run the smoke test locally: `cmake -B build -DLLVM_DIR=...`,
   `cmake --build build`, `test/run_demo.sh` (see README.md). If your change
   touches `src/LPTAInstrumentation.cpp`, confirm the demo still produces a
   sane event count — a silent drop to 0 events or a crash means something
   broke the before/after pairing.
2. If your change adds, renames, or changes the meaning of a field in the
   JSONL trace format, update `docs/trace-schema.json` **and** bump its
   `schema_version` (see that file's own header comment for the rule: new
   optional field = minor bump, anything else = major bump). Also update the
   fixtures under `test/fixtures/`.
3. Run formatting: `clang-format -i src/*.cpp` and `ruff format tools/`
   (or `black`/`darker` if `ruff` isn't available — LLVM's own pre-commit CI
   uses `darker` specifically so changed-lines-only formatting doesn't
   produce unrelated diffs).

## Branch naming

`<track>/<slug>`, where `<track>` is one of:

- `engine` — the analysis engine itself (ROADMAP.md phases)
- `ui` — reporting/visualization (PRODUCTIONIZATION.md §5), buildable and
  testable against `test/fixtures/*.jsonl` alone, no LLVM build required
- `ci` — workflows, governance, repo config
- `docs` — anything that's only markdown
- `fix` — bug fixes that don't fit neatly into the above

(`bootstrap/`, `test/`, `build/` are already in use from before this
convention was written down — new branches should use the list above.)

## PR process

- Every change goes through a PR, including from repo owners/maintainers —
  no direct pushes to `main`.
- Stack PRs on top of the branch they logically depend on (see the existing
  PR chain: several branch off `bootstrap/lpta-baseline` in parallel rather
  than off each other, to avoid one PR blocking on another's review).
- **No self-merge without review.** If you're an AI agent working on this
  repo: don't merge your own PR. Ask for a `/code-review` pass (this repo
  already uses it — see the review on PR #1 for the format), or ask the
  human owner to look before merging. GitHub won't let one account file a
  formal "request changes" review on its own PR, so until there's a
  genuinely separate reviewing identity, treat review-findings comments as
  the review record and get an explicit human go-ahead before merging to
  `main`.
- Note in the PR description which LLVM version(s) you tested against, and
  link to the ROADMAP.md phase or PRODUCTIONIZATION.md section the change
  belongs to.

## Multi-agent coordination

If more than one agent/session is likely to be working here concurrently:
before starting non-trivial work, check open PRs and branches
(`gh pr list`, `git branch -r`) for overlapping work, and prefer claiming a
distinct track (§ above) over touching files another open PR already
touches. See PRODUCTIONIZATION.md §6 for the fuller reasoning — this
project has already hit the "two sessions, no shared state" problem once.

## Code style

Follow the ambient style already in `src/` and `tools/` (C++17, LLVM-ish
naming for the C++, minimal-dependency Python for the tools). No formal
style guide beyond `clang-format`/`ruff format` yet — if this project ever
proposes anything for inclusion in `llvm-project` itself, LLVM's own
[Coding Standards](https://llvm.org/docs/CodingStandards.html) become
authoritative at that point, not this document.
