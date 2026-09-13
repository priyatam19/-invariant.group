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

Instrumentation, quantified traces, and HTML reports are verified on LLVM
**23.1.1** and the LLVM **18.1.3** baseline. See
[the migration revalidation](research/llvm23-revalidation.md) for results and
known heuristic limitations. Analysis cache tracking and automated LLVM source
patches remain planned features.

## Build and test

Requires Linux, CMake 3.21+, Python 3.10+, a C++17 compiler, and matching LLVM
headers, libraries, Clang, and opt. To install the official stable LLVM 23.1.1
Linux x86-64 development archive locally (approximately 2 GB download):

```bash
bash tools/install_llvm23.sh "$HOME/.local/llvm-23.1.1"
cmake -S . -B build -DLLVM_DIR="$HOME/.local/llvm-23.1.1/lib/cmake/llvm"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
bash test/run_demo.sh
```

The installer requires `curl`, `tar`, `zstd`, and `sha256sum`, verifies the
pinned release checksum, and refuses to overwrite an existing directory.
With no explicit `LLVM_DIR`, CMake searches for LLVM 23. The demo uses the
Clang and opt selected at configuration time. Use a separate build for LLVM 18:

```bash
cmake -S . -B build-18 -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build-18 --parallel 2
ctest --test-dir build-18 --output-on-failure
BUILD_DIR="$PWD/build-18" OUT_DIR="$PWD/test/out/llvm18" bash test/run_demo.sh
```

The plugin is `build/LPTAInstrumentation.so`. For a custom input, choose the
same installation used to build that plugin:

```bash
LPTA_LLVM_PREFIX="$HOME/.local/llvm-23.1.1"
"$LPTA_LLVM_PREFIX/bin/clang" -S -emit-llvm -O0 -Xclang -disable-O0-optnone -o input.ll input.c
LPTA_TRACE_OUT=trace.jsonl \
  "$LPTA_LLVM_PREFIX/bin/opt" -load-pass-plugin=./build/LPTAInstrumentation.so \
    -passes='default<O2>' -disable-output input.ll
python3 tools/lpta_report.py trace.jsonl -o report.html
```

The demo accepts `BUILD_DIR` and `OUT_DIR`. Optional `CLANG` and `OPT`
overrides must report the configured LLVM version. Custom `PLUGIN` overrides
must be built against that same LLVM installation.

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
