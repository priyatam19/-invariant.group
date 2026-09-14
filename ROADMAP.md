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

## Phase 2 — Analysis-liveness-aware candidate detection — **DONE**

Goal: fix the over-firing in Phase 1 by knowing whether an analysis was
actually **cached** (live) at the moment a pass invalidated it, not just
whether the pass's CFG edit was the kind DomTreeUpdater handles.

Implemented in `src/LPTAInstrumentation.cpp` (`FunctionAnalysisState`,
`afterAnalysis`/`analysisInvalidated`). Key implementation finding not
anticipated when this phase was scoped: reading LLVM 18's
`AnalysisManager::invalidate()` (`PassManagerImpl.h`) directly confirmed that
`AnalysisInvalidatedCallback` **only ever fires for an analysis that was
actually cached** — it's not a generic "this pass didn't preserve X" signal,
it's already exactly "X was live and just got thrown away." That meant no
separate `registerAnalysesClearedCallback` bookkeeping was needed (its
`StringRef`-only signature can't be correlated back to a specific IR-unit
pointer anyway — see RESEARCH.md §2.1 update) — `AfterAnalysisCallback` +
`AnalysisInvalidatedCallback` alone give ground truth.

- ~~Hook `registerAfterAnalysisCallback` / `registerAnalysisInvalidatedCallback`
  / `registerAnalysesClearedCallback`~~ → done with the first two only, see above.
- `incremental_update_candidate` is now: CFG changed AND DominatorTree was
  live at the start of this pass AND it was not preserved. Verified effect on
  the demo sample: **candidate count dropped from 15 (Phase 1) to 3** — the
  other 12 were exactly the false-positive pattern RESEARCH.md §5 predicted
  (DT never computed at that point in the pipeline). The 3 remaining
  candidates all show `dt_wasted_recompute_count` >= 2, i.e. DominatorTree
  was demonstrably rebuilt multiple times for those functions across the
  pipeline — this is no longer a suspicion.
- Added a `dt_wasted_recompute_count` counter (monotonic per function):
  incremented when an invalidated DominatorTree is later actually
  recomputed, not merely invalidated. Turns "candidate" from a suspicion
  into a measured, cumulative cost.
- New trace fields: `dt_live_before_pass`, `loop_info_live_before_pass`,
  `dt_wasted_recompute_count`, `schema_version` (now `"2.0.0"` — this changed
  `incremental_update_candidate`'s meaning, a major bump per
  `docs/trace-schema.json`'s own versioning rule).

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
