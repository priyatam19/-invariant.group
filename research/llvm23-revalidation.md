# LLVM 23.1.1 revalidation

Date: 2026-09-13. The migration uses the official stable release
[`llvmorg-23.1.1`](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.1),
commit `6dfe1677ab8dffbc6ec13d53a1e0215d75147689`.
The Linux X64 archive's SHA-256 was verified as
`b7ddbabd70fa1d206948bc83f59e59aa84eaf4cd09b6b89cc3ece28177710a6f`.
The apt.llvm.org 23 package index inspected during migration served a
23.1.2 development snapshot, so it was not used for stable-release evidence.

## Compatibility change

LLVM 23 moved `PassPlugin.h` from `llvm/Passes` to `llvm/Plugins`. The plugin
selects the available header with `__has_include`; the measurement and
candidate algorithms are unchanged. The release still uses `Any` callbacks,
so no speculative conversion to development-main IR-unit APIs was needed.

CMake defaults to LLVM 23, accepts an explicit LLVM 18 installation, and
records matching Clang/opt/plugin paths for the demo and CTest. Configuration
and demo overrides reject inconsistent tool versions. The official release
installer pins the archive and checksum. CI covers LLVM 18 and 23.1.1.

## Runtime results

All runs use the supplied three-function sample, `default<O2>`, and the same
migrated plugin source. Each run passes trace integrity/sequence checks,
`-verify-each`, byte-identical optimized IR with/without instrumentation,
function-filter equivalence, and HTML generation. CTest and the configured
demo also pass on both installations. Browser rendering was not tested.

| Compiler/input | Events | IR-change events | Historical candidate flags |
|---|---:|---:|---:|
| LLVM 18.1.3 / matching Clang | 336 | 51 | 15 |
| LLVM 23.1.1 / matching Clang | 365 | 52 | 15 |
| LLVM 23.1.1 / exact LLVM 18-generated IR | 365 | 52 | 15 |

The LLVM 18 totals match the original baseline. The newer pipeline has 29
additional events and one additional IR-change event on this sample; an equal
event count across releases is not an acceptance requirement. The exact-IR run
separates optimizer/pipeline effects from frontend differences. Full aggregate
changes are recorded in the comparison manifests below; totals alone do not
show semantic equivalence between compiler versions or performance gains.

Reproducible manifests:

- [LLVM 18.1.3](baselines/experiment-18.1.3.json)
- [LLVM 23.1.1](baselines/experiment-23.1.1.json)
- [LLVM 23.1.1 on the same LLVM 18 IR](baselines/experiment-23-on-18-ir.json)
- [Matching-Clang aggregate comparison](baselines/comparison-matching-clang.json)
- [Same-IR aggregate comparison](baselines/comparison-same-ir.json)

The experiment runner and comparison command are provided by the separate
`test/reproducible-experiments` PR. Its `--source` option allows it to validate
this branch without merging the branches. The recorded runner hash identifies
that tool; source hashes and Git revision identify the implementation tested.
The migration's CTest and demo work independently of that PR.

The 15 flags retain the original heuristic, including inclusive pass-manager
and adaptor events. They are not 15 demonstrated wasted analysis rebuilds.
Preservation-set semantics, analysis cache liveness, and exact CFG comparison
remain separate feature work, as explained in `MIGRATION.md`.

## Static survey on pinned sources

`tools/lpta_survey.py` reproduces the original file-level searches, records
patterns, source SHA, candidate source hashes, and hit locations, and refuses
dirty source checkouts or existing output files. The full results are in
[the original-main manifest](baselines/survey-original-main.json) and
[the stable-release manifest](baselines/survey-llvm-23.1.1.json).

```bash
python3 tools/lpta_survey.py /path/to/pinned/llvm-project -o /tmp/survey.json
```

| Metric | Original main `cff226a5…` | Stable 23.1.1 `6dfe1677…` |
|---|---:|---:|
| Transform `.cpp` files | 341 | 332 |
| Approximate NPM `run()` implementations | 236 | 236 |
| Files containing `PreservedAnalyses::none()` | 83 | 84 |
| Occurrences of that expression | 106 | 107 |
| Files matching incremental-DT search | 51 | 50 |
| Files matching CFG-utility search | 53 | 53 |
| Raw candidate-file intersection | 12 | 13 |

The development snapshot is newer than the stable release; these columns are
different source populations, not an LLVM 18-to-23 trend. No files disappear
from the original raw intersection. `Utils/LoopVersioning.cpp` is additionally
present in the stable-release intersection.

Source triage of the release's raw candidates:

| File under `llvm/lib/Transforms` | Release evidence and interpretation |
|---|---|
| `Instrumentation/ControlHeightReduction.cpp` | `run()` obtains FAM-owned DT at 2122; `SplitBlock` receives it at 1744; the changed path returns `none()` at 2128. Remains a candidate, subject to checking every CFG edit before claiming DT is valid. |
| `Scalar/PlaceSafepoints.cpp` | Local DT is rebuilt and passed to `SplitEdge` at 343; `none()` at 384. Updating a local tree does not prove a cached FAM result was preserved. |
| `IPO/Attributor.cpp` | `SplitBlockPredecessors` at 2593 omits DT; module `run()` returns `none()` at 4109. The CGSCC implementation also has preservation behavior requiring separate scope-aware review. |
| `Coroutines/CoroSplit.cpp` | `run()` removes unreachable blocks at 2305. Coroutine splitting changes functions and SCC structure; this is not established as an avoidable invalidation. |
| `Scalar/GVNSink.cpp` | `SplitBlockPredecessors` at 774 omits DT; `none()` at 849. Remains a source-triage candidate. |
| `Scalar/FlattenCFGPass.cpp` | CFG cleanup at 102; changed-path `none()` at 105. Remains a candidate requiring full mutation-path analysis. |
| `IPO/DeadArgumentElimination.cpp` | `SplitEdge` at 961 omits DT; the module pass can also replace functions. Function replacement complicates analysis ownership. |
| `Instrumentation/GCOVProfiling.cpp` | `SplitCriticalEdge` at 723 omits DT; changed-path `none()` at 580. Candidate, not a demonstrated cached-analysis waste. |
| `Instrumentation/PGOInstrumentation.cpp` | Shared edge instrumentation uses `SplitCriticalEdge` at 878; generation and use return `none()` at 2081 and 2451. One file, multiple passes; count files and return sites separately. |
| `Instrumentation/MemorySanitizer.cpp` | `removeUnreachableBlocks` at 1266 omits DT; PA starts from `none()` at 810. Other instrumentation changes require analysis-specific review. |
| `IPO/OpenMPOpt.cpp` | The merging path still initializes `DT=nullptr` before DT-shaped `SplitBlock` calls (1087/1096). Some early `none()` returns precede that path, so counting every return as the same opportunity is invalid. |
| `IPO/GlobalOpt.cpp` | Excluded from the blanket-invalidation category: FAM invalidation is per changed function at 2851, with proxy/CFG-set preservation afterward at 2861/2865. Manual bookkeeping alone is not proof of a soundness bug. |
| `Utils/LoopVersioning.cpp` | Newly in this release's raw intersection. FAM-owned DT at 321 is passed through versioning, `SplitBlock` at 98, cloning, and explicit `changeImmediateDominator` at 124; changed-path `none()` at 324. The old incremental-update regex misses this maintenance style. Requires full correctness review. |

After excluding GlobalOpt, the raw sets contain 11 and 12 remaining files,
respectively. These are triage sets, not validated counts of fixes. The original
survey's “12 files / 14 return sites” headline conflated the raw intersection
with its reviewed list; its percentage also divided files by pass
implementations. The historical report now carries an explicit correction.

Positive/reference cases still have explicit DT preservation in this release:
SimplifyCFG at 388, JumpThreading at 3215, GVN at 901, and SROA at 6353.
`getLoopPassPreservedAnalyses` still preserves DT through its helper at
`llvm/lib/Analysis/LoopAnalysisManager.cpp:143`. The helper-preservation and
null-DT false-positive cases therefore still matter for future automation.

This revalidation repeats runtime experiments and source triage. It does not
patch LLVM, prove all analysis preservation correct, or measure saved rebuild
time. Those require the planned lifecycle tracking and propose-and-verify work.
