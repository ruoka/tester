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
- ✅ `test_runner --list --jsonl`: `test_list_start`, `registered_test`, `test_list_summary`.

### 2.2 BDD ergonomics

- ✅ Nested `given` / `when` / `then` with `shared_ptr` capture pattern (see `examples/readme_bdd_example.test.c++`).
- 📋 Static analysis or compile-time hint when nested lambdas capture by reference (common footgun).
- 📋 Optional flat `test_case` + `section` guidance in README for non-BDD modules.

### 2.3 Dependencies & ordering

- ✅ `priority` and `depends_on` fields on `test_case`.
- 📋 Expose dependency graph in `--list` output.
- 📋 Fail fast with a clear message when dependency cycle is detected.
- ✅ Framework self-tests in `tester/*.test.c++` tagged `[self]`; CI runs `./tools/CB.sh debug test --jsonl --tags='\[self\]'` and requires `summary.passed`.

### 2.4 Matcher naming in JSONL

- ✅ `matcher` uses `extract_matcher_name(location2)` on the public wrapper name (`require_eq`, `check_contains`, …), not the inner `check`/`require` hub.
- ✅ Each `check_*` / `require_*` wrapper that delegates to `check`/`require` captures `const auto matcher_location = std::source_location::current()` at **wrapper entry** and passes it as `location2`. Relying on the hub’s default `matcher_location` is not enough for template wrappers (e.g. `require_eq` would emit `"require"`).
- ✅ `location1` (file/line/column on assertion events) still comes from the test call site via the wrapper’s `location` parameter.
- 📋 After changing `tester:assertions`, recompile **test** translation units (`*.test.c++`), not only `tester_assertions.pcm` — template wrappers are instantiated in each test object.
- ✅ Matcher naming on `check_nothrow` / `check_throws` / `check_throws_as` / `require_nothrow` / `require_throws` / `require_throws_as`.
- 📋 Extend the same naming to non-comparison paths (`message` events, custom predicates) where `matcher` is absent or generic today.

---

## 3. JSONL & AI-friendly output

Machine-parseable test and build output for CI and automation. Human output remains the default.

### 3.1 Structured assertion events

- ✅ `assertion_failed` / `assertion_passed` with `test_id`, `matcher`, `actual`, `expected`, `file`, `line`, `column`.
- ✅ `assertion_failed` always emitted in JSONL mode; `assertion_passed` when `--jsonl-output=always` (default policy: `failures`).
- ✅ `message` on `assertion_failed` / `assertion_passed` for exception assertions (`require_nothrow`, `check_throws*`, …).
- 📋 Add `expression` (source-level) field for non-comparison assertions (needs macro infrastructure).
- 📋 Document event ordering: assertion events stream during execution; `test` records batch at finalize time.

### 3.2 Run lifecycle events

- ✅ `run_start`, `run_end`, `case`, `test`, `summary`, `message`, `exception`, `eof`.
- ✅ `run_start` env metadata: `cwd`, structured `argv`, `config` (from `TESTER_CONFIG` when CB spawns `test_runner`).
- ✅ `run_start.env` object for curated test-relevant environment (`NET_DISABLE_NETWORK_TESTS`, `CURSOR_SANDBOX`) when set — omitted when empty.
- ✅ `summary` includes `tests_ok`, `tests_total`, `assertions_ok`, `assertions_total`, `passed`.
- ✅ `failed_test_ids: [...]` in `summary` / `run_end`.
- ✅ `first_failure: { test_id, file, line, message }` for direct navigation.
- ✅ `slowest` JSON array in `summary` when `--slowest=N` is set.

### 3.3 Exception metadata

- ✅ `exception` event has `message`, `file`, and `exception_type` (demangled `typeid`).

### 3.4 Correlation & multi-stream

- ✅ `run_id` on every CB and tester JSONL record for correlating build + test in one session.
- ✅ `parent_run_id` on `test_runner` events when CB spawns the child (`TESTER_PARENT_RUN_ID` env).
- ✅ `TESTER_CONFIG` env (`debug` / `release`) surfaced as `config` on `run_start`.

### 3.5 Log artifacts

- ✅ `.gitignore` excludes captured `*.jsonl` and `*.log` debug output — do not commit local JSONL captures (e.g. `assertions_*.jsonl`, `test_results_*.jsonl` from manual redirects); CI generates ephemeral artifacts instead.
- 📋 Write large stderr/stdout to files; emit `stderr_path` / `stdout_path` in JSONL.
- 📋 Bounded `*_head` snippet inline when artifacts are truncated (`--jsonl-output-max-bytes` already exists for test output).

### 3.6 CB JSONL (build phase)

- ✅ CB emits one paired `build_start` / `build_end` per invocation, plus `command_start` / `command_end`, `test_start` / `test_end`, and `eof`.
- ✅ `list --jsonl`: `list_start`, per-TU `unit` (`path`, `module`, `kind`, `imports`, `level`, `is_test`, …), `list_summary`.
- ✅ `unit.is_test`: `true` for `*.test.c++` / `*.test.c++m` or `test/` / `tests/` path segments; `false` for `tester/` framework trees (exact segment match — not substring `test` in `tester`).
- ✅ Per-translation-unit `compile_end` with `ok`, `duration_ms`, `source_path`, `object_path`, `pcm_path`, `module_name`, `cache_hit`.
- ✅ Per-translation-unit `compile_start` (paired with `compile_end`; `duration_ms` on `compile_end` measures compile wall time).
- ✅ Per-binary `link_end` (`executable_path`, `cache_hit`, `ok`, `duration_ms`); `link_start` still optional.
- ✅ Structured `argv: ["clang++", "..."]` on `command_start` / `command_end` alongside human `cmd` string.
- ✅ `cache_hit: true` on `compile_end` when incremental compile skips a translation unit.
- ✅ `rebuild_reason` on `compile_end` when `cache_hit:false` (e.g. `source_stale`, `pcm_stale:<module>`, `dependency_pcm_stale:<module>`, `profile_change`).
- ✅ `profile_changed` event with `profile_diff` on profile mismatch (scalar fields + token diff on `compile`/`cpp`; not repeated on each `compile_end`).

### 3.7 Recommended automation invocation

```bash
./tools/CB.sh debug test --jsonl --jsonl-output=always -- --tags='\[module\]'
```

`--tags` and `--list` may be passed directly after `test` (no `--` required). Use `--` only for uncommon `test_runner` flags.

---

## 4. C++ Builder (cb.c++)

Design rationale and comparison with CMake, Make, and other build tools: [`docs/cb.md`](cb.md).

### 4.1 Core build system

- ✅ Incremental compile cache (`object_cache_map`) and link cache (`link_cache_map`); each level's compile decisions are completed against a stable cache before workers start, failed workers are joined, and cache indexes use checked temporary-file replacement.
- ✅ Object-cache profile header (`format=cb-object-cache-v3`) with toolchain fields (`config`, `static_link`, `llvm`, `cxx`, `cxx_sig`, `clang_ver`, `std_cppm` as `path@size:mtime_ns`, `compile` / `cpp` including `--compile-flags`); invalidates on profile mismatch (`rebuild_reason: "profile_change"`).
- ✅ CB smoke harness (`tests/cb/`) and CI `cb-smoke` job (`profile_header`, `cache_hit`, `link_cache_hit`, `compile_start`, `source_list`, `compile_failure`, `link_failure`, `test_link_failure`, `implementation_pcm`, `test_lifecycle`, `cache_invalidate`, `profile_change`, `cache_status`).
- ✅ `cache status` subcommand (human + JSONL `cache_status`).
- ✅ `cache invalidate` subcommand (human + JSONL `cache_invalidate_end`).
- ✅ `profile_changed` JSONL event (single `profile_diff` on profile mismatch).
- ✅ Human-mode `profile_change` logging on stderr (non-JSONL).
- ✅ Profile value writer contract (verbatim tab-separated values; fields CB writes must not contain tab, newline, or `%`).
- ✅ Parallel compilation, topological module sort, preamble `import` scan in `cb.c++`.
- ✅ CB / tester implementation policy: standard C++ + `std::system` only; stack traces in `test_runner` via `<execinfo.h>` (POSIX exception).
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

### 4.4 Cache maintenance (optional — add if operational issues appear)

Not required for correctness today: profile v3 + timestamp rules already drive rebuilds. Consider only if users hit **disk bloat**, **stale orphan artifacts**, or need **visibility** into cache state without a full `clean`.

**Problem today:** `clean` removes the entire `build-<os>-<config>/` tree. `profile_change` clears the in-memory object-cache index and forces recompiles, but old `.o` / `.pcm` files may remain on disk. Long-lived trees (renamed TUs, removed modules, repeated flag experiments) can accumulate dead artifacts.

**Proposed `cache` subcommand** (alongside `build`, `test`, `list`, `clean`):

| Verb | Purpose |
|------|---------|
| `cache status` | Decode profile header, index entry counts, disk usage (`obj/`, `pcm/`, `cache/`), orphan/stale rows (cache points at missing paths; artifacts with no matching source/TU). JSONL: `cache_status` event. ✅ |
| `cache invalidate` | Delete `object-cache.txt` / `executable-cache.txt` / `compiler-version.txt` only — lighter than `clean`; next build treats everything as uncached without wiping artifacts. JSONL: `cache_invalidate_end`. ✅ |
| `cache prune` | Garbage-collect artifacts the current module graph no longer references; trim cache index rows for missing paths. Optional `--aggressive` after `profile_change` to drop all `obj/` / `pcm/` under the config. JSONL: `cache_prune_end` with counts. |

**Implementation sketch:** scan current TU graph → expected `{obj, pcm}` set; walk `obj/` and `pcm/`; delete files not in set; reconcile `object-cache.txt`. Reuse existing `object_cache_profile()` / `parse_object_cache_profile_fields()` for `status`.

**Smoke / CI:** add `cache_prune` cases only if the subcommand ships.

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
- ✅ macOS `/usr/local/llvm` setup guide — [clang-modules-macos.md](clang-modules-macos.md) ([LLVM Getting Started](https://llvm.org/docs/GettingStarted.html); [#92121](https://github.com/llvm/llvm-project/issues/92121), [#168287](https://github.com/llvm/llvm-project/issues/168287#issuecomment-3712718691)).

### 6.3 Automation guide

- ✅ `AGENTS.md` at repo root (canonical JSONL commands, triage workflow, event reference for agents/CI).
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
| Low | `cache status` / `invalidate` / `prune` subcommands | Only if disk/orphan/stale-cache issues show up in practice |

---

## References (in this repo)

- C++ Builder guide: `docs/cb.md`
- Public consumer example: [YarDB](https://github.com/ruoka/YarDB)
- Assertion implementation: `tester/tester-assertions.c++m`
- Matcher name extraction: `tester/tester-utils.c++m` (`extract_matcher_name`)
- JSONL observer: `tester/tester-jsonl_observer.c++m`
- Event publishing: `tester/tester-publisher.c++m`
- Build system: `tools/cb.c++`
- CLI entry: `tester/test_runner.c++`