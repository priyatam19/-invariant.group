# Research notes

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
"invalidating" an unbuilt analysis costs nothing. **This means the current
CFG-changed-and-not-preserved heuristic is a coarse first-pass signal, not a
finished verdict**; Phase 2 (see ROADMAP.md) needs to cross-reference against
`registerAfterAnalysisCallback`/`registerAnalysisInvalidatedCallback` to know
whether DT actually had a cached result at invalidation time, which is the
only case where discarding it is wasteful. Flagging this now so it isn't
mistaken for a validated finding later.
