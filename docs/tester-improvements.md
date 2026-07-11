# C++ Builder & Tester — Improvement Backlog

Single place for enhancement ideas across the **tester** framework and the **CB** (`tools/cb.c++`) build system.

**Status markers:** ✅ implemented · 🔶 partial · 📋 proposed

---

## 1. Assertion framework

Reviewed against `tester/tester-assertions.c++m` and common C++ test frameworks.

### 1.1 Approximate comparisons

- ✅ `check_near` / `require_near` wrap `floating_point_equal` with per-call epsilon control.
- 📋 Extend approximate comparisons to user-defined types (e.g. `std::vector<double>`) with element-wise tolerance helpers.

### 1.2 Container and range assertions

- ✅ `check_container_eq` / `require_container_eq` — sequence equality with first-mismatch diffs.
- ✅ `check_contains` / `require_contains` — element membership.
- 📋 Sequence prefix/suffix matchers for containers (mirror string `starts_with` / `ends_with`).
- 📋 Permutation / order-insensitive container equality.

### 1.3 String-focused assertions

- ✅ `check_contains` / `require_contains`, `check_has_substr` / `require_has_substr`.
- ✅ `check_starts_with` / `require_starts_with`, `check_ends_with` / `require_ends_with`.
- 📋 Case-insensitive and locale-aware string equality.
- 📋 Regex match assertions (`require_matches` / `check_matches`).

### 1.4 Predicate / matcher API

- 📋 Lightweight matchers composable with `check` / `require` (e.g. `that(x, is_gt(3))`) to reduce boilerplate lambdas.

### 1.5 Death / termination tests

- 📋 Verify code aborts or exits with a signal; optionally capture stderr/stdout for diagnostics.

---

## 2. Test runner & BDD

### 2.1 Tag filtering

- ✅ Regex and substring tag filters via `--tags=`.
- 📋 Document bracket-tag convention (`[module]` in scenario names) in one canonical example table.
- 📋 `test_runner --list` JSONL mode (machine-readable test catalogue for agents).

### 2.2 BDD ergonomics

- ✅ Nested `given` / `when` / `then` with `shared_ptr` capture pattern (see `examples/readme_bdd_example.test.c++`).
- 📋 Static analysis or compile-time hint when nested lambdas capture by reference (common footgun).
- 📋 Optional flat `test_case` + `section` guidance in README for non-BDD modules.

### 2.3 Dependencies & ordering

- ✅ `priority` and `depends_on` fields on `test_case`.
- 📋 Expose dependency graph in `--list` output.
- 📋 Fail fast with a clear message when dependency cycle is detected.

### 2.4 Matcher naming in JSONL

- ✅ `matcher` field uses `extract_matcher_name()` from `std::source_location` in `tester-utils.c++m` (e.g. `check_eq`, `require_contains`, not generic `"check"`).
- 📋 Extend the same naming to non-comparison paths (`message` events, `require_nothrow`, custom predicates) where `matcher` is absent or generic today.

---

## 3. JSONL & AI-friendly output

Machine-parseable test and build output for CI and automation. Human output remains the default.

### 3.1 Structured assertion events

- ✅ `assertion_failed` / `assertion_passed` with `test_id`, `matcher`, `actual`, `expected`, `file`, `line`, `column`.
- ✅ `assertion_failed` always emitted in JSONL mode; `assertion_passed` when `--jsonl-output=always` (default policy: `failures`).
- 📋 Add `expression` (source-level) and `message` fields for non-comparison assertions (`require_nothrow`, custom messages).
- 📋 Document event ordering: assertion events stream during execution; `test` records batch at finalize time.

### 3.2 Run lifecycle events

- ✅ `run_start`, `run_end`, `case`, `test`, `summary`, `message`, `exception`, `eof`.
- ✅ `summary` includes `tests_ok`, `tests_total`, `assertions_ok`, `assertions_total`, `passed`.
- ✅ `failed_test_ids: [...]` in `summary` / `run_end`.
- ✅ `first_failure: { test_id, file, line, message }` for direct navigation.
- 📋 `slowest` list as structured array in `summary` (today partly human-oriented).

### 3.3 Exception metadata

- 🔶 `exception` event has `message` and `file` but not a separate type name.
- 📋 Add `exception_type` (demangled or `typeid`) alongside `what`.

### 3.4 Correlation & multi-stream

- 📋 Shared `run_id` on every CB and tester JSONL record for correlating build + test in one session.
- 📋 Optional parent `run_id` when CB spawns `test_runner` as a child process.

### 3.5 Log artifacts

- ✅ `.gitignore` excludes captured `*.jsonl` and `*.log` debug output — do not commit local JSONL captures (e.g. `assertions_*.jsonl`, `test_results_*.jsonl` from manual redirects); CI generates ephemeral artifacts instead.
- 📋 Write large stderr/stdout to files; emit `stderr_path` / `stdout_path` in JSONL.
- 📋 Bounded `*_head` snippet inline when artifacts are truncated (`--jsonl-output-max-bytes` already exists for test output).

### 3.6 CB JSONL (build phase)

- 🔶 CB emits `build_start` / `build_end`, `command_start` / `command_end`, `test_start` / `test_end`, `eof`.
- ✅ Per-translation-unit `compile_end` with `ok`, `duration_ms`, `source_path`, `object_path`, `pcm_path`, `module_name`, `cache_hit`.
- 📋 Per-translation-unit `compile_start` (optional; `compile_end` is sufficient for triage today).
- 📋 Per-binary `link_start` / `link_end`.
- ✅ Structured `argv: ["clang++", "..."]` on `command_start` / `command_end` alongside human `cmd` string.
- ✅ `cache_hit: true` on `compile_end` when incremental compile skips a translation unit.

### 3.7 Recommended automation invocation

```bash
./tools/CB.sh debug test --jsonl --jsonl-output=always -- --tags='\[module\]'
```

`--tags` and `--list` may be passed directly after `test` (no `--` required). Use `--` only for uncommon `test_runner` flags.

---

## 4. C++ Builder (cb.c++)

### 4.1 Core build system

- ✅ Incremental compile cache (`object_cache_map`) and link cache (`link_cache_map`).
- ✅ Parallel compilation, topological module sort, `clang-scan-deps` integration.
- ✅ `debug` / `release` configurations; `clean`, `list`, `ci`, `--build-tests`.
- 📋 Multiple custom configurations beyond debug/release (e.g. `asan`, `coverage`).
- 📋 Export compile/link graph as JSON for external tools.
- 📋 CMake / `compile_commands.json` export (optional; conflicts with “zero config” philosophy).
- 📋 Richer diagnostics on module dependency cycles and missing PCM.
- 📋 Support alternate module naming conventions beyond current `*.c++m` / `*.impl.c++` rules.

### 4.2 Test integration

- ✅ Auto-link `test_runner` with discovered `*.test.c++` objects.
- ✅ Positional filter after `test` (substring on test id).
- ✅ Convenience forwarding to `test_runner` without `--` for: `--tags=`, `--list`, `--output=jsonl`, `--jsonl-output=…`, `--slowest=…`, `--result`, `--help`, and global `--jsonl`.
- ✅ Positional filter after `test` no longer consumes flags that start with `-` or known test_runner tokens.
- 📋 `test --watch` mode (rebuild + rerun on file change).

### 4.3 Discovery & layout

- ✅ Co-located `*.test.c++` next to sources (P1204R0 §7.1).
- ✅ `--include-examples` for standalone development.
- 📋 Explicit test include/exclude globs in CB CLI.
- 📋 First-class support for integration tests in a separate top-level `tests/` directory when embedded as a dependency.

---

## 5. Bootstrap scripts (`tools/CB.sh`)

Per-project wrappers compile `cb.c++` and invoke it with the right include paths.

### 5.1 Toolchain alignment

- ✅ Dev containers for tester, net, and xson on **Clang 21** / Debian bookworm.
- ✅ Consuming repos (e.g. fixer): `post-create.sh` bootstrap stamp skips rebuild when `HEAD` + submodule pointers are unchanged.
- ✅ Shared `CB.sh.core` + `CB.sh.template`; per-repo wrappers source the core (diff table in template header).
- 📋 Align nested `deps/tester` copies when parent repos bump the tester submodule pointer.

### 5.2 Robustness

- ✅ Cross-OS binary rebuild detection and `std.cppm` existence checks in `CB.sh.core` (all wrappers).
- ✅ JSONL-safe wrapper logging (`cb_log` → stderr when `--jsonl` / `--output=jsonl`).
- ✅ `NET_DISABLE_NETWORK_TESTS` sandbox hook only enabled in net wrapper (`CB_SANDBOX_DISABLE_NETWORK_TESTS=1`).

### 5.3 Sandbox & CI

- ✅ `CURSOR_SANDBOX` can auto-set `NET_DISABLE_NETWORK_TESTS` in network-heavy projects.
- 📋 Document sandbox behaviour in CONTRIBUTING.
- 📋 JSONL-first `ci` example in README: `./tools/CB.sh ci --jsonl`.

---

## 6. Documentation

### 6.1 Toolchain version

- ✅ README and CONTRIBUTING: Clang 21 on Linux.
- 📋 Keep LLVM version in sync across tester and consuming-project READMEs on every toolchain bump.
- 📋 Note that nested `deps/tester` copies inside other repos lag until their submodule pointer updates.

### 6.2 CLI reference

- 📋 Single table: which flags CB forwards without `--` vs which require `--`.
- 📋 `test_runner` JSONL event schema reference (field glossary per `type`).
- 📋 macOS `/usr/local/llvm` setup guide linked from Requirements.

### 6.3 Automation guide

- 📋 Short “For automation / CI” section in README (canonical JSONL command, event types, tag syntax).
- 📋 JSONL assertion event table in README (see §3.1).

---

## 7. Platform & environment

### 7.1 Linux

- ✅ LLVM 21 from apt.llvm.org; `std.cppm` at `/usr/lib/llvm-21/share/libc++/v1/std.cppm`.
- 📋 Verify `clang-scan-deps` in post-create and CI with a failing smoke module build.

### 7.2 macOS

- 🔶 Requires custom LLVM at `/usr/local/llvm` (documented; high friction for new contributors).
- 📋 Document minimum LLVM version and `flat_map` / module support matrix.
- 📋 Optional Homebrew LLVM path detection (explicitly unsupported today; document why).

### 7.3 Cross-platform cache

- 📋 Surface cache stamp / signature in `CB.sh list` or JSONL when link is skipped.
- 📋 `CB.sh clean` flag to invalidate only test objects without full rebuild.

---

## 8. Submodule / monorepo consumption

When tester is used as `deps/tester` inside a larger repo:

- ✅ CB resolves sibling `../tester` or local `deps/tester`.
- ✅ Parent repos should bump the submodule pointer after tester fixes (e.g. JSONL capture cleanup, `first_failure`); nested `deps/*/tester` copies lag until each submodule updates.
- 📋 Document resolution order in README (sibling vs nested vs `CB_FETCH_DEPS=1`).
- 📋 Avoid duplicating stale tester docs inside nested `deps/tester` trees — bump the submodule pointer instead.

---

## Priority sketch (suggested)

| Priority | Item | Rationale |
|----------|------|-----------|
| — | `first_failure` + `failed_test_ids` in JSONL summary | ✅ Done |
| — | CB forward `--tags` without `--` | ✅ Done |
| — | Precise matcher names in assertion JSONL (`check_eq`, …) | ✅ Done |
| — | `compile_end` + structured `argv` in CB JSONL | ✅ Done |
| — | Unified `CB.sh` template | ✅ Done (`tools/CB.sh.core`) |
| Low | Death tests, regex matchers, CMake export | Nice-to-have framework parity |

---

## References (in this repo)

- Assertion implementation: `tester/tester-assertions.c++m`
- Matcher name extraction: `tester/tester-utils.c++m` (`extract_matcher_name`)
- JSONL sink: `tester/tester-jsonl_sink.c++m`
- Output routing: `tester/tester-output.c++m`
- Build system: `tools/cb.c++`
- CLI entry: `tester/test_runner.c++`