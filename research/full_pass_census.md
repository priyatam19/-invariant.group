# Full New-Pass-Manager Census — LPTA Research

**Scope note:** the prior survey ([dt_invalidation_survey.md](dt_invalidation_survey.md)) was
narrowly scoped to `llvm/lib/Transforms` files that (a) call a CFG-mutating Utils
helper, (b) return a bare `PreservedAnalyses::none()`, and (c) have zero
`DomTreeUpdater` usage. This report is a broader, complete census: it starts
from *every* pass/analysis the New Pass Manager (NPM) knows about (Q1), narrows
to what a real `-O2` invocation actually schedules (Q2), classifies those by
whether they can change IR at all (Q3), and only then applies (and extends)
the prior survey's `none()`/preserve grep methodology to that IR-changing
subset (Q4–Q6). Six questions, each building on the last, per the task brief.

**Repo snapshot analyzed:** `llvm/llvm-project` @
`cff226a5716e4bc3589d4869066ea6687ed142ce` (same commit the prior survey used,
`main` @ clone time 2026-09-13) — the existing
`research/llvm-project` checkout was reused and its sparse-checkout **widened**
(not re-cloned) with:

```bash
cd /home/fuzzserver/llvm-pass-transformation-analyser/research/llvm-project
git sparse-checkout add llvm/lib/Passes llvm/lib/Transforms llvm/lib/Analysis llvm/lib/CodeGen
git sparse-checkout add llvm/lib/IR   # added later, needed for VerifierPass/Verifier.cpp
```

`llvm/lib/Transforms` and `llvm/lib/Analysis` were already present (prior
survey); `llvm/lib/Passes` (home of `PassRegistry.def` and `PassBuilder.cpp`),
`llvm/lib/CodeGen`, and `llvm/lib/IR` are new additions. Disk usage after
widening: `.git` 41M, `Passes` 436K, `Transforms` 18M, `Analysis` 5.8M,
`CodeGen` 15M, `IR` 3.2M — 90M total. `llvm/lib/CodeGen` was added per the task
brief but turned out **unused** by this report: the `-O2` pipeline expansion
(Q2) contains no CodeGen/MIR passes (`-passes=` pipeline text only reaches the
IR-level `default<O2>` pipeline, not instruction selection/register
allocation), so nothing in Q3–Q6 draws from it. Kept in the sparse-checkout
per the instruction to widen with that path; not a wasted step, just not load-
bearing for this particular report.

`opt` used: `/usr/lib/llvm-18/bin/opt`, **Ubuntu LLVM version 18.1.3** (the
task brief's suggested "use 18"). Note this is a *different* LLVM than the
source tree analyzed (which is a `main`-branch dev snapshot substantially
newer than 18.1.3 — it has `ir2vec-vocab`, `ctx-prof-*`, `runtime-libcall-info`
etc. that don't exist in 18.x). This mismatch is real and is called out
wherever it matters (see Caveats §7.1): the *pipeline shape* questions (Q2's
repetition counts, Q3–Q6's pass-by-pass classification) are answered against
what `opt`-18 actually schedules and are cross-checked against the *source*
of the pass names it prints, which does exist in both versions (all 71 leaf
pass names in the Q2 expansion resolved cleanly against the dev-snapshot
source). The registry *census* (Q1) is answered purely from the dev-snapshot
`PassRegistry.def`, so it is intentionally answering "how many passes does
this specific tree's NPM know about," not "how many does 18.1.3 know about" —
the two numbers would differ (fewer in 18.1.3).

---

## 1. Summary table — the six headline answers

| # | Question | Answer | Exact or sampled? |
|---|---|---|---|
| 1 | Distinct passes/analyses NPM knows about | **377 pass registrations / 72 analysis registrations** (449 total "slots"; 367/67 distinct name-strings, 429 distinct names after dedup) across 460 total macro invocations in `PassRegistry.def` (863 lines) | **Exact** (full-file parse) |
| 2 | Does `-O2` run "all of these," and does anything repeat? | No — only **75 of 429** distinct registered names (~17%) appear in the `-passes='default<O2>'` expansion (71 leaf passes + 4 required/invalidated analyses). **Yes, extensively**: `simplifycfg` and `instcombine` each appear **8×**, `sroa` and `licm` each **4×**, 11 more names tied at **2×** | **Exact** (full pipeline-text parse + cross-reference) |
| 3 | Of those that run, how many can change IR? | **67 of 71** leaf passes are structurally IR-changing; **4** are structurally non-mutating despite `*_PASS` registration (`verify`, `annotation-remarks`, `recompute-globalsaa`, `transform-warning`); **7** more DSL constructs (manager/adaptor keywords + `require`/`invalidate`) excluded as not-a-pass | **Exact** (all 71 `run()` bodies read) |
| 4 | Of the 67, bare `none()` vs. granular preserve? | **13** unconditionally-`none()`-on-change, **39** granular `PreservedAnalyses PA`-builder (28 of those use `.preserveSet<CFGAnalyses>()`), **11** delegate to a shared/local helper, **4** surprisingly return unconditional `all()` despite genuinely mutating IR | **Exact** (all 67 read; cross-checked against literal-text grep, which overcounts by 2) |
| 5 | Of the 50 preservers, *how* do they preserve? | Sampled ~35 of 50 in depth: **~17** use `DomTreeUpdater`/live-analysis incremental update, **~9** never touch the CFG at all, **11** delegate to a shared helper (heavy overlap with the DTU bucket for loop passes), **2** other/mixed patterns | **Sampled** (35 of 50 read; proportions, not exhaustive) |
| 6 | Of the bare-`none()` passes, why? | Read 21 total (13 from the O2 census + 8 fresh from the broader `llvm/lib/Transforms` corpus): **~13** genuinely global/structural IPO changes, **3 new + 2 already-known** explicit-comment tradeoff cases (new: `AddDiscriminators.cpp` `// FIXME: should be all()`), **4** conservative/no-comment, **2** no-incremental-path-exists | **Sampled** (21 read; not exhaustive) |

---

## 2. Question 1 — full NPM registry census

### 2.1 Methodology

```bash
cd /home/fuzzserver/llvm-pass-transformation-analyser/research/llvm-project/llvm/lib/Passes
wc -l PassRegistry.def                                    # 863
grep -oE '^[A-Z_]+\(' PassRegistry.def | sort | uniq -c | sort -rn   # per-macro invocation counts
```

`PassRegistry.def` (`llvm/lib/Passes/PassRegistry.def`) is an X-macro file: every
real invocation starts at column 0 (`^[A-Z_]+\(`); the two lines that look like
invocations but start indented (`PassRegistry.def:47`, `:396`) are the
*default-body* lines inside the `#define MODULE_ALIAS_ANALYSIS(...)  \
MODULE_ANALYSIS(...)` fallback macros, not real entries — confirmed by reading
them directly, not just counting. A **second, unrelated** `PassRegistry.def`
exists at `llvm/lib/Transforms/Vectorize/SandboxVectorizer/Passes/PassRegistry.def`
(41 lines) — a private registry for the experimental SandboxVectorizer's own
internal pass pipeline, not part of the core NPM registry this question asks
about. Excluded from all counts below.

### 2.2 Per-macro counts (exact, full-file)

| Macro | Count | Category |
|---|---|---|
| `FUNCTION_PASS` | 168 | pass |
| `MODULE_PASS` | 118 | pass |
| `FUNCTION_ANALYSIS` | 36 | analysis |
| `FUNCTION_PASS_WITH_PARAMS` | 29 | pass |
| `LOOP_PASS` | 24 | pass |
| `MODULE_ANALYSIS` | 22 | analysis |
| `MODULE_PASS_WITH_PARAMS` | 19 | pass |
| `CGSCC_PASS` | 8 | pass |
| `LOOP_ANALYSIS` | 5 | analysis |
| `FUNCTION_ALIAS_ANALYSIS` | 5 | analysis |
| `LOOP_PASS_WITH_PARAMS` | 4 | pass |
| `LOOPNEST_PASS` | 4 | pass |
| `FUNCTION_CALLBACK` | 4 | extension-point callback |
| `MODULE_LTO_CALLBACK` | 3 | extension-point callback |
| `CGSCC_PASS_WITH_PARAMS` | 3 | pass |
| `CGSCC_ANALYSIS` | 3 | analysis |
| `LOOP_CALLBACK` | 2 | extension-point callback |
| `MODULE_CALLBACK` | 1 | extension-point callback |
| `MODULE_ALIAS_ANALYSIS` | 1 | analysis |
| `CGSCC_CALLBACK` | 1 | extension-point callback |
| **Total** | **460** | |

**Passes: 377** (118+19+168+29+24+4+4+8+3). **Analyses: 72** (22+1+36+5+5+3).
**Extension-point callbacks: 11** (4+3+2+1+1) — **excluded** from the
pass/analysis grand total. These are `#ifdef`-guarded (not `#ifndef`-defaulted
like every other macro in the file — confirmed at `PassRegistry.def:833-863`),
meaning they're only expanded by the one call site in `PassBuilder.cpp` that
defines the macro; they register `PassBuilder`-extension-point invocation
hooks (e.g. `MODULE_CALLBACK("pipeline-start-callbacks",
invokePipelineStartEPCallbacks)`), not an actual pass or analysis class —
excluded per the same reasoning Q3 uses for adaptor exclusion.

### 2.3 Dedup by name-string

Joining multi-line macro invocations and extracting the quoted name literal
(`grep`-and-join script, reproducible — see §8) gives **367 distinct pass
name-strings** (out of 377 pass rows) and **67 distinct analysis name-strings**
(out of 72 analysis rows). The gap is legitimate, not a bug: some names are
registered at more than one IR-unit granularity under the *same* string,
disambiguated by pipeline-text nesting context — e.g. `"verify"` is both a
`MODULE_PASS` and a `FUNCTION_PASS` (two different C++ classes, both named
`VerifierPass`, working at different IR units); `"pass-instrumentation"` is
registered at all four of `MODULE_ANALYSIS`/`CGSCC_ANALYSIS`/
`FUNCTION_ANALYSIS`/`LOOP_ANALYSIS`. Five names are used as **both** a pass
name and an analysis name (`verify`, `no-op-module`, `no-op-function`,
`no-op-cgscc`, `no-op-loop`) — so the union of distinct pass ∪ analysis names
is 367 + 67 − 5 = **429** distinct registered names total.

**Grand total, stated two ways:**
- **Registration-row basis** (the more natural reading of "how many
  passes/analyses does NPM know about" — counts a pass registered at two IR
  units as two): **377 passes + 72 analyses = 449**.
- **Distinct-name basis** (dedup across granularity-overloading): **367
  passes + 67 analyses = 429** (367+67−5 overlap-names, if counted only once
  total, but the question asks pass count and analysis count separately, so
  367 and 67 are the cleanest per-category answers).

---

## 3. Question 2 — does `-O2` run "all of these," and does anything repeat?

### 3.1 Methodology

```bash
cat > trivial.ll << 'EOF'
define i32 @main() {
entry:
  ret i32 0
}
EOF
/usr/lib/llvm-18/bin/opt -passes='default<O2>' -print-pipeline-passes \
    -disable-output trivial.ll > o2_pipeline.txt
```

This worked standalone, first try (exit 0, no stderr) — no fallback to a
richer `.ll` file was needed. Output is a single line, 3858 characters, of
nested `name<params>(children,...)` syntax (e.g.
`annotation2metadata,forceattrs,...,function<eager-inv>(lower-expect,
simplifycfg<bonus-inst-threshold=1;...>,sroa<modify-cfg>,early-cse<>),
openmp-opt,...`).

**Naive comma-splitting is wrong** here because commas separate siblings *at
every nesting depth*, not just the top level (e.g. `function(...)` contains
its own comma-separated pass list). A small recursive-descent tokenizer was
written (`parse_pipeline.py`, grammar: `NAME['<'PARAMS'>']['('CHILDREN')']`
repeated with `,` separators) that walks the text once and records every
`NAME` token regardless of nesting depth, tagging each with whether it has
`<params>`, has `(children)` (→ a manager/adaptor wrapper, not a leaf pass),
or is `require<X>`/`invalidate<X>` (→ an analysis reference, not a pass). The
parser consumed the full 3858-character string with no leftover
(`parse ended at 3857 of 3857`), confirming the grammar model is complete for
this output.

### 3.2 Results

120 total `NAME` tokens found, decomposing exactly as:

- **71 distinct leaf pass names, 102 total occurrences** — real passes.
- **5 distinct manager/adaptor DSL keywords, 14 occurrences**: `function`×7,
  `loop-mssa`×3, `loop`×2, `cgscc`×1, `devirt`×1 (devirt takes a numeric
  param, `devirt<4>`).
- **`require<X>`, 3 distinct analyses, 3 occurrences**: `globals-aa`,
  `profile-summary`, `should-not-run-function-passes`.
- **`invalidate<X>`, 1 occurrence**: `invalidate<aa>` (targets the
  `AAManager`, registered under the name `"aa"` at `PassRegistry.def:353`).

Cross-referencing against the §2 registry (by joining/dedup-parsing
`PassRegistry.def` the same way, then matching quoted-name strings): **all 71
distinct leaf names, and all 4 `require`/`invalidate` targets, match a
registered `PassRegistry.def` name — 100%.** This is expected (`-print-
pipeline-passes` only ever prints canonical registered names) but is worth
stating as a positive confirmation the parser and registry-extraction are
both correct and mutually consistent. **None** of the 5 manager/adaptor
keywords (`function`, `cgscc`, `loop`, `loop-mssa`, `devirt`) match any
registered name — confirmed by grepping `PassBuilder.cpp` directly: these are
hardcoded pipeline-text keywords the parser special-cases
(`PassBuilder.cpp:793 Name.consume_front("function")`,
`:813 Name.consume_front("devirt<")`, `:1884/:1938 Name == "cgscc"/"loop"/
"loop-mssa"`), not `PassRegistry.def` entries.

So: of 429 distinct registered names, **75 (~17%) actually run under default
`-O2`** on this trivial input (71 passes + 4 analyses); the other 354 never
get scheduled by the default pipeline at all (they're LTO-only, sanitizer-
only, size-optimization-only, CodeGen-only, or opt-in-flag-gated passes and
analyses that a plain `-O2` never reaches).

### 3.3 Repetition — exact counts, quoted evidence

| Pass | Count |
|---|---|
| `simplifycfg` | **8** |
| `instcombine` | **8** |
| `sroa` | **4** |
| `licm` | **4** |
| `early-cse`, `globalopt`, `function-attrs`, `jump-threading`, `correlated-propagation`, `tailcallelim`, `loop-rotate`, `loop-deletion`, `vector-combine`, `globaldce`, `infer-alignment` | **2 each** (11-way tie) |
| (remaining 56 leaf names) | 1 each |

Evidence for the top 5 (fragments quoted directly from `o2_pipeline.txt`,
`,`-adjacency preserved, `<params>` elided for readability where noted):

- **`simplifycfg` (8×)** — appears once per major cleanup stage:
  `...coro-early,function<eager-inv>(lower-expect,simplifycfg<...>,sroa<modify-cfg>,early-cse<>)...`
  (pre-inline cleanup), `...function<eager-inv>(mem2reg,instcombine<...>,simplifycfg<...>)...`
  (post-mem2reg), inside the `cgscc(devirt<4>(inline,...,function<eager-inv;no-rerun>(sroa,
  early-cse,speculative-execution,jump-threading,correlated-propagation,
  simplifycfg<...>,instcombine<...>,...)))` per-SCC loop (×3 within that block alone,
  once after the loop-pass-manager nest too), post-coro-elide, and once more in the
  late vectorization-cleanup stage (`...loop-unroll<O2>,transform-warning,sroa<preserve-cfg>,
  infer-alignment,instcombine<...>,loop-mssa(licm<allowspeculation>),alignment-from-assumptions,
  loop-sink,instsimplify,div-rem-pairs,tailcallelim,simplifycfg<...>`).
- **`instcombine` (8×)** — same rhythm as `simplifycfg`, always paired with or
  immediately adjacent to it: `mem2reg,instcombine<max-iterations=1;...>,simplifycfg<...>`,
  `...jump-threading,correlated-propagation,simplifycfg<...>,instcombine<...>,aggressive-instcombine,...`,
  `mldst-motion<no-split-footer-bb>,gvn<>,sccp,bdce,instcombine<...>`, and 5 more sites through
  the loop-vectorize/SLP cleanup tail.
- **`sroa` (4×)**: pre-inline (`sroa<modify-cfg>,early-cse<>`), inside the per-SCC
  loop (`function<eager-inv;no-rerun>(sroa<modify-cfg>,early-cse<memssa>,...)`), post-
  loop-unroll-full (`...loop-unroll-full),sroa<modify-cfg>,vector-combine,...`), and
  late-pipeline (`loop-unroll<O2>,transform-warning,sroa<preserve-cfg>,infer-alignment,...`)
  — note the *params differ* across occurrences (`modify-cfg` vs. `preserve-cfg`), a real
  behavioral distinction between the early and late invocations, not just literal repetition.
- **`licm` (4×)**: twice inside one `loop-mssa(...)` nest with different params
  (`licm<no-allowspeculation>,loop-rotate<...>,licm<allowspeculation>,simple-loop-unswitch<...>`
  — LICM runs once conservatively before rotation and once more aggressively after), then
  once more post-coro-elide (`loop-mssa(licm<allowspeculation>),coro-elide,...`), and once in
  the late vectorization-cleanup stage.
- **`early-cse` (2×)**: `sroa<modify-cfg>,early-cse<>` (no MemorySSA, pre-inline) vs.
  `sroa<modify-cfg>,early-cse<memssa>` (with MemorySSA, inside the per-SCC loop) — same
  observation as `sroa`: literal-text repetition understates that these are *configured
  differently*, not identical re-runs.

---

## 4. Question 3 — of the 71 that run, how many can change IR?

### 4.1 Methodology

For each of the 71 leaf pass names, the registered C++ class name was
extracted from `PassRegistry.def` (handling both the plain `FOO_PASS("name",
ClassName())` form and the `FOO_PASS_WITH_PARAMS("name", "ClassName", lambda,
parser, "options")` form, which quotes the class name as its own string
literal). `grep -rn "PreservedAnalyses <Class>::run("` across
`llvm/lib/{Transforms,Analysis,IR}` located the definition for all 71 (64
matched directly; the remaining 7 use the "return-type-on-its-own-line"
signature style the prior survey's Caveat #1 already flagged, e.g.
`PreservedAnalyses\nFooPass::run(...)`, and were found with a looser
`grep "ClassName::run("`). Each `run()` body was then extracted by brace-
matching from its start line to the closing `}` and read.

### 4.2 Excluded: 7 non-pass DSL constructs (18 of the 120 raw tokens)

The 5 manager/adaptor keywords (`function`, `cgscc`, `loop`, `loop-mssa`,
`devirt<N>`) and the 2 analysis-reference verbs (`require<X>`,
`invalidate<X>`) are excluded from "a pass" entirely — confirmed via
`PassBuilder.cpp` (§3.2) that these are hardcoded pipeline-text keywords
expanding to C++ wrapper-template classes: `ModuleToFunctionPassAdaptor`,
`CGSCCToFunctionPassAdaptor`, `FunctionToLoopPassAdaptor`,
`DevirtSCCRepeatedPass`, `RequireAnalysisPass<AnalysisT, IRUnitT>`
(`PassBuilder.cpp:2152` and 4 more instantiation sites),
`InvalidateAnalysisPass<AnalysisT, IRUnitT>` (`PassBuilder.cpp:2157` and 4
more sites) — exactly the "`ModuleToFunctionPassAdaptor`,
`PassManager<...>` wrapper entries" the task brief named as the example
category to exclude.

### 4.3 Of the 71 real passes: 67 IR-changing, 4 structurally non-mutating

**4 passes never mutate IR despite `*_PASS` registration** (each confirmed by
reading the pass's own `run()`/`runImpl`, not inferred from the name):

- **`verify` → `VerifierPass`** (`llvm/lib/IR/Verifier.cpp:8276`) — pure
  checker; its body only reads a `VerifierAnalysis` result and calls
  `report_fatal_error` on failure. Unconditional `return
  PreservedAnalyses::all();`.
- **`annotation-remarks` → `AnnotationRemarksPass`**
  (`llvm/lib/Transforms/Scalar/AnnotationRemarks.cpp:85`) — `runImpl`
  (line 66) only builds a `DenseMap`/`OptimizationRemarkEmitter` and calls
  `ORE.emit(...)`; zero calls to anything that mutates an `Instruction`,
  `BasicBlock`, or `Function`. Unconditional `all()`.
- **`recompute-globalsaa` → `RecomputeGlobalsAAPass`**
  (`llvm/lib/Analysis/GlobalsModRef.cpp:1021`) — mutates only the *cached
  `GlobalsAA` analysis-result object* (`G->NonAddressTakenGlobals.clear()`
  etc., then recomputes it); never touches the `Module`'s actual IR.
  Unconditional `all()`.
- **`transform-warning` → `WarnMissedTransformationsPass`**
  (`llvm/lib/Transforms/Scalar/WarnMissedTransforms.cpp:100`) — calls
  `warnAboutLeftoverTransformations`, purely diagnostic (emits ORE remarks
  about loops that failed to vectorize/unroll). Unconditional `all()` on
  both return paths (early-return for `hasOptNone()`, and the normal path).

**67 passes are structurally IR-changing** — each has either a `Changed`-
dependent early return, or unconditionally calls a helper confirmed (by
reading, see §5) to mutate IR.

**Exact-count summary**: 71 leaf passes − 4 non-mutating = **67 structurally
IR-changing** (94% of what actually runs). Plus 7 non-pass DSL constructs
excluded separately, as instructed.

**Dynamic cross-check**: per the task brief, this report does not itself run
LPTA's `ir_changed`-per-invocation dynamic trace — RESEARCH.md §5 already
documents LPTA producing exactly that signal (336 pass-invocation records
with liveness-aware analysis tracking) from real pipeline runs, and that
dynamic corroboration is understood to be happening in parallel. The static
classification here should agree with it for all but pathological
"attempted a change but no-op'd on this specific input" cases.

---

## 5. Question 4 — of the 67 IR-changing passes, bare `none()` vs. granular preserve?

### 5.1 Exact breakdown

| Mechanism | Count | Passes |
|---|---|---|
| Unconditionally-`none()`-on-change (9 if/else + 4 ternary) | **13** | `constmerge`, `coro-cleanup`, `deadargelim`, `elim-avail-extern`, `globaldce`, `inferattrs`, `lower-expect`, `openmp-opt-cgscc`, `coro-split` (if/else); `annotation2metadata`, `coro-elide`, `forceattrs`, `openmp-opt` (ternary) |
| `PreservedAnalyses PA` / default-ctor builder pattern | **39** | see §5.3 |
| Delegates to a shared/local helper | **11** | 10 via `getLoopPassPreservedAnalyses()`; 1 (`jump-threading`) via its own local `getPreservedAnalysis()` |
| Unconditional `all()` despite genuine IR mutation (surprising) | **4** | `called-value-propagation`, `cg-profile`, `infer-alignment`, `inject-tli-mappings` |
| **Total** | **67** | 13+39+11+4 = 67 ✓ |

### 5.2 Literal-text grep vs. functional count — a real discrepancy, reproducing the prior survey's own caveat

```bash
grep -c "PreservedAnalyses::none()" <file>   # per-file, run across all 67 run() bodies
```

gives **15 files / 17 occurrences** of the literal string
`PreservedAnalyses::none()` — **2 more occurrences than the 13-pass
functional count above.** The extra 2 both come from **`GlobalOptPass`**
(`llvm/lib/Transforms/IPO/GlobalOpt.cpp:2854`,`2857`), which the *prior*
survey already flagged in its own §4 "nuanced" section:
`PreservedAnalyses PA = PreservedAnalyses::none();` is the pass's **starting
value**, immediately refined by `PA.preserveSet<CFGAnalyses>();` two lines
later — so `GlobalOpt` is correctly bucketed here under "PA-builder" (§5.3),
not "bare `none()`", even though a naive `grep -l` would flag it. This is the
identical false-positive trap the prior survey's `comm -12` cross-reference
methodology was built to avoid, now reproduced independently at the "is it
really bare `none()`" step rather than the "does it call a CFG-mutating
helper" step — see Caveats §7.2 for the general lesson.

### 5.3 The dominant mechanism: `PreservedAnalyses PA` builder (39/67, 58%)

```
adce, aggressive-instcombine, alignment-from-assumptions, always-inline, bdce,
constraint-elimination, coro-early, correlated-propagation, div-rem-pairs, dse,
early-cse, float2int, function-attrs, globalopt, gvn, inline, instcombine,
instsimplify, ipsccp, libcalls-shrinkwrap, loop-distribute, loop-load-elim,
loop-sink, loop-vectorize, lower-constant-intrinsics, mem2reg, memcpyopt,
mldst-motion, move-auto-init, reassociate, rel-lookup-table-converter,
rpo-function-attrs, sccp, simplifycfg, slp-vectorizer, speculative-execution,
sroa, tailcallelim, vector-combine
```

37 of these write `PreservedAnalyses PA;` (default-constructed — semantically
`none()`-equivalent, i.e. everything abandoned until selectively re-added)
directly in the pass's own `run()`; `sccp` and `libcalls-shrinkwrap` instead
write `auto PA = PreservedAnalyses();` (identical semantics, different
spelling). **This default-construction idiom is invisible to a grep for the
literal string `PreservedAnalyses::none()`** — a second, larger instance of
the same false-positive/false-negative risk noted in §5.2, this time in the
*other* direction (a text grep for `::none()` would *miss* these 39 passes'
starting state entirely, even though structurally it's the same "start from
nothing, add back what's safe" pattern). Concrete example,
`llvm/lib/Transforms/Scalar/CorrelatedValuePropagation.cpp:1447-1458`:

```cpp
PreservedAnalyses PA;
if (!Changed) {
  PA = PreservedAnalyses::all();
} else {
  ...
  PA.preserve<DominatorTreeAnalysis>();
  PA.preserve<LazyValueAnalysis>();
}
PA.abandon<LazyValueAnalysis>();   // forcefully discarded again, see §6.1
return PA;
```

**28 of the 39** additionally call `.preserveSet<CFGAnalyses>()` specifically:
`adce`, `aggressive-instcombine`, `alignment-from-assumptions`, `bdce`,
`constraint-elimination`, `coro-cleanup`* (see note), `coro-early`,
`div-rem-pairs`, `dse`, `early-cse`, `float2int`, `function-attrs`,
`globalopt`, `indvars`, `instcombine`, `instsimplify`, `loop-instsimplify`,
`loop-sink`, `loop-vectorize`, `mem2reg`, `memcpyopt`, `mldst-motion`,
`move-auto-init`, `rel-lookup-table-converter`, `slp-vectorizer`,
`speculative-execution`, `sroa`, `vector-combine`. (*`coro-cleanup`'s
`.preserveSet<CFGAnalyses>()` call is on an *internal*, per-function
`FuncPA` used to selectively invalidate before running a nested
`FunctionPassManager` — its own top-level `run()` still unconditionally
returns bare `none()` at module granularity, so it stays counted under the
"13 bare `none()`" bucket in §5.1, not here; flagged so the two counts don't
appear to silently disagree.)

### 5.4 Delegation to a shared helper (11/67)

10 loop-granularity passes (`indvars`, `licm`, `loop-deletion`, `loop-idiom`,
`loop-instsimplify`, `loop-rotate`, `loop-simplifycfg`, `loop-unroll`,
`loop-unroll-full`, `simple-loop-unswitch`) all follow the identical shape:

```cpp
if (!Changed) return PreservedAnalyses::all();
auto PA = getLoopPassPreservedAnalyses();   // shared helper, LoopAnalysisManager.cpp:141-148
PA.preserve<MemorySSAAnalysis>();           // (or similar, pass-specific extras)
return PA;
```

confirming the prior survey's Caveat #2 (`getLoopPassPreservedAnalyses()`
does call `.preserve<DominatorTreeAnalysis>()` internally — any grep-only
detector that doesn't resolve this indirection undercounts preservers, as
that survey already found for two different files). `jump-threading` is a
process apart but analogous: it delegates to its **own, pass-local** member
`getPreservedAnalysis()` (`JumpThreading.cpp:3229`), not the shared loop
helper — same *pattern* (indirection through a named function rather than
inlined `.preserve<>()` calls at the `run()` call site), different scope.

### 5.5 The surprising bucket: unconditional `all()` despite genuine IR mutation (4/67)

Explicitly flagged per the task brief's ask to surface genuine examples:

- **`called-value-propagation` → `CalledValuePropagationPass`**
  (`llvm/lib/Transforms/IPO/CalledValuePropagation.cpp:405-410`) — the
  **most surprising** of the four. Its static helper `runCVP(Module &M)`
  computes and **returns** a local `bool Changed` (set `true` whenever it
  attaches `!callees` metadata to an indirect call site, line 400), but the
  call site discards it outright:
  ```cpp
  PreservedAnalyses CalledValuePropagationPass::run(Module &M, ModuleAnalysisManager &) {
    runCVP(M);                        // return value silently dropped
    return PreservedAnalyses::all();
  }
  ```
  No comment explains the choice. Since `!callees` metadata isn't tracked by
  any `FunctionAnalysisManager`-cached analysis this pipeline stage depends
  on, `all()` is very plausibly harmless in practice — but the code
  *computes the exact signal needed to do this properly* and throws it away,
  which is a stronger "why even bother tracking `Changed`" smell than a
  merely-conservative pass that never tracked `Changed` at all.
- **`cg-profile` → `CGProfilePass`**
  (`llvm/lib/Transforms/Instrumentation/CGProfile.cpp:97-102`) — unconditionally
  calls `addModuleFlags(M, Counts)` (which does `M.addModuleFlag(Module::Append,
  "CG Profile", ...)`, definitely mutating the `Module`) then unconditionally
  `return PreservedAnalyses::all();`. No `Changed` tracking, no comment.
- **`infer-alignment` → `InferAlignmentPass`**
  (`llvm/lib/Transforms/Scalar/InferAlignment.cpp:192-198`) — **documented,
  deliberate**: `inferAlignment(F, AC, DT)` genuinely mutates instruction
  alignment, and the code says so: `// Changes to alignment shouldn't
  invalidated analyses.` immediately above `return PreservedAnalyses::all();`
  (sic — "invalidated" not "invalidate" in the source).
- **`inject-tli-mappings` → `InjectTLIMappings`**
  (`llvm/lib/Transforms/Utils/InjectTLIMappings.cpp:148-153`) — also
  **documented, deliberate**: `runImpl(TLI, F)` adds vector-function-ABI
  attributes to call sites; comment: `// Even if the pass adds IR attributes,
  the analyses are preserved.`

2 of 4 (`infer-alignment`, `inject-tli-mappings`) are explicitly justified in
comments — a real design position (attribute/metadata-only changes don't
affect the tracked structural analyses), not an oversight. The other 2
(`called-value-propagation`, `cg-profile`) have no such comment and are
better candidates for a correctness-review pass, in the same "candidate, not
confirmed bug" spirit the prior survey used for its own findings.

---

## 6. Question 5 — of the 50 preserving passes, *how* do they preserve?

Population: the 39 `PA`-builder passes (§5.3) + 11 helper-delegating passes
(§5.4) = **50**. Sampled well past the requested 15–25: all 50 `run()` bodies
were extracted and read at least once during Q3/Q4's classification; ~35 were
additionally cross-checked against their *whole source file* (not just
`run()`) for `DomTreeUpdater`/CFG-mutating-helper usage, via:

```bash
grep -cE "SplitBlock\(|SplitBlockPredecessors\(|MergeBlockIntoPredecessor\(|removeUnreachableBlocks\(|RemoveSuccessor\(|SplitEdge\(|SplitCriticalEdge\(|MergeBasicBlockIntoOnlyPred\(" <file>
grep -cE "DomTreeUpdater|\.applyUpdates\(|->applyUpdates\(|insertEdge\(|deleteEdge\(" <file>
```

reusing the prior survey's exact keyword lists (§1.2c/reproduction commands
there). **Rough proportions, not exhaustive** (per the task brief's own
framing) — of the ~35 read to this depth:

### (a) `DomTreeUpdater`/live-analysis incremental update, then explicit preserve — ~17

`aggressive-instcombine` (7 DTU-keyword hits), `ipsccp` (1), `adce` (3, and
its own comment says why: *"ADCE does not need DominatorTree, but require
DominatorTree here to update analysis if it is already available"* —
`ADCE.cpp:622-623`), `correlated-propagation` (4 — gets the live,
AM-owned `DominatorTree`, asserts `DT->verify(...)` post-transform, then
`PA.preserve<DominatorTreeAnalysis>()`), `gvn` (2 — already documented by the
prior survey: `DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Lazy)`
at `GVN.cpp:3516`), `licm` (2), `loop-simplifycfg` (9), `lower-constant-
intrinsics` (5), `sccp` (2), `sroa` (8 — prior survey's `SROA.cpp:6369/6416`),
`simple-loop-unswitch` (14 — the heaviest DTU user in the sample),
`simplifycfg` (5 — prior survey's `SimplifyCFGPass.cpp:275/392`),
`tailcallelim` (8), `jump-threading` (10 — prior survey's `JumpThreading.cpp:
253-254/3229`), `libcalls-shrinkwrap` (4), `loop-vectorize` (2), `slp-
vectorizer` (4). Several of these (`gvn`, `simplifycfg`, `sroa`, `jump-
threading`) are the exact positive examples the prior survey already
hand-verified in depth (§4 there) — referenced, not re-derived, here.

### (b) Never touches the CFG at all (pure instruction-level rewrite) — ~9

High-confidence members, verified by reading (no `SplitBlock`/`MergeBlock`/
etc. calls, no DTU, and the transform is semantically instruction-local):
`bdce` (bit-level dead-code elimination — replaces operands with `poison`,
doesn't restructure blocks), `alignment-from-assumptions` (adjusts alignment
metadata only), `div-rem-pairs` (pairs/replaces div+rem instruction pairs),
`float2int` (rewrites float ops to equivalent int ops in place), `move-auto-
init` (moves initialization calls within an existing block), `mem2reg`
(promotes `alloca`s to SSA values — classically does not add/remove blocks),
`instsimplify` (pure peephole simplification), `vector-combine` (pure
vector-instruction combining), and **`reassociate`** — confirmed directly by
reading `Reassociate.cpp:2809-2814`:
```cpp
if (MadeChange) {
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
return PreservedAnalyses::all();
```
`ReassociatePass::runImpl` only reorders/folds arithmetic expression trees
within existing blocks — `.preserveSet<CFGAnalyses>()` is trivially safe
because nothing in the function ever calls a block-restructuring API.

### (c) Delegates to a shared/local helper — 11

Same 11 as §5.4. **Important nuance**: this bucket is not disjoint from (a)
in practice — most of the 10 `getLoopPassPreservedAnalyses()`-users *also*
receive a live `DominatorTree`/`MemorySSA` via the loop pass's
`LoopStandardAnalysisResults &AR` bundle and may perform genuine incremental
updates internally (e.g. `LICM.cpp` threads `&AR.DT` into its analysis calls)
before calling the shared helper to package up the resulting
`PreservedAnalyses`. So mechanisms (a) and (c) frequently co-occur for the
loop-pass family — worth stating plainly rather than forcing an artificial
either/or split.

### (d) Other/mixed — 2

- **`GlobalOpt`** (already documented in depth by the prior survey, §4
  there — referenced, not re-derived): builds `PA = PreservedAnalyses::none()`
  then `PA.preserveSet<CFGAnalyses>()`, backed by a per-function
  `ChangedCFGCallback` lambda (`GlobalOpt.cpp:2843`) doing fine-grained
  per-function invalidation rather than incremental `DomTreeUpdater` calls —
  the prior survey's "Optimistic" pattern.
- **`CoroCleanup`** (new finding, this survey) — runs an inner
  `FunctionPassManager{SimplifyCFGPass}` per lowered function, with its own
  scoped `FuncPA.preserveSet<CFGAnalyses>()` used only to control that inner
  `FAM.invalidate(F, FuncPA)` call — then still returns a blanket module-
  level `PreservedAnalyses::none()` regardless (`CoroCleanup.cpp:277-297`).
  A structural granularity mismatch: the *pass* only has one
  `PreservedAnalyses` to return for the whole module, so its internal
  per-function bookkeeping can't be expressed at the interface boundary
  even though it clearly exists inside the implementation.

Remainder (~15 of 50) were read but not independently deep-classified beyond
the automated `cfg-mut`/`dtu` keyword-count signal; the proportions above are
estimated from the ~35 that were fully read, consistent with the task
brief's "doesn't need to be exhaustive/exact" instruction.

---

## 7. Question 6 — of the bare-`none()` passes, what are the reasons?

Population read: **21 total** — the 13 bare-`none()` passes from the O2
census (§5.1) plus **8 fresh** examples pulled from the broader
`llvm/lib/Transforms` corpus (outside the O2-scoped 71, and outside the
prior survey's already-covered 12) via:

```bash
grep -rl "return PreservedAnalyses::none();" llvm/lib/Transforms --include=*.cpp | sort  # 61 files total
comm -23 <(that list) <(prior-survey's 12 files ∪ this survey's 13 O2-scoped files)
```

The 8 new ones chosen for diversity: `HotColdSplitting.cpp`,
`MergeFunctions.cpp`, `PartialInlining.cpp`, `Internalize.cpp`,
`StripDeadPrototypes.cpp`, `LowerGuardIntrinsic.cpp`,
`AddDiscriminators.cpp`, `WholeProgramDevirt.cpp`.

### (i) Genuinely global/structural, no single incremental-update API applies — ~13

`coro-split` (removeUnreachableBlocks + coroutine-frame cloning across the
whole SCC, `CoroSplit.cpp:2317-2414`), `coro-cleanup` (see §6d),
`deadargelim`/`elim-avail-extern`/`globaldce` (delete functions/globals
module-wide, rewrite call sites), `openmp-opt`/`openmp-opt-cgscc`
(`Attributor`-based whole-program fixpoint), and 4 new: **`HotColdSplitting`**
(`HotColdSplitting.cpp:806-834`, outlines cold regions into brand-new
functions — genuine inter-procedural code motion, no incremental API for
"a chunk of one function became a new function"), **`PartialInlining`**
(`PartialInlining.cpp:1453-1491`, CFG-splits a callee and rewires call sites
across functions), **`MergeFunctions`** (`MergeFunctions.cpp:337-342`,
replaces call targets module-wide and deletes duplicate function bodies),
**`WholeProgramDevirt`** (`WholeProgramDevirt.cpp:793-813`, whole-program
type analysis rewriting indirect calls based on vtable layout). All four new
ones share the same shape:
```cpp
if (!Impl.run(M)) return PreservedAnalyses::all();
return PreservedAnalyses::none();
```
— a clean binary Changed-check, but landing on `none()` because the actual
*kind* of change (new functions, merged functions, rewritten call graph
edges) has no incremental-update object in LLVM to hand off to.

### (ii) Explicitly discusses the tradeoff in a comment — 3 new + 2 referenced

**New, and the most notable single finding of this report**:
**`AddDiscriminators.cpp:241-249`**:
```cpp
PreservedAnalyses AddDiscriminatorsPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (!addDiscriminators(F))
    return PreservedAnalyses::all();

  // FIXME: should be all()
  return PreservedAnalyses::none();
}
```
`addDiscriminators` (read in full, `AddDiscriminators.cpp:130-239`) only ever
calls `I.setDebugLoc(*NewDIL)` — attaching a DWARF discriminator to
disambiguate identical source lines, **debug-info metadata only**, no
functional IR change whatsoever. The author's own comment says the return
value is wrong (`all()`, not `none()`) — this is a stronger, more direct
self-flagged case than the prior survey's `PlaceSafepoints.cpp` `// TODO: can
we preserve more?` (which only wonders whether *more* could be preserved);
here the author states the code should preserve *everything* and it
currently preserves *nothing*. Also new: **`ForceFunctionAttrsPass`**
(`forceattrs`) and **`InferFunctionAttrsPass`** (`inferattrs`,
`InferFunctionAttrs.cpp:46-48`: *"Otherwise, we may have changed fundamental
function attributes, so clear out all the passes."*) — both comment-justify
returning `none()` on the basis that attribute changes can ripple into
inlining/AA decisions broadly enough that granular preservation isn't
trusted. Already-known, referenced not re-derived: `PlaceSafepoints.cpp:384`
(`// TODO: can we preserve more?`) and `Attributor.cpp:4110` (`// FIXME:
Think about passes we will preserve and add them here.`) from the prior
survey's §3.

### (iii) Conservative/simple implementation, no comment, headroom apparently available — 4

`constmerge` (`ConstantMergePass::run`, `ConstantMerge.cpp:250-253` — merges
identical constants; doesn't touch the CFG, no comment about preserving
more), `lower-expect` (removes `llvm.expect` intrinsics / converts to branch-
weight metadata; typically no CFG change), `annotation2metadata` (converts
`llvm.annotation` calls to metadata form; instruction-local), and **new**:
**`Internalize.cpp:275-280`**:
```cpp
PreservedAnalyses InternalizePass::run(Module &M, ModuleAnalysisManager &AM) {
  if (!internalizeModule(M))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
```
confirmed by reading `internalizeModule` (`Internalize.cpp:157-166`) that its
*only* mutation is `GV.setLinkage(GlobalValue::InternalLinkage)` — a pure
linkage/visibility attribute change touching no instructions, no CFG, no
call graph edges — yet it still discards everything, with no comment
explaining why (unlike the structurally-similar but self-aware
`AddDiscriminators` case in bucket (ii)).

### (iv) Legitimately invalidates something with no incremental-update path — 2 (overlaps with i)

**New**: **`StripDeadPrototypes.cpp:50-54`** (deletes dead function
*declarations* — `CallGraphAnalysis`/`GlobalsAA` have no exposed "remove one
node" incremental API at this pass's level, so `none()` is close to
necessary) and **`LowerGuardIntrinsic.cpp:56-60`**, confirmed by reading
`llvm::makeGuardControlFlowExplicit` (`GuardUtils.cpp:37`,
`SplitBlockAndInsertIfThen(...)`) — this pass **does** perform genuine
per-guard CFG splitting, with no `DominatorTree`/`DomTreeUpdater` threaded
into the call at all. Unlike `OpenMPOpt`'s `nullptr`-DT trap (prior survey's
Caveat #5), this isn't a wiring bug — `SplitBlockAndInsertIfThen` simply
isn't called with a DT argument here — but it is a live candidate for the
same kind of "could this have used the DT-aware overload" investigation the
prior survey ran on its 12 files, just outside this report's scope to
resolve.

---

## 8. Reproduction

```bash
# --- Q1: registry census ---
cd /home/fuzzserver/llvm-pass-transformation-analyser/research/llvm-project
git sparse-checkout add llvm/lib/Passes llvm/lib/Transforms llvm/lib/Analysis llvm/lib/CodeGen llvm/lib/IR
cd llvm/lib/Passes
grep -oE '^[A-Z_]+\(' PassRegistry.def | sed 's/(//' | sort | uniq -c | sort -rn

# --- Q2: O2 pipeline expansion ---
printf 'define i32 @main() {\nentry:\n  ret i32 0\n}\n' > /tmp/trivial.ll
/usr/lib/llvm-18/bin/opt -passes='default<O2>' -print-pipeline-passes \
    -disable-output /tmp/trivial.ll > /tmp/o2_pipeline.txt
# then parse with a nesting-aware tokenizer (naive comma-split is wrong) —
# grammar: NAME['<'PARAMS'>']['('CHILDREN')'] repeated with ',' at any depth.

# --- Q3/Q4: run() classification ---
# for each of the 71 leaf names, resolve name -> C++ class via PassRegistry.def,
# then:
grep -rn "PreservedAnalyses <Class>::run(" llvm/lib/{Transforms,Analysis,IR} --include=*.cpp
# extract the run() body by brace-matching from that line, then classify by
# presence of literal "PreservedAnalyses::none()"/"::all()", a
# "PreservedAnalyses PA" builder variable, a ternary combining both, or a
# single-token delegating return (e.g. "return runImpl(...)" or
# "return getLoopPassPreservedAnalyses()").

# --- Q5: preservation-mechanism grep (reusing prior survey's exact lists) ---
grep -cE "SplitBlock\(|SplitBlockPredecessors\(|MergeBlockIntoPredecessor\(|removeUnreachableBlocks\(|RemoveSuccessor\(|SplitEdge\(|SplitCriticalEdge\(|MergeBasicBlockIntoOnlyPred\(" <file>
grep -cE "DomTreeUpdater|\.applyUpdates\(|->applyUpdates\(|insertEdge\(|deleteEdge\(" <file>

# --- Q6: broader none() corpus for fresh sampling ---
grep -rl "return PreservedAnalyses::none();" llvm/lib/Transforms --include=*.cpp | sort   # 61 files
```

---

## 9. Caveats

1. **This report analyzes a `main`-dev-branch source tree with `opt`-18.1.3.**
   The dev tree has passes/analyses that don't exist in 18.1.3
   (`ir2vec-vocab`, `ctx-prof-*`, `runtime-libcall-info`, etc.), so Q1's exact
   449/429 totals are specific to this snapshot, not to "LLVM" in general or
   to 18.1.3 specifically. Q2-Q6 are grounded in what `opt`-18 *actually
   schedules and prints*, then cross-referenced against the dev-tree source
   for the pass names that appear — every one of the 71 leaf names + 4
   analysis references resolved cleanly (§3.2), so this mismatch did not
   silently produce wrong classifications, but a stricter methodology would
   pin `opt` and source to the same commit.

2. **Text-grep for `PreservedAnalyses::none()` both over- and under-counts**,
   confirmed twice in this report, not just asserted: it **over-counts** by 2
   occurrences (`GlobalOpt`'s "starting value, later refined" pattern, §5.2 —
   the exact same trap the prior survey's own §4 already flagged for the same
   file) and it **under-counts** entirely-differently-spelled-but-
   semantically-identical starting states (37 of the 39 `PA`-builder passes
   use default-constructed `PreservedAnalyses PA;`/`PreservedAnalyses()`,
   which starts in a `none()`-equivalent state with no `::none()` substring
   anywhere in the source, §5.3). A grep-only tool would need both an
   AST-level "what is the effective value at each return statement"
   dataflow pass and cross-function resolution of helper delegation (§5.4,
   §5.1's `getLoopPassPreservedAnalyses()`/`getPreservedAnalysis()` cases,
   both already flagged for the identical reason by the prior survey's
   Caveat #2) to get exact counts a human reading the code would agree with.

3. **The CFG-mutation and DTU keyword lists (§6, reused verbatim from the
   prior survey) are fixed vocabularies**, inheriting that survey's Caveat #3
   verbatim: raw terminator surgery (`BranchInst::Create` + manual old-
   terminator removal, `SplitBlockAndInsertIfThen` as used by
   `LowerGuardIntrinsic`/`GuardUtils.cpp`, confirmed in §7(iv) to be
   genuinely missed by the CFG-mutation keyword list despite doing real CFG
   splitting) isn't caught. `LowerGuardIntrinsic` is a concrete instance
   found in *this* report of exactly that gap — it does real CFG surgery via
   a different Utils entry point than the fixed keyword list checks for.

4. **Q3's "structurally IR-changing" classification is based on reading each
   pass's own `run()`/`runImpl`, not a full call-graph/dataflow trace into
   every helper it calls.** For the 4 passes classified as non-mutating
   (§4.3), the negative claim ("never touches IR") rests on reading the
   entire `runImpl`/equivalent body, which is a reasonably strong bar for
   these short/focused passes but is not an exhaustive proof — a
   sufficiently obscure indirect mutation inside a rarely-taken branch could
   in principle be missed. An AST-based checker with real data-flow (e.g.
   Clang's own AST matchers, or LLVM's own IR checked against a before/after
   diff as LPTA's dynamic instrumentation already does) would close this gap
   completely and is the natural next validation step, per §4.3's note that
   dynamic cross-checking is happening in parallel.

5. **Q5 and Q6 are explicitly sampled, not exhaustive**, per the task
   brief's own instruction (15–25 each; this report read 35/50 for Q5 and
   21 for Q6, both above the requested floor but nowhere near the full
   67-pass or 61-file populations available). The bucket proportions in §6
   and §7 should be read as "this is what a substantial, diverse sample
   looks like," not as validated population statistics.

6. **What's exact vs. sampled, stated plainly:**
   - **Exact** (full-population, reproducible by the commands in §8):
     Q1 (all 460 macro invocations counted and categorized), Q2 (the full
     3858-character pipeline text fully parsed with 0 leftover), Q3 (all 71
     leaf `run()` bodies read), Q4 (all 67 IR-changing `run()` bodies read
     and classified).
   - **Sampled** (explicitly, per the task brief's own request for these two
     questions): Q5 (35 of 50 read in depth; proportions estimated), Q6 (21
     of 61 available bare-`none()` files read; bucket assignment for those
     21 only).

7. **The `require<X>`/`invalidate<X>` DSL exclusion (Q3) is itself grep/text-
   based** (matching `PassBuilder.cpp` source strings for the hardcoded
   keyword comparisons) — verified by reading the actual `consume_front`/
   `Name ==` call sites (§3.2, §4.2), not inferred, but a newer LLVM version
   could in principle rename or restructure this parsing code, which would
   silently break the exclusion logic on a future re-run against a different
   commit.
