## What and why

<!-- What changed, and why. Link the ROADMAP.md phase or PRODUCTIONIZATION.md
     section this belongs to, if any. -->

## LLVM version(s) tested

<!-- e.g. "18.1.3" or "18.1.3 and 23.1.1" -->

## Trace schema impact

- [ ] No change to JSONL trace fields
- [ ] Added an optional field (minor `schema_version` bump in `docs/trace-schema.json`)
- [ ] Changed/removed a field's meaning (major bump; documented in RESEARCH.md/CHANGELOG.md)
- [ ] Updated `test/fixtures/*.jsonl` to match

## Checks run locally

- [ ] `cmake --build build` succeeds
- [ ] `test/run_demo.sh` produces a sane event count (not 0, no crash)
- [ ] `clang-format --dry-run --Werror src/*.cpp` clean (if `src/` touched)
- [ ] `ruff format --check tools/*.py` clean (if `tools/` touched)
- [ ] `python3 tools/validate_trace.py test/fixtures/*.jsonl` clean (if the schema or fixtures changed)

## Review

<!-- No self-merge without review — see CONTRIBUTING.md. If you're an AI
     agent, note here whether a /code-review pass was run and what it found. -->
