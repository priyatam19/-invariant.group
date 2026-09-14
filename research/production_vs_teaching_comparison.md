# Production LLVM vs. Teaching/Research LLVM Passes — Discipline Comparison

**Goal:** Re-run the grep-then-hand-verify methodology from
[`dt_invalidation_survey.md`](dt_invalidation_survey.md) against out-of-tree
teaching/tutorial/personal-research LLVM pass corpora, and answer: is
production LLVM (`llvm/lib/Transforms`) meaningfully more disciplined about
routing CFG edits through standard Utils APIs (`DomTreeUpdater`, `SplitBlock`,
`SplitEdge`, `removeUnreachableBlocks`, etc.) than code that never went
through LLVM code review — and does that difference make Phase 3's planned
AST-matcher approach (see `ROADMAP.md`) easier against production code?

**Date:** 2026-09-14. All corpora cloned shallow (`--depth 1`) into this
`research/` directory; nothing under the cloned repos was modified (read-only
research). Production baseline is unchanged from the original survey:
`llvm/llvm-project` @ `cff226a5716e4bc3589d4869066ea6687ed142ce`.

---

## 1. Corpora

### 1.1 Selected

| # | Repo | Commit (cloned) | Pushed | Size | Why selected |
|---|------|------------------|--------|------|---------------|
| 1 | [`banach-space/llvm-tutor`](https://github.com/banach-space/llvm-tutor) → `research/llvm-tutor/` | `624545760145782f5e4c00119497657a6d487476` | 2026-05-16 | 571 KB (repo), 3,436 GitHub stars | The canonical, actively-maintained (updated with every LLVM release) out-of-tree teaching collection. Explicitly includes two real CFG-topology passes (`DuplicateBB`, `MergeBB`), not just print-statistics examples. |
| 2 | [`mfaerevaag/loop-unroll`](https://github.com/mfaerevaag/loop-unroll) → `research/loop-unroll-mfaerevaag/` | `302cd0e419e9f7879a9c4e8cce143c145d2ca94d` | 2017-06-11 | 236 KB | Personal/research project implementing loop unrolling as an out-of-tree Legacy-PM pass, with real block-cloning, branch-rewiring, and hand-written `DominatorTree` maintenance — the single largest genuinely independent CFG-editing transformation found outside llvm-tutor. |
| 3 | [`kaivanwadia/DCEandLICM-Pass`](https://github.com/kaivanwadia/DCEandLICM-Pass) → `research/dceandlicm-cs380c/` | `128ba3e503b83d55656b8790d0cac0145d82e5df` | 2015-09-18 | 273 KB | A real university course assignment: UT Austin **CS 380C, Advanced Topics in Compilers**, Assignment 4 (Dead Code Elimination + Loop-Invariant Code Motion), built on a hand-rolled generic dataflow-analysis framework (`DFAFramework.h`, `Worklist.h`, `Meet.h`, `Transfer.h`). Legacy PM. Distinct institution/author from llvm-tutor and from corpus 2. |

### 1.2 Tried and rejected

| Candidate | Rejected because |
|---|---|
| `henrydyc/LLVM-Project` (USC CSCI-565 "Compiler Design" class project) | Inspected `project1/MyPass/MyPass.cpp` in full (raw-fetched from GitHub). It is a pure `ModulePass` doing call-graph/argument-type printing (`runOnModule`/`runOnFunction` that only counts calls and prints types) — **zero IR mutation of any kind**, let alone CFG edits. Targets an ancient LLVM API (`Function::ArgumentListType`, `getArgumentList()`), and at 6.5 KB is too thin to say anything about CFG-editing discipline. |
| `dust-storm/llvm-workspace` | GitHub API confirms `"fork": true` of `banach-space/llvm-tutor` itself (same description, same 571/518 KB size). Using it would just double-count llvm-tutor. |
| `zzzDavid/cs6120-a8` (Cornell CS6120 LICM assignment), `salekinsirajus/licm-optimization-pass-llvm` | Both implement LICM only (instruction hoisting, no basic-block creation/deletion/merging) — the same "no CFG topology edit exists" category already covered by the LICM half of corpus 3 (`DCEandLICM-Pass`). Not cloned; would have been redundant rather than "genuinely distinct." |
| `toshipiazza/LLVMCFG` (Georgia Tech CSCI-4975 final project — dynamic CFG/branch-tracking instrumentation) | Surfaced by search but not pursued: it *instruments* branches (inserts calls at existing branch sites) rather than *editing CFG topology* (no block insertion/deletion/merging), so it doesn't exercise the DominatorTree-invalidation question this survey is about. |
| `abenkhadra/llvm-pass-tutorial`, `alexjung/Writing-an-LLVM-Pass-using-the-new-PassManager`, `10x-Engineers/tutorial-llvm-pass` | All are single "HelloWorld"-style passes with no transformation logic (excluded per the task's explicit instruction to skip trivial hello-world passes). |

---

## 2. Methodology notes / deviations from the production survey

1. **Pass-manager era differs by corpus, not by "teaching vs. production."**
   `llvm-tutor` (corpus 1) turned out to be **100% New Pass Manager** — see
   §3.1; it is continuously rebased against upstream LLVM (README: "based on
   the latest version of LLVM, updated with every release"), so it tracks
   modern production style. Corpora 2 and 3 (2017 and 2015 respectively) are
   **100% Legacy PM** (`runOnLoop`/`runOnFunction` + `getAnalysisUsage`), which
   is an *age* effect (pre-dating NPM's dominance in `opt`), not necessarily a
   teaching-vs-production effect. This is called out explicitly rather than
   folded into one "teaching code" number.
2. **A same-named method is not the same as inheriting the base class.**
   llvm-tutor's `StaticCallCounter::runOnModule` and `MBAAdd::runOnBasicBlock`
   are ordinary helper methods on classes that inherit `AnalysisInfoMixin`/
   `PassInfoMixin` (NPM-only) — confirmed by grepping for any
   `ModulePass`/`FunctionPass`/`LoopPass`/`BasicBlockPass` base class or
   `RegisterPass<...>`/`char ...::ID = 0` anywhere in the repo (zero hits, §3.1).
   A naive "grep for `runOn`" count would have wrongly classified 4 llvm-tutor
   files as Legacy PM; this report uses the corrected, class-hierarchy-checked
   figure (0 Legacy PM passes in llvm-tutor).
3. **CFG-mutating keyword list was broadened** relative to the original
   survey's list (`SplitBlock`, `SplitBlockPredecessors`,
   `MergeBlockIntoPredecessor`, `removeUnreachableBlocks`, `RemoveSuccessor`,
   `SplitEdge`, `SplitCriticalEdge`, `MergeBasicBlockIntoOnlyPred`) to also
   include `SplitBlockAndInsertIfThenElse`, `DeleteDeadBlock`,
   `ReplaceInstWithInst`, and `CloneBasicBlock`, because the first pass of
   the original keyword list produced **zero** hits against llvm-tutor's two
   real CFG-editing passes even though they unambiguously edit CFG topology
   (confirmed by hand-reading — see §3.1). This is itself a finding: the
   *specific* Utils helper names production code gravitates to are not
   identical to the ones teaching code gravitates to, even when both are
   using real Utils helpers rather than raw surgery.
4. Every count below is reproducible from the shown command, run from inside
   the cloned repo directory. No number is invented or estimated.

---

## 3. Per-corpus results

### 3.1 `llvm-tutor` (`research/llvm-tutor/`, commit `6245457`)

```bash
cd research/llvm-tutor
find . -name '*.cpp' | wc -l                                     # 13
grep -rn "PreservedAnalyses [A-Za-z_:<>]*::run(" . --include=*.cpp | wc -l   # 10
grep -rln "public.*ModulePass\|public.*FunctionPass\|public.*LoopPass\|public.*BasicBlockPass\|RegisterPass<\|char.*::ID = 0" . --include=*.cpp --include=*.h | wc -l  # 0
grep -rn "PreservedAnalyses::none()" . --include=*.cpp | wc -l    # 7 (occurrences, some in ternaries)
grep -rn "PreservedAnalyses::all()"  . --include=*.cpp | wc -l    # 12
grep -rl "PreservedAnalyses PA" . --include=*.cpp | wc -l         # 0
grep -rlE "DomTreeUpdater" . --include=*.cpp | wc -l              # 0
grep -rn  "DominatorTree" . --include=*.cpp                       # only RIV.cpp:118 (an analysis pass)
grep -rlE "SplitBlock\(|SplitBlockPredecessors\(|MergeBlockIntoPredecessor\(|removeUnreachableBlocks\(|RemoveSuccessor\(|SplitEdge\(|SplitCriticalEdge\(|MergeBasicBlockIntoOnlyPred\(" . --include=*.cpp | wc -l  # 0 (original production keyword list)
grep -rlE "SplitBlockAndInsertIfThenElse|DeleteDeadBlock\(|ReplaceInstWithInst\(|CloneBasicBlock\(" . --include=*.cpp  # DuplicateBB.cpp, MergeBB.cpp, MBAAdd.cpp, MBASub.cpp
grep -rn "verifyFunction\|verifyModule" . --include=*.cpp | wc -l # 0
```

- **10** NPM `run()` implementations across 13 `.cpp` files. **0** actual
  Legacy PM passes (confirmed by class-hierarchy grep, not just method-name
  grep — see §2.2).
- **12** `PreservedAnalyses::all()` and **7** `::none()` occurrences, all via
  a `Changed ? none() : all()` (or equivalent) ternary — **0** files use the
  granular `PreservedAnalyses PA; PA.preserve<...>();` builder pattern that
  101/341 production files use. Every llvm-tutor pass makes an all-or-nothing
  call.
- **0** files reference `DomTreeUpdater`. The **only** mention of
  `DominatorTree` in the entire repo is `lib/RIV.cpp:118`
  (`DominatorTree *DT = &FAM.getResult<DominatorTreeAnalysis>(F);`), inside
  `RIV` — an **analysis** pass that does no IR mutation at all.
- Of 13 files, exactly **2** perform a genuine CFG-topology edit:
  `lib/DuplicateBB.cpp` and `lib/MergeBB.cpp` (confirmed by hand-reading both
  files in full, not just the broadened grep). Both:
  - Use real `Transforms/Utils` helpers for the mechanical edit — not raw
    `BranchInst::Create`. `DuplicateBB.cpp:158` calls
    `SplitBlockAndInsertIfThenElse(Cond, &*BBHead, &ThenTerm, &ElseTerm)` and
    `DuplicateBB.cpp:227` calls `ReplaceInstWithInst(Tail, IIT, Phi)`;
    `MergeBB.cpp:256` calls `DeleteDeadBlock(BB)`.
  - **Never acquire `DominatorTreeAnalysis` at all**, so there is no live DT
    to update incrementally in the first place. This is a materially
    different failure mode from production's: production's problem cases
    (§4 of the original survey) *have* a live DT and throw it away;
    llvm-tutor's CFG passes never ask for one.
  - Both call the DT-aware overload's non-DT form:
    `DeleteDeadBlock(BasicBlock *BB, DomTreeUpdater *DTU = nullptr, ...)`
    (verified against `llvm/include/llvm/Transforms/Utils/BasicBlockUtils.h`
    at the production survey's pinned commit, line 62) is called with only
    the 1 required argument in `MergeBB.cpp:256`; likewise
    `SplitBlockAndInsertIfThenElse(..., DomTreeUpdater *DTU = nullptr, ...)`
    (same header, line 511/554) is called with only 4 of its up-to-7
    arguments in `DuplicateBB.cpp:158`. This is *exactly* the
    "DT-aware overload exists but is called without the DT/DTU argument"
    pattern the original survey flagged in `Attributor.cpp:2593` and
    `GVNSink.cpp:777` (§3, items 3 and 5) — teaching code exhibits the
    identical shape of gap as several production near-misses.
  - `MergeBB.cpp:118-141` (`updateBranchTargets`) rewires predecessor
    terminators by raw `Term->setOperand(OpIdx, BBToRetain)` rather than a
    named Utils helper such as `replaceSuccessorWith` — this **is** a small
    piece of raw manual successor surgery, coexisting in the same function
    with the `DeleteDeadBlock` Utils call.
  - Both unconditionally return `PreservedAnalyses::none()` when `Changed`
    (`DuplicateBB.cpp:255`, `MergeBB.cpp:259`) — which, given no DT was ever
    fetched, is the *honest* answer, not a shortcut around available
    incremental-update machinery (contrast with production's
    `ControlHeightReduction.cpp:2128`, which keeps a live DT correct via
    `SplitBlock(..., &DT)` and *still* returns `none()`).
- **0** calls to `llvm::verifyFunction`/`verifyModule` anywhere in the repo
  (verification is presumably left to the `opt`/LIT test harness externally,
  not asserted from within the pass).

### 3.2 `loop-unroll` (mfaerevaag) (`research/loop-unroll-mfaerevaag/`, commit `302cd0e`)

```bash
cd research/loop-unroll-mfaerevaag
grep -n "runOnLoop\|runOnFunction" src/*.cpp src/*.h        # 1 pass: LoopUnroll::runOnLoop (Legacy PM, LPPassManager)
grep -n "getAnalysisUsage" -A6 src/LoopUnroll.h
grep -n "DomTreeUpdater" src/LoopUnroll.cpp                  # 0
grep -n "DT->addNewBlock\|DT->changeImmediateDominator\|DT->eraseNode\|DT->getNode" src/LoopUnroll.cpp  # 7 call sites
grep -n "BranchInst::Create(\|eraseFromParent()\|getInstList().pop_back\|getInstList().erase\|getInstList().splice" src/LoopUnroll.cpp
grep -n "verifyFunction\|verifyModule\|verifyLoop" src/LoopUnroll.cpp   # 0
```

- **1** pass, **1** `run()`-equivalent (`LoopUnroll::runOnLoop`, Legacy PM
  `LPPassManager`-based loop pass). 633 total lines across `src/`.
- `getAnalysisUsage` (`src/LoopUnroll.h:19-24`) does **not** call
  `AU.setPreservesCFG()` (correctly — the pass genuinely edits the CFG) and
  does not explicitly call `AU.addPreserved<DominatorTreeWrapperPass>()`
  either. Instead it delegates to `getLoopAnalysisUsage(AU)` (line 23), a
  shared LLVM helper (`llvm/lib/Transforms/Utils/LoopUtils.cpp`) that itself
  adds `addPreserved<DominatorTreeWrapperPass>()`/`addPreserved<LoopInfoWrapperPass>()`
  internally. **This is the exact same "preservation happens inside a
  helper function, invisible to a same-file grep" pattern the original
  survey documented as Caveat #2** (`InductiveRangeCheckElimination.cpp`/
  `LoopBoundSplit.cpp` calling `getLoopPassPreservedAnalyses()`). It shows up
  identically in a personal 2017 out-of-tree repo, not just in `llvm-project`
  itself — evidence that "preservation via helper, not literal string" is a
  structural feature of the LLVM loop-pass idiom in general, independent of
  code-review pedigree.
- The CFG edit itself (`unrollLoop()`, `foldBlockIntoPredecessor()`) is done
  by **raw, manual IR surgery with no Utils-helper call for the branch
  rewrite**: `src/LoopUnroll.cpp:495` `BranchInst::Create(Dest, Term);`
  immediately followed by `Term->eraseFromParent();` at line 496 — precisely
  the "manual `BranchInst::Create` + manually erasing the old terminator"
  pattern the original survey's Caveat #3 said was too imprecise to grep for
  in production but flagged as worth checking by hand. Also raw:
  `getInstList().pop_back()` (line 115, deleting the old unconditional
  branch), `getInstList().splice(...)` (line 122, merging blocks),
  `BB->eraseFromParent()` (line 151).
- Despite the raw terminator surgery, the same function
  (`foldBlockIntoPredecessor`, lines 108-152) **does** perform correct,
  fully manual incremental `DominatorTree` maintenance:
  `DT->getNode(BB)` / `DT->changeImmediateDominator(DI, PredDTN)` /
  `DT->eraseNode(BB)` (lines 129-135), and the loop-cloning code
  (`unrollLoop`, lines 386-394) calls `DT->addNewBlock(New, ...)` for every
  cloned block. This is functionally equivalent to what `DomTreeUpdater`
  would do, just hand-inlined rather than going through that (at the time,
  less-established) class. One Utils helper **is** used inline:
  `FoldSingleEntryPHINodes(BB)` (line 112).
- **0** calls to `verifyFunction`/`verifyModule`/`verifyLoop` anywhere.
- **Caveat specific to this corpus** (see §5): the `unrollLoop`/
  `foldBlockIntoPredecessor` logic is structurally near-identical to LLVM's
  own generic unrolling implementation
  (`llvm::UnrollLoop` in `llvm/lib/Transforms/Utils/LoopUnroll.cpp`) —
  variable names (`TripCount`, `TripMultiple`, `BreakoutTrip`,
  `ContinueOnTrue`, `LoopBlocksDFS`) and control flow match closely enough
  that this reads as an adapted/simplified copy of upstream LLVM code
  (instrumented with `my-unroll-threshold`/`my-unroll-count` CLI flags and
  `errs()` tracing for what appears to be a personal measurement project —
  see `data/*.csv`, `utils/benchmark.sh`, `utils/grapher.r` in the repo).
  **Its good DT hygiene is therefore best read as inherited from copying
  production LLVM code, not as independently-invented teaching-code
  discipline** — this corpus is not a clean example of a from-scratch
  student implementation, and its numbers should not be read as
  representative of typical from-scratch out-of-tree code.

### 3.3 `DCEandLICM-Pass` (CS380C, UT Austin) (`research/dceandlicm-cs380c/`, commit `128ba3e`)

```bash
cd research/dceandlicm-cs380c
grep -n "runOn" *.cpp *.h                                    # DCEPass::runOnFunction, LicmPass::runOnLoop
grep -n "getAnalysisUsage" -A6 DCEPass.cpp LicmPass.cpp
grep -rnE "SplitBlock\(|SplitBlockPredecessors\(|MergeBlockIntoPredecessor\(|removeUnreachableBlocks\(|RemoveSuccessor\(|SplitEdge\(|SplitCriticalEdge\(|MergeBasicBlockIntoOnlyPred\(|DeleteDeadBlock\(|SplitBlockAndInsertIfThenElse|CloneBasicBlock\(|BranchInst::Create\(" .   # 0 hits, every file
grep -rn "DominatorTree\|DomTreeUpdater" .                    # 1 hit: LicmPass.cpp:80 (addRequired only)
grep -rn "verifyFunction\|verifyModule\|verifyLoop" .         # 0
```

- **2** Legacy PM passes: `DCEPass::runOnFunction` (`DCEPass.cpp:17`) and
  `LicmPass::runOnLoop` (`LicmPass.cpp:20`), both built on a shared,
  hand-written generic dataflow-analysis framework (`DFAFramework.h`,
  `Worklist.h`, `Meet.h`, `Transfer.h`, `Equal.h`, `Hasher.h` — 1,071 lines
  total across the repo).
- **0** files anywhere match any CFG-mutating Utils helper or raw
  `BranchInst::Create`, in either the original or the broadened keyword
  list. Confirmed by hand-reading both `DCEPass.cpp::deleteInstructions` and
  `LicmPass.cpp::deleteInstructions`/hoisting logic in full: DCE deletes
  individual dead `Instruction`s in place
  (`inst->replaceAllUsesWith(replacement); inst->eraseFromParent();`,
  `DCEPass.cpp:63-64`) and LICM hoists individual instructions
  (`toHoist->moveBefore(loop->getLoopPreheader()->getTerminator());`,
  `LicmPass.cpp:68`) — **neither pass ever creates, deletes, or merges a
  basic block.** The CFG-invalidation question this survey investigates is
  vacuous for 100% of this corpus's transformation code, not because of
  sloppiness but because the assignment's scope (classic per-instruction
  DCE/LICM) never required a block-level edit.
- Both passes correctly call `AU.setPreservesCFG()`
  (`DCEPass.cpp:100`, `LicmPass.cpp:81`) — and, since neither pass actually
  touches CFG topology, this is a **true**, honest declaration, not an
  optimistic one.
- `LicmPass.cpp:80` requires `DominatorTreeWrapperPass` (standard for any
  loop pass) but the DT object is never dereferenced anywhere in the file —
  it is required only because the loop-pass contract expects it available,
  not because this pass does any DT-dependent transformation.
- **0** calls to `verifyFunction`/`verifyModule`/`verifyLoop`.

---

## 4. Comparison table

| Dimension | Production LLVM (`llvm/lib/Transforms`, 341 files) | `llvm-tutor` (13 files) | `loop-unroll` / mfaerevaag (1 pass) | `DCEandLICM-Pass` / CS380C (2 passes) |
|---|---|---|---|---|
| Pass-manager era | NPM (~236 `run()`s counted; some Legacy PM remnants noted but excluded, see original survey Caveat 1) | **100% NPM**, 0 Legacy PM (class-hierarchy verified) | **100% Legacy PM** (`LPPassManager`) | **100% Legacy PM** |
| Files/passes with a real CFG-topology edit | 53/341 files (~16%) match the CFG-mutating keyword list | 2/13 files (~15%) — `DuplicateBB`, `MergeBB` | 1/1 pass (the whole point of the pass) | **0/2 passes** — question is vacuous here |
| DominatorTree ever acquired in the CFG-editing code path | Yes, in most CFG-editing files (51/341 reference `DomTreeUpdater`/incremental-DT APIs somewhere) | **No** — the 2 CFG-editing passes never call `FAM.getResult<DominatorTreeAnalysis>()`; DT appears only in an unrelated analysis pass (`RIV.cpp`) | Yes — but via 7 hand-written `DT->addNewBlock`/`changeImmediateDominator`/`eraseNode`/`getNode` calls, not `DomTreeUpdater` | N/A (no CFG edits); DT required but unused |
| Raw `BranchInst::Create` + manual terminator erase (no Utils helper) | Present but not systematically counted in the original survey (flagged as a gap, Caveat 3) | 0 — both CFG passes route through `SplitBlockAndInsertIfThenElse`/`DeleteDeadBlock`/`ReplaceInstWithInst` (though `MergeBB` still hand-edits successor operands directly, `MergeBB.cpp:134`) | **Yes** — `LoopUnroll.cpp:495-496`, the exact pattern the production survey couldn't grep for | N/A |
| Calls a DT-aware Utils overload without passing the DT/DTU argument | Yes — `Attributor.cpp:2593` (`SplitBlockPredecessors`, 3-arg), `GVNSink.cpp:777` (same) | Yes — `DuplicateBB.cpp:158`, `MergeBB.cpp:256` (both leave `DomTreeUpdater *DTU = nullptr` at its default) | N/A (updates DT by hand instead) | N/A |
| `PreservedAnalyses PA` granular builder pattern | 101/341 files | **0/13** — always a binary `Changed ? none() : all()` ternary | N/A (Legacy PM) | N/A (Legacy PM) |
| `setPreservesCFG()`/`getAnalysisUsage` honesty (Legacy PM only) | N/A (NPM survey) | N/A | Omits `setPreservesCFG()` correctly (CFG does change); DT-preservation delegated to a shared helper (`getLoopAnalysisUsage`), invisible to same-file grep — mirrors original survey's Caveat 2 | Calls `setPreservesCFG()` in both passes, correctly (CFG never changes) |
| IR verification (`verifyFunction`/`verifyModule`) called from the pass itself | Not systematically checked in original survey | **0/13** | **0/1** | **0/2** |
| Error handling / comments about analysis lifecycle | Present in several (e.g. `PlaceSafepoints.cpp:339-340` explicitly discusses the DT-recompute-once tradeoff) | Extensive **design-intent** comments (ASCII-art CFG diagrams in `DuplicateBB.cpp`/`MergeBB.cpp` headers) but **no comments about analysis/DT lifecycle** specifically | Comments mostly reproduce/adapt LLVM's own inline comments (evidence of derivation from upstream, §3.2 caveat) | Comments explain the dataflow algorithm (meet/transfer functions) in depth; none about DT/CFG lifecycle (consistent with never touching CFG) |

---

## 5. Hand-verification summary (not just grep)

Every claim above involving a specific file:line was confirmed by opening
the file and reading the surrounding function, matching the standard the
original survey held itself to in its §3. Specifically hand-read in full:
`llvm-tutor/lib/MergeBB.cpp`, `llvm-tutor/lib/DuplicateBB.cpp`,
`llvm-tutor/lib/RIV.cpp` (DT reference only),
`loop-unroll-mfaerevaag/src/LoopUnroll.cpp` (entire 633-line file),
`dceandlicm-cs380c/DCEPass.cpp`, `dceandlicm-cs380c/LicmPass.cpp`. Class
hierarchies for `llvm-tutor/include/StaticCallCounter.h`,
`DynamicCallCounter.h`, `MBAAdd.h` were inspected specifically to correct a
naive method-name-only Legacy-PM miscount (§2, item 2).

---

## 6. Answering the question

**Is production LLVM more disciplined?** Yes, in a specific, measurable
sense: production's CFG edits are heavily concentrated in calls to a small,
consistent, *named* set of Utils functions (`SplitBlock`,
`SplitBlockPredecessors`, `SplitEdge`, `SplitCriticalEdge`,
`removeUnreachableBlocks`, `MergeBlockIntoPredecessor`, plus `DomTreeUpdater`
member calls) — 53/341 files, reproducible by one grep command. Across the
three non-reviewed corpora surveyed here, **not one** CFG edit went through
that exact same named-function surface. Instead we observed three distinct
alternate shapes: llvm-tutor routes through a *different* (but still
standard) subset of Utils helpers that happen not to take a DT argument at
all (`SplitBlockAndInsertIfThenElse`, `DeleteDeadBlock`, `ReplaceInstWithInst`)
plus one raw operand-surgery line; mfaerevaag's loop-unroll does the branch
rewrite with fully raw `BranchInst::Create`+`eraseFromParent()` while doing
DT maintenance completely by hand (no `DomTreeUpdater` at all); and
DCEandLICM-Pass never edits CFG topology in the first place, so there is
nothing to route through anything.

**Does that make automating DT-update-opportunity detection easier against
production code?** Yes, and the reason is concrete and directly observed
here, not just an impression: **production LLVM's CFG edits are
syntactically convergent on a handful of named call sites, and teaching/
research code's are not.** A Clang LibTooling AST-matcher (Phase 3,
`ROADMAP.md`) that matches call expressions to
`SplitBlock`/`SplitEdge`/`removeUnreachableBlocks`/`DomTreeUpdater::applyUpdates`
etc. would catch the overwhelming majority of real CFG-topology edits in
`llvm/lib/Transforms` (53/341 files) with a matcher of maybe eight function
names. The same matcher run against these three corpora would:
- **Silently miss** `loop-unroll-mfaerevaag/src/LoopUnroll.cpp:495-496` and
  its manual `DT->addNewBlock`/`changeImmediateDominator`/`eraseNode` calls
  entirely — there is no named Utils call site for a matcher to anchor on;
  the "incremental update opportunity" and even a hand-rolled *correct*
  incremental update both exist here in a form that is invisible to
  call-site-name matching, only recoverable by control/data-flow reasoning
  over raw `BranchInst`/`DominatorTree` API calls.
- Need a **longer, corpus-specific allow-list** to even recognize
  llvm-tutor's `DuplicateBB`/`MergeBB` as CFG edits at all (they don't use
  the production-typical helper names), despite those edits being
  perfectly legitimate, non-hacky Utils-helper usage.
- Find **nothing to flag** in `DCEandLICM-Pass`, correctly, since no CFG
  edit exists — a true negative, but one that also means this corpus
  contributes zero signal either way to validating a DT-focused tool.

So the answer is not "production code is written more carefully in some
diffuse sense" — it is the narrower, more useful claim that **production
code funnels almost all real CFG mutation through a small, stable, named
API surface, and that funneling — not the underlying correctness of any
individual pass — is precisely the property a name/pattern-based static
matcher depends on.** Against code that never went through the review
process that establishes and enforces that convention, the same class of
tool needs either a much longer per-corpus helper allow-list (llvm-tutor) or
abandons pattern-matching for actual dataflow tracing of raw pointer/API
calls (loop-unroll), or simply has nothing to find (DCEandLICM-Pass). This
is a direct, load-bearing reason the Phase 3 roadmap should treat "runs
against arbitrary out-of-tree/teaching code" as a materially harder target
than "runs against `llvm/lib/Transforms`," and should not assume a matcher
tuned on production call-site names will generalize.

---

## 7. Caveats

1. **Small N.** Three corpora, one real CFG-editing file in two of them
   combined (`DuplicateBB.cpp`, `MergeBB.cpp`) plus one in the third
   (`LoopUnroll.cpp`). This is nowhere near the production survey's 341-file
   population; treat every teaching-corpus percentage as anecdotal, not
   statistically representative of "teaching code in general."
2. **`loop-unroll-mfaerevaag` is not a clean from-scratch sample** (§3.2) —
   its DT hygiene is plausibly inherited from copying LLVM's own
   `UnrollLoop` implementation rather than being independently designed.
   Its raw-`BranchInst::Create` finding is still valid and observed, but its
   good DT-maintenance finding should not be read as "typical teaching code
   invents correct incremental DT updates on its own."
3. **DCEandLICM-Pass contributes a true negative, not evidence either way**
   on the raw-surgery-vs-Utils-helper axis, because it never touches CFG
   topology at all. It does, however, genuinely support the
   `setPreservesCFG()`-honesty and Legacy-PM-adoption comparisons.
4. **The broadened CFG-mutating keyword list (§2, item 3) was tuned on
   llvm-tutor's two files** — it is possible other Utils helpers not in
   either keyword list (original or broadened) exist and were missed in any
   of the four corpora; as in the original survey, this makes every "0
   matches" result a lower bound on what a smarter (AST-based, not
   text-based) matcher could find, not a proof of absence.
5. **No IR-diffing or build/compile verification was performed** on any of
   the three corpora (unlike the original survey's LLVM checkout, these are
   old/out-of-tree and not necessarily buildable against a current LLVM
   without porting). All findings are from static reading of the source,
   the same standard the original survey used for its hand-verified §3, but
   without the additional confidence a successful build would add.
6. **`llvm-tutor` is a moving target.** It is "updated with every LLVM
   release" per its own README, so line numbers and even the boolean facts
   above ("0 Legacy PM passes," "2 CFG-editing files") are pinned to commit
   `6245457` and may drift on `main`.
7. As in the original survey, `PreservedAnalyses::none()` (or, in Legacy PM,
   omitting `setPreservesCFG()`) is not automatically a defect — it can be
   the honest answer. Nothing in this report claims any of the four corpora
   contains a *bug*; the comparison is about API-usage discipline and its
   consequences for a static-analysis tool's tractability, not correctness.
