#!/usr/bin/env bash
# CB smoke tests: object-cache profile, cache_hit, flag_change via JSONL compile_end.
#
# Usage:
#   ./tests/cb/smoke.sh [--jsonl] [--case NAME]
#
# Requires: ./tools/CB.sh debug build  (tools/cb must exist)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"

SELECTED_CASE=""
START_MS=$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --jsonl) JSONL_MODE=1 ;;
    --case) shift; SELECTED_CASE="${1:-}" ;;
    --help|-h)
      echo "usage: smoke.sh [--jsonl] [--case NAME]"
      echo "cases: profile_header, cache_hit, link_cache_hit, compile_start, cache_invalidate, flag_change, legacy_cache, cache_status"
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done

should_run() {
  [[ -z "${SELECTED_CASE}" || "${SELECTED_CASE}" == "$1" ]]
}

test_profile_header() {
  should_run profile_header || return 0
  begin_case profile_header
  local work_dir cache_file
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  cache_file="$(object_cache_path "${work_dir}")"
  assert_profile_header "${cache_file}"
  assert_profile_contains "${cache_file}" 'format=cb-object-cache-v2' "profile_format_v2"
  assert_profile_contains "${cache_file}" $'\tstd_cppm=' "profile_std_cppm"
  assert_profile_contains "${cache_file}" $'\tcxx=' "profile_cxx"
  assert_profile_contains "${cache_file}" $'\tcxx_sig=' "profile_cxx_sig"
  assert_profile_contains "${cache_file}" $'\tclang_ver=' "profile_clang_ver"
  assert_jsonl_contains '"type":"compile_end"' "compile_end_event"
  end_case profile_header
}

test_cache_hit() {
  should_run cache_hit || return 0
  begin_case cache_hit
  local work_dir first_jsonl
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  first_jsonl="${LAST_JSONL}"
  assert_text_contains "${first_jsonl}" '"cache_hit":false' "first_build_compiles"

  run_cb_build "${work_dir}"
  assert_compile_cache_hits 1 "second_build_cache_hit"
  assert_jsonl_not_contains '"rebuild_reason":"flag_change"' "no_flag_change"
  end_case cache_hit
}

test_link_cache_hit() {
  should_run link_cache_hit || return 0
  begin_case link_cache_hit
  local work_dir
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  assert_jsonl_contains '"type":"link_end"' "link_end_event"
  assert_jsonl_contains '"cache_hit":false' "first_link"

  run_cb_build "${work_dir}"
  assert_link_cache_hits 1 "second_build_link_cache_hit"
  end_case link_cache_hit
}

test_compile_start() {
  should_run compile_start || return 0
  begin_case compile_start
  local work_dir
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  assert_jsonl_contains '"type":"compile_start"' "compile_start_event"
  assert_jsonl_contains '"type":"compile_end"' "compile_end_event"
  assert_compile_start_end_pairs
  assert_jsonl_contains '"rebuild_reason":"source_stale"' "compile_start_rebuild_reason"
  end_case compile_start
}

test_cache_invalidate() {
  should_run cache_invalidate || return 0
  begin_case cache_invalidate
  local work_dir cache_file
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 1 "seed_cache_hit"

  run_cb_cache_invalidate "${work_dir}"
  assert_jsonl_contains '"type":"cache_invalidate_end"' "cache_invalidate_event"
  assert_jsonl_contains '"object_cache_removed":true' "object_cache_removed"

  cache_file="$(object_cache_path "${work_dir}")"
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ ! -f "${cache_file}" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"object_cache_absent"}'
  else
    fail "object cache file should be absent after invalidate"
  fi

  run_cb_build "${work_dir}"
  assert_jsonl_contains '"cache_hit":false' "recompile_after_invalidate"
  end_case cache_invalidate
}

test_flag_change() {
  should_run flag_change || return 0
  begin_case flag_change
  local work_dir first_jsonl
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  first_jsonl="${LAST_JSONL}"
  assert_text_contains "${first_jsonl}" '"cache_hit":false' "seed_build"

  run_cb_build "${work_dir}" --compile-flags -DCB_SMOKE_FLAG=1
  assert_jsonl_contains '"type":"profile_changed"' "profile_changed_event"
  assert_jsonl_contains '"reason":"flag_change"' "profile_changed_reason"
  assert_jsonl_contains '"profile_diff"' "profile_diff_present"
  assert_jsonl_contains '"compile"' "profile_diff_compile_field"
  assert_jsonl_contains 'DCB_SMOKE_FLAG=1' "profile_diff_added_flag"
  assert_jsonl_contains '"rebuild_reason":"flag_change"' "compile_end_flag_change"
  assert_compile_end_has_no_profile_diff
  assert_jsonl_contains '"cache_hit":false' "recompile_after_flag_change"
  end_case flag_change
}

test_legacy_cache() {
  should_run legacy_cache || return 0
  begin_case legacy_cache
  local work_dir cache_file
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  cache_file="$(object_cache_path "${work_dir}")"
  write_legacy_object_cache "${cache_file}"

  run_cb_build "${work_dir}"
  assert_profile_header "${cache_file}"
  assert_profile_contains "${cache_file}" 'format=cb-object-cache-v2' "legacy_upgraded_format"
  end_case legacy_cache
}

test_cache_status() {
  should_run cache_status || return 0
  begin_case cache_status
  local work_dir
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  run_cb_cache_status "${work_dir}"
  assert_jsonl_contains '"type":"cache_status"' "cache_status_event"
  assert_jsonl_contains '"profile_match":true' "cache_status_profile_match"
  assert_jsonl_contains '"legacy_header":false' "cache_status_not_legacy"
  assert_jsonl_contains 'format=cb-object-cache-v2' "cache_status_current_profile"
  end_case cache_status
}

main() {
  require_cb
  trap cleanup_work_dir EXIT

  jsonl_emit '{"type":"smoke_start","schema":"cb-smoke","version":1}'
  log "cb smoke tests (cb=${CB_BIN}, build_dir=${BUILD_DIR})"

  test_profile_header
  test_cache_hit
  test_link_cache_hit
  test_compile_start
  test_cache_invalidate
  test_flag_change
  test_legacy_cache
  test_cache_status

  local end_ms duration_ms passed
  end_ms=$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)
  duration_ms=$((end_ms - START_MS))
  passed=$([[ "${FAILURES}" -eq 0 ]] && echo true || echo false)

  jsonl_emit "{\"type\":\"smoke_summary\",\"tests_run\":${TESTS_RUN},\"failures\":${FAILURES},\"passed\":${passed},\"duration_ms\":${duration_ms}}"
  jsonl_emit "{\"type\":\"smoke_end\",\"passed\":${passed},\"duration_ms\":${duration_ms}}"

  if [[ "${FAILURES}" -gt 0 ]]; then
    log "FAILED: ${FAILURES} assertion(s) failed (${TESTS_RUN} checks run)"
    exit 1
  fi

  log "OK: all ${TESTS_RUN} checks passed (${duration_ms}ms)"
}

main "$@"