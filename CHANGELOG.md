# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
here are for the tool itself; see each release's notes for the LLVM
compatibility matrix it was validated against (PRODUCTIONIZATION.md §1).

## [Unreleased]

### Added
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
