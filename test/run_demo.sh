#!/usr/bin/env bash
# End-to-end smoke test: compile test/sample.c to LLVM IR, run it through the
# real -O2 pipeline with the LPTA plugin loaded, and render the HTML report.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLANG="${CLANG:-/usr/lib/llvm-18/bin/clang}"
OPT="${OPT:-/usr/lib/llvm-18/bin/opt}"
PLUGIN="${PLUGIN:-$ROOT/build/LPTAInstrumentation.so}"
OUT_DIR="${OUT_DIR:-$ROOT/test/out}"

if [[ ! -f "$PLUGIN" ]]; then
  echo "Plugin not built. Run: cmake -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm && cmake --build build" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
"$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone -o "$OUT_DIR/sample.ll" "$ROOT/test/sample.c"

LPTA_TRACE_OUT="$OUT_DIR/trace.jsonl" \
  "$OPT" -load-pass-plugin="$PLUGIN" -passes='default<O2>' \
  -disable-output "$OUT_DIR/sample.ll"

python3 "$ROOT/tools/lpta_report.py" "$OUT_DIR/trace.jsonl" -o "$OUT_DIR/report.html"

echo "Trace:  $OUT_DIR/trace.jsonl"
echo "Report: $OUT_DIR/report.html"
