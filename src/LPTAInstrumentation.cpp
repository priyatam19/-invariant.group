//===- LPTAInstrumentation.cpp - LLVM Pass Transformation Analyzer ------===//
//
// Out-of-tree New-Pass-Manager plugin. Hooks PassInstrumentationCallbacks to
// snapshot IR before/after every pass invocation, quantifies the change, and
// records whether analyses that the change plausibly invalidated (chiefly
// DominatorTree/LoopInfo) were actually preserved. Emits one JSON record per
// pass invocation to a JSONL trace file for offline reporting.
//
// Load with:  opt -load-pass-plugin=./libLPTAInstrumentation.so \
//                 -passes='default<O2>' -disable-output input.ll
// Configure via environment variables (see README.md):
//   LPTA_TRACE_OUT     path to JSONL output (default: lpta_trace.jsonl)
//   LPTA_FILTER_FUNC   comma-separated function names to restrict tracing to
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace llvm;

namespace {

// ---------------------------------------------------------------------------
// IR unit resolution: every pass runs on a Module, Function, Loop, or SCC.
// We resolve down to the enclosing Function when possible (cheap, precise
// snapshots) and fall back to the whole Module otherwise.
// ---------------------------------------------------------------------------

struct ResolvedUnit {
  const Function *F = nullptr; // set when we could pin down a single function
  const Module *M = nullptr;   // always set when F is set (F's parent)
  std::string Name;            // function name, or "<module:NAME>"
  StringRef Kind; // "function" | "loop" | "module" | "scc" | "unknown"
};

const Function *unwrapToFunction(const Any &IR) {
  if (auto *FP = any_cast<const Function *>(&IR))
    return *FP;
  if (auto *LP = any_cast<const Loop *>(&IR))
    return (*LP)->getHeader()->getParent();
  return nullptr;
}

ResolvedUnit resolveUnit(const Any &IR) {
  ResolvedUnit RU;
  if (const Function *F = unwrapToFunction(IR)) {
    RU.F = F;
    RU.M = F->getParent();
    RU.Name = F->getName().str();
    RU.Kind = any_cast<const Loop *>(&IR) ? "loop" : "function";
    return RU;
  }
  if (auto *MP = any_cast<const Module *>(&IR)) {
    RU.M = *MP;
    RU.Name = ("<module:" + (*MP)->getName() + ">").str();
    RU.Kind = "module";
    return RU;
  }
  if (auto *CP = any_cast<const LazyCallGraph::SCC *>(&IR)) {
    RU.Kind = "scc";
    RU.Name = "<scc>";
    for (const LazyCallGraph::Node &N : **CP) {
      RU.M = N.getFunction().getParent();
      RU.Name = "<scc:" + (*CP)->getName() + ">";
      break;
    }
    return RU;
  }
  RU.Kind = "unknown";
  RU.Name = "<unknown>";
  return RU;
}

// ---------------------------------------------------------------------------
// Structural snapshot of the resolved unit: printed text + cheap stats.
// ---------------------------------------------------------------------------

struct Snapshot {
  std::string Text;
  unsigned Instrs = 0;
  unsigned BBs = 0;
  unsigned Edges = 0; // sum of successor counts, a coarse CFG-edge proxy
};

void accumulate(const Function &F, Snapshot &S) {
  for (const BasicBlock &BB : F) {
    ++S.BBs;
    S.Instrs += BB.size();
    S.Edges += succ_size(&BB);
  }
}

Snapshot takeSnapshot(const ResolvedUnit &RU) {
  Snapshot S;
  raw_string_ostream OS(S.Text);
  if (RU.F) {
    RU.F->print(OS);
    if (!RU.F->isDeclaration())
      accumulate(*RU.F, S);
  } else if (RU.M) {
    RU.M->print(OS, nullptr);
    for (const Function &F : *RU.M)
      if (!F.isDeclaration())
        accumulate(F, S);
  }
  OS.flush();
  return S;
}

// Space-optimized LCS length over lines, used to derive added/removed line
// counts without a full traceback. Capped to avoid O(n*m) blowup on huge
// module-level snapshots (falls back to a cheap multiset diff there).
void diffLines(StringRef Before, StringRef After, unsigned &Added,
               unsigned &Removed) {
  SmallVector<StringRef, 128> B, A;
  Before.split(B, '\n');
  After.split(A, '\n');

  const size_t N = B.size(), M = A.size();
  if (N * M > 4'000'000ULL) {
    // Fallback: symmetric-difference-by-multiset approximation.
    StringMap<int> Count;
    for (StringRef L : B)
      ++Count[L];
    for (StringRef L : A)
      --Count[L];
    Added = Removed = 0;
    for (auto &KV : Count) {
      if (KV.second > 0)
        Removed += KV.second; // present only in Before
      else
        Added += -KV.second; // present only in After
    }
    return;
  }

  std::vector<unsigned> Prev(M + 1, 0), Curr(M + 1, 0);
  for (size_t i = 1; i <= N; ++i) {
    for (size_t j = 1; j <= M; ++j) {
      Curr[j] = B[i - 1] == A[j - 1] ? Prev[j - 1] + 1
                                     : std::max(Prev[j], Curr[j - 1]);
    }
    std::swap(Prev, Curr);
  }
  unsigned LCS = Prev[M];
  Added = static_cast<unsigned>(M) - LCS;
  Removed = static_cast<unsigned>(N) - LCS;
}

// ---------------------------------------------------------------------------
// Trace sink: one JSON object per pass invocation, newline-delimited.
// Defined here (ahead of analysis-liveness tracking below) because
// afterAnalysis() emits an "analysis" record directly to it.
// ---------------------------------------------------------------------------

class TraceSink {
public:
  TraceSink() {
    SmallString<128> Path;
    if (const char *Env = std::getenv("LPTA_TRACE_OUT"))
      Path = Env;
    else
      Path = "lpta_trace.jsonl";

    std::error_code EC;
    OS = std::make_unique<raw_fd_ostream>(Path, EC, sys::fs::OF_None);
    if (EC) {
      errs() << "LPTA: failed to open trace output '" << Path
             << "': " << EC.message() << "\n";
      OS.reset();
    }

    if (const char *Filter = std::getenv("LPTA_FILTER_FUNC")) {
      SmallVector<StringRef, 8> Parts;
      StringRef(Filter).split(Parts, ',');
      for (StringRef P : Parts)
        if (!P.empty())
          FilterFuncs.insert(P.str());
    }
  }

  bool shouldTrace(const ResolvedUnit &RU) const {
    if (FilterFuncs.empty())
      return true;
    return RU.F && FilterFuncs.count(RU.F->getName().str());
  }

  void emit(json::Object &&Record) {
    if (!OS)
      return;
    Record["seq"] = Seq++;
    *OS << json::Value(std::move(Record)) << "\n";
  }

private:
  std::unique_ptr<raw_fd_ostream> OS;
  StringSet<> FilterFuncs;
  uint64_t Seq = 0;
};

TraceSink &sink() {
  static TraceSink Sink;
  return Sink;
}

// ---------------------------------------------------------------------------
// Analysis-liveness tracking (ROADMAP.md Phase 2).
//
// Phase 1's incremental_update_candidate fired on "CFG changed &&
// !DTPreserved", which over-fires: a pass that never had DominatorTree computed
// in the first place has nothing to wastefully throw away (see RESEARCH.md §5's
// SimplifyCFG example). This tracks whether DominatorTree/LoopInfo were
// actually *cached* (live) for a given Function, using two ground-truth signals
// confirmed by reading LLVM 18's AnalysisManager::invalidate()
// (PassManagerImpl.h):
//   - AfterAnalysisCallback fires when an analysis is (re)computed.
//   - AnalysisInvalidatedCallback fires ONLY when a *cached* result is
//     actually erased as a consequence of a pass's PreservedAnalyses -- never
//     for an analysis that was never computed. This is exactly the "was it
//     live, and did this pass throw it away" signal Phase 2 needs; no
//     separate bookkeeping is needed to distinguish "never computed" from
//     "computed then abandoned."
// Ordering: AnalysisManager::invalidate() runs before PassInstrumentation::
// runAfterPass() for the same pass invocation (PassManager.h), so any
// invalidation a pass causes has already updated this state by the time
// afterPassCommon() reads it -- but we want liveness *at the start* of the
// pass, so beforePass() snapshots it into PendingEntry before that happens.
// ---------------------------------------------------------------------------

constexpr StringLiteral DTAnalysisName = "DominatorTreeAnalysis";
constexpr StringLiteral LoopAnalysisName = "LoopAnalysis";
constexpr StringLiteral MemorySSAAnalysisName = "MemorySSAAnalysis";
constexpr StringLiteral ScalarEvolutionAnalysisName = "ScalarEvolutionAnalysis";

// The four Function-keyed analyses we track. MemorySSA/ScalarEvolution are
// the "cascade cost" half of the thesis (see RESEARCH.md/PRODUCTIONIZATION.md
// experiment notes): a bare PreservedAnalyses::none() doesn't just discard a
// cheap-to-rebuild DominatorTree, it drags these down too even though the
// pass's edit may have nothing to do with memory or induction-variable
// reasoning. BranchProbabilityInfo/RegionInfo are real dependents too (see
// RESEARCH.md §2 addendum) but are deliberately out of scope here to keep
// this first timing pass focused on the two analyses actually expensive
// enough to matter for a wall-clock experiment.
bool isTrackedAnalysis(StringRef Name) {
  return Name == DTAnalysisName || Name == LoopAnalysisName ||
         Name == MemorySSAAnalysisName || Name == ScalarEvolutionAnalysisName;
}

struct AnalysisTiming {
  bool Live = false;
  // Set on invalidation, cleared (and counted) on the next recompute -- this
  // is what turns "invalidated" into "invalidated AND later actually paid
  // for again", i.e. genuinely wasted work rather than a pass that happened
  // to invalidate an analysis nobody ever asked for again.
  bool PendingRecompute = false;
  unsigned WastedRecomputes = 0;
  uint64_t LastComputeCPUTimeUs = 0;
  uint64_t TotalCPUTimeUs = 0;
};

DenseMap<const Function *, StringMap<AnalysisTiming>> &analysisState() {
  static DenseMap<const Function *, StringMap<AnalysisTiming>> Map;
  return Map;
}

AnalysisTiming *getAnalysisTiming(const Function *F, StringRef Name) {
  auto It = analysisState().find(F);
  if (It == analysisState().end())
    return nullptr;
  auto NameIt = It->second.find(Name);
  if (NameIt == It->second.end())
    return nullptr;
  return &NameIt->second;
}

// ---------------------------------------------------------------------------
// Analysis-compute CPU timing (Tier 2 experiment: is the wasted-recompute
// signal actually expensive in wall-clock terms, and how much of that is
// DominatorTree itself vs. the more expensive MemorySSA/ScalarEvolution
// cascade). Uses process CPU time (user+sys via getrusage, the same
// technique LLVM's own -time-passes uses), not wall-clock: this sandbox is a
// shared, noisy machine, and CPU time is far less susceptible to scheduling
// jitter than steady_clock wall time.
//
// Before/AfterAnalysis fire strictly around the actual (re)computation --
// confirmed by reading AnalysisManager::getResultImpl (PassManagerImpl.h):
// both calls are skipped entirely on a cache hit (`if (Inserted)` gates all
// of it), so this stack captures computation cost only, never cache-lookup
// overhead, and Before/After are strictly paired the same way pass-level
// Before/After are.
// ---------------------------------------------------------------------------

struct PendingAnalysis {
  std::string Name;
  const Function *F; // may be null if this analysis isn't Function-keyed
  std::chrono::nanoseconds StartUser;
  std::chrono::nanoseconds StartSys;
};

std::vector<PendingAnalysis> &analysisTimingStack() {
  static std::vector<PendingAnalysis> Stack;
  return Stack;
}

void beforeAnalysis(StringRef Name, const Any &IR) {
  sys::TimePoint<> Now;
  std::chrono::nanoseconds User, Sys;
  sys::Process::GetTimeUsage(Now, User, Sys);
  analysisTimingStack().push_back(
      {Name.str(), unwrapToFunction(IR), User, Sys});
}

void afterAnalysis(StringRef Name, const Any &IR) {
  auto &Stack = analysisTimingStack();
  uint64_t ElapsedUs = 0;
  if (Stack.empty()) {
    errs() << "LPTA: after-analysis callback for '" << Name
           << "' with no matching before-analysis snapshot; timing dropped\n";
  } else {
    PendingAnalysis P = std::move(Stack.back());
    Stack.pop_back();
    sys::TimePoint<> Now;
    std::chrono::nanoseconds User, Sys;
    sys::Process::GetTimeUsage(Now, User, Sys);
    auto Delta = (User - P.StartUser) + (Sys - P.StartSys);
    ElapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(Delta).count();
  }

  if (!isTrackedAnalysis(Name))
    return;
  const Function *F = unwrapToFunction(IR);
  if (!F)
    return;

  AnalysisTiming &T = analysisState()[F][Name];
  bool Wasted = T.PendingRecompute;
  if (Wasted) {
    ++T.WastedRecomputes;
    T.PendingRecompute = false;
  }
  T.Live = true;
  T.LastComputeCPUTimeUs = ElapsedUs;
  T.TotalCPUTimeUs += ElapsedUs;

  json::Object Rec;
  Rec["schema_version"] = "2.2.0";
  Rec["record_type"] = "analysis";
  Rec["analysis"] = Name.str();
  Rec["unit_kind"] = "function";
  Rec["unit_name"] = F->getName().str();
  Rec["cpu_time_us"] = ElapsedUs;
  Rec["wasted_recompute"] = Wasted;
  sink().emit(std::move(Rec));
}

void analysisInvalidated(StringRef Name, const Any &IR) {
  if (!isTrackedAnalysis(Name))
    return;
  const Function *F = unwrapToFunction(IR);
  if (!F)
    return;
  AnalysisTiming *T = getAnalysisTiming(F, Name);
  if (!T)
    return; // Nothing was ever recorded live for this function; nothing to do.
  T->Live = false;
  T->PendingRecompute = true;
}

// ---------------------------------------------------------------------------
// Pending "before" state, matched LIFO with the corresponding "after" event
// (pass execution is a synchronous call stack, so a stack is sufficient).
// ---------------------------------------------------------------------------

struct PendingEntry {
  std::string PassID;
  ResolvedUnit Unit;
  Snapshot Before;
  bool Traced = false; // false for entries skipped by LPTA_FILTER_FUNC
  bool DTLiveAtStart = false;
  bool LoopInfoLiveAtStart = false;
  bool MemorySSALiveAtStart = false;
  bool ScalarEvolutionLiveAtStart = false;
  // Captured *after* our own before-snapshot work is done, so the delta to
  // afterPassCommon()'s first action (captured *before* any of our own
  // after-work) isolates the pass's own Pass::run() execution time --
  // excludes LPTA's own instrumentation overhead on both sides, the same
  // bracketing discipline verified for analysis timing (Tier 2).
  std::chrono::nanoseconds PassStartUser{};
  std::chrono::nanoseconds PassStartSys{};
};

std::vector<PendingEntry> &pendingStack() {
  static std::vector<PendingEntry> Stack;
  return Stack;
}

// Before/After (or Before/AfterInvalidated) instrumentation calls are always
// paired 1:1 for every pass the pass manager actually runs, in LIFO order
// (pass execution is a synchronous call stack). We always push here, even
// for units LPTA_FILTER_FUNC excludes, so the stack stays balanced and the
// matching "after" call can be identified purely by position, never by name.
bool isLiveAtStart(const Function *F, StringRef Name) {
  AnalysisTiming *T = F ? getAnalysisTiming(F, Name) : nullptr;
  return T && T->Live;
}

void beforePass(StringRef PassID, const Any &IR) {
  ResolvedUnit RU = resolveUnit(IR);
  bool Traced = sink().shouldTrace(RU);
  Snapshot Before = Traced ? takeSnapshot(RU) : Snapshot{};
  bool DTLive = isLiveAtStart(RU.F, DTAnalysisName);
  bool LoopLive = isLiveAtStart(RU.F, LoopAnalysisName);
  bool MSSALive = isLiveAtStart(RU.F, MemorySSAAnalysisName);
  bool SCEVLive = isLiveAtStart(RU.F, ScalarEvolutionAnalysisName);
  // Start the pass-execution clock last, right before control returns to
  // the pass manager (which immediately calls Pass::run()) -- everything
  // above this point is LPTA's own before-snapshot work and must not be
  // counted as part of the pass's own time.
  sys::TimePoint<> Now;
  std::chrono::nanoseconds StartUser, StartSys;
  sys::Process::GetTimeUsage(Now, StartUser, StartSys);
  pendingStack().push_back({PassID.str(), std::move(RU), std::move(Before),
                            Traced, DTLive, LoopLive, MSSALive, SCEVLive,
                            StartUser, StartSys});
}

void afterPassCommon(StringRef PassID, const PreservedAnalyses *PA,
                     bool Invalidated) {
  // Stop the clock first, before any of LPTA's own after-work (the After
  // snapshot, diffing, etc.) can contaminate the measured pass-execution
  // time. Stack-empty/untraced entries below discard this; the small
  // wasted GetTimeUsage() call in that (rare, error/filtered) case is not
  // worth branching around.
  sys::TimePoint<> Now;
  std::chrono::nanoseconds EndUser, EndSys;
  sys::Process::GetTimeUsage(Now, EndUser, EndSys);

  auto &Stack = pendingStack();
  if (Stack.empty()) {
    // Before/After calls are expected to be strictly paired (see the stack
    // discipline comment above beforePass); this fires only if that
    // assumption breaks on some LLVM version's callback ordering, in which
    // case the trace is silently missing events unless this is visible.
    errs() << "LPTA: after-pass callback for '" << PassID
           << "' with no matching before-pass snapshot; dropping event\n";
    return;
  }
  PendingEntry Entry = std::move(Stack.back());
  Stack.pop_back();
  if (!Entry.Traced)
    return;
  (void)PassID; // Identity comes from Entry; PassID is redundant here.

  auto PassDelta =
      (EndUser - Entry.PassStartUser) + (EndSys - Entry.PassStartSys);
  uint64_t PassCPUTimeUs =
      std::chrono::duration_cast<std::chrono::microseconds>(PassDelta).count();

  json::Object Rec;
  Rec["schema_version"] = "2.2.0";
  Rec["record_type"] = "pass";
  Rec["pass"] = Entry.PassID;
  Rec["unit_kind"] = Entry.Unit.Kind.str();
  Rec["unit_name"] = Entry.Unit.Name;
  Rec["invalidated"] = Invalidated;
  Rec["pass_cpu_time_us"] = PassCPUTimeUs;

  // The IR unit is gone; any liveness state tracked for it is meaningless
  // going forward (and, if the underlying Function's memory is ever reused,
  // actively misleading), so drop it now rather than let it go stale.
  if (Invalidated && Entry.Unit.F)
    analysisState().erase(Entry.Unit.F);

  // When the pass invalidates its IR unit outright (e.g. deletes the
  // function), it is not safe to re-print it: skip the "after" snapshot.
  if (!Invalidated) {
    Snapshot After = takeSnapshot(Entry.Unit);
    bool Changed = After.Text != Entry.Before.Text;
    unsigned Added = 0, Removed = 0;
    if (Changed)
      diffLines(Entry.Before.Text, After.Text, Added, Removed);
    bool CFGChanged =
        Entry.Before.BBs != After.BBs || Entry.Before.Edges != After.Edges;

    Rec["ir_changed"] = Changed;
    Rec["before_instr_count"] = Entry.Before.Instrs;
    Rec["after_instr_count"] = After.Instrs;
    Rec["before_bb_count"] = Entry.Before.BBs;
    Rec["after_bb_count"] = After.BBs;
    Rec["lines_added"] = Added;
    Rec["lines_removed"] = Removed;
    Rec["cfg_changed"] = CFGChanged;
  }

  if (PA) {
    bool AllPreserved = PA->areAllPreserved();
    bool DTPreserved = PA->getChecker<DominatorTreeAnalysis>().preserved();
    bool LIPreserved = PA->getChecker<LoopAnalysis>().preserved();
    bool CFGSetPreserved = PA->allAnalysesInSetPreserved<CFGAnalyses>();
    Rec["all_preserved"] = AllPreserved;
    Rec["dt_preserved"] = DTPreserved;
    Rec["loop_info_preserved"] = LIPreserved;
    Rec["cfg_analyses_set_preserved"] = CFGSetPreserved;

    bool CFGChanged = false;
    if (auto *V = Rec.get("cfg_changed"))
      CFGChanged = V->getAsBoolean().value_or(false);

    Rec["dt_live_before_pass"] = Entry.DTLiveAtStart;
    Rec["loop_info_live_before_pass"] = Entry.LoopInfoLiveAtStart;
    Rec["memoryssa_live_before_pass"] = Entry.MemorySSALiveAtStart;
    Rec["scev_live_before_pass"] = Entry.ScalarEvolutionLiveAtStart;

    auto wastedCount = [&](StringRef Name) -> unsigned {
      AnalysisTiming *T =
          Entry.Unit.F ? getAnalysisTiming(Entry.Unit.F, Name) : nullptr;
      return T ? T->WastedRecomputes : 0;
    };
    Rec["dt_wasted_recompute_count"] = wastedCount(DTAnalysisName);
    Rec["memoryssa_wasted_recompute_count"] =
        wastedCount(MemorySSAAnalysisName);
    Rec["scev_wasted_recompute_count"] =
        wastedCount(ScalarEvolutionAnalysisName);

    // The core LPTA signal (Phase 2, liveness-aware): the CFG demonstrably
    // changed, DominatorTree was actually cached going into this pass (not
    // merely legal to preserve), and it was not preserved. Requiring
    // liveness is what fixes Phase 1's over-firing on passes like SimplifyCFG
    // that get flagged even though DT was never computed at that point in
    // the pipeline, so there was nothing wasteful about "not preserving" it
    // (see RESEARCH.md §5). (!DTPreserved already implies !AllPreserved,
    // since getChecker<T>().preserved() is true whenever
    // PreservedAnalyses::all() was returned — see RESEARCH.md §2.2 — so
    // AllPreserved is kept only for its own field.)
    Rec["incremental_update_candidate"] =
        CFGChanged && Entry.DTLiveAtStart && !DTPreserved;
  }

  sink().emit(std::move(Rec));
}

void afterPass(StringRef PassID, const Any &, const PreservedAnalyses &PA) {
  afterPassCommon(PassID, &PA, /*Invalidated=*/false);
}

void afterPassInvalidated(StringRef PassID, const PreservedAnalyses &PA) {
  afterPassCommon(PassID, &PA, /*Invalidated=*/true);
}

void registerLPTACallbacks(PassBuilder &PB) {
  PassInstrumentationCallbacks *PIC = PB.getPassInstrumentationCallbacks();
  if (!PIC) {
    errs()
        << "LPTA: no PassInstrumentationCallbacks available; instrumentation "
           "disabled\n";
    return;
  }
  PIC->registerBeforeNonSkippedPassCallback(beforePass);
  PIC->registerAfterPassCallback(afterPass);
  PIC->registerAfterPassInvalidatedCallback(afterPassInvalidated);
  PIC->registerBeforeAnalysisCallback(beforeAnalysis);
  PIC->registerAfterAnalysisCallback(afterAnalysis);
  PIC->registerAnalysisInvalidatedCallback(analysisInvalidated);
}

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LPTAInstrumentation", LLVM_VERSION_STRING,
          [](PassBuilder &PB) { registerLPTACallbacks(PB); }};
}
