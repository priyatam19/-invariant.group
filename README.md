# LPTA — LLVM Pass Transformation Analyzer

LPTA records LLVM IR before and after every optimization pass, quantifies what
each pass changed, and cross-references that change against which analyses
the pass actually preserved. The headline signal is
`incremental_update_candidate`: **the CFG demonstrably changed, yet
DominatorTree was not preserved and the pass didn't just fall back to
`PreservedAnalyses::all()`** — exactly the "conservative invalidation instead
of incremental update" pattern described in [initial-idea.txt](initial-idea.txt).

See [RESEARCH.md](RESEARCH.md) for the API/prior-art writeup and
[ROADMAP.md](ROADMAP.md) for what's built vs. planned.
See [MIGRATION.md](MIGRATION.md) for the reproduced LLVM 18 baseline, known
measurement limitations, and the LLVM 23 migration plan.

## Status

Phase 1 (instrumentation + quantified trace + HTML report) is built and
working end to end against LLVM 18. See ROADMAP.md for what's next.

## Build

Requires `llvm-18-dev` (or another LLVM `-dev` package with matching
`opt`/`clang`; adjust `LLVM_DIR` accordingly — this machine has 14/15/17/18/20
installed side by side, so auto-detection is not reliable).

```bash
cmake -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Produces `build/LPTAInstrumentation.so`.

## Run

```bash
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone -o input.ll input.c

LPTA_TRACE_OUT=trace.jsonl \
  opt -load-pass-plugin=./build/LPTAInstrumentation.so \
      -passes='default<O2>' -disable-output input.ll

python3 tools/lpta_report.py trace.jsonl -o report.html
```

Or just run the smoke test, which does all of the above against
[test/sample.c](test/sample.c):

```bash
test/run_demo.sh
```

### Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `LPTA_TRACE_OUT` | `lpta_trace.jsonl` | JSONL trace output path |
| `LPTA_FILTER_FUNC` | (unset = all) | Comma-separated function names to restrict tracing to. Module/SCC-level pass events are skipped too when this is set, since they don't resolve to a single function name |

## Trace format

One JSON object per pass invocation, newline-delimited. Key fields:

- `pass`, `unit_kind` (`function`/`loop`/`module`/`scc`), `unit_name`
- `ir_changed`, `before_instr_count`/`after_instr_count`, `before_bb_count`/`after_bb_count`
- `lines_added`/`lines_removed` — line-level LCS diff of the printed IR text
- `cfg_changed` — basic-block count or total successor-edge count differs
- `dt_preserved`, `loop_info_preserved`, `cfg_analyses_set_preserved`, `all_preserved`
- `incremental_update_candidate` — the derived signal described above
- `invalidated` — true if the pass invalidated the IR unit outright (e.g. deleted a function); no `after_*` fields in that case

## Project layout

```
src/LPTAInstrumentation.cpp   the plugin (PassInstrumentationCallbacks hooks)
tools/lpta_report.py           JSONL -> self-contained HTML report
test/sample.c, run_demo.sh     smoke test
research/                      background research: LLVM source survey, notes
RESEARCH.md                    API references, prior art, design rationale
ROADMAP.md                     phased plan
```
