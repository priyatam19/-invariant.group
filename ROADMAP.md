# Roadmap

Grounded in [RESEARCH.md](RESEARCH.md). Each phase should stand on its own —
no phase requires the next one to already exist to be useful.

## Phase 1 — Instrumentation + quantified trace + report — **DONE**

`src/LPTAInstrumentation.cpp` (out-of-tree `-load-pass-plugin` for `opt`) +
`tools/lpta_report.py`. Delivers the primary GOAL from initial-idea.txt: a
single view connecting each pass to a quantified change and its declared
analysis-preservation behavior. Verified end to end against LLVM 18
(`test/run_demo.sh`): 336 pass invocations traced on a 3-function sample,
15 flagged as `incremental_update_candidate`.

Known limitations to carry into Phase 2 (not blockers, just scope):
- `incremental_update_candidate` is a coarse signal — "CFG changed and DT
  not preserved" — not "DT was live and got wastefully invalidated." See
  RESEARCH.md §5 for a concrete case where this over-fires (SimplifyCFG
  flagged even though it behaves correctly, because DT simply wasn't
  computed yet at that point in the pipeline).
- `cfg_changed` is a proxy (BB count or total successor-edge count differs),
  not real CFG isomorphism — cheap and directionally correct, not exact.
- Module/SCC-level passes snapshot the whole module; fine for dev-scale
  inputs, will need the same filtering/scoping real `-print-changed` has
  (`-filter-print-funcs`-equivalent — LPTA already has `LPTA_FILTER_FUNC`)
  for anything larger.

## Phase 2 — Analysis-liveness-aware candidate detection

Goal: fix the over-firing in Phase 1 by knowing whether an analysis was
actually **cached** (live) at the moment a pass invalidated it, not just
whether the pass's CFG edit was the kind DomTreeUpdater handles.

- Hook `registerAfterAnalysisCallback` / `registerAnalysisInvalidatedCallback`
  / `registerAnalysesClearedCallback` (already-verified signatures, §2.1 of
  RESEARCH.md) to track a live/cached set of `(IR unit, analysis)` pairs
  through the whole pipeline.
- Recompute `incremental_update_candidate` as: CFG changed AND DominatorTree
  was live at the start of this pass AND it was not preserved AND the pass
  didn't return `all()`. This is the difference between "theoretically could
  have preserved" and "actually threw away a cached, paid-for result."
- Add a `wasted_recompute` counter: for each invalidated-but-later-requested
  analysis, credit the trace with the cost that was actually paid to rebuild
  it (requires pairing an `AnalysisInvalidated` event with a later
  `BeforeAnalysis`/`AfterAnalysis` re-run of the same type on the same unit).
  This turns "candidate" from a suspicion into a measured cost.

## Phase 3 — Static source-level triage (the "Our Proposal" engine)

Goal: go from *tool identifies suspicious runtime behavior* to *tool proposes
the fix location in source*, using the same methodology the background
research agent used manually (RESEARCH.md §4) but automated and re-runnable.

- A Clang LibTooling / AST-matcher pass over `llvm/lib/Transforms/**/*.cpp`
  that finds, per translation unit: NPM `run()` bodies; calls to CFG-mutating
  `Utils` functions (`SplitBlock`, `SplitBlockPredecessors`,
  `removeUnreachableBlocks`, `SplitEdge`, `SplitCriticalEdge`, ...) reachable
  from that `run()`; and whether a `DomTreeUpdater`/`DominatorTree*` argument
  reaching that call site is (a) present, (b) sourced from a live analysis
  result (not a null-initialized local — see the `OpenMPOpt.cpp` trap in
  RESEARCH.md §4), and (c) actually threaded through to the `return` value's
  `PreservedAnalyses`.
- This directly replaces the manual grep-and-read process from the survey
  with something re-runnable against any LLVM revision, and — critically —
  closes the two known false-positive classes the manual survey already hit:
  indirect preservation through a shared helper (`getLoopPassPreservedAnalyses`)
  and DT-shaped-but-null arguments (`OpenMPOpt.cpp`). A tool that reproduces
  the manual survey's 12/14 candidates *and* doesn't reintroduce those two
  false-positive classes is the acceptance bar for this phase.
- Output: same JSONL shape as Phase 1/2 where possible, so `lpta_report.py`
  can render static candidates and dynamic (runtime-observed) candidates in
  one view, cross-referenced by pass name.

## Phase 4 — Propose-and-verify

Goal: close the loop the proposal asks for — "add those API calls and verify
for any breakages" — for the highest-confidence Phase 3 candidates only.

- For a candidate where a live `DominatorTree`/`DomTreeUpdater` is already
  in scope at the CFG-mutating call site (the common case per the survey —
  most candidates already compute a local DT for the mutation itself, they
  just don't propagate the preservation), generate a source patch that adds
  the missing `PA.preserve<DominatorTreeAnalysis>()` (or
  `PA.preserveSet<CFGAnalyses>()` where appropriate).
- Verify via: (a) LLVM's own `-verify-each` / `EXPENSIVE_CHECKS` dominator
  tree verifier running the patched pass across the existing LLVM regression
  test suite and a corpus of real IR; (b) differential testing — compile a
  corpus with patched vs. unpatched LLVM at matched optimization levels and
  diff the generated code (not just IR) for any behavioral divergence.
- This phase is explicitly gated on Phase 3 producing low-false-positive
  candidates; do not attempt automated patching against Phase 1/2's runtime
  heuristic alone, it isn't precise enough (see the SimplifyCFG false-fire
  in RESEARCH.md §5).

## Phase 5 (stretch) — Cross-run regression correlation

The GOAL text in initial-idea.txt also mentions cross-run comparison for
investigating performance regressions ("where cross-run comparison is
implemented"). Once Phase 1's trace format is stable: diff two JSONL traces
(e.g. before/after an LLVM version bump, or before/after a target pipeline
change) keyed by `(pass, unit_name)`, surface passes whose behavior changed
between runs (newly non-preserving, newly no-op, instruction-count delta
sign flip). Deliberately last — it's the most speculative part of the
original proposal and depends on everything above being stable first.
