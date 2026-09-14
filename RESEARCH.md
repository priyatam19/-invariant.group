# Research notes

The original API checks below concern LLVM 18.1.3, while the original static
survey concerns a development-main commit. The
[LLVM 23.1.1 revalidation](research/llvm23-revalidation.md) now records the stable
release results and corrects the survey's raw-versus-reviewed file counts.
Historical candidate counts below are triage evidence, not confirmed bugs.

## 1. The problem, restated precisely

Two related but distinct problems from [initial-idea.txt](initial-idea.txt):

1. **Visibility**: LLVM's opt pipeline runs dozens to hundreds of passes; there's
   no single view connecting *which pass ran* to *what it changed* to *how much
   that mattered*.
2. **The invalidation tension** (the actual proposal): the New Pass Manager's
   `PreservedAnalyses` model forces every pass author to choose between
   returning `::none()` (correct but wastes recomputation of DominatorTree,
   LoopInfo, MemorySSA, AA, ...) or manually preserving/updating analyses
   (fast but trusted blindly, and wrong updates cause silent miscompiles).
   Problem 1 is a prerequisite for tackling problem 2: you can't tell whether
   an invalidation was avoidable without first being able to see, quantified,
   what the pass actually changed.

## 2. Ground-truth APIs (verified against installed `llvm-18-dev`, not just docs)

This machine has llvm-14/15/17/18/20 `-dev` packages installed side by side
(`dpkg -l | grep llvm`), each with real headers under `/usr/include/llvm-N/`
and a matching `libLLVM-N.so` + `LLVMConfig.cmake` under `/usr/lib/llvm-N/`.
Everything below was checked against the LLVM **18.1.3** headers directly —
the public web docs (llvm.org/doxygen) serve tip-of-tree by default, which
already differs from 18.x in places (see the `Any`/`dyn_cast` note below), so
API claims here are pinned to what actually compiles on this machine.

### 2.1 `PassInstrumentationCallbacks` (`llvm/IR/PassInstrumentation.h`)

The extension point for hooking every pass invocation without forking `opt` or
writing a custom driver. Exact callback signatures (LLVM 18):

```cpp
using BeforePassFunc           = bool(StringRef, Any);
using BeforeSkippedPassFunc    = void(StringRef, Any);
using BeforeNonSkippedPassFunc = void(StringRef, Any);
using AfterPassFunc            = void(StringRef, Any, const PreservedAnalyses &);
using AfterPassInvalidatedFunc = void(StringRef, const PreservedAnalyses &);
using BeforeAnalysisFunc       = void(StringRef, Any);
using AfterAnalysisFunc        = void(StringRef, Any);
using AnalysisInvalidatedFunc  = void(StringRef, Any);
using AnalysesClearedFunc      = void(StringRef);
```

`Any` wraps `const IRUnitT*` for whichever IR unit the pass runs on (`Module`,
`Function`, `Loop`, or `LazyCallGraph::SCC`). **On LLVM 18 there is no
`dyn_cast<T>(Any)` specialization** (that convenience — and the `IRUnitRef`
alias — appears to have been added in a later/dev version; the tip-of-tree
doxygen page shows it, the installed 18.1.3 header does not). The working
pattern on 18 is the classic one:

```cpp
if (auto *FP = llvm::any_cast<const Function *>(&IR))
  return *FP;
```

Before/After calls for a given pass are always paired 1:1 and strictly nested
(LIFO) because pass execution is a synchronous call stack — confirmed by
reading `PassInstrumentation::runBeforePass`/`runAfterPass` in the header, not
assumed. LPTA exploits this: push a snapshot on every `BeforeNonSkippedPass`
call (even ones LPTA_FILTER_FUNC will skip, to keep the stack depth-correct
regardless of filtering), pop-and-match on `AfterPass`/`AfterPassInvalidated`.

A plugin registers callbacks from its `registerPassBuilderCallbacks` entry
point via `PassBuilder::getPassInstrumentationCallbacks()` — no need to patch
`opt` or LLVM itself; `-load-pass-plugin=./libFoo.so` is enough.

**Phase 2 addendum — `AnalysisInvalidatedFunc` semantics, confirmed by
reading the caller, not just the header comment.** `AnalysisManager<IRUnitT>
::invalidate(IR, PA)` (`llvm/IR/PassManagerImpl.h`) iterates only the
analysis results **currently cached** for that exact `IR`, calls each
`Result.invalidate(IR, PA, Inv)`, and fires
`PI->runAnalysisInvalidated(...)` **only for the ones that come back
true and get erased**. It returns immediately, before touching anything,
if `PA.allAnalysesInSetPreserved<AllAnalysesOn<IRUnitT>>()` (i.e. the pass
returned `all()`). Consequence: `AnalysisInvalidatedCallback` is not a
"this pass didn't preserve X" signal — it is already exactly "X was live
and has just been thrown away." No separate liveness bookkeeping was needed
to distinguish "never computed" from "computed then abandoned"; that
distinction is the callback's built-in semantics. This is what Phase 2's
`dt_live_before_pass`/`incremental_update_candidate` fix relies on.

Also confirmed: `AnalysisManager<IRUnitT>::clear(IRUnitT &IR, StringRef Name)`
(the source of `AnalysesClearedFunc`) passes our callback only a caller-
supplied `Name` string, never the `IR` reference itself — so there is no way
to correlate an `AnalysesCleared` event back to a specific tracked pointer
from inside the callback alone. Phase 2 does not use this callback for that
reason (see ROADMAP.md Phase 2); the ordering guarantee `AM.invalidate()`
before `PassInstrumentation::runAfterPass()` (`PassManager.h`, same
function) was the more useful fact: any invalidation a pass causes has
already happened by the time that pass's own `AfterPass` event fires.

### 2.2 Querying `PreservedAnalyses` from outside a pass (`llvm/IR/PassManager.h`)

The documented, public pattern (this is literally quoted in the header's own
doc comment, lines 164-170 of `PassManager.h` in 18.1.3):

```cpp
auto PAC = PA.getChecker<MyAnalysisType>();
if (PAC.preserved() || PAC.preservedSet<AllAnalysesOn<MyIRUnit>>() || ...)
```

Key semantics, read directly from `PreservedAnalysisChecker`/`PreservedAnalyses`:

- `getChecker<AnalysisT>().preserved()` is true if that analysis was
  explicitly preserved **or** if `PreservedAnalyses::all()` was returned
  (`all()` inserts a sentinel `AllAnalysesKey` that every checker matches).
- `areAllPreserved()` — true only if nothing was abandoned and the `all()`
  sentinel is present. This is the correct test for "pass claims to have
  changed nothing."
- `allAnalysesInSetPreserved<CFGAnalyses>()` — tests the curated "CFG-only"
  analysis set (defined as basic blocks + edges between them, per the header's
  own doc comment on `CFGAnalyses`) as a bundle, independent of any specific
  analysis type.

LPTA uses `getChecker<DominatorTreeAnalysis>()`, `getChecker<LoopAnalysis>()`,
and `allAnalysesInSetPreserved<CFGAnalyses>()` directly — no LLVM patch
required, this is all public API.

### 2.3 `DomTreeUpdater` (`llvm/Analysis/DomTreeUpdater.h`)

The API pass authors are *supposed* to reach for instead of invalidating:
`insertEdge`/`deleteEdge`/`applyUpdates` under an Eager or Lazy update
strategy, `DominatorTree::applyUpdates` for batched CFG edits. This already
exists and works — the survey in §4 confirms real passes (SimplifyCFG,
JumpThreading, GVN, SROA) use it correctly today. The gap is adoption, not
missing infrastructure.

## 3. Prior art — what already exists, and the gap LPTA fills

| Tool | Mechanism | Quantifies change? | Correlates to analysis preservation? |
|---|---|---|---|
| `opt -print-changed` / `-print-before-changed` ([D86360](https://reviews.llvm.org/D86360), [D88757](https://reviews.llvm.org/D88757)) | Prints IR text before/after each pass that changed it; banner-only for no-op passes | No — presence/absence of a textual diff only | No |
| `opt -print-changes` (the newer, IBM-contributed option — see Jamie Schmeiser, *"Understanding Changes made by a Pass in the Opt Pipeline,"* [LLVM Dev Meeting 2020](https://llvm.org/devmtg/2020-09/slides/Schmeiser-Understanding_Changes_made_by_pass_in_the_opt_pipeline.pdf)) | Shells out to the **system `diff`** binary (non-POSIX line-format flags) to render a patch-style `+`/`-` view; also ships `-dot-cfg-changes` for CFG graphs | No — visual diff only, explicitly line-format-based so it "will not recognize" changes like function-attribute-only edits (the talk's own caveat, slide 40) | No |
| `opt-viewer.py`/`opt-diff.py` | Parses YAML optimization-remark files (`-fsave-optimization-record`) | Partially — remarks are per-decision, not aggregated into a magnitude score | No — remarks are about missed/applied individual optimizations, not analysis lifecycle |
| `llvm-optiler` (community, [GitHub](https://github.com/TiElixir/llvm-optiler)) | Browser UI over captured pass snapshots | No | No — and doesn't support LLVM IR at all yet (Torch-MLIR/MLIR only); 5 commits, minimally active |
| **LPTA (this project)** | `PassInstrumentationCallbacks` hooks + LCS line diff + `PreservedAnalyses::getChecker<...>()` | **Yes** — instruction/BB count deltas, LCS-based added/removed line counts, CFG-edge-count delta | **Yes** — `dt_preserved`, `loop_info_preserved`, `cfg_analyses_set_preserved`, `all_preserved`, and the derived `incremental_update_candidate` flag |

The load-bearing gap: every existing tool answers "did this pass change the
IR, and what did the text look like" (a debugging aid). None of them close
the loop back to "...and was that change of a kind DomTreeUpdater could have
handled, given what the pass actually told the PassManager it preserved."
That correlation is LPTA's reason to exist, and it's a straightforward
consequence of combining two APIs (`PassInstrumentationCallbacks` +
`PreservedAnalyses::getChecker`) that already coexist in every LLVM build —
no upstream patch needed to get first data.

## 4. Is the invalidation problem actually pervasive? (survey, not a guess)

A background survey against a live clone of `llvm/llvm-project` (`main` @
`cff226a5716e4bc3589d4869066ea6687ed142ce`, cloned 2026-09-13) answered this
with grep-then-hand-verify methodology. Full report, every count reproducible
by the commands shown: [research/dt_invalidation_survey.md](research/dt_invalidation_survey.md).
Headline numbers:

- **~236** New-Pass-Manager `run()` implementations in `llvm/lib/Transforms`.
- **83 files / 106 occurrences** of a bare `PreservedAnalyses::none()`.
- **51 files** already do incremental `DomTreeUpdater`/`applyUpdates` work
  (SimplifyCFG, JumpThreading, GVN, SROA, LICM, ... — confirmed by reading the
  actual source, not just the grep hit).
- **12 files / 14 distinct `run()` return sites**, cross-referenced by hand,
  where a pass (a) calls a CFG-mutating utility, (b) returns a bare `none()`,
  and (c) has zero `DomTreeUpdater`/incremental-DT machinery anywhere in the
  file — the pattern from the proposal, made concrete.
- **Strongest single example**: `ControlHeightReduction.cpp:2128` obtains the
  *live, analysis-manager-owned* `DominatorTree`, threads it into
  `SplitBlock(..., &DT)` (which does a real incremental update on that exact
  object per `BasicBlockUtils.cpp:1062-1068`), and then still returns
  `PreservedAnalyses::none()` — discarding an analysis it had just kept
  correct. `PlaceSafepoints.cpp:384` and `Attributor.cpp:4110` carry LLVM's
  own `// TODO: can we preserve more?` / `// FIXME: Think about passes we
  will preserve` comments next to the same pattern — the authors already
  know.

Important caveat carried forward from that survey (see its §5 for the full
list): `PreservedAnalyses::none()` is *sometimes provably necessary*. The
survey identifies **candidates for investigation**, not confirmed bugs — each
needs a correctness review before a patch is proposed. It also surfaced a
concrete trap for any naive automated detector: `OpenMPOpt.cpp` passes a
`DominatorTree *DT` argument to `SplitBlock` that is statically `nullptr` in
that code path, so "calls a DT-aware overload" is not sufficient evidence on
its own — the detector must confirm the DT pointer is actually live.

## 5. What LPTA's own MVP run already shows (not just the static survey)

Running the Phase 1 plugin (see README.md) against a 3-function test file
through the real `-passes='default<O2>'` pipeline produced 336 pass-invocation
records, of which 15 were flagged `incremental_update_candidate`. One
instructive nuance surfaced immediately and is recorded here rather than
glossed over: several flagged records are `SimplifyCFGPass` invocations where
`dt_preserved=false` — but SimplifyCFG is precisely the pass the static survey
(§4) confirms does the *right* thing when DominatorTree is live. The
resolution is that DominatorTree simply hadn't been computed/cached yet at
that point in the pipeline, so there was nothing live to preserve —
"invalidating" an unbuilt analysis costs nothing. **This meant the Phase 1
CFG-changed-and-not-preserved heuristic was a coarse first-pass signal, not a
finished verdict.**

**Update (Phase 2, done):** liveness tracking via
`registerAfterAnalysisCallback`/`registerAnalysisInvalidatedCallback` (see
§2.1 addendum) fixed this. Re-running the identical sample: candidate count
went from 15 to **3**, and all 3 remaining candidates show
`dt_wasted_recompute_count >= 2` — DominatorTree was demonstrably rebuilt
multiple times for those functions across the pipeline, not merely
"theoretically preservable." The false positive described above (SimplifyCFG
on `loopy`) is no longer flagged. See ROADMAP.md Phase 2 for the
implementation and `docs/trace-schema.json`'s `dt_live_before_pass`/
`dt_wasted_recompute_count` fields for the new ground truth this is based on.

## 6. What actually depends on DominatorTree, and does updating it cascade?

Read the real `invalidate()` implementations in `llvm/lib/Analysis/*.cpp`
(via the `research/llvm-project` clone from §4) rather than assume. Answer:
**there is no automatic cascade; every dependent analysis needs its own
explicit preservation, and LLVM uses two genuinely different mechanisms for
different dependents:**

- **`LoopInfo`, `BranchProbabilityInfo`, `RegionInfo`** — each `invalidate()`
  (e.g. `LoopInfo::invalidate`, `LoopInfo.cpp:941`) checks *only* its own
  analysis type's checker against `preserved()`, `AllAnalysesOn<Function>`,
  or the `CFGAnalyses` bundle set. **It never looks at
  `DominatorTreeAnalysis`'s status at all.** Correctly, incrementally
  updating DT and calling `.preserve<DominatorTreeAnalysis>()` buys these
  three nothing — each needs its own separate `.preserve<T>()` (or the
  `CFGAnalyses` bundle) or it's discarded and rebuilt regardless of how
  correct DT is.
- **`MemorySSA`, `ScalarEvolution`** — use LLVM's `Invalidator` mechanism
  instead: `MemorySSAAnalysis::Result::invalidate` (`MemorySSA.cpp:2376`) is
  `!(own-preserved || all()) || Inv.invalidate<AAManager>(...) ||
  Inv.invalidate<DominatorTreeAnalysis>(...)`;
  `ScalarEvolution::invalidate` (`ScalarEvolution.cpp:15100`) adds
  `Inv.invalidate<LoopAnalysis>`/`Inv.invalidate<AssumptionAnalysis>` too.
  This is a **one-way safety net, not a free ride**: if DT (or AA, or
  LoopInfo) actually dies, these are forced to die too even if a pass
  incorrectly claimed to preserve them — but correctly preserving DT is
  still necessary, never sufficient, for MemorySSA/SCEV to survive; each
  still needs its own explicit preservation as well.
- `PostDominatorTree` is fully independent (`PostDominators.cpp:43`) — its
  own separate computation, not derived from `DominatorTree`.
- Asymmetry worth flagging: `LoopInfo`/`BranchProbabilityInfo`/`RegionInfo`
  don't hook the `Invalidator` at all, so there's no engine-level
  consistency check tying them to DT's real status the way MemorySSA/SCEV
  get. A pass could in principle claim `.preserveSet<CFGAnalyses>()` while
  its DT update was subtly wrong, and the framework wouldn't catch the
  inconsistency for those three the way it would for MemorySSA/SCEV.

Practical consequence for the original thesis: the real cost `none()`
wastes usually isn't DominatorTree's own recomputation (near-linear,
genuinely fast) — it's that a bare `none()` needlessly drags down MemorySSA
and ScalarEvolution too, both markedly more expensive to rebuild, purely
because nobody added the extra `.preserve<T>()` calls. §8 measures this
directly.

## 7. Production LLVM vs. teaching/research pass code

Does production LLVM's code review discipline make automated
DT-update-opportunity detection more tractable than it would be against
arbitrary out-of-tree code? Full study, with citations, in
[research/production_vs_teaching_comparison.md](research/production_vs_teaching_comparison.md).
Headline: yes, but for a narrow, concrete reason — production's CFG edits
converge on a small, stable, *named* Utils-function surface (53/341 files,
~8 function names); none of three surveyed non-reviewed corpora
(`llvm-tutor`, a personal loop-unroller, a real university course
assignment) converge on that same surface, each for a different reason
(different-but-still-standard helpers, fully raw `BranchInst` surgery, or
no CFG edits at all). A Phase 3 AST-matcher tuned on production call-site
names should work well against `llvm/lib/Transforms` but needs a materially
different approach for arbitrary out-of-tree code — a scoping constraint
for Phase 3, not an aside.

## 8. Experiment design: is incremental update actually cheaper?

Three-tier plan (full design rationale, benchmark choices, and rigor notes
were worked out in conversation; captured here for durability):

- **Tier 1** — synthetic/controlled microbenchmark (parameterized CFG
  shapes, no LLVM patch needed): isolates how the incremental-vs-full-
  recompute cost ratio scales with function size and edit locality, free of
  real-world variance. Not yet built.
- **Tier 2** — real-pipeline passive measurement (built, this section):
  extends `LPTAInstrumentation.cpp` with CPU-time capture (via
  `llvm::sys::Process::GetTimeUsage`, the same technique `-time-passes`
  uses — process user+sys CPU time, not wall-clock, since wall-clock on a
  shared/virtualized sandbox picks up scheduling noise this metric avoids)
  around every actual `BeforeAnalysis`/`AfterAnalysis` (re)computation of
  the four tracked analyses (DominatorTree, LoopInfo, MemorySSA,
  ScalarEvolution — confirmed these callbacks fire only around a genuine
  cache miss by reading `AnalysisManager::getResultImpl`'s `if (Inserted)`
  gate in `PassManagerImpl.h`, never on a cache hit). Emits a new
  `record_type: "analysis"` trace row per (re)computation with `cpu_time_us`
  and `wasted_recompute` (paired with the existing invalidate-then-recompute
  tracking from Phase 2, generalized from DT-only to all four analyses).
  `tools/lpta_timing_report.py` aggregates this into the headline number: what
  fraction of measured analysis-recompute CPU time was spent on a recompute
  that followed an invalidation nothing needed until it became needed again.
  **First result, on the 3-function demo sample** (small-N, not yet a real
  benchmark — see caveat below): repeated runs land in the **30-35%** range
  for total measured CPU time across the four analyses spent on wasted
  recomputes (DominatorTree alone: roughly two-thirds, out of 10 of 14
  computes flagged `wasted_recompute` — that *count* is exact and stable
  across runs, unlike the CPU-time percentage). The run-to-run drift in the
  timing percentage itself (33.0% one run, 34.6% another, same trace
  regenerated seconds apart) is direct empirical confirmation of the noise
  caveat below, not a bug — deliberately reporting a range here rather than
  a single misleadingly-precise number. Real magnitude requires Tier 2 run
  against `llvm-test-suite` or comparable real programs — at this sample's
  scale, individual `cpu_time_us` values (single-digit microseconds) are
  plausibly at or below OS timer-resolution noise; the *count*-based
  wasted-recompute signal is
  real and discrete, but the *timing* numbers at this scale are indicative
  only, not conclusive.

  **Second result, on 8 real programs from `llvm-test-suite`**
  (`llvm/llvm-test-suite` @ `4b2c892865cbbb05ee996c88794a58e17d3adb81`,
  `SingleSource/Benchmarks/{Linpack/linpack-pc.c, Misc/oourafft.c,
  Misc/whetstone.c, Misc/richards_benchmark.c, McGill/chomp.c,
  CoyoteBench/{fftbench.cpp,huffbench.c,almabench.c}}` — classic CPU
  benchmarks: Whetstone, Linpack, an FFT, a Huffman coder, chosen because
  they each compile standalone with no extra include paths and are large
  enough to move past the demo sample's noise floor, 900-4700 lines of `-O0`
  IR each): **66.2% of all measured analysis-recompute CPU time (9,037us
  total, across 1,882 individual recomputes) was spent on wasted
  recomputes** — not a small-N artifact, an order of magnitude more
  measured CPU time than the demo sample. Per-analysis breakdown makes the
  §6 cascade argument concrete rather than theoretical:

  | Analysis | Computes | Wasted | Total CPU | Wasted CPU | % wasted |
  |---|---|---|---|---|---|
  | DominatorTreeAnalysis | 659 | 518 | 1,854us | 1,490us | 80.4% |
  | LoopAnalysis | 404 | 264 | 1,621us | 988us | 61.0% |
  | MemorySSAAnalysis | 344 | 207 | **3,680us** | 2,053us | 55.8% |
  | ScalarEvolutionAnalysis | 475 | 335 | 1,882us | 1,448us | 76.9% |

  MemorySSA has the *lowest* wasted-fraction of the four but the *highest*
  absolute CPU cost by a wide margin (nearly double DT's) — exactly the §6
  prediction that the expensive part isn't DT's own recomputation (fast,
  near-linear) but the cascade into genuinely costly dependents. DT itself
  shows the highest waste fraction (80.4%) because it sits at the root of
  that cascade and gets invalidated most often.

  Reproduction:
  ```bash
  git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/llvm/llvm-test-suite.git research/llvm-test-suite
  cd research/llvm-test-suite && git sparse-checkout set SingleSource/Benchmarks
  # compile each file: clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone <file> -o <file>.ll
  # run each: LPTA_TRACE_OUT=<name>.jsonl opt -load-pass-plugin=./build/LPTAInstrumentation.so \
  #   -passes='default<O2>' -disable-output <file>.ll
  python3 tools/lpta_timing_report.py *.jsonl
  ```
  Caveat carried forward from Tier 1: this is 8 files, not a statistically
  representative sample of "real programs" in general, and per-analysis
  costs will vary with how loop/memory/induction-variable-heavy a given
  program is — Whetstone/Linpack/FFT/Huffman are numerically dense by
  design, which plausibly inflates ScalarEvolution's share specifically.
  Treat 66.2% as "large and real on this corpus," not as a universal
  constant.

- **Tier 3** — A/B with a real source patch (gated on Phase 3/4, not yet
  buildable): the only tier that actually proves a specific fix helps.
  Checked whether instrumentation alone could simulate this (have the
  plugin intercept and rewrite a pass's returned `PreservedAnalyses` before
  the pass manager acts on it) — confirmed this is not possible:
  `AnalysisManager::invalidate()` runs *before* `PassInstrumentation::
  runAfterPass()` for the same pass invocation (`PassManager.h`), so
  invalidation has already happened by the time any plugin callback could
  observe or react to it. Tier 3 genuinely requires a source patch (e.g. add
  the missing `.preserve<DominatorTreeAnalysis>()` to
  `ControlHeightReduction.cpp:2128`, per §4's top candidate) and two rebuilt
  `opt` binaries compared end-to-end — exactly ROADMAP.md Phase 4's scope,
  not a shortcut around it.
