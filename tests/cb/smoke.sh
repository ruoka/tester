#!/usr/bin/env bash
# CB smoke tests: object-cache profile, cache_hit, profile_change via JSONL compile_end.
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
      echo "cases: profile_header, cache_hit, link_cache_hit, compile_start, source_stale, source_list, compile_failure, link_failure, test_link_failure, link_rebuild_reason, implementation_pcm, rebuild_summary, test_lifecycle, cache_invalidate, profile_change, cache_status, jsonl_modes, jsonl_failure_mode"
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
  assert_profile_contains "${cache_file}" 'format=cb-object-cache-v3' "profile_format_v3"
  assert_profile_contains "${cache_file}" $'\tstd_cppm=' "profile_std_cppm"
  assert_profile_contains "${cache_file}" '@' "profile_std_cppm_sig"
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
  assert_jsonl_not_contains '"rebuild_reason":"profile_change"' "no_profile_change"
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
  assert_jsonl_contains '"rebuild_reason":"not_in_cache"' "compile_start_rebuild_reason"
  assert_jsonl_contains '"rebuild":{' "compile_start_rebuild_object"
  assert_jsonl_contains '"message":"Rebuilding' "compile_start_rebuild_message"
  assert_jsonl_contains '"rebuild_summary":{' "build_end_rebuild_summary"
  end_case compile_start
}

test_source_stale() {
  should_run source_stale || return 0
  begin_case source_stale
  local work_dir
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 1 "source_stale_seed_cache_hit"

  printf '%s\n' '// edited after cache seed' >> "${work_dir}/hello.c++"
  run_cb_build "${work_dir}"
  assert_compile_end "hello.c++" false source_stale true "edited_source_rebuild"
  assert_jsonl_contains '"rebuild":{"kind":"source_stale"' "edited_source_rebuild_object"
  assert_jsonl_contains '"hint":"Source mtime newer than cached compile timestamp."' "edited_source_hint"
  assert_rebuild_summary source_stale 1 "" "edited_source_summary"
  end_case source_stale
}

test_source_list() {
  should_run source_list || return 0
  begin_case source_list
  local work_dir
  work_dir="$(prepare_work_dir)"

  run_cb_list "${work_dir}"
  assert_jsonl_event_count list_start 1 "single_list_start"
  assert_jsonl_event_count unit 1 "single_list_unit"
  assert_jsonl_event_count list_summary 1 "single_list_summary"
  assert_jsonl_event_value list_start config debug "list_config"
  assert_jsonl_event_value list_start include_tests true "list_includes_tests"
  assert_jsonl_event_value unit path hello.c++ "list_unit_path"
  assert_jsonl_event_value unit kind non_module "list_unit_kind"
  assert_jsonl_event_value list_summary units_total 1 "list_units_total"
  assert_jsonl_event_value list_summary main_count 1 "list_main_count"
  end_case source_list
}

test_compile_failure() {
  should_run compile_failure || return 0
  begin_case compile_failure
  local work_dir
  work_dir="$(prepare_work_dir)"

  printf '%s\n' 'int broken( {' > "${work_dir}/broken.c++"
  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_build "${work_dir}"; then
    fail "broken source unexpectedly compiled"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"compile_failure_exit"}'
  fi
  assert_compile_end "broken.c++" false not_in_cache false "failed_compile_end"
  assert_jsonl_event_count build_end 1 "single_failed_build_end"
  assert_jsonl_event_value build_end ok false "failed_build_end_status"
  assert_jsonl_contains '"type":"eof"' "failure_jsonl_eof"
  end_case compile_failure
}

test_link_failure() {
  should_run link_failure || return 0
  begin_case link_failure
  local work_dir
  work_dir="$(prepare_work_dir)"

  printf '%s\n' \
    'int missing();' \
    'int main() { return missing(); }' > "${work_dir}/hello.c++"
  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_build "${work_dir}"; then
    fail "unresolved symbol unexpectedly linked"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"link_failure_exit"}'
  fi
  assert_jsonl_event_value link_end ok false "failed_link_end"
  assert_jsonl_event_value build_end ok false "link_failed_build_end"
  end_case link_failure
}

test_test_link_failure() {
  should_run test_link_failure || return 0
  begin_case test_link_failure
  local work_dir
  work_dir="$(prepare_work_dir)"

  printf '%s\n' \
    'int missing();' \
    'int main() { return missing(); }' > "${work_dir}/test_runner.c++"
  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_test "${work_dir}"; then
    fail "broken test_runner unexpectedly linked"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"test_link_failure_exit"}'
  fi
  assert_jsonl_event_value link_end ok false "failed_test_link_end"
  assert_jsonl_event_value build_end ok false "test_link_failed_build_end"
  end_case test_link_failure
}

test_link_rebuild_reason() {
  should_run link_rebuild_reason || return 0
  begin_case link_rebuild_reason
  local work_dir
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  assert_link_end "/hello" false missing_executable true "first_link_missing_executable"
  assert_jsonl_contains '"rebuild":{"kind":"missing_executable"' "first_link_rebuild_object"

  run_cb_build "${work_dir}"
  assert_link_cache_hits 1 "link_rebuild_seed_cache_hit"

  printf '%s\n' '// force object change' >> "${work_dir}/hello.c++"
  run_cb_build "${work_dir}"
  assert_link_end "/hello" false object_changed true "relink_after_object_change"
  assert_jsonl_contains '"message":"Linking' "relink_message"
  end_case link_rebuild_reason
}

test_implementation_pcm() {
  should_run implementation_pcm || return 0
  begin_case implementation_pcm
  local work_dir
  work_dir="$(prepare_work_dir)"

  printf '%s\n' \
    'export module sample;' \
    'export int sample_value();' > "${work_dir}/sample.c++m"
  printf '%s\n' \
    'module sample;' \
    'int sample_value() { return 1; }' > "${work_dir}/sample.impl.c++"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 3 "implementation_seed_cache_hits"

  printf '%s\n' '// interface changed' >> "${work_dir}/sample.c++m"
  run_cb_build "${work_dir}"
  assert_compile_end "sample.impl.c++" false pcm_stale true "implementation_rebuilt_for_interface_pcm"
  assert_jsonl_contains '"module":"sample"' "implementation_rebuild_module"
  assert_jsonl_contains '"trigger_path":' "implementation_rebuild_trigger"
  end_case implementation_pcm
}

test_rebuild_summary() {
  should_run rebuild_summary || return 0
  begin_case rebuild_summary
  local work_dir
  work_dir="$(prepare_work_dir)"

  printf '%s\n' \
    'export module sample;' \
    'export int sample_value();' > "${work_dir}/sample.c++m"
  printf '%s\n' \
    'module sample;' \
    'int sample_value() { return 1; }' > "${work_dir}/sample.impl.c++"
  printf '%s\n' \
    'import sample;' \
    'int main() { return sample_value(); }' > "${work_dir}/hello.c++"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 3 "rebuild_summary_seed_cache_hits"

  printf '%s\n' '// interface changed for summary' >> "${work_dir}/sample.c++m"
  run_cb_build "${work_dir}"
  assert_rebuild_summary source_stale 1 "" "summary_source_stale"
  assert_rebuild_summary pcm_stale 1 sample "summary_pcm_stale_top_module"
  assert_jsonl_contains '"top_modules":["sample"]' "summary_top_modules_exact"
  end_case rebuild_summary
}

test_test_lifecycle() {
  should_run test_lifecycle || return 0
  begin_case test_lifecycle
  local work_dir
  work_dir="$(prepare_work_dir)"

  printf '%s\n' \
    'import std;' \
    'int main()' \
    '{' \
    '    const auto* config = std::getenv("TESTER_CONFIG");' \
    '    const auto* parent = std::getenv("TESTER_PARENT_RUN_ID");' \
    '    return config && std::string_view{config} == "debug" && parent && *parent ? 0 : 1;' \
    '}' > "${work_dir}/test_runner.c++"

  run_cb_test "${work_dir}"
  assert_jsonl_event_count build_start 1 "single_test_build_start"
  assert_jsonl_event_count build_end 1 "single_test_build_end"
  assert_jsonl_contains '"type":"test_end"' "test_end_event"
  end_case test_lifecycle
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

test_profile_change() {
  should_run profile_change || return 0
  begin_case profile_change
  local work_dir first_jsonl
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}"
  first_jsonl="${LAST_JSONL}"
  assert_text_contains "${first_jsonl}" '"cache_hit":false' "seed_build"

  run_cb_build "${work_dir}" --compile-flags -DCB_SMOKE_FLAG=1
  assert_jsonl_contains '"type":"profile_changed"' "profile_changed_event"
  assert_jsonl_contains '"reason":"profile_change"' "profile_changed_reason"
  assert_jsonl_contains '"profile_diff"' "profile_diff_present"
  assert_jsonl_contains '"compile"' "profile_diff_compile_field"
  assert_jsonl_contains 'DCB_SMOKE_FLAG=1' "profile_diff_added_flag"
  assert_jsonl_contains '"rebuild_reason":"profile_change"' "compile_end_profile_change"
  assert_compile_end_has_no_profile_diff
  assert_jsonl_contains '"cache_hit":false' "recompile_after_profile_change"
  end_case profile_change
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
  assert_jsonl_contains 'format=cb-object-cache-v3' "cache_status_current_profile"
  end_case cache_status
}

test_jsonl_modes() {
  should_run jsonl_modes || return 0
  begin_case jsonl_modes
  local work_dir
  work_dir="$(prepare_work_dir)"

  run_cb_build "${work_dir}" --jsonl=failures
  assert_jsonl_event_count build_start 1 "failures_build_start"
  assert_jsonl_event_count build_end 1 "failures_build_end"
  assert_jsonl_not_contains '"type":"command_start"' "failures_no_command_start"
  assert_jsonl_not_contains '"type":"compile_end"' "failures_no_successful_compile"
  assert_jsonl_contains '"compile_rebuilt":1' "failures_rollup_rebuilt"

  run_cb_build "${work_dir}" --jsonl=summary
  assert_jsonl_event_count build_start 1 "summary_build_start"
  assert_jsonl_event_count build_end 1 "summary_build_end"
  assert_jsonl_not_contains '"type":"command_' "summary_no_commands"
  assert_jsonl_not_contains '"type":"compile_' "summary_no_compiles"
  assert_jsonl_contains '"compile_cache_hits":1' "summary_rollup_cache_hit"
  end_case jsonl_modes
}

test_jsonl_failure_mode() {
  should_run jsonl_failure_mode || return 0
  begin_case jsonl_failure_mode
  local work_dir
  work_dir="$(prepare_work_dir)"
  printf '%s\n' 'int broken( {' > "${work_dir}/broken.c++"

  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_build "${work_dir}" --jsonl=failures; then
    fail "broken source unexpectedly compiled in failures mode"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"jsonl_failure_mode_exit"}'
  fi
  assert_jsonl_event_value command_end ok false "failures_command_end"
  assert_jsonl_not_contains '"cmd":' "failures_argv_without_cmd"
  assert_compile_end "broken.c++" false not_in_cache false "failures_compile_end"
  assert_jsonl_contains '"rebuild":{"kind":"not_in_cache"' "failures_compile_rebuild_object"
  assert_jsonl_event_value build_end ok false "failures_build_end_status"
  end_case jsonl_failure_mode
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
  test_source_stale
  test_source_list
  test_compile_failure
  test_link_failure
  test_test_link_failure
  test_link_rebuild_reason
  test_implementation_pcm
  test_rebuild_summary
  test_test_lifecycle
  test_cache_invalidate
  test_profile_change
  test_cache_status
  test_jsonl_modes
  test_jsonl_failure_mode

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