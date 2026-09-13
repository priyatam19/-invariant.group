# DominatorTree Invalidation Survey — LPTA Research

**Goal:** Quantify how often LLVM `Transforms` passes (New Pass Manager) discard
`DominatorTree`/CFG analyses via `PreservedAnalyses::none()` (or by omitting
`.preserve<DominatorTreeAnalysis>()`/`.preserveSet<CFGAnalyses>()`) in code paths
that make exactly the kind of CFG edit `DomTreeUpdater`/`DominatorTree::applyUpdates`/
`insertEdge`/`deleteEdge` is designed to patch up incrementally — instead of doing the
incremental update and preserving the analysis.

**Repo snapshot analyzed:** `llvm/llvm-project` @ `cff226a5716e4bc3589d4869066ea6687ed142ce`
(HEAD of `main` at clone time, 2026-09-13).

All counts below are reproducible from the exact commands shown. Nothing was invented.

---

## 1. Methodology

### 1.1 Getting the source

```bash
cd /home/fuzzserver/llvm-pass-transformation-analyser/research
git clone --depth 1 --filter=blob:none --sparse \
  https://github.com/llvm/llvm-project.git llvm-project
cd llvm-project
git sparse-checkout set --skip-checks \
  llvm/lib/Transforms llvm/lib/Analysis \
  llvm/include/llvm/IR/PassManager.h \
  llvm/include/llvm/Analysis/DomTreeUpdater.h \
  llvm/include/llvm/IR/Dominators.h
```

**Result:** the direct GitHub clone worked on the first try — no fallback to
`apt-get source` or tarball download was needed. Method used: **shallow + blobless +
sparse `git clone`**, `--skip-checks` was required on `sparse-checkout set` only
because individual file paths (e.g. `PassManager.h`) were mixed with directory paths
in cone mode.

- Clone: ~2 seconds (fast network / cached objects on GitHub's CDN)
- Sparse-checkout population: ~4 seconds
- Total wall time: well under 10 seconds
- Disk usage: **67M total** (36M `.git`, 18M `llvm/lib/Transforms`, 5.8M `llvm/lib/Analysis`)
- Files in scope: **341** `.cpp` files under `llvm/lib/Transforms`, **125** under
  `llvm/lib/Analysis`, 440,389 total lines across the `Transforms` `.cpp` files.
- `.git` history was **not** removed (67M is small enough that there's no reason
  to; `git log`/`git blame` remain available for follow-up work).

### 1.2 Grep methodology (step 2)

All commands were run from
`/home/fuzzserver/llvm-pass-transformation-analyser/research/llvm-project`.
Scope for every count is `llvm/lib/Transforms/**/*.cpp` unless stated otherwise.

---

## 2. Summary table of counts

| # | Metric | Command (abbreviated) | Count |
|---|--------|------------------------|-------|
| 1 | NPM `run()` implementations, same-line signature | `grep -rn "PreservedAnalyses [A-Za-z_:<>]*::run(" llvm/lib/Transforms --include=*.cpp \| wc -l` | **212** |
| 2 | NPM `run()` implementations, return-type-on-own-line signature | `grep -A1 "^PreservedAnalyses$"` filtered to lines with `::run(` | **24** |
| 3 | **Total NPM `run()` implementations (1+2, denominator)** | sum of above | **≈236** |
| 4 | Files containing literal `PreservedAnalyses::none()` | `grep -rl "PreservedAnalyses::none()" ...` | **83** files |
| 5 | Total occurrences of `PreservedAnalyses::none()` (some files have several `run()`s) | `grep -rn ...` | **106** |
| 6 | Files with `return PreservedAnalyses::none();` specifically | `grep -rl "return PreservedAnalyses::none()"` | **61** files |
| 7 | Files using the `PreservedAnalyses PA` builder pattern | `grep -rl "PreservedAnalyses PA"` | **101** files (118 occurrences) |
| 8 | Of those 101, files that **never** call `.preserve<DominatorTreeAnalysis>()` or `.preserveSet<CFGAnalyses>()` anywhere in the file | per-file loop, see §2c below | **14** files |
| 9 | Files referencing `DomTreeUpdater` / `applyUpdates(` / `DT->insertEdge(` / `DT->deleteEdge(` (i.e. already do incremental DT maintenance) | `grep -rlE "DomTreeUpdater\|\.applyUpdates(\|..."` | **51** files |
| 10 | Files with CFG-mutating calls (`SplitBlock`, `SplitBlockPredecessors`, `MergeBlockIntoPredecessor`, `removeUnreachableBlocks`, `RemoveSuccessor`, `SplitEdge`, `SplitCriticalEdge`, `MergeBasicBlockIntoOnlyPred`) | `grep -rlE "SplitBlock(\|..."` | **53** files |
| 11 | Of the 83 files with `PreservedAnalyses::none()`, those with **zero** DomTreeUpdater/incremental-DT mentions anywhere in the file | `comm -23` of #4 and #9 | **80** files |
| 12 | **Final cross-reference: files that (a) call a CFG-mutating utility, (b) contain a literal `PreservedAnalyses::none()`, and (c) never mention `DomTreeUpdater`/`applyUpdates`/`insertEdge`/`deleteEdge` anywhere in the file** | `comm -12` of #10 and #11 | **12 files** (covering **14** distinct `run()`-return sites — see §3) |

So: of ~236 NPM `run()` implementations in `llvm/lib/Transforms`, **83 files (35%)**
contain at least one bare `PreservedAnalyses::none()`, and after cross-referencing
against actual CFG-mutating calls and the complete absence of any incremental-DT
machinery in the same file, **12 files / 14 return sites** are concrete,
inspected-by-hand candidates where the pattern in the hypothesis is directly visible.
This is a lower bound — see Caveats (§5) for why the true number is almost certainly
higher (the grep is conservative and per-file, not per-function).

---

## 3. Top candidates (step 2d) — file:line + reason

All 12 files below were individually opened and the cited lines checked by hand
(not just grep hits) to confirm the CFG-mutating call and the `none()` return are
in the same pass. Ordered roughly by strength of evidence.

### 1. `llvm/lib/Transforms/Instrumentation/ControlHeightReduction.cpp:2128` — strongest evidence
`ControlHeightReductionPass::run()` (function starts at line 2112) obtains the
**live, analysis-manager-owned** `DominatorTree` at line 2122:
`auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);` and threads it by reference into
the `CHR` helper class (`DominatorTree &DT;` member at line 373). Inside `CHR::run()`,
line 1744 calls `SplitBlock(EntryBlock, Scope->BranchInsertPoint, &DT)` — the
`DominatorTree*`-taking overload of `SplitBlock`, which (per
`llvm/lib/Transforms/Utils/BasicBlockUtils.cpp:1062-1068`) does a real incremental
`DT->addNewBlock(...)`/`DT->changeImmediateDominator(...)` update **on that exact DT
object**. Despite the DT being kept correct in place, `run()` unconditionally returns
`PreservedAnalyses::none()` at line 2128 — throwing away an already-valid analysis.

### 2. `llvm/lib/Transforms/Scalar/PlaceSafepoints.cpp:384`
`PlaceSafepointsPass::run()` returns `PreservedAnalyses::none()` with the comment
`// TODO: can we preserve more?` immediately above it (line 383). The internal
`runImpl` builds a local `DominatorTree DT; DT.recalculate(F);` (lines 279-280) and
then calls `SplitEdge(Term->getParent(), Header, &DT)` at line 343, which incrementally
updates that DT. The comment right above the `SplitEdge` call (lines 339-340) even
says: *"The split loop structure here is so that we only need to recalculate the
dominator tree once. Alternatively, we could just keep it up to date..."* — LLVM's own
authors flagging the exact tension this project investigates.

### 3. `llvm/lib/Transforms/IPO/Attributor.cpp:4110`
`AttributorPass::run()` calls `runAttributorOnFunctions(...)` and, if it returns
true, returns `PreservedAnalyses::none()` with the adjacent comment
`// FIXME: Think about passes we will preserve and add them here.` (line 4109).
`SplitBlockPredecessors(NormalDestBB, {BB}, ".dead")` is called at line 2593
(no DT argument passed, though a DT-aware overload exists — see Utils note below).

### 4. `llvm/lib/Transforms/Coroutines/CoroSplit.cpp:2414`
`CoroSplitPass::run()` calls `removeUnreachableBlocks(F)` at line 2351 (inside the
same function, in the coroutine-splitting loop) and unconditionally returns
`PreservedAnalyses::none()` at line 2414.

### 5. `llvm/lib/Transforms/Scalar/GVNSink.cpp:852`
`GVNSinkPass::run()` calls into `GVNSink::run()`, whose sinking logic calls
`SplitBlockPredecessors(BBEnd, C.Blocks, ".gvnsink.split")` at line 777 — the
3-argument overload with **no** `DominatorTree*`/`DomTreeUpdater*` argument, even
though `BasicBlockUtils.h` exposes both DT-aware and DTU-aware overloads of
`SplitBlockPredecessors`. `run()` returns `PreservedAnalyses::none()` at line 852.

### 6. `llvm/lib/Transforms/Scalar/FlattenCFGPass.cpp:105`
`FlattenCFGPass::run()` (NPM version, lines 96-105) loops
`while (iterativelyFlattenCFG(F, AA)) { removeUnreachableBlocks(F); ... }` then
returns `EverChanged ? PreservedAnalyses::none() : PreservedAnalyses::all();` —
no DT is threaded through `iterativelyFlattenCFG` or `removeUnreachableBlocks` at all.

### 7. `llvm/lib/Transforms/IPO/DeadArgumentElimination.cpp:1118`
`DeadArgumentEliminationPass::run()` drives `removeDeadStuffFromFunction`, which
calls `SplitEdge(NewCB->getParent(), II->getNormalDest())` at line 961 (no DT arg),
then `run()` returns `PreservedAnalyses::none()` at line 1118.

### 8. `llvm/lib/Transforms/Instrumentation/GCOVProfiling.cpp:580`
`GCOVProfilerPass::run()` calls `Profiler.runOnModule(...)`, whose per-function
instrumentation calls `SplitCriticalEdge(TI, SuccNum)` at line 723 (no DT arg), then
returns `PreservedAnalyses::none()` at line 580.

### 9. `llvm/lib/Transforms/Instrumentation/PGOInstrumentation.cpp:2081`
`PGOInstrumentationGen::run()` calls `InstrumentAllFunctions(...)`, whose
implementation calls `SplitCriticalEdge(TI, SuccNum)` at line 878, then returns
`PreservedAnalyses::none()` at line 2081.

### 10. `llvm/lib/Transforms/Instrumentation/PGOInstrumentation.cpp:2453`
Second `run()` in the same file: `PGOInstrumentationUse::run()` calls
`annotateAllFunctions(...)` (same underlying instrumentation code path, same
`SplitCriticalEdge` at line 878) and independently returns
`PreservedAnalyses::none()` at line 2453.

### 11. `llvm/lib/Transforms/Instrumentation/MemorySanitizer.cpp` (~line 810)
`MemorySanitizerPass::run()` builds `PreservedAnalyses PA = PreservedAnalyses::none();`
then only calls `.abandon<GlobalsAA>()` — never `.preserve<DominatorTreeAnalysis>()`.
`Msan.sanitizeFunction(...)` calls `removeUnreachableBlocks(F)` at line 1266 with no
DT threading.

### 12. `llvm/lib/Transforms/IPO/OpenMPOpt.cpp:6005, 6053, 6124`
Three separate `run()` returns (`OpenMPOptPass::run()` twice,
`OpenMPOptCGSCCPass::run()` once) return `PreservedAnalyses::none()`. `SplitBlock`
is called repeatedly with a `DominatorTree *DT` argument (e.g. line 1167), **but**
`DT` is declared `DominatorTree *DT = nullptr;` at line 1158 and is never reassigned
in that scope before use — so despite the DT-aware call signature, no actual
incremental update happens here (see caveat below; this is a case where the
*plumbing* for incremental update exists but is wired to a null pointer).

**Total distinct return sites cited above: 14**, across the 12 files.

---

## 4. Positive/reference examples (step 2e) — passes that already do it right

Confirmed by direct grep, not memory:

- **SimplifyCFG** — `llvm/lib/Transforms/Scalar/SimplifyCFGPass.cpp`
  - `#include "llvm/Analysis/DomTreeUpdater.h"` at line 29
  - `DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Eager);` at line 275
  - `SimplifyCFGPass::run()` (line 378) conditionally acquires
    `DominatorTree *DT = &AM.getResult<DominatorTreeAnalysis>(F)` (line 384) and, if
    used, calls `PA.preserve<DominatorTreeAnalysis>();` at line 392.

- **JumpThreading** — `llvm/lib/Transforms/Scalar/JumpThreading.cpp`
  - `JumpThreadingPass::run()` (line 240) constructs
    `std::make_unique<DomTreeUpdater>(&DT, nullptr, DomTreeUpdater::UpdateStrategy::Lazy)`
    at lines 253-254, flushes it (`getDomTreeUpdater()->flush();`, line 261), and
    verifies it with asserts (lines 264-276).
  - `JumpThreadingPass::getPreservedAnalysis()` explicitly calls
    `PA.preserve<DominatorTreeAnalysis>();` at line 3229.

- **GVN** — `llvm/lib/Transforms/Scalar/GVN.cpp`
  - `#include "llvm/Analysis/DomTreeUpdater.h"` at line 32
  - `DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Lazy);` at line 3516
  - `GVNPass::run()` (line 898) calls `PA.preserve<DominatorTreeAnalysis>();` at
    line 922.

- **SROA** — `llvm/lib/Transforms/Scalar/SROA.cpp`
  - `DomTreeUpdater *const DTU;` member at line 176; local
    `DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Lazy);` at lines 6369
    and 6416.
  - `SROAPass::run()` (line 6366) calls `PA.preserveSet<CFGAnalyses>();` (line 6376,
    conditionally) and unconditionally `PA.preserve<DominatorTreeAnalysis>();`
    (line 6377).

- **LICM, JumpTableToSwitch, DFAJumpThreading, LoopSimplifyCFG, SCCP,
  BreakCriticalEdges, TailRecursionElimination**, and 44 other files also match
  the `DomTreeUpdater`/`applyUpdates(`/`insertEdge(`/`deleteEdge(` grep (full list
  of 51 files reproducible via the command in §2, item 9) but were not individually
  hand-verified beyond the grep hit — treat as secondary confirmations, not primary
  citations.

### A nuanced "does the right thing but via `preserveSet`" example

- **GlobalOpt** — `llvm/lib/Transforms/IPO/GlobalOpt.cpp:2828-2862`. This file
  *did* show up in the raw `PreservedAnalyses::none()` grep and in the CFG-mutating
  grep (`removeUnreachableBlocks(F)` at line 1971), which is why it initially looked
  like a §3 candidate. On inspection, `GlobalOptPass::run()` builds
  `PreservedAnalyses PA = PreservedAnalyses::none();` (line 2854) but then explicitly
  calls `PA.preserveSet<CFGAnalyses>();` (line 2857) with the comment *"The only
  place we modify the CFG is when calling removeUnreachableBlocks(), but there we
  make sure to invalidate analyses for modified functions"* — i.e. it does per-function
  granular invalidation via a `ChangedCFGCallback` lambda (line 2843) rather than
  incremental `DomTreeUpdater` calls, then declares the module-level CFG analyses
  preserved. This is exactly the **"Optimistic" failure mode** described in the
  project proposal (preserve + fragile manual bookkeeping) rather than the
  "Conservative" one (blind `none()`) — worth flagging as a *different* kind of
  candidate for a follow-up soundness check, not an invalidation-pervasiveness one.
  **Excluded from the §3 list** because it does not match the literal
  "returns none() without preserving DT" criterion.

---

## 5. Caveats

1. **Denominator is approximate.** The 236 figure for "total NPM `run()`
   implementations" is a regex heuristic over two signature-formatting styles
   (same-line and return-type-on-own-line). It excludes Legacy Pass Manager
   `getAnalysisUsage`/`runOnFunction` overrides entirely (LLVM still ships some
   Legacy PM code, e.g. `FlattenCFGLegacyPass::runOnFunction` at
   `FlattenCFGPass.cpp:84`, which has no `PreservedAnalyses` concept at all and was
   not counted). It also does not distinguish `Function`, `Module`, `Loop`,
   `LazyCallGraph::SCC`, etc. pass kinds, and would miss any signature style not
   matching either grep (e.g. macro-generated `run()` bodies, of which none were
   observed but cannot be ruled out).

2. **The "never preserves DT" grep (#8, 14 files) has confirmed false positives.**
   Spot-checking flagged two: `InductiveRangeCheckElimination.cpp` and
   `LoopBoundSplit.cpp` both `return getLoopPassPreservedAnalyses();`, a shared
   helper in `llvm/lib/Analysis/LoopAnalysisManager.cpp:141-148` that *does* call
   `PA.preserve<DominatorTreeAnalysis>()` internally — the literal string just isn't
   in the pass's own file. Any tool built on this heuristic must resolve
   indirect/helper-function preservation, not just grep the immediate file.

3. **The CFG-mutation grep (§2 item 10) is a fixed keyword list** (`SplitBlock`,
   `SplitBlockPredecessors`, `MergeBlockIntoPredecessor`, `removeUnreachableBlocks`,
   `RemoveSuccessor`, `SplitEdge`, `SplitCriticalEdge`,
   `MergeBasicBlockIntoOnlyPred`). It does **not** catch raw manual CFG surgery via
   `BranchInst::Create` + old-terminator removal, `replaceSuccessorWith`, or
   `removePredecessor` used without going through a Utils helper — the user's step 2d
   description mentions `BranchInst::Create` combined with old-terminator removal as
   a pattern to check, but that requires cross-referencing two separate call sites
   and was judged too imprecise for grep-only detection (high false-positive rate,
   since most `BranchInst::Create` calls are ordinary code generation, not CFG
   *mutation* of existing blocks). This means §3's list of 12 files is a
   **conservative lower bound** — a tool that also understands raw terminator
   replacement would likely find more.

4. **"Same file" is a weaker proxy than "same function."** All 12 candidates in §3
   were manually confirmed to have the CFG-mutating call reachable from the same
   `run()` (either inline or via a helper called from `run()`), not just coincidentally
   present elsewhere in a large file — but this required hand-reading, not grep alone.
   A real LPTA static-analysis pass should do actual call-graph/data-flow tracing
   instead of file-proximity grep.

5. **`OpenMPOpt.cpp` is a genuine trap for a naive tool.** It passes a
   `DominatorTree *DT` argument to `SplitBlock(...)` (satisfying a naive "calls a
   DT-aware overload" check), but `DT` is statically `nullptr` in that code path
   (declared `DominatorTree *DT = nullptr;` and never reassigned before use at
   `OpenMPOpt.cpp:1158/1167`). Any automated detector must verify the DT pointer is
   actually non-null / actually sourced from the analysis manager, not just check
   that a DT-shaped parameter is being passed.

6. **`PreservedAnalyses::none()` is sometimes provably necessary**, not lazy — e.g.
   when a pass deletes functions/blocks in ways that genuinely invalidate DT with no
   cheap incremental path, or when multiple unrelated analyses (not just DT) are
   invalidated together and `none()` is the honest answer. This survey flags
   *candidates for investigation*, not confirmed bugs; each of the 12 files in §3
   needs a correctness review (does an incremental update actually suffice here?)
   before proposing a patch.

7. **Grep counts include comments/strings in a few edge cases** (e.g. a
   `// TODO: can we preserve more?` comment was itself found via a broader context
   grep, not counted as a code hit) — all *counted* metrics in §2 are from patterns
   anchored to actual C++ syntax (`::run(`, `PreservedAnalyses::none()`, function
   calls with `(`), so this risk is low but not zero for a fully automated re-run.

---

## 6. Reproducing this survey

```bash
cd /home/fuzzserver/llvm-pass-transformation-analyser/research/llvm-project
# item 1 (denominator, same-line)
grep -rn "PreservedAnalyses [A-Za-z_:<>]*::run(" llvm/lib/Transforms --include=*.cpp | wc -l
# item 4 (files with none())
grep -rl "PreservedAnalyses::none()" llvm/lib/Transforms --include=*.cpp | wc -l
# item 9 (incremental DT files)
grep -rlE "DomTreeUpdater|DT->applyUpdates\(|DT->insertEdge\(|DT->deleteEdge\(|DominatorTree::insertEdge|\.applyUpdates\(" llvm/lib/Transforms --include=*.cpp | wc -l
# item 10 (CFG-mutating files)
grep -rlE "SplitBlock\(|SplitBlockPredecessors\(|MergeBlockIntoPredecessor\(|removeUnreachableBlocks\(|RemoveSuccessor\(|SplitEdge\(|SplitCriticalEdge\(|MergeBasicBlockIntoOnlyPred\(" llvm/lib/Transforms --include=*.cpp | wc -l
# item 12 (final cross-reference)
comm -12 <(comm -23 <(grep -rl "PreservedAnalyses::none()" llvm/lib/Transforms --include=*.cpp | sort) \
                     <(grep -rlE "DomTreeUpdater|DT->applyUpdates\(|DT->insertEdge\(|DT->deleteEdge\(|DominatorTree::insertEdge|\.applyUpdates\(" llvm/lib/Transforms --include=*.cpp | sort)) \
          <(grep -rlE "SplitBlock\(|SplitBlockPredecessors\(|MergeBlockIntoPredecessor\(|removeUnreachableBlocks\(|RemoveSuccessor\(|SplitEdge\(|SplitCriticalEdge\(|MergeBasicBlockIntoOnlyPred\(" llvm/lib/Transforms --include=*.cpp | sort)
```
