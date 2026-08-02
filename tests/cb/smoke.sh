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
      # Read from the cases themselves; the hand-written list had already gone stale.
      echo "cases: $(grep -oE '^  should_run [a-z_]+' "${BASH_SOURCE[0]}" | awk '{print $2}' | paste -sd, - | sed 's/,/, /g')"
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
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  run_cb_build "${work_dir}"
  first_jsonl="${LAST_JSONL}"
  assert_text_contains "${first_jsonl}" '"cache_hit":false' "first_build_compiles"

  run_cb_build "${work_dir}"
  assert_compile_cache_hits 2 "second_build_cache_hit"
  assert_jsonl_not_contains '"rebuild_reason":"profile_change"' "no_profile_change"
  end_case cache_hit
}

test_link_cache_hit() {
  should_run link_cache_hit || return 0
  begin_case link_cache_hit
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  run_cb_build "${work_dir}"
  assert_jsonl_contains '"type":"link_end"' "link_end_event"
  assert_jsonl_contains '"cache_hit":false' "first_link"
  assert_jsonl_contains '"signature":"' "first_link_signature"

  run_cb_build "${work_dir}"
  assert_link_cache_hits 1 "second_build_link_cache_hit"
  assert_link_end "/hello" true "" true "skipped_link_cache_hit"
  assert_jsonl_contains '"signature":"' "skipped_link_signature"
  end_case link_cache_hit
}

test_clean_tests() {
  should_run clean_tests || return 0
  begin_case clean_tests
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  printf '%s\n' 'int smoke_test_marker = 1;' > "${work_dir}/smoke.test.c++"

  run_cb_build "${work_dir}"
  assert_jsonl_event_value build_end ok true "clean_tests_seed_build"

  local test_obj="${work_dir}/${BUILD_DIR}/obj/smoke.test.o"
  local hello_obj="${work_dir}/${BUILD_DIR}/obj/hello.o"
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ -f "${test_obj}" && -f "${hello_obj}" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"clean_tests_seed_objects"}'
  else
    fail "expected ${test_obj} and ${hello_obj} after seed build"
  fi

  run_cb_clean "${work_dir}" --tests
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ ! -f "${test_obj}" && -f "${hello_obj}" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"clean_tests_removed_only_test_obj"}'
  else
    fail "clean --tests should remove ${test_obj} and keep ${hello_obj}"
  fi

  run_cb_build "${work_dir}"
  assert_compile_end "smoke.test.c++" false not_in_cache true "clean_tests_recompiles_test"
  assert_compile_end "hello.c++" true "" true "clean_tests_keeps_hello_cache_hit"
  end_case clean_tests
}

test_parallel_main_link() {
  should_run parallel_main_link || return 0
  begin_case parallel_main_link
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # Two mains exercise parallel link_executables workers; decisions must be
  # snapshotted before any thread mutates the in-memory link cache.
  printf '%s\n' \
    'import std;' \
    'int main() { std::puts("cb-smoke-world"); return 0; }' > "${work_dir}/world.c++"

  run_cb_build "${work_dir}"
  assert_jsonl_event_value build_end ok true "parallel_main_build_ok"
  assert_jsonl_event_count link_end 2 "parallel_main_two_links"

  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ -x "${work_dir}/${BUILD_DIR}/bin/hello" && -x "${work_dir}/${BUILD_DIR}/bin/world" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"parallel_main_binaries"}'
  else
    fail "expected both ${BUILD_DIR}/bin/hello and ${BUILD_DIR}/bin/world after parallel link"
  fi

  run_cb_build "${work_dir}"
  assert_link_cache_hits 2 "parallel_main_second_build_link_cache_hits"
  end_case parallel_main_link
}

test_compile_start() {
  should_run compile_start || return 0
  begin_case compile_start
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 2 "source_stale_seed_cache_hit"

  printf '%s\n' '// edited after cache seed' >> "${work_dir}/hello.c++"
  run_cb_build "${work_dir}"
  assert_compile_end "hello.c++" false source_stale true "edited_source_rebuild"
  assert_jsonl_contains '"rebuild":{"kind":"source_stale"' "edited_source_rebuild_object"
  assert_jsonl_contains '"hint":"Source mtime newer than cached compile timestamp."' "edited_source_hint"
  assert_rebuild_summary source_stale 1 "" "edited_source_summary"
  end_case source_stale
}

test_header_stale() {
  should_run header_stale || return 0
  begin_case header_stale
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # A textual #include is invisible to the module graph; only the compiler depfile
  # records it. Without -MMD tracking this edit rebuilt nothing.
  printf '%s\n' '#pragma once' 'inline int header_value() { return 1; }' > "${work_dir}/value.h++"
  printf '%s\n' '#include "value.h++"' 'int main() { return header_value() - 1; }' > "${work_dir}/hello.c++"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 2 "header_stale_seed_cache_hit"

  printf '%s\n' '// touched header' >> "${work_dir}/value.h++"
  run_cb_build "${work_dir}"
  assert_compile_end "hello.c++" false header_stale true "edited_header_rebuild"
  assert_jsonl_contains '"rebuild":{"kind":"header_stale"' "edited_header_rebuild_object"
  assert_jsonl_contains '"trigger_path":' "edited_header_trigger_path"
  assert_rebuild_summary header_stale 1 "" "edited_header_summary"

  run_cb_build "${work_dir}"
  assert_compile_cache_hits 2 "header_stale_settles_to_cache_hit"
  end_case header_stale
}

test_header_stale_relative_include() {
  should_run header_stale_relative_include || return 0
  begin_case header_stale_relative_include
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # Documented `-I include/path` is relative. Clang -MMD then records
  # `include/value.h++` rather than an absolute path; filtering that spelling
  # against absolute source_dir used to skip the header and answer the edit
  # below with a cache hit.
  mkdir -p "${work_dir}/include"
  printf '%s\n' '#pragma once' 'inline int header_value() { return 1; }' > "${work_dir}/include/value.h++"
  printf '%s\n' '#include <value.h++>' 'int main() { return header_value() - 1; }' > "${work_dir}/hello.c++"

  run_cb_build "${work_dir}" -I include
  run_cb_build "${work_dir}" -I include
  assert_compile_cache_hits 2 "relative_include_seed_cache_hit"

  printf '%s\n' '// touched header via relative -I' >> "${work_dir}/include/value.h++"
  run_cb_build "${work_dir}" -I include
  assert_compile_end "hello.c++" false header_stale true "relative_include_edited_header_rebuild"
  assert_jsonl_contains '"rebuild":{"kind":"header_stale"' "relative_include_rebuild_object"
  assert_rebuild_summary header_stale 1 "" "relative_include_summary"

  run_cb_build "${work_dir}" -I include
  assert_compile_cache_hits 2 "relative_include_settles_to_cache_hit"
  end_case header_stale_relative_include
}

test_header_missing() {
  should_run header_missing || return 0
  begin_case header_missing
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # Deleting a header the depfile still names used to skip the unstatable
  # prerequisite and answer with a cache hit — shipping a binary built against
  # content that no longer exists. Force a rebuild so clang reports the include.
  printf '%s\n' '#pragma once' 'inline int header_value() { return 1; }' > "${work_dir}/value.h++"
  printf '%s\n' '#include "value.h++"' 'int main() { return header_value() - 1; }' > "${work_dir}/hello.c++"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 2 "header_missing_seed_cache_hit"

  rm -f "${work_dir}/value.h++"
  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_build "${work_dir}"; then
    fail "deleted included header should force a rebuild that fails the compile"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"header_missing_build_fails"}'
  fi
  assert_compile_end "hello.c++" false header_missing false "deleted_header_rebuild"
  assert_jsonl_contains '"rebuild":{"kind":"header_missing"' "deleted_header_rebuild_object"
  assert_jsonl_contains '"trigger_path":' "deleted_header_trigger_path"
  assert_rebuild_summary header_missing 1 "" "deleted_header_summary"
  assert_jsonl_event_value build_end ok false "deleted_header_build_end"
  end_case header_missing
}

test_depfile_unusable() {
  should_run depfile_unusable || return 0
  begin_case depfile_unusable
  local work_dir depfile
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # The depfile is the only record of a unit's textual includes, so an unreadable one
  # cannot be read as "this unit includes no headers". It was, and the header edit below
  # was then answered with a cache hit and a stale object — the state an upgrade or a
  # wiped obj/ leaves behind, with warm objects and no `.d` beside them.
  printf '%s\n' '#pragma once' 'inline int header_value() { return 1; }' > "${work_dir}/value.h++"
  printf '%s\n' \
    'module;' \
    '#include "value.h++"' \
    'export module counter;' \
    'export int counted() { return header_value(); }' > "${work_dir}/counter.c++m"
  printf '%s\n' 'import counter;' 'int main() { return counted() - 1; }' > "${work_dir}/hello.c++"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  # A modular unit's depfile comes from its --precompile step rather than -c, so this
  # also pins that both steps leave one behind: if either did not, the new reason would
  # rebuild that unit on every single build.
  assert_compile_cache_hits 3 "depfile_unusable_seed_cache_hits"

  depfile="${work_dir}/${BUILD_DIR}/obj/counter.o.d"
  assert_file_exists "${depfile}" "depfile_written_by_precompile"

  rm -f "${depfile}"
  printf '%s\n' '// touched while the depfile was gone' >> "${work_dir}/value.h++"
  run_cb_build "${work_dir}"
  assert_compile_end "counter.c++m" false depfile_unusable true "deleted_depfile_rebuild"
  assert_jsonl_contains '"rebuild":{"kind":"depfile_unusable"' "deleted_depfile_rebuild_object"
  assert_jsonl_contains "\"trigger_path\":\"${BUILD_DIR}/obj/counter.o.d\"" "deleted_depfile_trigger_path"
  assert_rebuild_summary depfile_unusable 1 "" "deleted_depfile_summary"

  run_cb_build "${work_dir}"
  assert_compile_cache_hits 3 "depfile_unusable_settles_to_cache_hit"

  # Truncated mid-write: present, but without the `target:` every depfile opens with.
  printf '%s' 'garbage without a colon' > "${depfile}"
  run_cb_build "${work_dir}"
  assert_compile_end "counter.c++m" false depfile_unusable true "malformed_depfile_rebuild"

  run_cb_build "${work_dir}"
  assert_compile_cache_hits 3 "malformed_depfile_settles_to_cache_hit"
  end_case depfile_unusable
}

test_strict_arguments() {
  should_run strict_arguments || return 0
  begin_case strict_arguments
  local work_dir status output
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # CB used to fall off the end of its argument chain, so a mistyped flag was
  # silently ignored and the run reported success for something never requested.
  for bad in --totally-bogus --tag=whatever --jobs=0 --jobs=abc; do
    status=0
    output="$(cd "${work_dir}" && "${CB_BIN}" "${STD_CPPM}" debug build "${bad}" 2>&1)" || status=$?
    TESTS_RUN=$((TESTS_RUN + 1))
    if [[ "${status}" -eq 2 ]]; then
      jsonl_emit "{\"type\":\"smoke_assert_passed\",\"matcher\":\"rejects_${bad}\"}"
    else
      fail "expected exit 2 for ${bad}, got ${status}: ${output}"
    fi
  done

  # The --tags typo is common enough to name explicitly.
  status=0
  output="$(cd "${work_dir}" && "${CB_BIN}" "${STD_CPPM}" debug test --tag=x 2>&1)" || status=$?
  assert_text_contains "${output}" "--tags=" "tags_typo_suggestion"

  # --junit= is a test_runner forward token: CB must not reject it as unknown.
  # The fixture has no tests, so the run fails after build — only the parse matters.
  status=0
  output="$(cd "${work_dir}" && "${CB_BIN}" "${STD_CPPM}" debug test --jsonl=failures --junit="${work_dir}/report.xml" 2>&1)" || status=$?
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ "${status}" -eq 2 ]] || printf '%s' "${output}" | grep -Fq "Unknown argument: --junit="; then
    fail "CB should forward --junit= to test_runner, not reject it (status=${status})"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"forwards_junit_flag"}'
  fi

  # A valid job cap still builds.
  run_cb_build "${work_dir}" --jobs=2
  assert_jsonl_event_value build_end ok true "jobs_flag_builds"
  end_case strict_arguments
}

test_source_list() {
  should_run source_list || return 0
  begin_case source_list
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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

# list writes compile_commands.json for the active TU set so clangd can see the real argv
# builders (including -fmodule-file= after update_module_flags). One entry per source: the
# step that reads that file — --precompile for modular interfaces, -c otherwise.
test_compile_commands() {
  should_run compile_commands || return 0
  begin_case compile_commands
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  printf '%s\n' \
    'export module counter;' \
    'export int counted() { return 1; }' > "${work_dir}/counter.c++m"
  printf '%s\n' \
    'import counter;' \
    'int main() { return counted() - 1; }' > "${work_dir}/hello.c++"

  run_cb_list "${work_dir}"
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ ! -f "${work_dir}/compile_commands.json" ]]; then
    fail "list did not write compile_commands.json"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"compile_commands_exists"}'
  fi

  TESTS_RUN=$((TESTS_RUN + 1))
  if python3 - "${work_dir}" <<'PY'
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
db = json.loads((root / "compile_commands.json").read_text())
if not isinstance(db, list) or len(db) != 2:
    raise SystemExit(f"expected 2 entries, got {db!r}")
by_file = {}
for entry in db:
    for key in ("directory", "file", "arguments"):
        if key not in entry:
            raise SystemExit(f"missing {key} in {entry!r}")
    if not isinstance(entry["arguments"], list) or not entry["arguments"]:
        raise SystemExit(f"arguments must be a non-empty list: {entry!r}")
    by_file[Path(entry["file"]).name] = entry
if set(by_file) != {"counter.c++m", "hello.c++"}:
    raise SystemExit(f"unexpected files: {sorted(by_file)}")
mod = by_file["counter.c++m"]["arguments"]
src = by_file["hello.c++"]["arguments"]
if "-std=c++23" not in mod or "-std=c++23" not in src:
    raise SystemExit("missing -std=c++23")
if "--precompile" not in mod:
    raise SystemExit("modular entry must use --precompile")
if "-c" not in src:
    raise SystemExit("non-modular entry must use -c")
if not any(a.startswith("-fmodule-file=counter=") for a in src):
    raise SystemExit(f"importer missing -fmodule-file=counter=: {src}")
print("ok")
PY
  then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"compile_commands_shape"}'
  else
    fail "compile_commands.json failed shape checks"
  fi
  end_case compile_commands
}

# list also writes graph.json — the same inventory as unit / list_summary JSONL, one file for
# tools that do not want to parse the stream.
test_graph_json() {
  should_run graph_json || return 0
  begin_case graph_json
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  printf '%s\n' \
    'export module counter;' \
    'export int counted() { return 1; }' > "${work_dir}/counter.c++m"
  printf '%s\n' \
    'import counter;' \
    'int main() { return counted() - 1; }' > "${work_dir}/hello.c++"

  run_cb_list "${work_dir}"
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ ! -f "${work_dir}/graph.json" ]]; then
    fail "list did not write graph.json"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"graph_json_exists"}'
  fi

  TESTS_RUN=$((TESTS_RUN + 1))
  if python3 - "${work_dir}" <<'PY'
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
g = json.loads((root / "graph.json").read_text())
if g.get("schema") != "cb-graph" or g.get("version") != 1:
    raise SystemExit(f"bad schema/version: {g!r}")
if not isinstance(g.get("units"), list) or len(g["units"]) != 2:
    raise SystemExit(f"expected 2 units, got {g.get('units')!r}")
if g.get("units_total") != 2 or g.get("main_count") != 1:
    raise SystemExit(f"bad totals: {g!r}")
by_path = {u["path"]: u for u in g["units"]}
if set(by_path) != {"counter.c++m", "hello.c++"}:
    raise SystemExit(f"unexpected paths: {sorted(by_path)}")
mod = by_path["counter.c++m"]
src = by_path["hello.c++"]
if mod.get("module") != "counter" or not mod.get("is_modular"):
    raise SystemExit(f"bad modular unit: {mod!r}")
if "counter" not in src.get("imports", []):
    raise SystemExit(f"importer missing counter import: {src!r}")
if src.get("level", -1) < mod.get("level", -1):
    raise SystemExit(f"topo levels wrong: mod={mod.get('level')} src={src.get('level')}")
print("ok")
PY
  then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"graph_json_shape"}'
  else
    fail "graph.json failed shape checks"
  fi
  end_case graph_json
}

test_compile_failure() {
  should_run compile_failure || return 0
  begin_case compile_failure
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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
  # ok:false alone is not actionable: the compiler's own text must reach stdout.
  assert_jsonl_contains '"diagnostics":{"text":"' "failed_compile_diagnostics"
  assert_jsonl_diagnostics_contain "${work_dir}" compile_end "error:" "failed_compile_diagnostics_text"
  assert_jsonl_diagnostics_contain "${work_dir}" command_end "error:" "failed_command_diagnostics_text"
  # std::system yields a wait status; exit_code must be the child's real code.
  assert_jsonl_event_value command_end exit_code 1 "failed_command_exit_code"
  assert_jsonl_event_value command_end signaled false "failed_command_not_signaled"
  end_case compile_failure
}

# A TU that compiles with -Wall warnings used to leave the capture on disk and attach it to
# no event — ok:true meant silence. Failures mode must still emit that compile_end so the
# warning cannot accumulate invisibly between clean builds.
test_compile_warning() {
  should_run compile_warning || return 0
  begin_case compile_warning
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  printf '%s\n' 'int main() { int unused = 1; return 0; }' > "${work_dir}/hello.c++"

  run_cb_build "${work_dir}" --jsonl=trace
  assert_compile_end "hello.c++" false not_in_cache true "warning_compile_ok"
  assert_jsonl_diagnostics_contain "${work_dir}" compile_end "unused" "warning_on_compile_end"
  assert_jsonl_diagnostics_contain "${work_dir}" command_end "unused" "warning_on_command_end"
  assert_jsonl_event_value build_end ok true "warning_build_still_ok"

  rm -rf "${work_dir}/${BUILD_DIR}"
  run_cb_build "${work_dir}" --jsonl=failures
  assert_compile_end "hello.c++" false not_in_cache true "failures_mode_warning_compile"
  assert_jsonl_diagnostics_contain "${work_dir}" compile_end "unused" "failures_mode_warning_text"
  assert_jsonl_event_value compile_end ok true "failures_mode_warning_ok"
  end_case compile_warning
}

# A modular unit runs --precompile then compiles the pcm. Those steps must not share one
# capture file: the second redirect truncates, and compile_end.diagnostics.path would name
# an empty log while the warning text lived only in the in-memory head.
test_modular_compile_warning() {
  should_run modular_compile_warning || return 0
  begin_case modular_compile_warning
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  printf '%s\n' \
    'export module warn;' \
    '#warning "modular_precompile_warning"' \
    'export int value() { return 0; }' > "${work_dir}/warn.c++m"
  printf '%s\n' 'import warn;' 'int main() { return value(); }' > "${work_dir}/hello.c++"

  run_cb_build "${work_dir}" --jsonl=trace
  assert_compile_end "warn.c++m" false not_in_cache true "modular_warning_compile_ok"
  assert_jsonl_diagnostics_contain "${work_dir}" compile_end "modular_precompile_warning" \
    "modular_warning_on_compile_end"
  assert_jsonl_event_value build_end ok true "modular_warning_build_still_ok"
  end_case modular_compile_warning
}

test_link_failure() {
  should_run link_failure || return 0
  begin_case link_failure
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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
  assert_jsonl_diagnostics_contain "${work_dir}" link_end "missing" "failed_link_diagnostics_text"
  end_case link_failure
}

test_test_link_failure() {
  should_run test_link_failure || return 0
  begin_case test_link_failure
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  printf '%s\n' \
    'export module sample;' \
    'export int sample_value();' > "${work_dir}/sample.c++m"
  printf '%s\n' \
    'module sample;' \
    'int sample_value() { return 1; }' > "${work_dir}/sample.impl.c++"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 4 "implementation_seed_cache_hits"

  printf '%s\n' '// interface changed' >> "${work_dir}/sample.c++m"
  run_cb_build "${work_dir}"
  assert_compile_end "sample.impl.c++" false pcm_stale true "implementation_rebuilt_for_interface_pcm"
  assert_jsonl_contains '"module":"sample"' "implementation_rebuild_module"
  assert_jsonl_contains '"trigger_path":' "implementation_rebuild_trigger"
  end_case implementation_pcm
}

test_dotted_module_name() {
  should_run dotted_module_name || return 0
  begin_case dotted_module_name
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  printf '%s\n' \
    'export module demo.core;' \
    'export int answer() { return 42; }' > "${work_dir}/core.c++m"
  printf '%s\n' \
    'export module demo.app;' \
    'import demo.core;' \
    'int main() { return answer() == 42 ? 0 : 1; }' > "${work_dir}/app.c++m"

  # Inventory alone answers this: the graph edges and modular flags are list fields.
  run_cb_list "${work_dir}"
  assert_jsonl_contains '"module":"demo.core"' "dotted_core_module"
  assert_jsonl_contains '"module":"demo.app"' "dotted_app_module"
  assert_jsonl_contains '"is_modular":true' "dotted_is_modular"
  assert_jsonl_contains '"imports":["demo.core"]' "dotted_import_edge"
  end_case dotted_module_name
}

test_gmf_preamble() {
  should_run gmf_preamble || return 0
  begin_case gmf_preamble
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  printf '%s\n' \
    'module;' \
    'extern "C" {' \
    'typedef int gmf_t;' \
    '}' \
    'export module gmf_demo;' \
    'export int g() { return 1; }' > "${work_dir}/gmf.c++m"
  printf '%s\n' \
    'import gmf_demo;' \
    'int main() { return g() == 1 ? 0 : 1; }' > "${work_dir}/main.c++"

  # The global-module-fragment preamble is a scan question; list sees the named module.
  run_cb_list "${work_dir}"
  assert_jsonl_contains '"module":"gmf_demo"' "gmf_named_module"
  assert_jsonl_contains '"kind":"interface"' "gmf_interface_kind"
  assert_jsonl_contains '"is_modular":true' "gmf_is_modular"
  end_case gmf_preamble
}

test_import_trailing_comment() {
  should_run import_trailing_comment || return 0
  begin_case import_trailing_comment
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # Trailing // comments that mention preamble-ending keywords (class/struct/...)
  # used to set seen_real_code before the import on that line was recorded, and
  # then also drop later imports. That erased the module graph edge so interface
  # edits no longer rebuilt importers (stale binaries).
  printf '%s\n' \
    'export module sample;' \
    'export int sample_value();' > "${work_dir}/sample.c++m"
  printf '%s\n' \
    'export module helpers;' \
    'export int helper_value() { return 2; }' > "${work_dir}/helpers.c++m"
  printf '%s\n' \
    'import sample; // class helpers for sample_value' \
    'import helpers; // struct bridge' \
    'int main() { return sample_value() + helper_value(); }' > "${work_dir}/main.c++"
  printf '%s\n' \
    'module sample;' \
    'int sample_value() { return 1; }' > "${work_dir}/sample.impl.c++"

  run_cb_list "${work_dir}"
  assert_jsonl_contains '"path":"main.c++"' "import_comment_main_listed"
  assert_jsonl_contains '"imports":["sample","helpers"]' "import_comment_keeps_both_edges"

  run_cb_build "${work_dir}"
  assert_jsonl_event_value build_end ok true "import_comment_build_ok"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 5 "import_comment_seed_cache_hits"

  printf '%s\n' '// interface changed' >> "${work_dir}/sample.c++m"
  run_cb_build "${work_dir}"
  assert_compile_end "main.c++" false pcm_stale true "import_comment_importer_rebuilt"
  assert_jsonl_contains '"module":"sample"' "import_comment_rebuild_module"
  end_case import_trailing_comment
}

test_commented_out_imports() {
  should_run commented_out_imports || return 0
  begin_case commented_out_imports
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # The scanner matched module regexes against raw lines, so text that is not code
  # became real edges: an import inside a block comment, inside `#if 0`, or inside a
  # string literal. Comments and literals can span lines, and `#if 0` bodies do not
  # start with `#`, so none of them are visible one line at a time.
  printf '%s\n' \
    'export module scanned;' \
    '/* import phantom_block;' \
    '   import phantom_second; */' \
    '#if 0' \
    'import phantom_dead;' \
    '#if 1' \
    'import phantom_nested;' \
    '#endif' \
    '#endif' \
    'import helpers; // struct bridge must survive' \
    'export int scanned_value() { return helper_value(); }' > "${work_dir}/scanned.c++m"
  printf '%s\n' \
    'export module helpers;' \
    'export int helper_value() { return 2; }' > "${work_dir}/helpers.c++m"
  # Separate unit: a literal can only be mistaken for an import while the preamble is
  # still being scanned, and a declaration may not precede an import in one preamble.
  printf '%s\n' \
    'export module literal_scan;' \
    'export const char* text = "import phantom_literal; /* not a comment";' > "${work_dir}/literal_scan.c++m"
  printf '%s\n' \
    'import scanned;' \
    'int main() { return scanned_value() - 2; }' > "${work_dir}/main.c++"

  run_cb_list "${work_dir}"
  assert_jsonl_contains '"imports":["helpers"]' "commented_imports_only_real_edge"
  assert_jsonl_not_contains 'phantom_block' "commented_imports_no_block_comment_edge"
  assert_jsonl_not_contains 'phantom_dead' "commented_imports_no_if_zero_edge"
  assert_jsonl_not_contains 'phantom_nested' "commented_imports_no_nested_if_zero_edge"
  assert_jsonl_not_contains 'phantom_literal' "commented_imports_no_string_literal_edge"
  end_case commented_out_imports
}

test_spliced_and_raw_literals() {
  should_run spliced_and_raw_literals || return 0
  begin_case spliced_and_raw_literals
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # Three constructs reach past the end of their line, and the cleaner stopped at the
  # newline: a backslash-newline splice continues a string literal and a `//` comment
  # alike (phase 2 runs before either is recognised — clang warns `-Wcomment` on the
  # second), and a raw string has no escapes at all, so only `)delimiter"` ends it. Each
  # one carried an `import` that is not code into the module graph, where a phantom edge
  # can close a loop through a real one and abort a valid build.
  printf '%s\n' \
    'export module scanned;' \
    'import helpers;' \
    '// a line comment continued with a backslash \' \
    'import phantom_comment;' \
    'export const char* spliced = "text \' \
    'import phantom_spliced; more";' \
    'export const char* raw = R"(' \
    'import phantom_raw;' \
    ')";' \
    'export const char* tagged = R"tag(' \
    'import phantom_tagged;' \
    ')tag";' \
    'export const char* url = "http://example.com";' \
    'export int scanned_value() { return helper_value(); }' > "${work_dir}/scanned.c++m"
  printf '%s\n' \
    'export module helpers;' \
    'export int helper_value() { return 2; }' > "${work_dir}/helpers.c++m"
  printf '%s\n' \
    'import scanned;' \
    'int main() { return scanned_value() - 2; }' > "${work_dir}/main.c++"

  run_cb_list "${work_dir}"
  assert_jsonl_contains '"imports":["helpers"]' "spliced_literals_only_real_edge"
  assert_jsonl_not_contains 'phantom_comment' "spliced_literals_no_spliced_comment_edge"
  assert_jsonl_not_contains 'phantom_spliced' "spliced_literals_no_spliced_string_edge"
  assert_jsonl_not_contains 'phantom_raw' "spliced_literals_no_raw_string_edge"
  assert_jsonl_not_contains 'phantom_tagged' "spliced_literals_no_delimited_raw_string_edge"
  end_case spliced_and_raw_literals
}

test_spliced_directives() {
  should_run spliced_directives || return 0
  begin_case spliced_directives
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # Phase 2 joins a continued line before the compiler recognises anything on it, and the
  # scanner read physical lines: `#if \` / `0` was an unrecognised directive rather than a
  # dead region, so its body stayed live and could point a phantom edge at a module that
  # exists — closing a loop through the real edge and aborting a valid project with
  # `Cyclic dependency detected`. The same split hides a real `import \` / `helpers;` from
  # the graph, which is worse: a missing edge silently builds in the wrong order.
  printf '%s\n' \
    'export module cycle_a;' \
    '#if \' \
    '0' \
    'import cycle_b;' \
    '#endif' \
    '#if 0 \' \
    '&& OLD_FEATURE' \
    'import phantom_spliced_condition;' \
    '#endif' \
    '#if \   ' \
    '0' \
    'import phantom_trailing_space;' \
    '#en\' \
    'dif' \
    'import \' \
    'helpers;' \
    'export int a_value() { return helper_value() + 1; }' > "${work_dir}/cycle_a.c++m"
  printf '%s\n' \
    'export module cycle_b;' \
    'import cycle_a;' \
    'export int b_value() { return a_value() + 1; }' > "${work_dir}/cycle_b.c++m"
  printf '%s\n' \
    'export module helpers;' \
    'export int helper_value() { return 2; }' > "${work_dir}/helpers.c++m"
  printf '%s\n' \
    'import cycle_b;' \
    'int main() { return b_value() - 4; }' > "${work_dir}/main.c++"

  # Checked rather than assumed: without splicing, list aborts on the false cycle, and a bare
  # run_cb_list would take the whole harness down with it and report nothing.
  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_list "${work_dir}"; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"spliced_directives_list_ok"}'
  else
    fail "list failed on a project clang accepts: a spliced directive invented an edge"
  fi
  assert_jsonl_contains '"module":"cycle_a","kind":"interface","imports":["helpers"]' "spliced_directives_only_real_edge"
  assert_jsonl_not_contains 'Cyclic dependency' "spliced_directives_no_false_cycle"
  assert_jsonl_not_contains 'phantom_spliced_condition' "spliced_directives_no_spliced_condition_edge"
  assert_jsonl_not_contains 'phantom_trailing_space' "spliced_directives_no_trailing_space_edge"
  end_case spliced_directives
}

test_mentioned_raw_opener() {
  should_run mentioned_raw_opener || return 0
  begin_case mentioned_raw_opener
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # An `R"(` in a comment or a literal is text, not a raw-string opener. Deciding otherwise is
  # what honouring [lex.pptoken]'s reversion costs a regex scanner, and it cost both bugs the
  # reversion was meant to prevent: a mention with no closer stopped every later splice to the
  # end of the file, so a dead `#if \` body came back as a phantom edge — a cycle through a
  # module that exists — and a real `import \` stayed split, losing an edge without a word.
  # Phase 2 is one substitution, so a mention cannot reach the graph from any of the three.
  printf '%s\n' \
    'export module cycle_a;' \
    '// spelling note: a raw string starts with R"(' \
    'import \' \
    'helpers;' \
    '#if \' \
    '0' \
    'import cycle_b;' \
    '#endif' \
    '/* mentioned again in a block comment: R"( */' \
    'export const char* mentions = "R\"( in a literal";' \
    'export int a_value() { return helper_value() + 1; }' > "${work_dir}/cycle_a.c++m"
  printf '%s\n' \
    'export module cycle_b;' \
    'import cycle_a;' \
    'export int b_value() { return a_value() + 1; }' > "${work_dir}/cycle_b.c++m"
  printf '%s\n' \
    'export module helpers;' \
    'export int helper_value() { return 2; }' > "${work_dir}/helpers.c++m"
  printf '%s\n' \
    'import cycle_a;' \
    'int main() { return a_value() - 3; }' > "${work_dir}/main.c++"

  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_list "${work_dir}"; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"mentioned_raw_opener_list_ok"}'
  else
    fail "list failed on a project clang accepts: a mentioned R\"( froze the splice pass"
  fi
  assert_jsonl_contains '"module":"cycle_a","kind":"interface","imports":["helpers"]' "mentioned_raw_opener_keeps_real_edge"
  assert_jsonl_not_contains 'Cyclic dependency' "mentioned_raw_opener_no_false_cycle"
  assert_jsonl_not_contains 'cycle_b"]' "mentioned_raw_opener_no_dead_body_edge"
  end_case mentioned_raw_opener
}

test_dead_conditional_arms() {
  should_run dead_conditional_arms || return 0
  begin_case dead_conditional_arms
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # `#if 0` has more spellings than the bare constant, and the scanner recognised only
  # the bare one: parentheses hid the constant, a short-circuited operand made the
  # directive match nothing at all — leaving its body live and the `#endif` depth off by
  # one — and every `#elif` revived the region, dead condition or not. Each one leaks a
  # phantom edge, and the whole point is that dead branches are where you park the import
  # you no longer use.
  printf '%s\n' \
    'export module scanned;' \
    '#if (0)' \
    'import phantom_paren;' \
    '#endif' \
    '#if ((false))' \
    'import phantom_double_paren;' \
    '#endif' \
    '#if 0 && OLD_FEATURE' \
    'import phantom_and;' \
    '#endif' \
    '#if false && ALSO_OLD' \
    'import phantom_false_and;' \
    '#endif' \
    '#if 0&&TIGHT' \
    'import phantom_tight;' \
    '#endif' \
    '#if 0' \
    'import phantom_first_arm;' \
    '#elif 0' \
    'import phantom_elif;' \
    '#endif' \
    '#if 1' \
    'import helpers;' \
    '#elif 0' \
    'import phantom_elif_after_live;' \
    '#endif' \
    '#if 0' \
    'import phantom_before_live_arm;' \
    '#elif 1' \
    'import extras;' \
    '#endif' \
    'export int scanned_value() { return helper_value() + extra_value(); }' > "${work_dir}/scanned.c++m"
  printf '%s\n' \
    'export module helpers;' \
    'export int helper_value() { return 2; }' > "${work_dir}/helpers.c++m"
  printf '%s\n' \
    'export module extras;' \
    'export int extra_value() { return 3; }' > "${work_dir}/extras.c++m"
  printf '%s\n' \
    'import scanned;' \
    'int main() { return scanned_value() - 5; }' > "${work_dir}/main.c++"

  run_cb_list "${work_dir}"
  # Both live imports sit in taken arms of the same conditionals that hold the phantoms,
  # so they pin the other half of the contract: elide the dead arm, keep the live one.
  assert_jsonl_contains '"imports":["helpers","extras"]' "dead_arms_live_edges_kept"
  assert_jsonl_not_contains 'phantom_paren' "dead_arms_no_paren_edge"
  assert_jsonl_not_contains 'phantom_double_paren' "dead_arms_no_double_paren_edge"
  assert_jsonl_not_contains 'phantom_and' "dead_arms_no_short_circuit_edge"
  assert_jsonl_not_contains 'phantom_false_and' "dead_arms_no_false_short_circuit_edge"
  assert_jsonl_not_contains 'phantom_tight' "dead_arms_no_unspaced_short_circuit_edge"
  assert_jsonl_not_contains 'phantom_first_arm' "dead_arms_no_first_arm_edge"
  assert_jsonl_not_contains 'phantom_elif' "dead_arms_no_dead_elif_edge"
  assert_jsonl_not_contains 'phantom_elif_after_live' "dead_arms_no_dead_elif_after_live_edge"
  assert_jsonl_not_contains 'phantom_before_live_arm' "dead_arms_no_arm_before_live_elif_edge"
  end_case dead_conditional_arms
}

test_commented_import_no_false_cycle() {
  should_run commented_import_no_false_cycle || return 0
  begin_case commented_import_no_false_cycle
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # A phantom edge from a commented-out import is not merely spurious: pointing it at
  # a module that really exists closes a loop through the real edge, and list dies with
  # a cyclic-dependency error naming modules the source never connected. Commenting out
  # an import you used to have is the ordinary way to hit this.
  printf '%s\n' \
    'export module cycle_a;' \
    '/* import cycle_b; */' \
    'export int a_value() { return 1; }' > "${work_dir}/cycle_a.c++m"
  printf '%s\n' \
    'export module cycle_b;' \
    'import cycle_a;' \
    'export int b_value() { return a_value() + 1; }' > "${work_dir}/cycle_b.c++m"
  printf '%s\n' \
    'import cycle_b;' \
    'int main() { return b_value() == 2 ? 0 : 1; }' > "${work_dir}/main.c++"

  run_cb_build "${work_dir}"
  assert_jsonl_event_value build_end ok true "false_cycle_build_ok"
  assert_jsonl_not_contains 'Cyclic dependency' "false_cycle_no_cycle_error"
  end_case commented_import_no_false_cycle
}

test_module_safe_name() {
  should_run module_safe_name || return 0
  begin_case module_safe_name
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # Partition demo:part and flat module demo_part must keep distinct artifacts.
  # Convention: ':' / '.' fold to '-', while '_' stays literal.
  printf '%s\n' \
    'export module demo:part;' \
    'export int part_value() { return 1; }' > "${work_dir}/demo-part.c++m"
  printf '%s\n' \
    'export module demo_part;' \
    'export int flat_value() { return 2; }' > "${work_dir}/demo_part.c++m"
  printf '%s\n' \
    'export module demo.app;' \
    'export int app_value() { return 3; }' > "${work_dir}/demo-app.c++m"

  run_cb_build "${work_dir}"
  assert_jsonl_event_value build_end ok true "module_safe_name_build_ok"
  assert_jsonl_contains '-fmodule-file=demo:part=' "module_flag_partition"
  assert_jsonl_contains '-fmodule-file=demo_part=' "module_flag_flat"
  assert_jsonl_contains 'demo-part.pcm' "partition_pcm_hyphen"
  assert_jsonl_contains 'demo_part.pcm' "flat_pcm_underscore"
  assert_jsonl_contains 'demo-app.pcm' "dotted_pcm_hyphen"
  assert_jsonl_not_contains "-fmodule-file=demo:part=${BUILD_DIR}/pcm/demo_part.pcm" "partition_not_collapsed"

  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ -f "${work_dir}/${BUILD_DIR}/pcm/demo-part.pcm" && -f "${work_dir}/${BUILD_DIR}/pcm/demo_part.pcm" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"distinct_pcm_files"}'
  else
    fail "expected distinct pcm files for demo:part and demo_part under ${BUILD_DIR}/pcm"
  fi
  end_case module_safe_name
}

test_same_basename_collision() {
  should_run same_basename_collision || return 0
  begin_case same_basename_collision
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # Same basename in different directories is unsupported: names must stay unique.
  # scan_and_order refuses before any compile, so list is enough.
  mkdir -p "${work_dir}/left" "${work_dir}/right"
  printf '%s\n' 'int marker_left() { return 1; }' > "${work_dir}/left/util.c++"
  printf '%s\n' 'int marker_right() { return 2; }' > "${work_dir}/right/util.c++"

  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_list "${work_dir}"; then
    fail "same-basename sources in different directories should fail fast"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"same_basename_collision_exit"}'
  fi
  assert_jsonl_contains '"type":"cb_error"' "same_basename_cb_error"
  assert_jsonl_contains 'Duplicate translation unit key' "same_basename_duplicate_key"
  assert_jsonl_contains 'object/module names must stay unique' "same_basename_uniqueness_hint"
  end_case same_basename_collision
}

test_reserved_std_collision() {
  should_run reserved_std_collision || return 0
  begin_case reserved_std_collision
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # Project sources named std map onto CB's reserved libc++ std.pcm / std.o paths.
  # The reservation is checked during scan_and_order, so list refuses without compiling.
  printf '%s\n' 'int project_std() { return 1; }' > "${work_dir}/std.c++"

  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_list "${work_dir}"; then
    fail "project source std.c++ should fail fast instead of overwriting reserved std.o"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"reserved_std_collision_exit"}'
  fi
  assert_jsonl_contains '"type":"cb_error"' "reserved_std_cb_error"
  assert_jsonl_contains 'Duplicate object path' "reserved_std_duplicate_object"
  assert_jsonl_contains 'reserved std module object' "reserved_std_object_label"
  assert_jsonl_contains 'object/module names must stay unique' "reserved_std_uniqueness_hint"
  end_case reserved_std_collision
}

test_nested_deps_skipped() {
  should_run nested_deps_skipped || return 0
  begin_case nested_deps_skipped
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # Nested package checkouts (deps/<pkg>/deps/...) must not join the parent scan,
  # or vendored tester smoke fixtures collide across packages.
  mkdir -p "${work_dir}/deps/net/deps/tester/tests/cb/fixture" \
           "${work_dir}/deps/cryptic/deps/tester/tests/cb/fixture"
  printf '%s\n' 'int nested_net() { return 1; }' > "${work_dir}/deps/net/deps/tester/tests/cb/fixture/hello.c++"
  printf '%s\n' 'int nested_cryptic() { return 2; }' > "${work_dir}/deps/cryptic/deps/tester/tests/cb/fixture/hello.c++"

  run_cb_list "${work_dir}"
  assert_jsonl_event_count unit 1 "nested_deps_single_unit"
  assert_jsonl_event_value unit path hello.c++ "nested_deps_root_hello"
  assert_jsonl_not_contains 'deps/net/deps/' "nested_deps_net_absent"
  assert_jsonl_not_contains 'deps/cryptic/deps/' "nested_deps_cryptic_absent"
  end_case nested_deps_skipped
}

test_vendored_tester_tests_skipped() {
  should_run vendored_tester_tests_skipped || return 0
  begin_case vendored_tester_tests_skipped
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # First-level deps/tester/tests fixtures are still under tester/, so is_test stays
  # false; skipping only nested deps/*/deps is not enough for parent repos.
  mkdir -p "${work_dir}/deps/tester/tests/cb/fixture" \
           "${work_dir}/deps/tester/tester"
  printf '%s\n' 'int vendored_fixture() { return 1; }' > "${work_dir}/deps/tester/tests/cb/fixture/hello.c++"
  printf '%s\n' 'int framework_lib() { return 2; }' > "${work_dir}/deps/tester/tester/lib.c++"

  run_cb_list "${work_dir}"
  assert_jsonl_event_count unit 2 "vendored_tester_unit_count"
  assert_jsonl_contains '"path":"hello.c++"' "vendored_tester_root_hello"
  assert_jsonl_contains '"path":"deps/tester/tester/lib.c++"' "vendored_tester_framework_lib"
  assert_jsonl_not_contains 'deps/tester/tests/' "vendored_tester_tests_absent"
  end_case vendored_tester_tests_skipped
}

test_build_output_trees_skipped() {
  should_run build_output_trees_skipped || return 0
  begin_case build_output_trees_skipped
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # CB / Make / CMake all use top-level build-* directories. CMake drops
  # CMakeCXXCompilerId.cpp there; two configure trees would collide if scanned.
  mkdir -p "${work_dir}/build-cmake-darwin-debug/CMakeFiles/4.1.2/CompilerIdCXX" \
           "${work_dir}/build-cmake-darwin-release/CMakeFiles/4.1.2/CompilerIdCXX" \
           "${work_dir}/build-make-darwin-debug/obj" \
           "${work_dir}/build-darwin-debug/obj"
  printf '%s\n' 'int cmake_debug() { return 1; }' \
    > "${work_dir}/build-cmake-darwin-debug/CMakeFiles/4.1.2/CompilerIdCXX/CMakeCXXCompilerId.cpp"
  printf '%s\n' 'int cmake_release() { return 2; }' \
    > "${work_dir}/build-cmake-darwin-release/CMakeFiles/4.1.2/CompilerIdCXX/CMakeCXXCompilerId.cpp"
  printf '%s\n' 'int make_obj() { return 3; }' > "${work_dir}/build-make-darwin-debug/obj/stale.c++"
  printf '%s\n' 'int cb_obj() { return 4; }' > "${work_dir}/build-darwin-debug/obj/stale.c++"

  run_cb_list "${work_dir}"
  assert_jsonl_event_count unit 1 "build_output_single_unit"
  assert_jsonl_event_value unit path hello.c++ "build_output_root_hello"
  assert_jsonl_not_contains 'build-cmake-' "build_output_cmake_absent"
  assert_jsonl_not_contains 'build-make-' "build_output_make_absent"
  assert_jsonl_not_contains 'build-darwin-' "build_output_cb_absent"
  end_case build_output_trees_skipped
}

test_project_test_dir_included() {
  should_run project_test_dir_included || return 0
  begin_case project_test_dir_included
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # Conventional project test/ trees must join debug scans (include_tests=true).
  # A hard-skip of test/ previously dropped them before is_test/include_tests ran.
  mkdir -p "${work_dir}/test" "${work_dir}/src/test" "${work_dir}/tests" \
           "${work_dir}/deps/tester/test/fixture"
  printf '%s\n' 'int singular_test() { return 1; }' > "${work_dir}/test/widget.test.c++"
  printf '%s\n' 'int nested_test() { return 2; }' > "${work_dir}/src/test/nested.test.c++"
  printf '%s\n' 'int plural_test() { return 3; }' > "${work_dir}/tests/plural.test.c++"
  printf '%s\n' 'int vendored_singular() { return 4; }' > "${work_dir}/deps/tester/test/fixture/hello.c++"

  run_cb_list "${work_dir}"
  assert_jsonl_event_count unit 4 "project_test_dir_unit_count"
  assert_jsonl_contains '"path":"test/widget.test.c++"' "project_test_singular_path"
  assert_jsonl_contains '"path":"src/test/nested.test.c++"' "project_test_nested_path"
  assert_jsonl_contains '"path":"tests/plural.test.c++"' "project_test_plural_path"
  assert_jsonl_contains '"path":"test/widget.test.c++","kind":"non_module","imports":[],"level":0,"has_main":false,"is_test":true' "project_test_singular_is_test"
  assert_jsonl_contains '"path":"src/test/nested.test.c++","kind":"non_module","imports":[],"level":0,"has_main":false,"is_test":true' "project_test_nested_is_test"
  assert_jsonl_event_value list_summary test_count 3 "project_test_dir_test_count"
  assert_jsonl_not_contains 'deps/tester/test/' "project_test_vendored_tester_absent"

  # Release keeps include_tests=false, so project test trees stay out of the inventory.
  local status=0
  LAST_JSONL="$(
    cd "${work_dir}"
    "${CB_BIN}" "${STD_CPPM}" release list --jsonl=trace 2>/dev/null
  )" || status=$?
  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ "${status}" -ne 0 ]]; then
    fail "release list exited ${status}"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"project_test_release_list_exit"}'
  fi
  assert_jsonl_event_count unit 1 "project_test_release_unit_count"
  assert_jsonl_event_value unit path hello.c++ "project_test_release_root_only"
  assert_jsonl_not_contains '"path":"test/widget.test.c++"' "project_test_release_singular_absent"
  assert_jsonl_not_contains '"path":"tests/plural.test.c++"' "project_test_release_plural_absent"
  end_case project_test_dir_included
}

test_deps_package_tests_skipped() {
  should_run deps_package_tests_skipped || return 0
  begin_case deps_package_tests_skipped
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # Project test/ must still join, but first-level deps/<pkg>/test and
  # deps/<pkg>/tests belong to the vendored package and must not.
  mkdir -p "${work_dir}/test" \
           "${work_dir}/deps/xson/test/fixture" \
           "${work_dir}/deps/cryptic/tests" \
           "${work_dir}/deps/xson"
  printf '%s\n' 'int project_test() { return 1; }' > "${work_dir}/test/widget.test.c++"
  printf '%s\n' 'int xson_lib() { return 2; }' > "${work_dir}/deps/xson/lib.c++"
  printf '%s\n' 'int xson_pkg_test() { return 3; }' > "${work_dir}/deps/xson/pkg.test.c++"
  printf '%s\n' 'int xson_dir_test() { return 4; }' > "${work_dir}/deps/xson/test/fixture/suite.test.c++"
  printf '%s\n' 'int cryptic_bench() { return 5; }' > "${work_dir}/deps/cryptic/tests/benchmark.c++"

  run_cb_list "${work_dir}"
  assert_jsonl_contains '"path":"test/widget.test.c++"' "deps_pkg_tests_project_test_present"
  assert_jsonl_contains '"path":"deps/xson/lib.c++"' "deps_pkg_tests_lib_present"
  assert_jsonl_contains '"path":"deps/xson/pkg.test.c++"' "deps_pkg_tests_colocated_present"
  assert_jsonl_not_contains 'deps/xson/test/' "deps_pkg_tests_xson_test_absent"
  assert_jsonl_not_contains 'deps/cryptic/tests/' "deps_pkg_tests_cryptic_tests_absent"
  end_case deps_package_tests_skipped
}

test_rebuild_summary() {
  should_run rebuild_summary || return 0
  begin_case rebuild_summary
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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
  assert_compile_cache_hits 4 "rebuild_summary_seed_cache_hits"

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
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

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

test_test_runner_exact_name() {
  should_run test_runner_exact_name || return 0
  begin_case test_runner_exact_name
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # Seed a fresh bin/test_runner that passes.
  printf '%s\n' 'int main() { return 0; }' > "${work_dir}/test_runner.c++"
  run_cb_test "${work_dir}"
  assert_jsonl_event_value test_end ok true "seed_test_runner_ok"

  # Real runner now fails. A substring impostor (aaa_test_runner / contest_runner)
  # used to steal link_test_runner while run_tests still executed the stale
  # bin/test_runner — a silent CI pass.
  printf '%s\n' 'int main() { return 1; }' > "${work_dir}/test_runner.c++"
  printf '%s\n' 'int main() { return 0; }' > "${work_dir}/aaa_test_runner.c++"
  printf '%s\n' 'int main() { return 0; }' > "${work_dir}/contest_runner.c++"

  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_test "${work_dir}"; then
    fail "stale or impostor test_runner executed (expected failing exact test_runner)"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"exact_test_runner_executed"}'
  fi

  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ -x "${work_dir}/${BUILD_DIR}/bin/test_runner" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"canonical_test_runner_present"}'
  else
    fail "expected bin/test_runner"
  fi

  TESTS_RUN=$((TESTS_RUN + 1))
  if [[ -x "${work_dir}/${BUILD_DIR}/bin/aaa_test_runner" && -x "${work_dir}/${BUILD_DIR}/bin/contest_runner" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"impostor_mains_linked_normally"}'
  else
    fail "expected substring impostors linked as ordinary mains"
  fi
  end_case test_runner_exact_name
}

test_cache_invalidate() {
  should_run cache_invalidate || return 0
  begin_case cache_invalidate
  local work_dir cache_file
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  run_cb_build "${work_dir}"
  run_cb_build "${work_dir}"
  assert_compile_cache_hits 2 "seed_cache_hit"

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
  local work_dir first_jsonl std_pcm std_profile before_pcm_mtime after_pcm_mtime
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  std_pcm="${work_dir}/${BUILD_DIR}/pcm/std.pcm"
  std_profile="${work_dir}/${BUILD_DIR}/cache/std-module-profile.txt"

  run_cb_build "${work_dir}"
  first_jsonl="${LAST_JSONL}"
  assert_text_contains "${first_jsonl}" '"cache_hit":false' "seed_build"
  if [[ -f "${std_pcm}" && -f "${std_profile}" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"std_module_profile_written"}'
  else
    fail "std.pcm and std-module-profile.txt should exist after seed build"
  fi
  before_pcm_mtime="$(stat -c %Y "${std_pcm}" 2>/dev/null || stat -f %m "${std_pcm}")"

  # Ensure mtime granularity cannot hide a rebuild on fast filesystems.
  sleep 1
  run_cb_build "${work_dir}" --compile-flags -DCB_SMOKE_FLAG=1
  assert_jsonl_contains '"type":"profile_changed"' "profile_changed_event"
  assert_jsonl_contains '"reason":"profile_change"' "profile_changed_reason"
  assert_jsonl_contains '"profile_diff"' "profile_diff_present"
  assert_jsonl_contains '"compile"' "profile_diff_compile_field"
  assert_jsonl_contains 'DCB_SMOKE_FLAG=1' "profile_diff_added_flag"
  assert_jsonl_contains '"rebuild_reason":"profile_change"' "compile_end_profile_change"
  assert_compile_end_has_no_profile_diff
  assert_jsonl_contains '"cache_hit":false' "recompile_after_profile_change"
  assert_jsonl_contains 'std.cppm' "std_cppm_command_after_profile_change"
  assert_jsonl_contains '--precompile' "std_precompile_after_profile_change"
  after_pcm_mtime="$(stat -c %Y "${std_pcm}" 2>/dev/null || stat -f %m "${std_pcm}")"
  if [[ "${after_pcm_mtime}" -gt "${before_pcm_mtime}" ]]; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"std_pcm_rebuilt_on_profile_change"}'
  else
    fail "std.pcm should be rebuilt when the object-cache profile changes"
  fi
  if grep -Fq 'DCB_SMOKE_FLAG=1' "${std_profile}"; then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"std_module_profile_updated"}'
  else
    fail "std-module-profile.txt should record the new compile profile"
  fi
  # std.o must use the same compile_flags as std.pcm / project TUs (not a
  # hardcoded subset that drops --compile-flags such as sanitizers).
  TESTS_RUN=$((TESTS_RUN + 1))
  if CB_SMOKE_JSONL="${LAST_JSONL}" python3 - <<'PY'
import json, os, sys
text = os.environ.get("CB_SMOKE_JSONL", "")
for line in text.splitlines():
    try:
        event = json.loads(line)
    except json.JSONDecodeError:
        continue
    if event.get("type") != "command_start":
        continue
    argv = event.get("argv") or []
    joined = " ".join(argv)
    if "std.o" in joined and "-c" in argv and "DCB_SMOKE_FLAG=1" in joined:
        raise SystemExit(0)
raise SystemExit(1)
PY
  then
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"std_o_uses_compile_flags"}'
  else
    fail "std.o compile command should include --compile-flags (DCB_SMOKE_FLAG=1)"
  fi
  end_case profile_change
}

test_cache_status() {
  should_run cache_status || return 0
  begin_case cache_status
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  run_cb_build "${work_dir}"
  run_cb_cache_status "${work_dir}"
  assert_jsonl_contains '"type":"cache_status"' "cache_status_event"
  assert_jsonl_contains '"profile_match":true' "cache_status_profile_match"
  assert_jsonl_contains 'format=cb-object-cache-v3' "cache_status_current_profile"

  # All four files under cache/, not the two the report used to describe. The std module
  # profile was the gap that mattered: `cache invalidate` deletes it — that is what makes CB
  # rebuild std.pcm — while `cache status` never said whether it was there or still valid.
  assert_jsonl_contains '"executable_cache_exists":true' "cache_status_executable_cache_exists"
  assert_jsonl_contains '"std_module_profile_exists":true' "cache_status_std_module_profile_exists"
  assert_jsonl_contains '"std_module_profile_match":true' "cache_status_std_module_profile_match"
  assert_jsonl_contains '"compiler_stamp_exists":true' "cache_status_compiler_stamp_exists"

  run_cb_cache_invalidate "${work_dir}"
  assert_jsonl_contains '"std_module_profile_removed":true' "cache_invalidate_reports_std_module_profile"
  end_case cache_status
}

test_std_module_reported() {
  should_run std_module_reported || return 0
  begin_case std_module_reported
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  # The std module is a modular unit CB compiles like any other; it is only absent from the
  # scan. It used to build through execute_system_command, so the two most expensive steps of
  # a cold build were the only ones reporting no compile_end, no reason and no cache hit.
  run_cb_build "${work_dir}"
  assert_compile_end "std.cppm" false own_pcm_missing true "std_module_cold_build_reason"

  run_cb_build "${work_dir}"
  assert_compile_end "std.cppm" true "" true "std_module_cache_hit"

  # The pcm is the object's input, so an object-only reason reuses the pcm that is there:
  # one step, not two. Every other unit is a cache hit on this build, so a --precompile
  # anywhere in the commands would be the std module rebuilding a pcm it did not need to.
  rm -f "${work_dir}/${BUILD_DIR}/obj/std.o"
  run_cb_build "${work_dir}"
  assert_compile_end "std.cppm" false object_missing true "std_module_object_only_rebuild"
  assert_jsonl_not_contains '"--precompile"' "std_module_object_only_skips_precompile"
  end_case std_module_reported
}

test_modular_object_stale() {
  should_run modular_object_stale || return 0
  begin_case modular_object_stale
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # Two-phase writes the BMI before the object. A successful --precompile followed by a
  # failed/skipped object step leaves pcm newer than .o; without an object_stale check the
  # modular unit cache-hits while importers rebuild against the new interface and link the
  # old implementation.
  printf '%s\n' \
    'export module skew;' \
    'export int value() { return 1; }' > "${work_dir}/skew.c++m"
  printf '%s\n' \
    'import skew;' \
    'int main() { return value() == 1 ? 0 : 1; }' > "${work_dir}/main.c++"

  run_cb_build "${work_dir}"
  assert_jsonl_event_value build_end ok true "modular_object_stale_seed"

  sleep 1
  touch "${work_dir}/${BUILD_DIR}/pcm/skew.pcm"
  run_cb_build "${work_dir}"
  assert_compile_end "skew.c++m" false object_stale true "modular_own_pcm_newer_rebuilds_object"
  # Object-only: reuse the pcm that is already there (no --precompile).
  assert_jsonl_not_contains '"--precompile"' "modular_object_stale_skips_precompile"
  assert_jsonl_event_value build_end ok true "modular_object_stale_build_ok"
  end_case modular_object_stale
}

test_modular_one_phase_object_missing() {
  should_run modular_one_phase_object_missing || return 0
  begin_case modular_one_phase_object_missing
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  # One-phase BMIs are not inputs for a pcm→.o step (Clang 22+ writes reduced BMIs).
  # object_missing / object_stale must re-read the source with -fmodule-output=, matching
  # build_std_module — not compile_pcm_object_argv.
  printf '%s\n' \
    'export module oneshot;' \
    'export int value() { return 3; }' > "${work_dir}/oneshot.c++m"
  printf '%s\n' \
    'import oneshot;' \
    'int main() { return value() == 3 ? 0 : 1; }' > "${work_dir}/main.c++"

  run_cb_build "${work_dir}" --modules=one-phase
  assert_jsonl_event_value build_end ok true "one_phase_object_missing_seed"

  rm -f "${work_dir}/${BUILD_DIR}/obj/oneshot.o"
  run_cb_build "${work_dir}" --modules=one-phase
  assert_compile_end "oneshot.c++m" false object_missing true "one_phase_object_missing_rebuilds"
  assert_jsonl_contains '-fmodule-output=' "one_phase_object_missing_rereads_source"
  assert_jsonl_not_contains '"--precompile"' "one_phase_object_missing_no_precompile"
  assert_jsonl_event_value build_end ok true "one_phase_object_missing_build_ok"
  end_case modular_one_phase_object_missing
}

test_module_phases() {
  should_run module_phases || return 0
  begin_case module_phases
  local work_dir cache_file
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
  rm -f "${work_dir}/hello.c++"

  printf '%s\n' \
    'export module phases;' \
    'export int phased() { return 7; }' > "${work_dir}/phases.c++m"
  printf '%s\n' \
    'import phases;' \
    'int main() { return phased() == 7 ? 0 : 1; }' > "${work_dir}/main.c++"

  # Default is two-phase: --precompile writes the BMI, a second step turns it into the object.
  run_cb_build "${work_dir}"
  assert_jsonl_event_value build_end ok true "two_phase_build"
  assert_jsonl_contains '"--precompile"' "two_phase_precompiles"
  assert_jsonl_not_contains '-fmodule-output=' "two_phase_no_module_output"
  assert_file_exists "${work_dir}/${BUILD_DIR}/pcm/phases.pcm" "two_phase_pcm"
  cache_file="$(object_cache_path "${work_dir}")"
  assert_profile_contains "${cache_file}" $'\tmodule_phases=two-phase' "two_phase_profile"

  # Switching schemes is a profile change: the BMIs differ, so no object may survive it.
  run_cb_build "${work_dir}" --modules=one-phase
  assert_jsonl_event_value build_end ok true "one_phase_build"
  assert_jsonl_contains '"module_phases":{"old":"two-phase","new":"one-phase"}' "one_phase_profile_diff"
  assert_compile_end "phases.c++m" false profile_change true "one_phase_rebuilds_module"
  assert_compile_end "std.cppm" false profile_change true "one_phase_rebuilds_std"

  # One command per modular unit, and it still publishes the BMI importers read.
  assert_jsonl_contains '-fmodule-output=' "one_phase_module_output"
  assert_jsonl_not_contains '"--precompile"' "one_phase_no_precompile"
  assert_file_exists "${work_dir}/${BUILD_DIR}/pcm/phases.pcm" "one_phase_pcm"
  assert_file_exists "${work_dir}/${BUILD_DIR}/obj/phases.o" "one_phase_object"
  assert_compile_start_end_pairs "one_phase_compile_pairs"

  run_cb_build "${work_dir}" --modules=one-phase
  assert_compile_cache_hits 2 "one_phase_cache_hit"
  assert_jsonl_not_contains '"rebuild_reason":"profile_change"' "one_phase_profile_stable"

  # And back: two-phase must not adopt the one-phase BMIs either.
  run_cb_build "${work_dir}"
  assert_jsonl_contains '"module_phases":{"old":"one-phase","new":"two-phase"}' "back_to_two_phase_diff"
  assert_jsonl_event_value build_end ok true "back_to_two_phase_build"

  TESTS_RUN=$((TESTS_RUN + 1))
  if run_cb_build "${work_dir}" --modules=half-phase; then
    fail "--modules=half-phase should be rejected"
  else
    jsonl_emit '{"type":"smoke_assert_passed","matcher":"module_phases_rejects_unknown"}'
  fi
  end_case module_phases
}

test_jsonl_modes() {
  should_run jsonl_modes || return 0
  begin_case jsonl_modes
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"

  run_cb_build "${work_dir}" --jsonl=failures
  assert_jsonl_event_count build_start 1 "failures_build_start"
  assert_jsonl_event_count build_end 1 "failures_build_end"
  assert_jsonl_not_contains '"type":"command_start"' "failures_no_command_start"
  assert_jsonl_not_contains '"type":"compile_end"' "failures_no_successful_compile"
  assert_jsonl_contains '"compile_rebuilt":2' "failures_rollup_rebuilt"

  run_cb_build "${work_dir}" --jsonl=summary
  assert_jsonl_event_count build_start 1 "summary_build_start"
  assert_jsonl_event_count build_end 1 "summary_build_end"
  assert_jsonl_not_contains '"type":"command_' "summary_no_commands"
  assert_jsonl_not_contains '"type":"compile_' "summary_no_compiles"
  assert_jsonl_contains '"compile_cache_hits":2' "summary_rollup_cache_hit"
  end_case jsonl_modes
}

test_jsonl_failure_mode() {
  should_run jsonl_failure_mode || return 0
  begin_case jsonl_failure_mode
  local work_dir
  prepare_work_dir
  work_dir="${LAST_WORK_DIR}"
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
  trap cleanup_on_exit EXIT

  jsonl_emit '{"type":"smoke_start","schema":"cb-smoke","version":1}'
  log "cb smoke tests (cb=${CB_BIN}, build_dir=${BUILD_DIR})"

  test_profile_header
  test_cache_hit
  test_link_cache_hit
  test_clean_tests
  test_parallel_main_link
  test_compile_start
  test_source_stale
  test_header_stale
  test_header_stale_relative_include
  test_header_missing
  test_depfile_unusable
  test_strict_arguments
  test_source_list
  test_compile_commands
  test_graph_json
  test_compile_failure
  test_compile_warning
  test_modular_compile_warning
  test_link_failure
  test_test_link_failure
  test_link_rebuild_reason
  test_implementation_pcm
  test_dotted_module_name
  test_import_trailing_comment
  test_commented_out_imports
  test_spliced_and_raw_literals
  test_spliced_directives
  test_mentioned_raw_opener
  test_dead_conditional_arms
  test_commented_import_no_false_cycle
  test_gmf_preamble
  test_module_safe_name
  test_same_basename_collision
  test_reserved_std_collision
  test_nested_deps_skipped
  test_vendored_tester_tests_skipped
  test_build_output_trees_skipped
  test_project_test_dir_included
  test_deps_package_tests_skipped
  test_rebuild_summary
  test_test_lifecycle
  test_test_runner_exact_name
  test_cache_invalidate
  test_profile_change
  test_cache_status
  test_std_module_reported
  test_modular_object_stale
  test_modular_one_phase_object_missing
  test_module_phases
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