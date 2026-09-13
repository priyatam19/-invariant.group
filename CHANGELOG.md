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

### Fixed
- Script-injection risk in the HTML report's embedded trace data (unescaped
  `</script>` in a trace value could close the tag early).
- Loop-kind and Function-kind trace events on the same function collided
  under one report section due to a shared `unit_name`.

### Known limitations
- `incremental_update_candidate` is a coarse heuristic (CFG changed + DT not
  preserved), not liveness-aware — see ROADMAP.md Phase 2.
- Verified against LLVM 18.1.3 (and, per PR #3, LLVM 23.1.1); the
  before/after instrumentation-pairing assumption is not yet verified on
  every LLVM version in between.
