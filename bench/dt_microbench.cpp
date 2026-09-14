//===- dt_microbench.cpp - Tier 1 synthetic DT update microbenchmark ----===//
//
// Standalone tool (not an opt plugin): builds a synthetic linear-chain CFG
// of N basic blocks, applies one controlled edit (split an edge by inserting
// a new block at position k), and times two ways of bringing DominatorTree
// back up to date:
//   - incremental: DominatorTree::applyUpdates() with the 3-update batch
//     that exactly describes the edit.
//   - full: DominatorTree::recalculate() from scratch.
// Sweeps N (function size) and k (edit locality, as a % of N) to get a
// controlled scaling curve free of real-world variance -- see
// RESEARCH.md §8, Tier 1.
//
// Why a linear chain, not a more "realistic" CFG: it isolates the one
// variable this experiment is about (how update cost scales with size and
// edit position) from loop-nesting/branch-factor effects that Tier 2's
// real-program measurements already cover from the other direction. It is
// also the shape where the answer is *not* obvious a priori: in a pure
// linear chain, DominatorTree depth equals position in the chain, so
// inserting one block increases the DT-depth of every subsequent block by
// one -- whether that forces the incremental updater to touch O(N-k) nodes
// (same order as recomputing from scratch) or genuinely stays local is
// exactly what this measures instead of assumes.
//
// Uses llvm::sys::Process::GetTimeUsage (process user+sys CPU time, the
// same technique -time-passes and LPTAInstrumentation.cpp's Tier 2 use),
// not wall-clock, for the same noise-avoidance reason as Tier 2.
//
// Output: one JSON object per trial to stdout (JSONL), matching this
// project's existing trace convention so the same kind of tooling
// (validate_trace.py's json.loads-per-line pattern, pandas, jq, ...) works
// on it unmodified. Fields: n_blocks, position_pct, method
// ("incremental"|"full"), cpu_time_us, trial.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CFGUpdate.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstdlib>
#include <vector>

using namespace llvm;

namespace {

// Experiment design constants. Deliberately literal constants here, not
// command-line flags: these are the actual experiment design (what's being
// swept and how many repetitions), which should be visible and reviewable
// in source, not hidden behind flags nobody will realistically vary run to
// run. Edit these directly for a different sweep.
constexpr unsigned Sizes[] = {100, 1000, 10000, 100000};
constexpr unsigned PositionPercents[] = {10, 50, 90};
constexpr unsigned TrialsPerConfig = 30;

// A linear chain of N blocks: entry -> bb0 -> bb1 -> ... -> bb{N-1} -> ret.
// Deliberately minimal block bodies (just a terminator): DominatorTree's
// cost depends on CFG shape (blocks/edges), not on instruction count within
// a block, so keeping bodies empty avoids conflating the two.
Function *buildChain(Module &M, unsigned N) {
  LLVMContext &Ctx = M.getContext();
  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
  auto *F = Function::Create(FT, Function::InternalLinkage, "chain", M);

  std::vector<BasicBlock *> BBs;
  BBs.reserve(N);
  for (unsigned i = 0; i < N; ++i)
    BBs.push_back(BasicBlock::Create(Ctx, "bb" + std::to_string(i), F));

  for (unsigned i = 0; i + 1 < N; ++i)
    IRBuilder<>(BBs[i]).CreateBr(BBs[i + 1]);
  IRBuilder<>(BBs[N - 1]).CreateRetVoid();
  return F;
}

// Splits the edge BBs[k] -> BBs[k+1] by inserting a new block between them,
// and appends the exactly-corresponding DominatorTree update batch to
// Updates (used only by the incremental path; the full-recompute path
// ignores it). Mutates the IR first, matching applyUpdates()'s documented
// contract ("the current CFG ... provides ... the pre-view" via the
// reverse of these updates -- i.e. the edit must already be applied before
// calling applyUpdates with updates describing it).
void splitEdgeAt(Function &F, unsigned K,
                 std::vector<DominatorTree::UpdateType> &Updates) {
  std::vector<BasicBlock *> BBs;
  BBs.reserve(F.size());
  for (BasicBlock &BB : F)
    BBs.push_back(&BB);

  BasicBlock *From = BBs[K];
  BasicBlock *To = BBs[K + 1];
  auto *NewBB = BasicBlock::Create(F.getContext(), "split", &F, To);
  IRBuilder<>(NewBB).CreateBr(To);
  From->getTerminator()->setSuccessor(0, NewBB);

  using UK = cfg::UpdateKind;
  Updates.emplace_back(UK::Insert, From, NewBB);
  Updates.emplace_back(UK::Insert, NewBB, To);
  Updates.emplace_back(UK::Delete, From, To);
}

uint64_t cpuTimeUs(function_ref<void()> Body) {
  sys::TimePoint<> Now;
  std::chrono::nanoseconds User0, Sys0, User1, Sys1;
  sys::Process::GetTimeUsage(Now, User0, Sys0);
  Body();
  sys::Process::GetTimeUsage(Now, User1, Sys1);
  auto Delta = (User1 - User0) + (Sys1 - Sys0);
  return std::chrono::duration_cast<std::chrono::microseconds>(Delta).count();
}

void emit(unsigned N, unsigned PositionPct, StringRef Method, uint64_t Us,
          unsigned Trial) {
  outs() << "{\"n_blocks\":" << N << ",\"position_pct\":" << PositionPct
         << ",\"method\":\"" << Method << "\",\"cpu_time_us\":" << Us
         << ",\"trial\":" << Trial << "}\n";
}

// One fresh chain + edit + timed update, for one method. Building a fresh
// module/function per trial (rather than reusing and undoing the edit)
// keeps trials fully independent -- no risk of one trial's mutation or
// cache state leaking into the next.
void runTrial(unsigned N, unsigned PositionPct, unsigned Trial) {
  unsigned K =
      std::max<unsigned>(1, std::min<unsigned>(N - 2, N * PositionPct / 100));

  {
    LLVMContext Ctx;
    Module M("bench", Ctx);
    Function *F = buildChain(M, N);
    DominatorTree DT;
    DT.recalculate(*F);
    std::vector<DominatorTree::UpdateType> Updates;
    splitEdgeAt(*F, K, Updates);
    uint64_t Us = cpuTimeUs([&] { DT.applyUpdates(Updates); });
    // NDEBUG is defined in our Release build, so a plain assert() here
    // would silently compile away and verify nothing -- checked explicitly
    // instead. Only on the first trial of the *smallest* size per position:
    // DominatorTree::verify() is dramatically more expensive than
    // recalculate() itself (empirically superlinear enough that including
    // it at N=100000 turned a ~2 minute sweep into 20+ minutes), and
    // correctness of the update logic doesn't depend on N -- if the same
    // splitEdgeAt()/applyUpdates() code path produces a valid tree at a
    // cheap size, it does at every size.
    if (Trial == 0 && N == Sizes[0] && !DT.verify()) {
      errs() << "dt_microbench: incremental update produced an invalid "
                "DominatorTree at n_blocks="
             << N << " position_pct=" << PositionPct << "\n";
      std::exit(1);
    }
    emit(N, PositionPct, "incremental", Us, Trial);
  }

  {
    LLVMContext Ctx;
    Module M("bench", Ctx);
    Function *F = buildChain(M, N);
    DominatorTree DT;
    DT.recalculate(*F);
    std::vector<DominatorTree::UpdateType> Updates; // unused; same edit
    splitEdgeAt(*F, K, Updates);
    uint64_t Us = cpuTimeUs([&] { DT.recalculate(*F); });
    emit(N, PositionPct, "full", Us, Trial);
  }
}

} // namespace

int main() {
  for (unsigned N : Sizes)
    for (unsigned PositionPct : PositionPercents)
      for (unsigned Trial = 0; Trial < TrialsPerConfig; ++Trial)
        runTrial(N, PositionPct, Trial);
  return 0;
}
