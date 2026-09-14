#!/usr/bin/env bash
# End-to-end smoke test: compile test/sample.c to LLVM IR, run it through the
# real -O2 pipeline with the LPTA plugin loaded, and render the HTML report.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
if [[ ! -f "$BUILD_DIR/lpta-tools.txt" ]]; then
  echo "Configure and build LPTA first; missing $BUILD_DIR/lpta-tools.txt" >&2
  exit 1
fi
mapfile -t TOOLCHAIN < "$BUILD_DIR/lpta-tools.txt"
CLANG="${CLANG:-${TOOLCHAIN[1]}}"
OPT="${OPT:-${TOOLCHAIN[2]}}"
PLUGIN="${PLUGIN:-${TOOLCHAIN[3]}}"
OUT_DIR="${OUT_DIR:-$ROOT/test/out}"

if [[ ! -f "$PLUGIN" ]]; then
  echo "Plugin not built. Run: cmake --build $BUILD_DIR" >&2
  exit 1
fi

# Reject a mismatched override before loading a plugin into opt.
for tool in "$CLANG" "$OPT"; do
  version="$("$tool" --version)"
  if [[ ! "$version" =~ version[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+) ]] || [[ "${BASH_REMATCH[1]}" != "${TOOLCHAIN[0]}" ]]; then
    echo "$tool must match the plugin's LLVM ${TOOLCHAIN[0]}" >&2
    exit 1
  fi
done

mkdir -p "$OUT_DIR"
"$CLANG" -S -emit-llvm -O0 -Xclang -disable-O0-optnone -o "$OUT_DIR/sample.ll" "$ROOT/test/sample.c"

LPTA_TRACE_OUT="$OUT_DIR/trace.jsonl" \
  "$OPT" -load-pass-plugin="$PLUGIN" -passes='default<O2>' \
  -disable-output "$OUT_DIR/sample.ll"

python3 "$ROOT/tools/lpta_report.py" "$OUT_DIR/trace.jsonl" -o "$OUT_DIR/report.html"

echo "Trace:  $OUT_DIR/trace.jsonl"
echo "Report: $OUT_DIR/report.html"
