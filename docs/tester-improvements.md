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

- ✅ Mixed-signedness comparisons compare mathematical values. Comparing through `std::common_type_t<int, unsigned>` (which is `unsigned`) converted the signed operand, so `check_eq(-1, 4294967295u)` reported **equal** and `check_lt(-1, 1u)` reported **not less**. The six relational matchers now route mixed signedness through the `std::cmp_*` family, which is correct rather than rejecting the comparison with a `static_assert`. Equal values still match across signedness, so `require_eq(v.size(), 3)` keeps working; same-signedness comparisons are untouched; and every integral pair takes this path. Operands are still reported as written, not as converted.
- ✅ Promotion closes the same wraparound for `bool`, the character types and unscoped enumerations. Excluding them from `std::cmp_*` — which is specified over the signed and unsigned integer types, not the integral ones — left `check_eq(char{-1}, 4294967295u)` and `check_gt(char{-1}, 0u)` reporting the converted answer, exactly the operands the exclusion named. Integral promotion turns each of them into a type `std::cmp_*` accepts, and unary `+` is that promotion, so the comparators promote before comparing and the seven-line exclusion list is gone. `check_eq('a', 97u)` and `check_eq(true, 1u)` still pass. An enumeration compared against its own type is left alone, since promoting both sides would bypass an `operator==` written for the enum; a `[self]` test pins that with an enumeration whose comparison disagrees with its values. Two *different* unscoped enumerations take `cmp_*` as well — neither operand is `std::integral`, so the old trait missed them and `check_eq(Signed{-1}, Unsigned{max})` wrapped through `common_type` the same way enum-vs-unsigned did.
- ✅ Composites are compared member by member, and read as their members. Two heterogeneous pairs have a common type of their own, so comparing through it converted every member the way scalars were converted before `std::cmp_*`: `check_eq(std::pair{std::string::npos, 0}, std::pair{-1L, 0})` reported equal, a silent false pass. The ordering matchers had the mirror of it — `check_lt(std::pair{-1, 0}, std::pair{4294967295u, 0})` reported false — so fixing only equality would have left the two disagreeing about the same pair. Equality folds `values_equal` over the members; ordering is lexicographic on `values_less`, with no equality in the tie-break, which keeps the epsilon out of ordering exactly as it is for scalars. Tuple-likeness is spelled as the protocol (`std::tuple_size`), so `std::array` and user types are covered. Reporting followed: a composite was named by type, so a failure said nothing about which member differed, and containers of composites did not compile at all because the matchers formatted elements with `<<`. Both now go through `value_to_display_string`, which renders `(18446744073709551615, 0)` and recurses, so a character member reads `'a' (97)` inside a tuple.
- ✅ `std::optional` and `std::expected` unwrap to the contained value before comparing. They are not tuple-like, so the composite walk never reached them. Their `common_type` with a bare integer is the wrapper itself, and constructing that wrapper from the integer reintroduced the wraparound: `check_eq(std::optional<int>{-1}, 4294967295u)` reported equal, as did the same shape with `std::expected` and `check_contains` over a vector of optionals. Engagement is checked first (matching `std::optional` / `std::expected`'s own rules); engaged values go through `values_equal` / `values_less`. Reporting follows: a failure shows the contained value or `nullopt` / `unexpected(...)`, not the demangled type name.
- ✅ Remaining wrappers that reintroduced the wraparound unwrap the same way: `std::reference_wrapper` via `get()`, `std::chrono::duration` via a common-duration `count()` compared with the scalar rule (mixed-signed reps otherwise build an unsigned common type), `std::unexpected` via `error()`, and `std::atomic` via `load()`. Without that, `check_eq(std::ref(n), 4294967295u)`, `check_eq(duration<int>{-1}, duration<unsigned>{max})`, and container matchers over `unexpected` / `atomic` still passed. Cross-period floating reps keep chrono's floating `common_type` — widening those through `intmax_t` is ill-formed and broke `check_eq(1.5ms, 1500us)`.
- ✅ `std::chrono::time_point` unwraps to epoch tick counts before comparing. It is not duration-like, but two points whose duration reps differ in signedness still share a `common_type`: `equal_to` / `operator==` then compare epochs through an unsigned common duration and wrap `-1` into max. Same-period counts go through the scalar rule directly; different periods mirror durations (floating `common_type`, else `intmax_t` widen). Reporting shows the epoch count.
- ✅ Two operand formatters instead of three. `tester:console_observer` carried a line-by-line copy of `value_to_display_string`, with a comment saying that any change to how an operand reads belonged in all three places — the composite branch would have been the third copy. It delegates now, so the console and the display string cannot drift; `observe_value` still decides the JSONL `value` and `kind` separately, because that channel answers a different question (a number must not arrive quoted).
- ✅ Container matchers compare elements by the scalar rule. `compare_containers` used `*a == *b` and the container overload of `check_contains` used `std::ranges::contains`, so both kept converting after the scalar matchers had stopped: `check_contains(std::vector<std::size_t>{std::string::npos}, -1)` passed and so did `check_container_eq({-1}, {4294967295u})`. Elements now go through `numeric_equal_to` on their common type, which is one rule rather than two — the floating-point epsilon reaches container elements as a consequence, so `require_container_eq({0.1 + 0.2}, {0.3})` passes as `check_eq` always did. Element types with no common type keep their own `operator==`.
- ✅ Character operands are reported by value, and the JSONL line parses again. Streamed as itself a character went into a field the schema calls a number, so a failing `check_eq('a', 98u)` emitted `"actual":a`, and a negative `char` emitted a stray `0xff` byte that is not valid UTF-8 — an unparseable line, which is the whole contract with agents. Quoting instead would lose the value, since escaping replaces a stray byte with U+FFFD, and `char{-1}` is exactly the operand a mixed-signedness failure reports. JSONL now carries `97` and the console carries `'a' (97)`. The wide character types also become reportable at all: C++20 deleted their narrow stream inserters, so the old branch was ill-formed for them.
- ✅ One comparator per matcher, so every relational matcher is the same one-line shape. `check_eq` used to branch on floating point in the wrapper and cast its operands, while the other eleven passed operands through untouched; the type-dependent rules now live inside the comparators. The cast was redundant, because the comparator converts in its parameters and display uses six-digit `operator<<` either way. `check_near` / `require_near` keep their cast deliberately: `T` is deducible from the epsilon argument, so `check_near(1.0f, 2.0f, 0.1)` makes `T` double while `common_type_t<A,E>` is float.
- ✅ `check_neq` no longer contradicts `check_eq` on floating point. `check_neq` compared exactly while `check_eq` compared within an epsilon, so a pair inside the tolerance satisfied both matchers at once — the same class of defect as the mixed-signedness pass. Inequality is now the negation of the epsilon comparison. Ordering stays exact, as in every comparable framework.
- ✅ Assertions take operands by **forwarding reference**, so nothing is copied and move-only types are comparable. By value cost one copy per operand per layer: a copy-counting type measured **six copies per assertion**, two each in the wrapper, the hub and the comparators. A plain `const&` does not work — deduction against it drops the const of a string literal's element type, making `A` become `char[4]`, whose common type is `char*` and cannot hold the `const char*` the literal decays to, so `check_eq("abc", "abc")` stops compiling. Forwarding references preserve it, and `std::common_type_t` already decays, so every comparator still sees the same `T` and the hub signature is unchanged. Operands are only ever read: the comparators and `report_assertion` take them by `const&`, so nothing is forwarded onward. A `[self]` test pins zero copies and zero moves across seven assertions, plus a `std::unique_ptr` comparison.
- ✅ Unprintable operands are reported by demangled type name. Two `typeid(...).name()` sites reported the mangled spelling (`N6tester8selftest10assertions12_GLOBAL__N_17countedE`) while exception types already went through `demangle_type`; this surfaced the moment move-only and non-streamable operands became comparable.
- ✅ An infinity or a NaN operand is quoted, so the line still parses. Every floating-point operand went out as a JSON number, but JSON has no way to write either value: `require_eq(infinity, infinity)` emitted `"actual":inf` and the NaN case emitted `nan`, and a parser rejects the whole line rather than the token, so one float assertion cost a consumer the entire event. They are now `"inf"` / `"-inf"` / `"nan"` and finite operands stay numbers — `null` would have parsed while discarding which of the three it was, and identity is precisely what a float comparison reports when it fails on something other than tolerance. The gap was in the validator's reach, not only in the writer: `tests/jsonl/validate.py` ran trace mode — the only mode carrying operands from *passing* assertions — over `[self][harness]` alone, so the framework's own float tests were never parsed. It now runs the whole `[self]` suite in trace, plus a `[.jsonl-nonfinite-probe]` for the failure path, and a `[self]` test pins the quoted forms next to a finite operand.

### 1.8 Matcher naming

- 📋 Rename `check_neq` / `check_lteq` / `check_gteq` to `_ne` / `_le` / `_ge` (keeping aliases), and reconsider `succeed` / `failed` / `warning`.

---

## 2. Test runner & BDD

### 2.1 Tag filtering

- ✅ Regex and substring tag filters via `--tags=`.
- 📋 Document bracket-tag convention (`[module]` in scenario names) in one canonical example table.
- 🔶 Tags are data on the case, but they are still authored in the name. `extract_bracket_tags` used to run on the rendered display name every time a decision needed them — once per case in `has_hidden_tag`, again in `match_literal_tags`, and a third time in the catalogue — so filtering read text the framework had itself composed, and three call sites had to agree about what a tag is. `test_case::tags` is filled once at registration from the description, and the filter, the hidden-tag check and `registered_test` read it; `runner::included` keeps a name-only overload for callers holding just a string (`examples/test_regex.c++`), which delegates to the same two-argument core after parsing. What remains is the authoring half: `test_order{ .tags = {…} }` instead of brackets in the name, plus OR (`[a],[b]`) and negation (`~[a]`) in the query language.
- ✅ `test_runner --list --jsonl=failures`: `test_list_start`, `registered_test`, `test_list_summary`.

### 2.2 BDD ergonomics

- ✅ A step — `given` / `when` / `then` and their `and_` forms, or `section` — runs at its assignment, inside the body that declared it, so `[&]` is safe and each step sees what the ones before it did. Queuing the step instead ran it after that body had returned, which left every reference capture dangling and could only be documented around: `examples/readme_bdd_example.test.c++` shared its state through a `shared_ptr` for no reason other than this. What the runner reports is unchanged — a step is still its own test case, listed after the case it belongs to, counting the assertions it made itself and failing alone — and the [self] suite pins both halves: a scenario whose steps hold its locals by reference, and a spawned probe where one step fails and its sibling still runs.
- ✅ The insert-position search went with it. A queued step used to be placed in the run list by scanning the pending names for BDD keywords, to approximate the order the steps were written in; the call stack gives that order exactly.
- 📋 Optional flat `test_case` + `section` guidance in README for non-BDD modules.

### 2.3 Dependencies & ordering

- ✅ `priority` and `depends_on` fields on `test_case`.
- 📋 Expose dependency graph in `--list` output.
- ✅ Fail fast with a clear message when a dependency cycle is detected: a re-visit while ancestors are still resolving throws `Cyclic test dependency involving "<id>"` instead of recursing until the stack overflows.
- ✅ Ordering metadata that cannot mean what it says stops the run. Two cases claiming one id used to overwrite each other in the map `depends_on` resolves through, so an edge pointed at whichever registered last and the run still looked ordered; a `depends_on` id nothing registered contributed no edge and no diagnostic, so a typo read as "no ordering constraint" and passed. Both now throw before the first test runs, from the place the silence was — the id map build and the level walk — and both messages name the id, since which one collided or went missing is the whole diagnosis. `label()` picks the id over the display name for all three messages, cycles included. The suite already had no duplicates, standalone or with examples, so nothing had been relying on the old behaviour; two spawned probes (`[.duplicate-id-probe]`, `[.unknown-dependency-probe]`) pin exit code 1, no signal, and the message text.
- ✅ Nothing to re-sort after a step registers: only `scenario` and `test_case` accept a `test_order`, and a step now runs where it is written rather than joining the list the sort had already put in order.
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
- ✅ `rebuild_reason` on `compile_end` when `cache_hit:false` (short kind: `not_in_cache`, `source_stale`, `header_stale`, `depfile_unusable`, `pcm_stale`, `dependency_pcm_stale`, `profile_change`, …).
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
- ✅ Header dependency tracking: compiles emit `-MMD -MF <object>.d` and a newer project header yields `rebuild_reason: "header_stale"` with the header in `rebuild.trigger_path`. Toolchain headers are excluded (already covered by `cxx_sig` / `clang_ver`). A depfile that cannot be read yields `rebuild_reason: "depfile_unusable"` rather than a cache hit: the `.d` is the only record of a unit's textual includes, so an unreadable one is not the same answer as "this unit includes no headers", and conflating them held a stale object across header edits after an upgrade or a wiped `obj/`.
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
- ✅ Import scanning ignores non-code text. Comments, string literals and `#if 0` bodies are stripped from the preamble before the module regexes run, so a commented-out `import` no longer becomes a graph edge. Previously such an edge could close a loop through a real one and abort a valid build with `Cyclic dependency detected` — commenting out an import you used to have was enough to trigger it. Scanning stays regex-based and therefore compiler-independent (no `clang-scan-deps`), which matters for planned GCC support: one alternation covers comments and literals, and `#if` depth is counted because balanced delimiters are not a regular language. Genuine `#ifdef` branches remain over-approximated by design. Costs ~2 ms per translation unit, all of it `std::regex` overhead rather than the pattern.
- ✅ The scanner splices continued lines before it reads anything, as translation phase 2 does. The cleaner had learned to follow a splice through a string literal and a `//` comment, but a splice can appear anywhere, and the directive filter still read physical lines: `#if \\` on its own line was an unrecognised directive rather than a dead region, so its body stayed live and could point a phantom edge at a module that exists, closing a loop through the real edge and aborting a valid project with `Cyclic dependency detected`. The other direction is worse and was not in the report: `import \\` / `helpers;` is a real edge the scanner did not see at all, and a missing edge builds in the wrong order instead of failing loudly. `splice_physical_lines` now runs first and deletes a backslash, the horizontal whitespace C++23 allows after it, and the newline — so the cleaner and the directive filter see what the compiler sees. Doing phase 2 once let the `//` and quoted-literal branches go back to stopping at the newline, and the scan came out no slower than before (`list` on this repo: 129 ms against a 138 ms baseline, min of twelve). It is one `regex_replace` over the preamble, raw-string bodies included — see the scope decision below.
- ✅ The splice pass stays a pattern, and a raw-string body that needs a tokenizer is out of scope. [lex.pptoken] reverts phase-2 splices inside a raw string before the closer is identified, and honouring it was tried twice. Copying the body verbatim once an `R"(` appeared treated a **mention** of `R"(` as an opener — in a `//` comment, a block comment, or an ordinary literal — and an unclosed mention froze every later splice to the end of the file: the dead `#if \` body came back as a phantom edge and a real `import \` stayed split, which is both of the bugs the splice pass was added to fix, arriving from the other side. Gating the reversion on lexical state fixed that and cost a five-state walk over every character, plus a second description of comments and literals that had to agree with the cleaning regex about where each one ends. That is a tokenizer, and this scanner is regex-based by design, so the reversion is gone: a raw string whose body ends a line with `)\` is read as closing one line early and may contribute a phantom edge, the same over-approximation `#ifdef` branches already get, and a mention of `R"(` in a comment or literal — much likelier text — is inert. `smoke.sh` pins the mention rather than the reversion.
- ✅ Every spelling of the dead-branch idiom is elided, not just the bare `#if 0`. Three spellings leaked: parentheses hid the constant (`#if (0)`, `#if ((false))`); a short-circuited operand (`#if 0 && OLD_FEATURE`) failed the full-line directive match, so the line was not recognised as a directive at all — its body stayed live *and* the matching `#endif` left the depth off by one, which could suppress a later real region; and every `#elif` reopened the region regardless of its condition, reviving the second arm of `#if 0 / #elif 0`. A condition is now judged dead after whitespace and grouping parentheses are dropped, when it reads as `0`, `false`, or short-circuits from either (`0 && anything`), and a dead `#elif` arm is elided even after a live one. `!0` and `0 || X` stay live: over-approximating costs a spurious edge, guessing wrong drops a real one. A `[self]` smoke case pins all nine phantom spellings, and two live imports in taken arms of the same conditionals pin the other half of the contract.
- ✅ One atomic cache writer instead of three. `save_object_cache`, `save_executable_cache` and `save_std_module_profile` each spelled out the same temp-file / write / close-check / rename / remove-on-error sequence — 87 lines for three callers whose only real differences are what they write and what the file is called in the message. `write_cache_file(path, what, writer)` owns the sequence, so all three fail identically and a fourth cache cannot be added with a subtly different failure mode; the executable cache's "empty means delete" branch turned out to be `remove_if_exists`, which already existed.
- 📋 Fold the three operand formatters into one. `value_to_display_string` and `observe_value` in `tester:utils` and `format_value` in the console observer carry the same branch list, and the character fix above had to be applied in each — the second and third copies were found only because a `[self]` test and then the console output disagreed with the first. The console copy exists because the traits it needs are internal to `tester:utils`.
- 📋 Make the CB smoke harness give up its work directories when a case fails. `begin_case` / `end_case` clean up on the happy path, so a failed case or an aborted run is the leak — and each one holds a full build tree. 860 of them had collected in `$TMPDIR` and filled the disk, after which every run died at the first case with exit 1 and no message, since the harness reports assertion failures but not a build that could not write. Keeping the tree on failure is worth having; abandoning it silently is not.
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
| `cache invalidate` | Delete the four files under `cache/` only — lighter than `clean`; next build treats everything as uncached without wiping artifacts. JSONL: `cache_invalidate_end`. ✅ |
| `cache prune` | Garbage-collect artifacts the current module graph no longer references; trim cache index rows for missing paths. Optional `--aggressive` after `profile_change` to drop all `obj/` / `pcm/` under the config. JSONL: `cache_prune_end` with counts. |

**Implementation sketch:** scan current TU graph → expected `{obj, pcm}` set; walk `obj/` and `pcm/`; delete files not in set; reconcile `object-cache.txt`. Reuse existing `object_cache_profile()` / `parse_object_cache_profile_fields()` for `status`.

**Smoke / CI:** add `cache_prune` cases only if the subcommand ships.

### 4.5 Observer contract shape

- ✅ Phase events carry value types instead of positional tails. `compile_end` took ten arguments, four of them indistinguishable `string_view`s and two adjacent interchangeable `bool`s, and the base declaration left them unnamed, so neither the contract nor the call site said which was which. It now takes `compile_unit` (the four fields observers format, projected out of `translation_unit` by `compile_unit_of`) and `step_result` (`ok`, `cache_hit`, `interval`, `rebuild_info`, `diagnostics`), filled with designated initializers at the call site. `link_end` went from seven arguments to two, `compile_start` from five to two, and the `started`/`finished` pairs in `build_end`, `test_end`, `command_end`, `compile_end`, and `link_end` became one `interval` with `elapsed_ms()`, which also removed five copies of the same `duration_cast`. All 181 trace events of a full build stayed byte-identical.
- ✅ Both observable projections out of a translation unit are named builders: `compile_unit_of` for compile events and `source_unit_of` for the inventory. `source_unit` used to be filled by a nine-field positional aggregate — four same-typed strings and three adjacent bools, transposable in silence — inside the `list_sources` loop, along with the display-path join. `source_inventory` is built with designated initializers too, which is why its members carry defaults: an omitted field then warns under `-Wmissing-designated-field-initializers` unless it is meant to be filled later, as `units` is.
- ✅ No `emit_` wrappers remain in `cb.c++`. `emit_compile_start`, `emit_compile_end`, and `emit_link_end` existed because the interface was positional: their whole body was mapping a unit onto unnamed arguments, including two copies of the `is_modular ? pcm_path : {}` projection that could have disagreed. Call sites now notify directly, as the other ~40 already did.
- Note for future methods: the `= {}` defaults these methods carried were dead. `notify` calls through a pointer to member, and those never apply default arguments, so every argument must be passed — which is why the optional tail belongs in a struct with defaulted members rather than in the parameter list.
- ✅ The build's emit-once latch is a scope, not producer state. `build_scope` emits `build_start` on construction and exactly one `build_end` on the way out — success only if `succeeded()` was reached, failure otherwise, including during unwinding. That deleted the `build_phase` enum, the `current_phase` / `phase_started` / `build_end_emitted` members, `emit_failed_build_end`, and the catch-and-rethrow blocks in `build()` and `run_tests()`, which each had to remember the same five-step ritual. The latch deliberately did not move into the observers: each would have to reimplement it and they could then disagree about whether the build ended. The invariant was already pinned by the smoke harness (`single_failed_build_end` asserts one `build_end` with `ok:false`, plus the link-failure and test-link-failure cases), and the failed-build event order is unchanged — `build_end` still precedes `cb_error`, now because unwinding runs the destructor before the handler in `main`.
- ✅ `emit_profile_changed` was a predicate wrapper with one caller: a function named `emit_` that usually emitted nothing. The condition now reads at the call site in `compile_units`.
- ✅ Reading the object cache returns everything it learned. `load_object_cache` used to return the entry map and leave two more answers behind in `build_system` members — `object_cache_miss_reason` and `profile_diff` — which the single caller collected on the next line and `needs_recompile` read to attribute a miss to `profile_change`. The members had to be reset at the top of the load because the channel outlived the call. It now returns an `object_cache_load` holding `entries`, `miss_reason` and `profile_diff`, and `needs_recompile` takes that instead of the bare map, which is one parameter for one.
- ✅ How a spawned process ended is one contract type. `shell_result` was declared privately in `cb.c++` although both its members — `process_status` and `diagnostics` — were already contract types, so `command_end` and `test_end` took it apart and passed `ok()`, `status` and `diag` as separate arguments. The `ok` among them was not an independent fact but `status.ok()`, which meant the interface admitted an event claiming success beside a status reporting a signal. `output::process_result` now travels whole: `command_end` is down from six arguments to four, `test_end` from three to two, and `invoke_shell` returns the type the event carries. It also gave `diag` a default member initializer, which retired a `-Wmissing-designated-field-initializers` warning at the `invoke_shell` construction site.
- ✅ Compiles and links pair their events with a scope, the way the build already did. `compile_scope` emits `compile_start` and exactly one `compile_end`; a modular unit compiles in two steps and either can fail, so each exit path used to carry its own copy of the event and had to agree with the others about the unit, the reason and the clock. `link_scope` is the exit half only — there is no `link_start` — and it replaced three copies of `link_end` plus a `linked` bool whose only job was stopping the catch-all from emitting a second one, which is an emit-once latch written as a local variable. `link_executable` then had nothing left to return: it handed back `output::diagnostics` that was always `{}`, because the linker's output travels on `link_failure`.
- ✅ Compiling and linking share the runner instead of two copies of it. Both phases spawned a `jthread` per job behind a `job_gate`, tracked `failed` / `failure` / `failure_mutex`, checked the flag before and after acquiring a slot, joined everything, then rethrew the first exception — about twenty-five lines of the same concurrency, and the two copies had to agree about the relaxed loads, the recheck, and which exception survives. `run_in_parallel(jobs, limit, work)` owns that; the callers keep what actually differs — levels and the object cache on one side, link signatures on the other — and the failing link now attaches the linker's output to its `link_end` and rethrows rather than recording the failure itself. Cache hits are reported in their own pass before the workers start, which is the one visible change: a level's hit events now precede its compiles instead of interleaving with thread spawning, an order that was never deterministic.
- ✅ Compilation and Linking are the same two halves, and each section holds only what its phase uses. `link_executable` / `link_executables` already mirrored `compile_unit` / `compile_units`, but the Linking section also held three members the phase does not own: `linkable_object_paths` (also read by `link_test_runner_argv` and `compute_test_runner_signature`) moved beside `collect_module_ldflags`, the other shared projection over the unit list, and `dependency_signature` / `dependency_signatures_joined` moved beside the executable cache they serve, which the test-runner link reads too. Linking now reads as types, then one executable, then what identifies a link, then the pass, and its relink decisions are built by the same `filter | transform | ranges::to` pipeline as the compile decisions instead of a push_back loop. Both banners say the phases mirror each other and that the shared half lives in Cache Management and General Utilities.
- ✅ The test outcome is reported where the run is, not where the command is. `run_tests` ends on `return result.ok();`: `test_scope` raises the failure as an `error` after `test_end`, which is where the JSONL stream's `cb_error` after a failed run has always come from, and `test_failure_message` in `cb-observer.h++` composes the sentence beside the rebuild sentences that moved there for the same reason. The guard is that only a *reported* outcome raises it — on the way out of a throw the failure belongs to somebody else and the handler in `main` reports it, so an unwind emits `test_end` with `ok:false` and exactly one `cb_error`, the real one. Wire byte-identical on every reachable path, including the signal branch no fixture exercises, which was checked by calling the message function directly. Compiling that check standalone also surfaced that `cb-observer.h++` used `std::ranges::any_of` without including `<algorithm>` — it only ever compiled because `cb.c++` includes it first.
- ✅ The passing-run sentence moved to the console observer, the failing ones did not. "All tests passed!" is console prose with no wire counterpart, so `cb-console_observer::test_end` renders it from `result.ok()` and `run_tests` ends on `return true` — the same move as the skipped-link line. The failure sentences stayed in the command deliberately: they go out as `error`, which the JSONL observer turns into `cb_error`, so moving them would silently delete a wire event. Whether a failing test run should emit `cb_error` at all is a separate question — `AGENTS.md` calls it "CB fatal/diagnostic" and a failing runner is a normal outcome — but that is a wire change, not a refactor, and `test_end.ok:false` already carries the fact for anyone parsing stdout.
- ✅ `test_scope` completes the set: every start/end pair in `cb.c++` is now stated by a scope rather than by two calls with code between them. This one is prevention rather than repair — nothing between `test_start` and `test_end` throws today, since a failing runner comes back as a status and the throwing statements (`setenv`, the missing-runner check) sit above the start — so the gain is that the interval belongs to the scope instead of two locals, and that the property survives a step being added later. The default `process_result` reports exit `-1`, so an unwind reports the run as failed rather than dropping the event: verified by throwing between the two events behind an env guard, which emits `test_end` with `ok:false` before `cb_error`. Events are otherwise byte-identical to the pre-scope code on both a passing and a failing run.
- ✅ One link-signature format instead of two. `compute_link_signature` and `compute_test_runner_signature` each built the same four-field flag tail (`|flags=|link=|modules=|imports=`) by hand, so a fifth field would have had to be added twice or the two caches would have disagreed about what invalidates them. `link_signature(input_paths, import_flags)` in Cache Management owns the format; each caller supplies only its input list, which is the sole real difference. The test-object list was likewise spelled out twice (argv builder, signature) with the `is_test and not has_main` predicate written a third time in `collect_test_module_ldflags`; `test_units()` and `test_object_paths()` hold it once. Cache entries are byte-identical to what the previous code wrote — verified by relinking both executables and diffing `executable-cache.txt`.
- ✅ The test-runner link reads like the link phase it is. It was one 65-line function doing everything inline: validating the runner unit with the same predicate written twice for `count_if` and `find_if`, computing two flag lists with `collect_module_ldflags(runner->imports)` called twice, deciding, linking, and saving. It is now the same three parts as `link_executables` — `test_runner_unit` names the unit, `link_test_runner_executable` is the counterpart of `link_executable` and owns the scope, and `link_test_runner` decides, reports a hit or links, and saves the signature. Both the argv builder and the signature take the `translation_unit` directly; the side bag `test_runner_inputs` went away once the runner stopped being optional. The console prints the rebuild reason before the success line now rather than after, which is the order the executable links already read in; `info` and `success` never reach the JSONL observer, so the stream is unchanged.
- ✅ `link_failure` is gone, and the answer was fewer exception types rather than one more. It existed to carry `diagnostics` from `link_executable` up to the worker that owned the `link_scope` — a boundary `compile_unit` does not have, since it owns its own scope and attaches the compiler's output at the throw site. `link_executable` now takes the rebuild reason and owns its `link_scope` too, so it throws a plain `std::runtime_error` like the compile path and the two work functions read the same: open the scope, run the toolchain, `failed()` and throw or `succeeded()`. Both phase workers reduce to the call plus the cache write.
- ✅ The test-runner link reports through `link_scope` like the other two. It was the third place open-coding `link_end` — a hit pair, a failure pair and a success pair — and the only one left holding its own "Skipping link (up-to-date)" `info`, which printed twice for `test_runner` once the console observer started rendering hits from the event. Missing that second call site is the argument for the scopes: while two places produce an event, a change to one of them is a change to half the behaviour.
- ✅ The build no longer writes console prose about skipped links. `link_executables` emitted an `info` saying "Skipping link (up-to-date)" beside the `link_end` that already carried `cache_hit` and the path, so a formatting decision — which steps deserve a line — sat in the producer, and only the console observer ever saw it (the JSONL observer does not override `info`). The console observer renders it from the event now, and its comment records why compiled hits stay silent: one line per up-to-date unit would be dozens saying so, while one or two executables would otherwise vanish from the output entirely. Console output on a cached build is byte-identical.
- ✅ Cache hits report through the same scope, so one type owns each event pair. `compile_scope` and `link_scope` gained a hit constructor — no reason, no duration, `ok` from the start, since nothing runs that could fail — and the two branches that open-coded the notifies are a single declaration whose destructor does the reporting. The hit's timing stays zero by construction rather than by measuring an interval that happens to round to 0 ms, which is what keeps the wire identical: all 71 cache-hit events of a fully cached build match byte for byte.
- ✅ `update_module_flags` sits with the flags it finishes rather than under `// Compilation`, where it was the only member that compiled nothing. It is the second half of `initialize_build_flags`: that seeds `module_flags` with four entries, this appends one `-fmodule-file=` per modular unit, and they are split only because the units are unknown until `scan_and_order` has run.
- ✅ A test runner without a main is not a configuration, so the lookup returns a reference and says so. `find_test_runner_unit` returned `const translation_unit*` and `nullptr` meant "link the test objects on their own", which reaches the linker and dies with `undefined symbol: main` after a full build, under twenty lines of clang command — the answer was already known before the first compile. `test_runner_unit()` throws the sentence `run_tests` used to check for afterwards, so both misconfigurations (test sources with no runner, and a project with no tests at all) report "test_runner not found — make sure .test.c++ files or test_runner.c++ exist". Three conditionals went with it: `has_test_runner_link_inputs` existed only to ask whether a runner-less project had anything to link, `test_runner_inputs` carried a `has_runner_unit` flag that chose between two success sentences and two flag lists, and the argv builder and the signature both guarded an object path that is now always there. The post-link existence check in `run_tests` is gone too — the link either produced the runner or threw.
- ✅ The std module reports like the unit it is. `build_std_pcm` / `build_std_o` built through `execute_system_command`, so on a cold build the two most expensive steps emitted a bare `command_end` — no `compile_end`, no `cache_hit`, no `rebuild_reason`, nothing in `rebuild_summary` — and on a warm build they said nothing at all rather than saying they were up to date. In `--jsonl=trace` every scanned unit explained its decision while the ones that dominate the wall clock explained nothing, and a failing std build reached stdout as `ok:false` with the compiler's output attached to no compile. They are now one `compile_scope` for module `std`: `needs_std_module_rebuild` is its `needs_recompile`, returning the same `std::optional<rebuild_info>` with the reasons a modular unit already has (`own_pcm_missing`, `profile_change`, `own_pcm_stale` reach the pcm and rebuild both artefacts; `object_missing` and `object_stale` reuse the pcm that is there), and `build_std_module` is its `compile_unit`, running the steps through `run_step` so a failure attaches `diagnostics` like any other compile. The console gained the sentence it was missing — "Rebuilding std.cppm because object file is missing" — and `compile_total` now counts what CB actually compiled, which is why every smoke `assert_compile_cache_hits` moved by one. The reasons were checked against all five states: cold, warm, deleted `std.o`, touched `std.pcm`, and a changed `--compile-flags` profile; the two object-only states run one step, pinned by a smoke case asserting no `--precompile` appears.
- ✅ The two cache subcommands describe the same four files, in one value each. `cache_status` took seven positional arguments and described two files: it detailed the object cache, counted executable-cache rows, and never mentioned `std-module-profile.txt` or `compiler-version.txt` — while `cache_invalidate_end` reported three removals out of four, because `remove_if_exists(std_module_profile_path())` discarded its result. The silent one is the one that matters: deleting the std module profile is what makes the next build rebuild `std.pcm`, so `cache invalidate` had a headline effect that `cache status` could not show and its own event did not report. `cache_inventory` and `cache_removals` replace the argument lists — the same DTO move as `compile_unit` / `step_result`, and the reason an eighth `bool` did not join six others — and both commands now read from one description of the cache, with entry counts and profile matches printed only where they exist. `cache status` reporting the compiler stamp as present straight after an invalidate is real and documented: `current_profile` requires probing `clang++ --version`, and that probe is what writes the stamp.
- 📋 `update_module_flags` deduplicates against `module_flags` while `append_range` appends to it — a lazily evaluated predicate reading the container the same call is growing, which is the aliasing `append_range` is specified to avoid. The filter also cannot fire: the flag is a function of module name and pcm path, and `scan_and_order` already rejects duplicate pcm paths and reserves the std module's.
- ✅ The latch has one owner. `publisher::print_test_statistics(std::optional<bool>&, bool&, std::string_view)` took the runner's private bookkeeping by mutable reference so a free function in another module could flip it — two owners for one decision. The runner now emits `run_end` itself from a single `report_run_end`, whichever path decided the verdict: `end_run` calls it directly outside `"run"` mode, and `report_summary` calls it after the aggregates when the run deferred it.
- ✅ `rebuild_info` carries facts, and the prose is derived from them. It was `kind` plus five paths plus two computed fields: `hint`, a pure function of `kind`, and `message`, a sentence the build system composed and both observers relayed. `rebuild_hint`, `compile_rebuild_message` and `link_rebuild_message` now live in `cb-observer.h++` and are called at write time — the way `see_event` already worked — so a producer can no longer ship telemetry whose prose disagrees with its fields, and the sentences can be tested without running a build. `rebuild_info` is down to `kind`, `module`, `pcm_path`, `trigger_path`.
- ✅ The nested `object_path` came from the event rather than the reason: it was always the compiled unit's object and always empty for links, so `write_rebuild` takes it from `compile_unit` and `finalize_rebuild` had nothing left to do. `make_rebuild` went with it — a four-parameter factory with three same-typed defaulted strings, called seven times as `make_rebuild(kind, {}, {}, path)`. Designated initializers name the field instead.
- ✅ Reasons about a module artefact are the third named projection out of a translation unit, beside `compile_unit_of` and `source_unit_of`. `own_pcm_missing`, `own_pcm_stale`, `pcm_stale` and `dependency_pcm_stale` all name the same three things about the unit whose module triggered them — `module`, `pcm_path`, `full_path` — which was written out six times as a four-line designated initializer, once per site, with the unit's name (`tu`, `interface`, `dep_tu`) repeated three times inside it. `pcm_rebuild(kind, unit)` says it once, and each site is one line. The two blocks that fill in a reason inherited from a dependency are `attributed_to(reason, unit)`, which also settled that they differed only in a guard that was a no-op.
- ✅ The object-cache load carries the diff as the miss, not a reason beside a diff. `object_cache_load` held `std::optional<rebuild_kind> miss_reason` and a bare `object_cache_profile_diff` documented in prose as "filled for `profile_change`" — the coupling the type could have stated instead. The optional was also a `bool` in disguise: `profile_change` was the only value ever assigned to it, so `compile_units` guarded the notification with `miss_reason == profile_change`, a comparison that cannot be false once the optional is engaged, and `needs_recompile` dereferenced it into a `kind` it already knew. It is now one `std::optional<object_cache_profile_diff> profile_change`: engaged means the blanket miss happened and holds the field that caused it. The struct itself stays — it is not only a return, it is `needs_recompile`'s parameter, so a `tuple` would put `std::get<0>(cache).contains(...)` in the one place that most needs to say `entries`.
- ✅ `transitive_pcm_newer_than_object` returns the reason. It used to return `bool` and assign to a `rebuild_info&` out-parameter that meant nothing when the bool was false, so the caller had to declare an empty reason before asking the question — the same hidden channel as the object-cache members, in argument form. `visited` stays a parameter: it is the walk's own state, not an answer travelling back.
- ✅ `display_path` is computed once per translation unit. The join `path.empty() ? filename : path + "/" + filename` had appeared in `tu_label` and again in `source_unit_of`, and the rebuild sentences need it because `compile_unit.source` is the absolute path.
- ✅ Removed the dead `profile_change_rebuild` notification. Nothing published it, and its console override composed the sentence `compile_rebuild_message` now owns for `profile_change`.
- ✅ `tester:publisher` is gone, and each part of the framework publishes its own events — the arrangement the cb side already had, where build code calls `notify` directly. Ten of its exports were a single fan-out apiece, so a call reached an observer through three hops that decided nothing (`runner::report_results` → `publisher::print_test_results` → `notify` → `observer::test_results`). What it did earn moved to whoever owns the facts: `assertion_event` and `message_event` are built in `:assertions`, next to the matchers that have the operands and both source locations; the capture-buffer read and reset live in `:engine`, their only caller; the run lifecycle, catalogue, aggregates, slowest ranking and failure index live in `:runner`. Run-scoped state — `slowest`, the run argv, the thread-local current test id — is in `:data` with the run's other data. Two exports were dead: `print_test_cases` had no caller, and with it went `observer::test_cases` and the console override, since publisher was the only route to them. All three JSONL streams (`[self]`, standalone, `--list`) are byte-identical to the previous build once timestamps and durations are stripped, and console output differs only in printed pointer values.
- ✅ `tester.c++m` is only the list of re-exported partitions. It also held two function definitions and a namespace alias, which is where a reader looked for the module's contents and found three lines of implementation instead. `set_slowest` and `set_run_argv` are in `:runner` — both settings are read by events the runner emits, the argv by `run_start` and the count by the statistics — and the `bdd` alias is in `:behavior_driven_development` beside the namespace it abbreviates. Since `:runner` and the BDD partition open namespace `tester` directly, `tester::set_slowest` and `tester::bdd` are unchanged for callers.
- ✅ The runner's reporting methods say what they do, and each mode closes its own stream. Four `print_*` methods were named for a terminal the runner stopped writing to when the observers arrived — each one only calls `notify`, and whether anything is printed is the console observer's decision, not theirs. They are `list_tests`, `report_results`, `report_failures` and `report_summary`, and the private `emit_run_end` became `report_run_end` so the class has one verb rather than three. `emit_output_eof` is gone from the public surface: both of its callers were the last call of their mode, so ending the stream was a step the caller had to remember rather than something the mode knew, and the private `close_output` is now called by `list_tests` and `report_summary`. `main` is four calls instead of five, and all three JSONL streams are unchanged with `eof` still last and emitted once.
- ✅ The tester observer carries event objects, the way the cb one does. `run_start` took seven positional arguments, four of them `string_view`, `run_end` six and `statistics` four, with none of them named even in the declaration — an implementer had to read the console observer to learn what they were. They are `run_info`, `run_verdict` and `run_summary`, the same DTO move that `compile_unit` and `step_result` made on the build side, and the summary virtual is `summary` after the event it writes. `test_catalogue` takes a `catalogue` — matched tests in registration order plus the registered total — so filtering is decided once by the runner instead of each sink walking `data::test_cases` with a predicate and counting for itself. `emit_eof` is `eof`, the last member named for the act rather than the event. The console observer stopped recomputing the slowest ranking from `data::test_results` while being handed one: it used the span's *size* as the count and sorted again, so two sinks derived the same list twice, and the runner's sort is now stable to keep the ranking of equal durations meaningful. Both output channels are byte-identical across `[self]`, standalone, `--list` and failures mode, and 824 events schema-validate.
- ✅ Reading the capture buffer is not an observation. `observer::captured_output` and `reset_captured_output` were a query plus a mutation on a broadcast interface: `:engine` asked every registered observer and kept the first non-empty answer, so with two registered the winner was decided by iteration order, and only the console observer ever answered. Buffering is now `output_capture`, a two-method interface the console observer implements and registers from `activate`, with `captured_text` / `reset_captured_text` in `:observer` as the engine's entry points. Returning a `std::string` rather than a view also removes the lifetime question of handing out a view into a buffer another thread may be writing to.
- ✅ The console observer takes its own lock, as the JSONL observer already did for every event. A test may report assertions from a thread it started — the reason the current test id is `thread_local` — and every console event writes to `std::ostringstream stream`, which is both the terminal text and what a failing test attaches to its result, so two concurrent assertions could interleave inside one buffer. `clear_buffer` is the unlocked entry point for callers already holding the lock, and the two public template writers that wrote to the buffer without one are gone: they had no callers, the event-shaped overrides replaced them, and with them went the last of the console observer's own formatting code (`function_name_impl`, `format_value`, `demangle_impl`, the `chrono_clock` duplicate) which display names in `:assertions` had already made dead.
- 📋 Assertion accounting is still not thread-safe. The lock above makes the console observer's buffer safe, but `data::statistics()` counters are plain integers incremented by whichever thread reports, so concurrent assertions from test-spawned threads can lose counts. Either say in the docs that reporting is single-threaded, or make the counters atomic — the second is the smaller change and would let a `[self]` test pin it.
- 📋 Extract `translation_unit` and `parse_translation_unit` into their own header. Independent of the observer work: they are a self-contained island in a 3206-line file, and extracting them is what would let the preamble scanner be tested directly instead of only through `smoke.sh` fixture directories.

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
- 📋 `make tools` only works on Linux: it globs `tools/*.c++`, and `core_pc.c++` includes `<elf.h>`. CB does not scan `tools/`, so that utility has no other build path and nothing notices it rot. Either guard the source for non-Linux, move it out of the default glob, or drop it.
- 📋 Give the Makefile path a CI lane, or accept it as release-time only. It is a supported alternative to CB — `make tests` scans modules with `clang-scan-deps`, builds into `build-<os>/`, and its runner passes `[self]` 58/58 — but nothing verifies it between releases, so it can rot silently. A full rebuild there is also the only pass that shows every warning: it surfaced two dead templates in `tester-console_observer.c++m` and an unused lambda capture in `tester-utils.c++m` that CB's cache had stopped re-emitting, both now removed. Until a lane exists, [release-policy.md](release-policy.md) makes running it a release criterion.
- 📋 Document sandbox behaviour in CONTRIBUTING.
- 📋 JSONL-first `ci` example in README: `./tools/CB.sh ci --jsonl`.
- ✅ Every CI step that reads as a check is one, and the job that could not check anything is gone. `test-examples` was `continue-on-error`, ran the suite through `|| true`, and then looked for a `result` event the framework stopped emitting — three independent reasons it could not fail, on the job whose entire purpose was asserting that the failure demos still fail. Repairing it turned out to be the wrong fix: `tools/CB.sh` sets `CB_INCLUDE_EXAMPLES_MODE=always`, so a standalone `./tools/CB.sh debug test` already includes `examples/`, and the job was installing LLVM and rebuilding the same 36 units to run the same tests as `build-and-test`. It is retired, and its one unique check is a step there: selecting `[.demo]` must exit non-zero *and* report `passed: false`, with the command as the `if` condition since the non-zero exit is the expected outcome. `build-and-test` also stopped writing a synthetic `{"type":"eof"}` line for the JSONL validation step to inspect — a check of a file CI had just written itself, justified by a comment claiming all tests live in `examples/` and fail — and runs the whole suite instead, which is 146 passing tests and a real stream. Static analysis stays advisory, now by decision: the job is named `static-analysis (advisory)`, says why in a comment (neither tool is given the module graph, so a module interface reports diagnostics no fix can remove), and uploads its output as an artifact so the findings can be read rather than being discarded to `/dev/null`.

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
- ✅ What a consumer can rely on is written down: [release-policy.md](release-policy.md) names the public surface (the re-exported partitions, both CLIs, the JSONL schema, the `CB_*` bootstrap variables) against the parts that may change in any release (engine internals, `cb.c++` structure, everything under `build-*/cache/`, `tester/details/`, the hidden probe fixtures), and states the versioning rules — additive JSONL fields are MINOR because consumers must ignore what they do not know, raising the minimum compiler is breaking whatever the source compatibility, and a deprecated name survives at least one MINOR. It also fixes the bar: no release until this repository's own review rates it at or close to 9 / 10, which makes "pre-release" a stated position rather than an omission.
- ✅ [CHANGELOG.md](../CHANGELOG.md) records public-surface changes as they land, with an `Unreleased` section covering everything after the November 2025 pre-release — JSONL and its schema, the observer model, CB's telemetry and cache inspection, eager BDD steps, strict ordering metadata, the mixed-signedness comparison fixes, and the scanner fixes. Cutting a release renames the section; it is not reconstructed from the git log afterwards.
- 📋 Cut the first release once the policy's criteria are met: a tag consumers can pin instead of a commit, and release notes leading with breaking changes and the minimum compiler.

---

## 9. Code style & tooling

- 📋 Add `.clang-format` / `.clang-tidy`, and normalize `!` / `&&` against `not` / `and` in `tester-assertions.c++m` (the file mixes both today).

---

## Priority sketch

Ordered by consequence, not by effort. Items marked **verified** were reproduced against the current tree — the reproduction is recorded in the linked section so the next person need not rediscover it. Everything above `Low` is open; completed work is marked ✅ in its own section rather than repeated here.

| Priority | Item | Rationale |
|----------|------|-----------|
| Medium | Warnings from successful compiles are invisible (§3.6) | **Verified:** clang reports them, CB attaches them to no event, so they reach neither JSONL nor stderr — warnings accumulate unnoticed |
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
- Event contract and registry: `tester/tester-observer.c++m` (each module publishes through `notify`)
- Build system: `tools/cb.c++`
- CLI entry: `tester/test_runner.c++`