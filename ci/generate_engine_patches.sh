#!/usr/bin/env bash
# Generate two independent Firefox engine patches from the patched feature
# submodule working tree. The output is based on the exact gitlink ref that the
# feature branch records, so it applies to a clean `git submodule update`.
#
#   01-proxy.patch  WindowProxy glue only (no trace dependency)
#   02-trace.patch  standalone trace + JIT/VM/WASM hooks
#
# Usage: bash ci/generate_engine_patches.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FF="$ROOT/firefox"
OUT="$ROOT/patches"
TMP="$OUT/.tmp"
mkdir -p "$OUT" "$TMP"

cd "$FF"
BASE=$TMP/index-base.idx
cp "$(git rev-parse --git-path index)" "$BASE"

python - "$ROOT" <<'PY'
import pathlib, subprocess, sys
root = pathlib.Path(sys.argv[1])
tmp = root / "patches" / ".tmp"
head = subprocess.check_output(
    ["git", "show", "HEAD:js/src/vm/moz.build"], text=True)
proxy = head.replace('    "RuntimeFuses.cpp",\n',
                     '    "RuntimeFuses.cpp",\n    "RuyijsWindowProxy.cpp",\n', 1)
trace = head.replace('    "Stack.cpp",\n',
                     '    "Stack.cpp",\n    "StandaloneTrace.cpp",\n    "StandaloneTracePro.cpp",\n', 1)
assert proxy != head and trace != head
(tmp / "moz.proxy").write_text(proxy, encoding="utf-8", newline="")
(tmp / "moz.trace").write_text(trace, encoding="utf-8", newline="")
PY

# --- 01 proxy patch: new files + moz.build proxy line ---
cp "$BASE" "$TMP/proxy.idx"
GIT_INDEX_FILE="$TMP/proxy.idx" git add \
  js/src/vm/RuyijsWindowProxy.h js/src/vm/RuyijsWindowProxy.cpp
blob=$(git hash-object -w "$TMP/moz.proxy")
GIT_INDEX_FILE="$TMP/proxy.idx" git update-index --add --cacheinfo \
  100644,"$blob",js/src/vm/moz.build
GIT_INDEX_FILE="$TMP/proxy.idx" git diff --cached > "$OUT/01-proxy.patch"

# --- 02 trace patch: trace files + hooks + moz.build trace lines ---
cp "$BASE" "$TMP/trace.idx"
GIT_INDEX_FILE="$TMP/trace.idx" git add \
  js/src/jit/ABIFunctionList-inl.h \
  js/src/jit/BaselineCodeGen.cpp \
  js/src/jit/CacheIR.cpp \
  js/src/vm/Interpreter.cpp \
  js/src/vm/JSContext.cpp \
  js/src/wasm/WasmJS.cpp \
  js/src/wasm/WasmModule.cpp \
  js/src/vm/StandaloneTrace.cpp \
  js/src/vm/StandaloneTrace.h \
  js/src/vm/StandaloneTracePro.cpp
blob=$(git hash-object -w "$TMP/moz.trace")
GIT_INDEX_FILE="$TMP/trace.idx" git update-index --add --cacheinfo \
  100644,"$blob",js/src/vm/moz.build
GIT_INDEX_FILE="$TMP/trace.idx" git diff --cached > "$OUT/02-trace.patch"

rm -rf "$TMP"
echo "generated from $(git rev-parse HEAD):"
wc -l "$OUT/01-proxy.patch" "$OUT/02-trace.patch"
