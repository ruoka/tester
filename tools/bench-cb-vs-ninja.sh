#!/usr/bin/env bash
# Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
# SPDX-License-Identifier: MIT
# See the LICENSE file in the project root for full license text.
#
# Wall-clock build-time comparison: tools/cb (direct, no CB.sh) vs CMake+Ninja.
#
# CB is invoked with the same argv/env CB.sh.core would exec (absolute -I,
# --include-examples, SDKROOT, LDFLAGS) so the object-cache profile matches
# the wrapper path. Ninja is driven via cmake --build; pass --verbose (default)
# to print full command lines, closer to CB --jsonl command argv noise.
#
# Usage:
#   ./tools/bench-cb-vs-ninja.sh
#   ./tools/bench-cb-vs-ninja.sh --modules=one-phase    # CB single-step modules
#   ./tools/bench-cb-vs-ninja.sh --cb-only              # compare CB revisions separately
#   ./tools/bench-cb-vs-ninja.sh --quiet-ninja          # default Ninja progress
#   ./tools/bench-cb-vs-ninja.sh --jobs=4 --cold=2
#   ./tools/bench-cb-vs-ninja.sh --results=/tmp/out.jsonl
#
# Parses stdout of this script for a human summary; machine rows are JSONL on
# the results file (and echoed). Exit 0 on success.

set -euo pipefail

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TOOLS_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

HOST="$(uname -s | tr '[:upper:]' '[:lower:]')"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)"
JOBS="$NCPU"
COLD_N=3
NOOP_N=5
TOUCH_N=3
NINJA_VERBOSE=1
RUN_NINJA=1
MODULES=two-phase
RESULTS="${TMPDIR:-/tmp}/cb-vs-ninja-bench.jsonl"
CONFIG=debug
LLVM_PREFIX="${LLVM_PREFIX:-/usr/local/llvm}"
CXX_COMPILER="${CXX_COMPILER:-$LLVM_PREFIX/bin/clang++}"
STD_CPPM="${STD_CPPM:-$LLVM_PREFIX/share/libc++/v1/std.cppm}"
CB_BIN="${CB_BIN:-$TOOLS_DIR/cb}"
TOUCH_TEST="${TOUCH_TEST:-tester/tester-assertions.test.c++}"
TOUCH_MOD="${TOUCH_MOD:-tester/tester-assertions.c++m}"

usage() {
  sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

for arg in "$@"; do
  case "$arg" in
    -h|--help) usage 0 ;;
    --cb-only) RUN_NINJA=0 ;;
    --quiet-ninja) NINJA_VERBOSE=0 ;;
    --verbose-ninja) NINJA_VERBOSE=1 ;;
    --modules=one-phase|--modules=two-phase) MODULES="${arg#*=}" ;;
    --jobs=*) JOBS="${arg#*=}" ;;
    --cold=*) COLD_N="${arg#*=}" ;;
    --noop=*) NOOP_N="${arg#*=}" ;;
    --touch=*) TOUCH_N="${arg#*=}" ;;
    --results=*) RESULTS="${arg#*=}" ;;
    --config=*) CONFIG="${arg#*=}" ;;
    *)
      echo "Unknown argument: $arg" >&2
      usage 2
      ;;
  esac
done

CB_DIR="build-${HOST}-${CONFIG}"
CMAKE_DIR="build-cmake-${HOST}-${CONFIG}"
case "$CONFIG" in
  debug) CMAKE_BUILD_TYPE=Debug ;;
  release) CMAKE_BUILD_TYPE=Release ;;
  *)
    echo "Unsupported --config=$CONFIG (use debug|release)" >&2
    exit 2
    ;;
esac

if [[ ! -x "$CB_BIN" ]]; then
  echo "CB binary missing; bootstrapping via CB.sh once..." >&2
  ./tools/CB.sh "$CONFIG" list --jsonl=failures >/dev/null
fi
if [[ ! -x "$CB_BIN" ]]; then
  echo "ERROR: $CB_BIN not executable" >&2
  exit 1
fi
if [[ ! -f "$STD_CPPM" ]]; then
  echo "ERROR: std.cppm not found at $STD_CPPM" >&2
  exit 1
fi
if [[ ! -s "$TOUCH_TEST" || ! -s "$TOUCH_MOD" ]]; then
  echo "ERROR: touch targets missing: $TOUCH_TEST / $TOUCH_MOD" >&2
  exit 1
fi
if [[ "$RUN_NINJA" == "1" ]]; then
  command -v cmake >/dev/null
  command -v ninja >/dev/null
fi
command -v python3 >/dev/null

# Match tools/CB.sh.core env before exec.
if [[ "$(uname -s)" == "Darwin" ]] && command -v xcrun >/dev/null 2>&1; then
  SDKROOT="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
  if [[ -n "$SDKROOT" && -d "$SDKROOT" ]]; then
    export SDKROOT
  fi
fi
export LDFLAGS="-Wl,-rpath,$LLVM_PREFIX/lib ${LDFLAGS-}"
# Removing the local build tree is only a cold std build when the shared cache is disabled.
# Respect an explicit non-empty override so the same script can measure clean-with-shared-cache.
export CB_STD_CACHE_DIR="${CB_STD_CACHE_DIR-}"

rm -f "$RESULTS"

now_ms() {
  python3 -c 'import time; print(int(time.time() * 1000))'
}

# Avoid bash brace-expansion eating Python dict literals — pass fields as argv.
emit_json() {
  python3 -c 'import json,sys; print(json.dumps(json.loads(sys.argv[1])))' "$1" | tee -a "$RESULTS"
}

emit_timed() {
  python3 -c 'import json,sys; print(json.dumps({"scenario": sys.argv[1], "ms": int(sys.argv[2]), "rc": int(sys.argv[3])}))' \
    "$1" "$2" "$3" | tee -a "$RESULTS"
}

cb_cmd() {
  # Same argv prefix as CB.sh.core exec (absolute -I + examples), plus --modules=.
  "$CB_BIN" "$STD_CPPM" -I "$PROJECT_ROOT/tester" --include-examples \
    --modules="$MODULES" "$@"
}

cmake_build() {
  local args=(--build "$CMAKE_DIR" --parallel "$JOBS")
  if [[ "$NINJA_VERBOSE" == "1" ]]; then
    args+=(--verbose)
  fi
  cmake "${args[@]}"
}

run_timed() {
  local label="$1"
  shift
  local start end ms rc
  start="$(now_ms)"
  set +e
  "$@" >/tmp/bench-cb-vs-ninja.out 2>/tmp/bench-cb-vs-ninja.err
  rc=$?
  set -e
  end="$(now_ms)"
  ms=$((end - start))
  emit_timed "$label" "$ms" "$rc"
  return 0
}

extract_cb_build_end() {
  local label="$1"
  python3 - "$label" <<'PY' | tee -a "$RESULTS"
import json, sys
label = sys.argv[1]
for line in open("/tmp/bench-cb-vs-ninja.out"):
    try:
        o = json.loads(line)
    except json.JSONDecodeError:
        continue
    if o.get("type") == "build_end":
        print(json.dumps({
            "scenario": f"{label}_detail",
            "ok": o.get("ok"),
            "duration_ms": o.get("duration_ms"),
            "rebuild_summary": o.get("rebuild_summary") or {},
        }))
        break
PY
}

assert_cb_ok() {
  python3 <<'PY'
import json
ok = None
rs = None
for line in open("/tmp/bench-cb-vs-ninja.out"):
    try:
        o = json.loads(line)
    except json.JSONDecodeError:
        continue
    if o.get("type") == "build_end":
        ok = o.get("ok")
        rs = o.get("rebuild_summary") or {}
assert ok is True, f"CB build_end ok={ok}"
assert "profile_change" not in rs, f"unexpected profile_change: {rs}"
PY
}

assert_cmake_ok() {
  [[ -x "$CMAKE_DIR/test_runner" ]]
  if grep -qE 'ninja: build stopped|FAILED:|CMake Error' /tmp/bench-cb-vs-ninja.err /tmp/bench-cb-vs-ninja.out 2>/dev/null; then
    echo "CMake/Ninja build reported failure:" >&2
    tail -40 /tmp/bench-cb-vs-ninja.err /tmp/bench-cb-vs-ninja.out >&2 || true
    exit 1
  fi
}

emit_json "$(python3 -c '
import json, sys
print(json.dumps({
    "scenario": "meta",
    "host": sys.argv[1],
    "config": sys.argv[2],
    "jobs": int(sys.argv[3]),
    "ninja_verbose": sys.argv[4] == "1",
    "modules": sys.argv[5],
    "cb_bin": sys.argv[6],
    "cxx": sys.argv[7],
    "cmake_dir": sys.argv[8],
    "cb_dir": sys.argv[9],
    "ninja_enabled": sys.argv[10] == "1",
}))
' "$HOST" "$CONFIG" "$JOBS" "$NINJA_VERBOSE" "$MODULES" "$CB_BIN" "$CXX_COMPILER" "$CMAKE_DIR" "$CB_DIR" "$RUN_NINJA")"

echo "=== Warm: align CB object-cache profile to --modules=$MODULES (untimed) ===" >&2
# Do not warm via CB.sh — it defaults to two-phase and would fight --modules=.
cb_cmd "$CONFIG" build --jsonl=failures --jobs="$JOBS" >/dev/null
cb_cmd "$CONFIG" cache status --jsonl=failures 2>/dev/null | python3 -c '
import json, sys
for line in sys.stdin:
    o = json.loads(line)
    if o.get("type") == "cache_status":
        assert o.get("profile_match") is True, o
        profile = o.get("current_profile") or ""
        print("profile_match OK", profile, file=sys.stderr)
'

echo "===== COLD FULL ($COLD_N x) =====" >&2
for ((i = 1; i <= COLD_N; i++)); do
  echo "-- CB cold $i" >&2
  cb_cmd "$CONFIG" clean >/dev/null
  run_timed "cb_cold_$i" cb_cmd "$CONFIG" build --jsonl=failures --jobs="$JOBS"
  extract_cb_build_end "cb_cold_$i"
  assert_cb_ok
done

if [[ "$RUN_NINJA" == "1" ]]; then
  for ((i = 1; i <= COLD_N; i++)); do
    echo "-- CMake+Ninja cold $i" >&2
    rm -rf "$CMAKE_DIR"
    run_timed "cmake_configure_$i" \
      cmake -S . -B "$CMAKE_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
        -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
        -DLLVM_PREFIX="$LLVM_PREFIX"
    run_timed "cmake_cold_$i" cmake_build
    assert_cmake_ok
  done
fi

echo "===== NO-OP ($NOOP_N x) =====" >&2
for ((i = 1; i <= NOOP_N; i++)); do
  run_timed "cb_noop_$i" cb_cmd "$CONFIG" build --jsonl=failures --jobs="$JOBS"
  extract_cb_build_end "cb_noop_$i"
  assert_cb_ok
done
if [[ "$RUN_NINJA" == "1" ]]; then
  for ((i = 1; i <= NOOP_N; i++)); do
    run_timed "cmake_noop_$i" cmake_build
  done
fi

echo "===== TOUCH TEST TU ($TOUCH_N x) =====" >&2
for ((i = 1; i <= TOUCH_N; i++)); do
  touch "$TOUCH_TEST"
  run_timed "cb_touch_test_$i" cb_cmd "$CONFIG" build --jsonl=failures --jobs="$JOBS"
  extract_cb_build_end "cb_touch_test_$i"
  assert_cb_ok
done
if [[ "$RUN_NINJA" == "1" ]]; then
  for ((i = 1; i <= TOUCH_N; i++)); do
    touch "$TOUCH_TEST"
    run_timed "cmake_touch_test_$i" cmake_build
    assert_cmake_ok
  done
fi

echo "===== TOUCH MODULE INTERFACE ($TOUCH_N x) =====" >&2
for ((i = 1; i <= TOUCH_N; i++)); do
  touch "$TOUCH_MOD"
  run_timed "cb_touch_module_$i" cb_cmd "$CONFIG" build --jsonl=failures --jobs="$JOBS"
  extract_cb_build_end "cb_touch_module_$i"
  assert_cb_ok
done
if [[ "$RUN_NINJA" == "1" ]]; then
  for ((i = 1; i <= TOUCH_N; i++)); do
    touch "$TOUCH_MOD"
    run_timed "cmake_touch_module_$i" cmake_build
    assert_cmake_ok
  done
fi

echo "===== SUMMARY =====" >&2
python3 - "$RESULTS" <<'PY'
import json, re, statistics, sys
from collections import defaultdict

path = sys.argv[1]
rows = []
meta = {}
for line in open(path):
    line = line.strip()
    if not line.startswith("{"):
        continue
    o = json.loads(line)
    if o.get("scenario") == "meta":
        meta = o
        continue
    if "ms" in o and "scenario" in o:
        rows.append(o)

groups = defaultdict(list)
for r in rows:
    m = re.match(r"(.+?)_\d+$", r["scenario"])
    if m:
        groups[m.group(1)].append(r["ms"])

print(f"{'scenario':28} {'n':>3} {'mean_ms':>8} {'min':>8} {'max':>8}")
for key in sorted(groups):
    xs = groups[key]
    print(f"{key:28} {len(xs):3} {statistics.mean(xs):8.0f} {min(xs):8} {max(xs):8}")

if "cmake_configure" in groups and "cmake_cold" in groups:
    first = statistics.mean(groups["cmake_configure"]) + statistics.mean(groups["cmake_cold"])
    print(f"cmake_first_time (configure+build) mean_ms: {first:.0f}")

def ratio(a, b):
    return statistics.mean(groups[a]) / statistics.mean(groups[b])

pairs = [
    ("cb_cold", "cmake_cold"),
    ("cb_noop", "cmake_noop"),
    ("cb_touch_test", "cmake_touch_test"),
    ("cb_touch_module", "cmake_touch_module"),
]
print()
if meta.get("ninja_enabled", True):
    ninja_label = "verbose Ninja" if meta.get("ninja_verbose") else "quiet Ninja"
    modules = meta.get("modules") or "two-phase"
    print(f"CB (--modules={modules}) / {ninja_label} ratios (mean wall-clock):")
    for a, b in pairs:
        if a in groups and b in groups:
            print(f"  {a} / {b}: {ratio(a, b):.2f}x")
else:
    print("CB-only run; Ninja scenarios skipped.")

print(f"\nresults: {path}")
if meta:
    print(
        f"modules={meta.get('modules')} ninja_verbose={meta.get('ninja_verbose')} "
        f"ninja_enabled={meta.get('ninja_enabled')} jobs={meta.get('jobs')} "
        f"config={meta.get('config')}"
    )
PY

echo "Done. Results: $RESULTS" >&2
