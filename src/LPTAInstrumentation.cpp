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

#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

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
  StringRef Kind;               // "function" | "loop" | "module" | "scc" | "unknown"
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
// Pending "before" state, matched LIFO with the corresponding "after" event
// (pass execution is a synchronous call stack, so a stack is sufficient).
// ---------------------------------------------------------------------------

struct PendingEntry {
  std::string PassID;
  ResolvedUnit Unit;
  Snapshot Before;
  bool Traced = false; // false for entries skipped by LPTA_FILTER_FUNC
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
void beforePass(StringRef PassID, const Any &IR) {
  ResolvedUnit RU = resolveUnit(IR);
  bool Traced = sink().shouldTrace(RU);
  Snapshot Before = Traced ? takeSnapshot(RU) : Snapshot{};
  pendingStack().push_back({PassID.str(), std::move(RU), std::move(Before), Traced});
}

void afterPassCommon(StringRef PassID, const PreservedAnalyses *PA,
                     bool Invalidated) {
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

  json::Object Rec;
  Rec["pass"] = Entry.PassID;
  Rec["unit_kind"] = Entry.Unit.Kind.str();
  Rec["unit_name"] = Entry.Unit.Name;
  Rec["invalidated"] = Invalidated;

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

    // The core LPTA signal: the CFG demonstrably changed, yet DominatorTree
    // was not preserved. This is exactly the pattern the project proposal
    // wants surfaced: a place where an explicit DomTreeUpdater call could
    // likely have replaced a blanket invalidation. (Note: !DTPreserved
    // already implies !AllPreserved, since getChecker<T>().preserved() is
    // true whenever PreservedAnalyses::all() was returned — see
    // RESEARCH.md §2.2 — so AllPreserved is kept only for its own field.)
    Rec["incremental_update_candidate"] = CFGChanged && !DTPreserved;
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
    errs() << "LPTA: no PassInstrumentationCallbacks available; instrumentation "
              "disabled\n";
    return;
  }
  PIC->registerBeforeNonSkippedPassCallback(beforePass);
  PIC->registerAfterPassCallback(afterPass);
  PIC->registerAfterPassInvalidatedCallback(afterPassInvalidated);
}

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LPTAInstrumentation", LLVM_VERSION_STRING,
          [](PassBuilder &PB) { registerLPTACallbacks(PB); }};
}
