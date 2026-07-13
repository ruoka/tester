#!/usr/bin/env bash
# Shared helpers for CB smoke tests (object cache / JSONL compile_end contract).

set -euo pipefail

CB_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CB_SMOKE_ROOT="$(cd "${CB_LIB_DIR}/../.." && pwd)"
CB_FIXTURE_SRC="${CB_LIB_DIR}/fixture/hello.c++"

BUILD_DIR="${BUILD_DIR:-}"
if [[ -z "${BUILD_DIR}" ]]; then
  case "$(uname -s)" in
    Darwin) BUILD_DIR="build-darwin-debug" ;;
    Linux) BUILD_DIR="build-linux-debug" ;;
    *) BUILD_DIR="build-debug" ;;
  esac
fi

CB_BIN="${CB_BIN:-${CB_SMOKE_ROOT}/tools/cb}"
STD_CPPM="${STD_CPPM:-${LLVM_PATH:-}}"

JSONL_MODE=0
LAST_JSONL=""
LAST_WORK_DIR=""
FAILURES=0
TESTS_RUN=0
START_MS=0

jsonl_emit() {
  [[ "${JSONL_MODE}" -eq 1 ]] || return 0
  printf '%s\n' "$1"
}

log() {
  printf '%s\n' "$*" >&2
}

fail() {
  FAILURES=$((FAILURES + 1))
  log "FAIL: $*"
  if [[ -n "${LAST_JSONL}" ]]; then
    log "--- cb jsonl (last run, last 40 lines) ---"
    printf '%s\n' "${LAST_JSONL}" | tail -40 >&2
    log "--- end jsonl ---"
  fi
  jsonl_emit '{"type":"smoke_assert_failed","message":"assertion_failed"}'
}

resolve_std_cppm() {
  if [[ -n "${STD_CPPM}" && -f "${STD_CPPM}" ]]; then
    return 0
  fi
  case "$(uname -s)" in
    Darwin) STD_CPPM="/usr/local/llvm/share/libc++/v1/std.cppm" ;;
    Linux) STD_CPPM="/usr/lib/llvm-21/share/libc++/v1/std.cppm" ;;
    *) STD_CPPM="" ;;
  esac
  if [[ ! -f "${STD_CPPM}" ]]; then
    log "std.cppm not found — set LLVM_PATH or STD_CPPM"
    return 1
  fi
}

cb_needs_rebuild() {
  local cb_source="${CB_SMOKE_ROOT}/tools/cb.c++"
  [[ -f "${cb_source}" ]] || return 1
  [[ ! -x "${CB_BIN}" ]] && return 0
  [[ "${cb_source}" -nt "${CB_BIN}" ]] && return 0
  find "${CB_SMOKE_ROOT}/tools" -maxdepth 1 -name '*.h++' -newer "${CB_BIN}" -print -quit 2>/dev/null | grep -q .
}

require_cb() {
  resolve_std_cppm || exit 1
  if cb_needs_rebuild; then
    log "Rebuilding cb (sources newer than ${CB_BIN})..."
    (cd "${CB_SMOKE_ROOT}" && ./tools/CB.sh debug build >/dev/null)
  fi
  if [[ ! -x "${CB_BIN}" ]]; then
    log "cb not found at ${CB_BIN} — run ./tools/CB.sh debug build first"
    exit 1
  fi
}

prepare_work_dir() {
  local dir
  dir="$(mktemp -d "${TMPDIR:-/tmp}/cb_smoke.XXXXXX")"
  cp "${CB_FIXTURE_SRC}" "${dir}/hello.c++"
  LAST_WORK_DIR="${dir}"
  rm -rf "${dir}/${BUILD_DIR}"
  printf '%s' "${dir}"
}

cleanup_work_dir() {
  if [[ -n "${LAST_WORK_DIR}" && -d "${LAST_WORK_DIR}" ]]; then
    rm -rf "${LAST_WORK_DIR}"
    LAST_WORK_DIR=""
  fi
}

run_cb_build() {
  local work_dir=$1
  shift
  local status=0
  LAST_JSONL="$(
    cd "${work_dir}"
    "${CB_BIN}" "${STD_CPPM}" debug build --jsonl "$@" 2>/dev/null
  )" || status=$?
  return "${status}"
}

run_cb_cache_status() {
  local work_dir=$1
  shift
  local status=0
  LAST_JSONL="$(
    cd "${work_dir}"
    "${CB_BIN}" "${STD_CPPM}" debug cache status --jsonl "$@" 2>/dev/null
  )" || status=$?
  return "${status}"
}

run_cb_cache_invalidate() {
  local work_dir=$1
  shift
  local status=0
  LAST_JSONL="$(
    cd "${work_dir}"
    "${CB_BIN}" "${STD_CPPM}" debug cache invalidate --jsonl "$@" 2>/dev/null
  )" || status=$?
  return "${status}"
}

object_cache_path() {
  local work_dir=$1
  printf '%s/%s/cache/object-cache.txt' "${work_dir}" "${BUILD_DIR}"
}

assert_text_contains() {
  local haystack=$1
  local needle=$2
  local label=${3:-contains}
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ "${haystack}" == *"${needle}"* ]]; then
    jsonl_emit "{\"type\":\"smoke_assert_passed\",\"matcher\":\"${label}\"}"
    return 0
  fi
  fail "expected output to contain: ${needle}"
  return 0
}

assert_text_not_contains() {
  local haystack=$1
  local needle=$2
  local label=${3:-not_contains}
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ "${haystack}" != *"${needle}"* ]]; then
    jsonl_emit "{\"type\":\"smoke_assert_passed\",\"matcher\":\"${label}\"}"
    return 0
  fi
  fail "expected output NOT to contain: ${needle}"
  return 0
}

assert_jsonl_contains() {
  assert_text_contains "${LAST_JSONL}" "$1" "${2:-jsonl_contains}"
}

assert_jsonl_not_contains() {
  assert_text_not_contains "${LAST_JSONL}" "$1" "${2:-jsonl_not_contains}"
}

assert_profile_header() {
  local cache_file=$1
  local label=${2:-profile_header}
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ ! -f "${cache_file}" ]]; then
    fail "object cache missing: ${cache_file}"
    return 0
  fi
  local first_line
  first_line="$(head -1 "${cache_file}")"
  if [[ "${first_line}" == profile$'\t'* ]]; then
    jsonl_emit "{\"type\":\"smoke_assert_passed\",\"matcher\":\"${label}\"}"
    return 0
  fi
  fail "expected profile header, got: ${first_line}"
  return 0
}

assert_profile_contains() {
  local cache_file=$1
  local needle=$2
  local label=${3:-profile_contains}
  local first_line
  first_line="$(head -1 "${cache_file}")"
  assert_text_contains "${first_line}" "${needle}" "${label}"
}

assert_compile_end_has_no_profile_diff() {
  local label=${1:-compile_end_no_profile_diff}
  TESTS_RUN=$((TESTS_RUN + 1))
  if python3 - "${LAST_JSONL}" <<'PY'
import json, sys
text = sys.argv[1]
for line in text.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        continue
    if obj.get("type") == "compile_end" and "profile_diff" in obj:
        print("compile_end must not carry profile_diff (use profile_changed)", file=sys.stderr)
        raise SystemExit(1)
raise SystemExit(0)
PY
  then
    jsonl_emit "{\"type\":\"smoke_assert_passed\",\"matcher\":\"${label}\"}"
    return 0
  fi
  fail "compile_end events must not include profile_diff"
  return 0
}

assert_link_cache_hits() {
  local min_hits=$1
  local label=${2:-link_cache_hits}
  TESTS_RUN=$((TESTS_RUN + 1))
  if python3 - "${min_hits}" "${LAST_JSONL}" <<'PY'
import json, sys
min_hits = int(sys.argv[1])
text = sys.argv[2]
hits = 0
for line in text.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        continue
    if obj.get("type") == "link_end" and obj.get("cache_hit") is True:
        hits += 1
if hits >= min_hits:
    raise SystemExit(0)
print(f"link_end cache_hit count {hits} < {min_hits}", file=sys.stderr)
raise SystemExit(1)
PY
  then
    jsonl_emit "{\"type\":\"smoke_assert_passed\",\"matcher\":\"${label}\"}"
    return 0
  fi
  fail "expected at least ${min_hits} link_end with cache_hit:true"
  return 0
}

assert_compile_start_end_pairs() {
  local label=${1:-compile_start_end_pairs}
  TESTS_RUN=$((TESTS_RUN + 1))
  if python3 - "${LAST_JSONL}" <<'PY'
import json, sys
text = sys.argv[1]
starts = {}
ends = {}
for line in text.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        continue
    t = obj.get("type")
    src = obj.get("source_path")
    if not src:
        continue
    if t == "compile_start":
        starts[src] = starts.get(src, 0) + 1
    elif t == "compile_end":
        ends[src] = ends.get(src, 0) + 1
if not starts:
    print("no compile_start events", file=sys.stderr)
    raise SystemExit(1)
for src, count in starts.items():
    if ends.get(src, 0) < count:
        print(f"missing compile_end for {src}", file=sys.stderr)
        raise SystemExit(1)
raise SystemExit(0)
PY
  then
    jsonl_emit "{\"type\":\"smoke_assert_passed\",\"matcher\":\"${label}\"}"
    return 0
  fi
  fail "every compile_start must have a matching compile_end"
  return 0
}

assert_compile_cache_hits() {
  local min_hits=$1
  local label=${2:-compile_cache_hits}
  TESTS_RUN=$((TESTS_RUN + 1))
  if python3 - "${min_hits}" "${LAST_JSONL}" <<'PY'
import json, sys
min_hits = int(sys.argv[1])
text = sys.argv[2]
hits = 0
for line in text.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        continue
    if obj.get("type") == "compile_end" and obj.get("cache_hit") is True:
        hits += 1
if hits >= min_hits:
    raise SystemExit(0)
print(f"compile_end cache_hit count {hits} < {min_hits}", file=sys.stderr)
raise SystemExit(1)
PY
  then
    jsonl_emit "{\"type\":\"smoke_assert_passed\",\"matcher\":\"${label}\"}"
    return 0
  fi
  fail "expected at least ${min_hits} compile_end with cache_hit:true"
  return 0
}

begin_case() {
  local name=$1
  log ""
  log "=== case: ${name} ==="
  jsonl_emit "{\"type\":\"smoke_case_start\",\"name\":\"${name}\"}"
  cleanup_work_dir
}

end_case() {
  local name=$1
  cleanup_work_dir
  jsonl_emit "{\"type\":\"smoke_case_end\",\"name\":\"${name}\"}"
}