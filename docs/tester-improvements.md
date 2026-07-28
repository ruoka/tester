# C++ Builder & Tester — Improvement Backlog

Single place for enhancement ideas across the **tester** framework and the **CB** (`tools/cb.c++`) build system.

**Status markers:** ✅ implemented · 🔶 partial · 📋 proposed

---

## 1. Assertion framework

Reviewed against `tester/tester-assertions.c++m` and common C++ test frameworks.

### 1.1 Approximate comparisons

- ✅ `check_near` / `require_near` wrap `floating_point_equal` with per-call epsilon control.
- ✅ Default epsilon floored at four times `std::numeric_limits<T>::epsilon()`. A flat `1e-9` was unreachable for `float`, whose machine epsilon (~1.19e-7) is larger, so a single ulp of ordinary rounding error compared unequal. `double` / `long double` keep the historical `1e-9` — their epsilon is far below it, so the floor never applies.
- 📋 Extend approximate comparisons to user-defined types (e.g. `std::vector<double>`) with element-wise tolerance helpers.

### 1.2 Container and range assertions

- ✅ `check_container_eq` / `require_container_eq` — sequence equality with first-mismatch diffs.
- ✅ `check_contains` / `require_contains` — element membership.
- 📋 Sequence prefix/suffix matchers for containers (mirror string `starts_with` / `ends_with`).
- 📋 Permutation / order-insensitive container equality.
- 📋 Broaden `is_container` to `std::ranges::input_range`; support `std::formatter`-only element types in `format_container`.

### 1.3 String-focused assertions

- ✅ `check_contains` / `require_contains`, `check_has_substr` / `require_has_substr`.
- ✅ `check_starts_with` / `require_starts_with`, `check_ends_with` / `require_ends_with`.
- 📋 Case-insensitive and locale-aware string equality.
- 📋 Regex match assertions (`require_matches` / `check_matches`).

### 1.4 Predicate / matcher API

- 📋 Lightweight matchers composable with `check` / `require` (e.g. `that(x, is_gt(3))`) to reduce boilerplate lambdas.

### 1.5 Death / termination tests

- 📋 Verify code aborts or exits with a signal; optionally capture stderr/stdout for diagnostics. The child-spawn and wait-status decoding helpers this needs already exist in `tester/details/selftest_spawn.h++` (`signaled` / `signal`).

### 1.6 Exception assertions

- ✅ `check_throws_as<E>(callable)` / `require_throws_as<E>(callable)` name the expected type as a template argument; a derived exception satisfies a base-class expectation.
- ✅ The older form taking an exception *instance* survives as a deprecated forwarder, so consumers pinning an older tester keep compiling. The instance's value was always discarded — it existed only to deduce `E`.

### 1.7 Comparison semantics & argument passing

- ✅ Mixed-signedness comparisons compare mathematical values. Comparing through `std::common_type_t<int, unsigned>` (which is `unsigned`) converted the signed operand, so `check_eq(-1, 4294967295u)` reported **equal** and `check_lt(-1, 1u)` reported **not less**. The six relational matchers now route every `std::integral` pair through the `std::cmp_*` family after unary `+` (integral promotions make `bool` / character types legal for `cmp_*`, closing the same wraparound for `check_eq(char{-1}, UINT_MAX)`). Equal values still match across signedness, so `require_eq(v.size(), 3)` keeps working. Operands are still reported as written, not as converted.
- ✅ One comparator per matcher, so every relational matcher is the same one-line shape. `check_eq` used to branch on floating point in the wrapper and cast its operands, while the other eleven passed operands through untouched; the type-dependent rules now live inside the comparators. The cast was redundant, because the comparator converts in its parameters and display uses six-digit `operator<<` either way. `check_near` / `require_near` keep their cast deliberately: `T` is deducible from the epsilon argument, so `check_near(1.0f, 2.0f, 0.1)` makes `T` double while `common_type_t<A,E>` is float.
- ✅ `check_neq` no longer contradicts `check_eq` on floating point. `check_neq` compared exactly while `check_eq` compared within an epsilon, so a pair inside the tolerance satisfied both matchers at once — the same class of defect as the mixed-signedness pass. Inequality is now the negation of the epsilon comparison. Ordering stays exact, as in every comparable framework.
- ✅ Assertions take operands by **forwarding reference**, so nothing is copied and move-only types are comparable. By value cost one copy per operand per layer: a copy-counting type measured **six copies per assertion**, two each in the wrapper, the hub and the comparators. A plain `const&` does not work — deduction against it drops the const of a string literal's element type, making `A` become `char[4]`, whose common type is `char*` and cannot hold the `const char*` the literal decays to, so `check_eq("abc", "abc")` stops compiling. Forwarding references preserve it, and `std::common_type_t` already decays, so every comparator still sees the same `T` and the hub signature is unchanged. Operands are only ever read: the comparators and the publisher take them by `const&`, so nothing is forwarded onward. A `[self]` test pins zero copies and zero moves across seven assertions, plus a `std::unique_ptr` comparison.
- ✅ Unprintable operands are reported by demangled type name. Two `typeid(...).name()` sites reported the mangled spelling (`N6tester8selftest10assertions12_GLOBAL__N_17countedE`) while exception types already went through `demangle_type`; this surfaced the moment move-only and non-streamable operands became comparable.

### 1.8 Matcher naming

- 📋 Rename `check_neq` / `check_lteq` / `check_gteq` to `_ne` / `_le` / `_ge` (keeping aliases), and reconsider `succeed` / `failed` / `warning`.

---

## 2. Test runner & BDD

### 2.1 Tag filtering

- ✅ Regex and substring tag filters via `--tags=`.
- 📋 Document bracket-tag convention (`[module]` in scenario names) in one canonical example table.
- 📋 Make tags first-class via `test_order{ .tags = {…} }` rather than encoding them in the test name, and extend the query language with OR (`[a],[b]`) and negation (`~[a]`).
- ✅ `test_runner --list --jsonl=failures`: `test_list_start`, `registered_test`, `test_list_summary`.

### 2.2 BDD ergonomics

- ✅ Nested `given` / `when` / `then` with `shared_ptr` capture pattern (see `examples/readme_bdd_example.test.c++`).
- 📋 Static analysis or compile-time hint when nested lambdas capture by reference (common footgun).
- 📋 Optional flat `test_case` + `section` guidance in README for non-BDD modules.

### 2.3 Dependencies & ordering

- ✅ `priority` and `depends_on` fields on `test_case`.
- 📋 Expose dependency graph in `--list` output.
- 📋 Fail fast with a clear message when dependency cycle is detected.
- 📋 Report unresolved `depends_on` ids and duplicate test ids instead of silently ignoring them.
- 📋 Re-sort test cases after nested registration so `priority` / `depends_on` take effect inside `scenario`.
- ✅ Framework self-tests in `tester/*.test.c++` tagged `[self]`; CI runs `./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'` and requires `summary.passed`.

### 2.4 Matcher naming in JSONL

- ✅ `matcher` uses `extract_matcher_name(location2)` on the public wrapper name (`require_eq`, `check_contains`, …), not the inner `check`/`require` hub.
- ✅ Each `check_*` / `require_*` wrapper that delegates to `check`/`require` captures `const auto matcher_location = std::source_location::current()` at **wrapper entry** and passes it as `location2`. Relying on the hub’s default `matcher_location` is not enough for template wrappers (e.g. `require_eq` would emit `"require"`).
- ✅ `location1` (file/line/column on assertion events) still comes from the test call site via the wrapper’s `location` parameter.
- 📋 After changing `tester:assertions`, recompile **test** translation units (`*.test.c++`), not only `tester_assertions.pcm` — template wrappers are instantiated in each test object.
- ✅ Matcher naming on `check_nothrow` / `check_throws` / `check_throws_as` / `require_nothrow` / `require_throws` / `require_throws_as`.
- 📋 Extend the same naming to non-comparison paths (`message` events, custom predicates) where `matcher` is absent or generic today.

### 2.5 Registration & execution control

- ✅ `make_test_case` no longer calls `std::terminate()` when the registering wrapper lives outside `tester::basic` / `tester::behavior_driven_development`; it falls back to the wrapper's unqualified name. Unreachable today (`make_test_case` is not exported and both wrappers match a prefix), so this removed a trap rather than fixing a live defect.
- 📋 Let `runner` own its tag string, and make the observer instance non-latching.
- 📋 `--shuffle` / `--seed` / `--repeat` flags, and per-test timeouts.

---

## 3. JSONL & AI-friendly output

Machine-parseable test and build output for CI and automation. Human output remains the default.

### 3.1 Structured assertion events

- ✅ `assertion_failed` / `assertion_passed` with `test_id`, `matcher`, `actual`, `expected`, `file`, `line`, `column`.
- ✅ Unified `--jsonl=summary|failures|trace`; failed assertions in failures/trace and passing assertions in trace.
- ✅ `message` on `assertion_failed` / `assertion_passed` for exception assertions (`require_nothrow`, `check_throws*`, …).
- ✅ Operands are always valid UTF-8: `jsonl::escape` validates sequences and substitutes U+FFFD for invalid bytes, rejecting overlong encodings, surrogates and out-of-range code points. Arbitrary test data can no longer make a line unparseable.
- ✅ String operands reported verbatim; only exception matchers report type names, demangled at the matcher rather than in the reporting layer.
- 📋 Add `expression` (source-level) field for non-comparison assertions (needs macro infrastructure).
- 📋 Document event ordering: assertion events stream during execution; `test` records batch at finalize time.

### 3.2 Run lifecycle events

- ✅ `run_start`, `run_end`, `case`, `test`, `summary`, `message`, `exception`, `eof`.
- ✅ `run_start` env metadata: `cwd`, structured `argv`, `config` (from `TESTER_CONFIG` when CB spawns `test_runner`).
- ✅ `run_start.env` object for curated test-relevant environment (`NET_DISABLE_NETWORK_TESTS`, `CURSOR_SANDBOX`) when set — omitted when empty.
- ✅ `summary` includes `tests_ok`, `tests_total`, `assertions_ok`, `assertions_total`, `passed`.
- ✅ `tests_ok` derived from per-test assertion outcomes, so a non-fatal `check_*` failure cannot leave `tests_ok == tests_total` alongside a non-empty `failed_test_ids`.
- ✅ `failed_test_ids: [...]` in `summary` / `run_end`.
- ✅ `first_failure: { test_id, file, line, message }` for direct navigation.
- ✅ `slowest` JSON array in `summary` when `--slowest=N` is set.
- ✅ Compact summary/failures modes suppress case events, passing test rollups, duplicate `run_end`, and trace-only fields.

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
- ✅ `rebuild_reason` on `compile_end` when `cache_hit:false` (short kind: `not_in_cache`, `source_stale`, `pcm_stale`, `dependency_pcm_stale`, `profile_change`, …).
- ✅ Structured `rebuild` object on compile/link events (`kind`, `module`, paths, `hint`, `message`, optional `see_event`) plus `build_end.rebuild_summary` (`by kind` counts + `top_modules`).
- ✅ `profile_changed` event with `profile_diff` on profile mismatch (scalar fields + token diff on `compile`/`cpp`; not repeated on each `compile_end`).
- ✅ Compact CB modes suppress successful command/TU events and enrich `build_end` with compile/link/cache/failure totals.
- 📋 Surface compiler **warnings** from successful compiles. `diagnostics` is only attached when `ok:false`, so warnings on a TU that compiles reach neither stdout JSONL nor stderr — a project can accumulate them invisibly. Verified by replaying a `command_start` argv by hand: clang reports the warning, CB reports nothing.

### 3.7 Recommended automation invocation

```bash
./tools/CB.sh debug test --jsonl=failures --tags='\[module\]'
```

`--tags` and `--list` may be passed directly after `test` (no `--` required). Use `--` only for uncommon `test_runner` flags.

### 3.8 Additional report formats

- 📋 JUnit XML observer. Small against the existing observer contract, and it unlocks the native test UI on every major CI platform.

### 3.9 Output performance & shared code

- ✅ One JSONL implementation shared by both sides: `jsonl::escape` and the envelope helpers live in `tester/details/jsonl.h++`, included by `tester-jsonl_observer.c++m`, `test_runner.c++` and `tools/cb-jsonl_observer.h++`.
- 🔶 CB joins JSON string arrays through `join_json_strings`, but `tester-jsonl_observer.c++m` still hand-rolls index loops with `if(i) os << ','` in `write_string_array`, `write_string_map` and `write_failed_test_ids` — the pattern `AGENTS.md` explicitly prohibits. `write_failed_test_ids` also duplicates `write_string_array`.
- 📋 Batch the per-event `std::flush` in `trace` mode.

---

## 4. C++ Builder (cb.c++)

Design rationale and comparison with CMake, Make, and other build tools: [`docs/cb.md`](cb.md).

### 4.1 Core build system

- ✅ Incremental compile cache (`object_cache_map`) and link cache (`link_cache_map`); each level's compile decisions are completed against a stable cache before workers start, failed workers are joined, and cache indexes use checked temporary-file replacement.
- ✅ Object-cache profile header (`format=cb-object-cache-v3`) with toolchain fields (`config`, `static_link`, `llvm`, `cxx`, `cxx_sig`, `clang_ver`, `std_cppm` as `path@size:mtime_ns`, `compile` / `cpp` including `--compile-flags`); invalidates on profile mismatch (`rebuild_reason: "profile_change"`).
- ✅ CB smoke harness (`tests/cb/`) and CI `cb-smoke` job, including summary/failures/trace mode contracts.
- ✅ `cache status` subcommand (human + JSONL `cache_status`).
- ✅ `cache invalidate` subcommand (human + JSONL `cache_invalidate_end`).
- ✅ `profile_changed` JSONL event (single `profile_diff` on profile mismatch).
- ✅ Human-mode `profile_change` logging on stderr (non-JSONL).
- ✅ Profile value writer contract (verbatim tab-separated values; fields CB writes must not contain tab, newline, or `%`).
- ✅ Parallel compilation, topological module sort, preamble `import` scan in `cb.c++`.
- ✅ Header dependency tracking: compiles emit `-MMD -MF <object>.d` and a newer project header yields `rebuild_reason: "header_stale"` with the header in `rebuild.trigger_path`. Toolchain headers are excluded (already covered by `cxx_sig` / `clang_ver`).
- ✅ Bounded parallelism: `--jobs=N` caps concurrent compile/link processes via `std::counting_semaphore`, defaulting to `hardware_concurrency()`.
- ✅ Build diagnostics on stdout: failing commands are captured to a per-target file and a `diagnostics` object (`text` capped at 8 KiB, `path`, `bytes`, `truncated`) rides on `compile_end` / `link_end` / `command_end`.
- ✅ Decoded child status: `exit_code` / `signaled` / `signal` from the `std::system` wait status, raw value kept as `wait_status`.
- ✅ Strict argument parsing: unknown arguments exit `2` instead of being ignored (a `--tag=` typo no longer silently runs the full suite); a non-existent `.cppm` first argument is reported as such.
- ✅ CB / tester implementation policy: standard C++ + `std::system` only; stack traces in `test_runner` via `<execinfo.h>` (POSIX exception).
- ✅ `debug` / `release` configurations; `clean`, `list`, `ci`, `--build-tests`.
- 📋 Multiple custom configurations beyond debug/release (e.g. `asan`, `coverage`).
- 📋 Export compile/link graph as JSON for external tools.
- 📋 Emit `compile_commands.json` from `list` — nearly free given the existing argv builders, and it unblocks clangd. (Full CMake export stays optional and does conflict with the “zero config” philosophy; `compile_commands.json` alone does not.)
- 📋 Richer diagnostics on module dependency cycles and missing PCM.
- 📋 Support alternate module naming conventions beyond current `*.c++m` / `*.impl.c++` rules.
- ✅ Import scanning ignores non-code text. Comments, string literals (including multi-line raw strings with custom delimiters) and `#if 0` bodies are stripped from the preamble before the module regexes run, so a commented-out `import` no longer becomes a graph edge. Previously such an edge could close a loop through a real one and abort a valid build with `Cyclic dependency detected` — commenting out an import you used to have was enough to trigger it. Scanning stays regex-based and therefore compiler-independent (no `clang-scan-deps`), which matters for planned GCC support: one alternation covers comments and ordinary literals, raw strings are peeled with a linear scan (delimiter backrefs are not portable in `std::regex`), and `#if` depth is counted because balanced delimiters are not a regular language. Genuine `#ifdef` branches remain over-approximated by design. Costs ~2 ms per translation unit, mostly `std::regex` overhead rather than the pattern.
- ✅ Every spelling of the dead-branch idiom is elided, not just the bare `#if 0`. Three spellings leaked: parentheses hid the constant (`#if (0)`, `#if ((false))`); a short-circuited operand (`#if 0 && OLD_FEATURE`) failed the full-line directive match, so the line was not recognised as a directive at all — its body stayed live *and* the matching `#endif` left the depth off by one, which could suppress a later real region; and every `#elif` reopened the region regardless of its condition, reviving the second arm of `#if 0 / #elif 0`. A condition is now judged dead after whitespace and grouping parentheses are dropped, when it reads as `0`, `false`, or short-circuits from either (`0 && anything`), and a dead `#elif` arm is elided even after a live one. `!0` and `0 || X` stay live: over-approximating costs a spurious edge, guessing wrong drops a real one. A `[self]` smoke case pins all nine phantom spellings, and two live imports in taken arms of the same conditionals pin the other half of the contract.
- 📋 Reduce the preamble cleaning window. The cleaner processes up to `max_lines` (1000) per file while the deepest real declaration in this repo sits at line 18. A fixed cap is unsafe — a global module fragment may carry hundreds of `#include` lines before `export module` — so this needs proper end-of-fragment detection.
- 📋 Extract `translation_unit` / source scanning, the cache layer, and CLI parsing out of `cb.c++`. The single-file property is already gone; what remains is one very large file.
- 📋 Use `std::from_chars` in `parse_usize`; consider content hashing instead of mtime in the object cache.

### 4.2 Test integration

- ✅ Auto-link `test_runner` with discovered `*.test.c++` objects.
- ✅ Positional filter after `test` (substring on test id).
- ✅ Convenience forwarding to `test_runner` without `--` for: `--tags=`, `--list`, `--jsonl[=summary|failures|trace]`, `--jsonl-output-max-bytes=…`, `--slowest=…`, `--result`, and `--help`.
- ✅ Positional filter after `test` no longer consumes flags that start with `-` or known test_runner tokens.
- ✅ Assertion matcher contract tests (`tester/tester-assertions.test.c++`): pass and fail paths for relational, float, boolean, exception, container, string and messaging matchers, with failure paths run in a spawned child under hidden tags.
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
- ✅ Bootstrap watches `tester/details/*.h++` as well as `tools/*.h++`: `cb-jsonl_observer.h++` includes the shared JSONL header from outside `tools/`, so editing it previously rebuilt nothing.
- ✅ JSONL-safe wrapper logging (`cb_log` → stderr when `--jsonl[=summary|failures|trace]`).
- ✅ `NET_DISABLE_NETWORK_TESTS` sandbox hook only enabled in net wrapper (`CB_SANDBOX_DISABLE_NETWORK_TESTS=1`).
- 📋 Remove the hardcoded `--branch grok` from `CB.sh.core`.

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
- ✅ JSONL event schema reference — [docs/jsonl-schema.json](jsonl-schema.json) (JSON Schema 2020-12, per-`type` required fields and value constraints), enforced against live streams by [tests/jsonl/validate.py](../tests/jsonl/validate.py) in CI.
- ✅ macOS `/usr/local/llvm` setup guide — [clang-modules-macos.md](clang-modules-macos.md) ([LLVM Getting Started](https://llvm.org/docs/GettingStarted.html); [#92121](https://github.com/llvm/llvm-project/issues/92121), [#168287](https://github.com/llvm/llvm-project/issues/168287#issuecomment-3712718691)).

### 6.3 Automation guide

- ✅ `AGENTS.md` at repo root (canonical JSONL commands, triage workflow, event reference for agents/CI).
- ✅ MCP stdio bridge `tools/cb_mcp.py` + `.cursor/mcp.json` for CB agent tools (`cb_list` / `cb_build` / `cb_test` / …); smoke `./tests/mcp/smoke.sh`.
- 📋 Verify the MCP bridge against a spec-compliant client, and switch to newline-delimited stdio framing if that confirms a mismatch. The smoke test currently mirrors the bridge's own framing rather than exercising the wire format independently.
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

## 9. Code style & tooling

- 📋 Add `.clang-format` / `.clang-tidy`, and normalize `!` / `&&` against `not` / `and` in `tester-assertions.c++m` (the file mixes both today).

---

## Priority sketch

Ordered by consequence, not by effort. Items marked **verified** were reproduced against the current tree — the reproduction is recorded in the linked section so the next person need not rediscover it. Everything above `Low` is open; completed work is marked ✅ in its own section rather than repeated here.

| Priority | Item | Rationale |
|----------|------|-----------|
| Medium | Warnings from successful compiles are invisible (§3.6) | **Verified:** clang reports them, CB attaches them to no event, so they reach neither JSONL nor stderr — warnings accumulate unnoticed |
| Medium | Unresolved `depends_on` / duplicate test ids (§2.3) | Silently ignored today, so a typo'd dependency looks like a passing run |
| Medium | JUnit XML observer (§3.8) | Cheap against the existing observer contract; unlocks CI test UIs |
| Medium | `compile_commands.json` from `list` (§4.1) | Nearly free given existing argv builders; unblocks clangd |
| Low | Death tests, regex / predicate matchers, container prefix-suffix (§1.2–1.5) | Framework parity. Death tests are cheaper than they look — the spawn and signal-decode helpers already exist |
| Low | Index-loop joins in tester's observer (§3.9) | Violates the project's own `AGENTS.md` rule; no behavioural impact |
| Low | Decompose `cb.c++` (§4.1) | Large refactor with no user-visible change; the smoke suite is strong enough to support it when desired |
| Low | `--shuffle` / `--seed` / `--repeat`, timeouts (§2.5) | Nice-to-have; flushes out inter-test coupling |
| Low | `cache prune` (§4.4) | Only if disk bloat or orphaned artifacts show up in practice |

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