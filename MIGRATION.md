# LLVM migration and initial PR plan

Review date: 2026-09-13. This document records the existing implementation,
checks performed during the review, and the work needed for the migration.
LLVM 23.1.1 has now been built and tested alongside LLVM 18.1.3. See
[the revalidation results](research/llvm23-revalidation.md). The initial review
and proposed work breakdown below are retained as historical context.

## Current implementation

LPTA is an out-of-tree LLVM New Pass Manager instrumentation plugin, plus a
Python report generator. It does not modify LLVM or propose source patches yet.

1. `src/LPTAInstrumentation.cpp` registers before-pass, after-pass, and
   invalidated-pass callbacks through `PassBuilder`. A stack pairs nested
   invocations. Function and loop events snapshot the enclosing function;
   module and SCC events snapshot the whole module.
2. Each snapshot contains printed IR and instruction, basic-block, and edge
   counts. Changed snapshots produce line-addition/removal counts using LCS,
   with a multiset approximation for large inputs. Snapshot text is temporary;
   the JSONL trace stores measurements, not the before/after IR itself.
3. The plugin records declared analysis preservation and flags CFG-count
   changes for which its DominatorTree preservation check is false.
4. `tools/lpta_report.py` renders the JSONL into a self-contained HTML report
   grouped by IR unit, with a name filter and candidate highlights.

There is one C sample and a shell demo. There is no checked-in CI workflow,
automated assertion suite, versioned experiment manifest, analysis-lifecycle
tracking, static analysis engine, or source-patch verification engine.

At the initial review the project directory had no Git history and the
remote was empty. The public repository is now confirmed as
[`priyatam19/-invariant.group`](https://github.com/priyatam19/-invariant.group),
with an initial README commit on `main`. This baseline is proposed on
`bootstrap/lpta-baseline`; experiment and migration PRs can target that branch
until the baseline is merged. The nested `research/llvm-project` checkout
is a separate Git repository, excluded by `.gitignore`.

## LLVM 18 baseline reproduced

A fresh build with the installed LLVM **18.1.3** development package completed
successfully, using CMake 4.2.1, GNU C++ 13.3.0, and Release mode. Clang and opt
both came from `/usr/lib/llvm-18/bin`.

| Measurement | Result |
|---|---:|
| Pass events | 336 |
| Events changing printed IR | 51 |
| Candidate flags under the existing heuristic | 15 |
| Distinct report unit names | 7 |
| Function / loop / module / SCC events | 267 / 11 / 31 / 27 |
| Unrecognized IR-unit kinds | 0 |

Additional checks passed:

- `opt -verify-each` on the O2 sample pipeline.
- Byte-identical optimized textual IR with and without the plugin on that
  same input and pipeline.
- Contiguous trace sequence numbers.
- Function filtering: `branchy` produced 89 events; `helper,loopy` produced
  189; a nonexistent name produced zero. Filtered records matched the
  corresponding full-trace records after removing sequence numbers.
- HTML report generation.

These checks cover the supplied sample; they do not establish correctness on
a broader corpus or validate the usefulness of each candidate flag. The HTML
was generated but not checked in a browser during this review.

The exact input and implementation hashes, counts, and commands are recorded in
[`research/baselines/llvm-18.1.3.json`](research/baselines/llvm-18.1.3.json).
Review build and output artifacts are local under
`/tmp/lpta-review-llvm18-build` and `/tmp/lpta-review-llvm18-output`.

## Findings that affect interpretation

### Candidate events are not distinct optimization opportunities

Of the 15 flags, three are `SimplifyCFGPass` invocations. The other 12 are
pass managers or adaptors that include changes made by nested passes. Their
snapshots measure inclusive change and can count the same transformation
again. Preserve the historical count, but distinguish wrapper events from
transformation events before ranking opportunities or comparing releases.

Even the three SimplifyCFG flags are only heuristic candidates. The current
plugin does not know whether a cached DominatorTree existed before the pass,
whether one was actually invalidated, or whether it was later recomputed.

### Preservation flags omit set-based preservation

The implementation only calls `getChecker<Analysis>().preserved()` for DT and
LoopInfo. Their LLVM invalidation implementations also recognize
`AllAnalysesOn<Function>` and `CFGAnalyses` preservation through the checker.
The reproduced trace has 14 events with `cfg_analyses_set_preserved=true` but
`dt_preserved=false`. This is a reporting/interpretation gap; it does not mean
that all 14 events are additional candidate flags.

The fix must account for analysis sets, explicit abandonment, and the IR unit
to which the preservation claim applies. Module and loop preservation claims
cannot simply be treated as function-analysis cache observations. Also,
`PreservedAnalyses::all()` is a preservation declaration, not a general proof
that a pass made no IR changes.

Reference: the release's
[DominatorTree invalidation implementation](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.1/llvm/lib/IR/Dominators.cpp)
and
[LoopInfo invalidation implementation](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.1/llvm/lib/Analysis/LoopInfo.cpp).

### CFG changes and transformation magnitudes are approximations

Basic-block and total edge counts miss rewiring that preserves both counts.
Large-input line diffs ignore ordering. Module/SCC events snapshot the whole
module, and loop events snapshot the enclosing function. Version comparisons
must state these limitations and avoid treating the counts as a performance
measurement. Repeated events also need invocation/parent identities; a key
consisting only of pass name and function name is not unique.

### The static survey has different provenance and a counting inconsistency

The source survey used LLVM development `main` commit
`cff226a5716e4bc3589d4869066ea6687ed142ce`, not LLVM 18. This checkout was clean
during review. Repeating its file-level searches reproduced:

- 341 transform `.cpp` files.
- 83 files and 106 occurrences of `PreservedAnalyses::none()`.
- 51 files matching the documented incremental-DT search.
- 53 files matching the documented CFG-utility search.
- 12 files in the raw candidate intersection, **including `GlobalOpt.cpp`**.

The survey later excludes GlobalOpt, leaving **11 unique files** in its
manually listed candidates. Its numbered list contains PGOInstrumentation
twice, for two passes. The claimed 14 return sites require a fresh audit;
their correctness and reachability were not revalidated in this review.
Also, 83 files divided by approximately 236 pass implementations is not a
valid percentage of passes: the numerator and denominator count different
things. These findings should be corrected explicitly, retaining the original
source revision and methodology for reproducibility.

## Migration target and revalidation

The official
[latest stable release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.1)
checked on 2026-09-13 is **LLVM 23.1.1**, published on 2026-09-08. GitHub's
release API reports `prerelease=false` and `draft=false`. Pin
`llvmorg-23.1.1` and its resolved commit for this migration; recheck the latest
release if implementation starts later.

Use separate build/output directories for LLVM 18.1.3 and LLVM 23.1.1. Resolve
Clang, opt, headers, libraries, and CMake configuration from the same selected
installation. Do not use unversioned tools from PATH for the comparison.

Revalidation has two separate tracks:

1. **Runtime experiments:** reproduce the original 18.1.3 result, then run
   identical fixtures and explicit pipelines on 23.1.1. Include both the same
   LLVM 18-generated IR (where accepted) and fresh IR from each matching Clang,
   so frontend changes are distinguishable from optimizer changes. Record
   input hashes, tool versions, commands, target settings, implementation
   revision, trace schema/heuristic version, and output paths. Repeat the
   verifier, instrumentation-transparency, filtering, and report checks.
   Expand coverage to nested pipelines, deleted IR units, preservation sets,
   CFG rewiring, and known analysis lifecycle cases. Changed event totals are
   expected across LLVM versions; explain them rather than asserting equality.
2. **Static research:** retain the original development snapshot as its own
   baseline. Rerun the documented survey on the stable release tag, record
   the full SHA and search definitions, and review each candidate's mutation
   paths and analysis ownership. Report candidates that remain, disappear,
   or are newly found. Do not present the old development survey as an LLVM
   18 result or infer correctness from a grep match.

Do not mix changes to the candidate heuristic with the compiler-version
comparison without labeling each experiment. Use the existing heuristic on
both versions first, then rebaseline the corrected heuristic separately.

## Proposed PR boundaries

After publishing a clean initial baseline, start with these bounded changes.
The branch names below are proposals, not branches or PRs already created.

| PR | Proposed branch | Scope and acceptance |
|---|---|---|
| 1. Reproducible experiments | `test/reproducible-experiments` | Add a repeatable runner, manifests, meaningful regression checks, and comparison summaries in new `tools/`, `test/`, and versioned research files. Reproduce the 18.1.3 baseline and report repeated/nested events without ambiguous matching. Keep the existing heuristic identified explicitly. |
| 2. LLVM 23.1.1 support | `build/llvm-23` | Update toolchain selection, build compatibility where necessary, demo defaults, and CI for LLVM 18 and 23. Build and run with matching tools; rerun runtime experiments and the pinned static survey. Document actual differences and the corrected survey counts. |
| 3. Analysis lifecycle and reporting correctness | `feat/analysis-lifecycle` | Correct set/scope-aware preservation reporting and wrapper attribution, then track cached analyses, invalidations, and later recomputation. Add focused fixtures and expose the evidence in the report. Version the changed heuristic and rerun both toolchains. |

PRs 1 and 2 can begin on separate branches with file ownership kept explicit:
PR 1 owns the new experiment tooling; PR 2 owns `CMakeLists.txt`, toolchain/CI
configuration, and `test/run_demo.sh`. PR 2's final evidence should use PR 1's
runner once it is available. Agree on the trace metadata contract before
starting PR 3; it changes the plugin and report together. Each PR should be
testable and reviewable without merging unrelated feature work.

Automated LLVM source patching remains a later task, after candidate evidence
and preservation correctness have been established.
