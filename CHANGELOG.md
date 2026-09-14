# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
here are for the tool itself; see each release's notes for the LLVM
compatibility matrix it was validated against (PRODUCTIONIZATION.md §1).

## [Unreleased]

### Added
- **Pass-execution CPU timing** (`pass_cpu_time_us` on `record_type: "pass"`
  records, schema 2.1.0 → 2.2.0): times each pass's own `Pass::run()` call,
  distinct from Tier 2's existing analysis-recompute timing. Used to answer
  RESEARCH.md §10: the 13 bare-`none()` passes from §9 account for only
  0.007% of total measured pass-execution CPU time on an 8-program real
  corpus, and 10 of the 13 never even exercise their invalidating path at
  all on non-coroutine, non-OpenMP C code (they always return `all()`
  because there's nothing to do). Not a meaningful optimization target,
  unlike the DT-focused candidates in §4.
- LLVM New Pass Manager instrumentation plugin (`src/LPTAInstrumentation.cpp`):
  records IR before/after every pass, quantifies the change, and flags
  `incremental_update_candidate` events per the project's core hypothesis.
- JSONL → HTML report generator (`tools/lpta_report.py`).
- Static-survey research on DominatorTree invalidation pervasiveness in
  `llvm-project` (`research/dt_invalidation_survey.md`).
- Governance/CI scaffolding: LICENSE, CONTRIBUTING.md, CODEOWNERS, issue/PR
  templates, `docs/trace-schema.json` as a versioned contract, lint/CodeQL/
  schema-contract GitHub Actions workflows.

- Analysis-liveness tracking (ROADMAP.md Phase 2): `registerAfterAnalysisCallback`/
  `registerAnalysisInvalidatedCallback` hooks track whether DominatorTree/LoopInfo
  were actually cached for a function, not just whether a pass declared them
  preserved. New trace fields: `dt_live_before_pass`, `loop_info_live_before_pass`,
  `dt_wasted_recompute_count`, `schema_version`.
- **Tier 2 analysis-timing experiment** (RESEARCH.md §8): CPU-time capture
  (`llvm::sys::Process::GetTimeUsage`, same technique as `-time-passes`)
  around every actual analysis (re)computation of DominatorTree, LoopInfo,
  MemorySSA, and ScalarEvolution. New `record_type: "analysis"` trace rows
  with `cpu_time_us`/`wasted_recompute`; new `record_type: "pass"` fields
  `memoryssa_live_before_pass`/`scev_live_before_pass`/
  `memoryssa_wasted_recompute_count`/`scev_wasted_recompute_count`. New
  `tools/lpta_timing_report.py` aggregates the headline stat: what fraction
  of analysis-recompute CPU time was spent on a wasted (invalidate-then-
  recompute) cycle. First (small-N, indicative-only) result on the demo
  sample: 34.6% overall, 60.0% for DominatorTree specifically.
- Research: `research/production_vs_teaching_comparison.md` (does
  production LLVM's code-review discipline make automated detection more
  tractable than against out-of-tree code? yes, and RESEARCH.md §7/§6
  explains the concrete why).
- **Tier 1 synthetic microbenchmark** (`bench/dt_microbench.cpp`, new
  `lpta_dt_microbench` CMake target + `tools/lpta_dt_microbench_report.py`):
  controlled linear-chain scaling curve for `DominatorTree::applyUpdates`
  vs. `recalculate()`. Incremental update won in every configuration tested
  (100 to 100,000 blocks × 3 edit positions), but the margin ranged from
  1.4x to 15.9x depending on edit locality relative to dominator-tree
  depth, not just function size — see RESEARCH.md §8.
- Research: `research/full_pass_census.md` (RESEARCH.md §9) — a complete
  census of every NPM-registered pass/analysis (449 total), what a real
  `-O2` invocation actually schedules (75, ~17%) and how much it repeats
  (`simplifycfg`/`instcombine` 8× each), and a full preservation-mechanism
  breakdown of the 67 IR-changing passes that run: 13 bare `none()`, 39
  granular preserve, 11 helper-delegated, 4 surprising unconditional
  `all()`. Cross-checked against real dynamic trace data from this
  project's own corpus, which surfaced a gap static analysis alone
  couldn't: `SimplifyCFGPass` has correct `DomTreeUpdater` code but still
  fails to preserve a live DominatorTree in 236 real invocations, because
  its preservation is conditional on which internal branch a given edit
  takes.

### Verified against real programs
- Tier 2 run against 8 real `llvm-test-suite` benchmarks (not just the
  demo sample): **66.2% of measured analysis-recompute CPU time was spent
  on wasted recomputes**, an order of magnitude more measured CPU time
  than the demo sample. MemorySSA has the lowest wasted-fraction (55.8%)
  but by far the highest absolute cost (~2x DominatorTree's) — the
  expensive part is the MemorySSA/ScalarEvolution cascade, not
  DominatorTree's own recomputation, exactly as RESEARCH.md §6 predicted
  and now measured. See RESEARCH.md §8.

### Changed
- **`incremental_update_candidate` semantics (schema_version 1.0.0 → 2.0.0,
  major bump).** Now requires `dt_live_before_pass` in addition to
  `cfg_changed && !dt_preserved`. Effect on the demo sample: candidate count
  dropped from 15 to 3, with the 3 remaining backed by
  `dt_wasted_recompute_count >= 2` (DominatorTree demonstrably rebuilt
  multiple times) rather than a theoretical "could have preserved" guess.
  See RESEARCH.md §5 for the false-positive case this fixes.
- **Trace schema 2.0.0 → 2.1.0 (minor bump — additive only).** Every record
  now carries `record_type` (`"pass"` or `"analysis"`); existing `"pass"`
  fields and their meanings are unchanged.

### Fixed
- Script-injection risk in the HTML report's embedded trace data (unescaped
  `</script>` in a trace value could close the tag early).
- Loop-kind and Function-kind trace events on the same function collided
  under one report section due to a shared `unit_name`.

### Known limitations
- Verified against LLVM 18.1.3 (and, per PR #3, LLVM 23.1.1); the
  before/after instrumentation-pairing assumption is not yet verified on
  every LLVM version in between.
- Analysis-liveness/timing tracking covers Function-keyed analyses
  (DominatorTree, LoopInfo, MemorySSA, ScalarEvolution) only; Module/SCC/
  Loop-keyed analyses, and BranchProbabilityInfo/RegionInfo (real DT
  dependents per RESEARCH.md §6), are not tracked yet.
  `registerAnalysesClearedCallback` is deliberately not used — its
  callback signature can't be correlated back to a specific IR-unit pointer
  (see RESEARCH.md §2.1 addendum).
- `cpu_time_us` values on small inputs (e.g. the demo sample) are plausibly
  at or below OS timer-resolution noise — treat as indicative only until
  measured against a real benchmark corpus (RESEARCH.md §8, Tier 2).
