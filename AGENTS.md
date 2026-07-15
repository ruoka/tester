# Agent instructions — tester

Guidance for AI agents and automation using this repo’s JSONL output.

## Golden rule

Use **`--jsonl=failures`**. Parse **stdout only** (one JSON object per line, `schema: "tester-jsonl"`). Treat **stderr** as human/CB wrapper logs — do not parse it for pass/fail.

## Canonical commands

```bash
# Framework contract tests (CI gate — preferred while fixing)
./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'

# Translation-unit inventory (modules, imports, compile levels)
./tools/CB.sh debug list --jsonl=failures

# Test catalogue (ids, tags, depends_on for scoped runs)
./tools/CB.sh debug test --list --jsonl=failures

# Full suite (standalone only — includes examples/)
./tools/CB.sh debug test --jsonl=failures

# Build with compile telemetry
./tools/CB.sh debug build --jsonl=trace
```

**Tag syntax:** bracket tags must be escaped in shell: `--tags='\[self\]'`. Substring/regex filters also work: `--tags='Test case 3'`.

**Hidden tags:** bracket tags starting with `.` (Catch2-style, e.g. `[.demo]`, `[.jsonl-probe]`) are **skipped on unfiltered runs**. Select explicitly: `--tags='\[.demo\]'`.

**Scoped runs:** Prefer `--tags='\[self\]'` for framework work. Unfiltered standalone `./tools/CB.sh debug test` should report `summary.passed: true` — intentional failure demos in `examples/` use `[.demo]` (hidden unless requested).

**Embedded in a parent repo** (fixer, YarDB, net, xson): the parent's `AGENTS.md` applies — scope with the parent project's tags (`[fixer]`, `[yardb]`, etc.), not `[self]`. See [docs/cb.md](docs/cb.md).

**Unified JSONL modes:**
- `--jsonl` / `--jsonl=failures` — aggregates plus actionable failures (recommended)
- `--jsonl=summary` — lifecycle and final aggregates only
- `--jsonl=trace` — complete build/test telemetry, including passing assertions

## Triage workflow (test failure)

1. Find the last `summary` on stdout (`run_end` is trace-only).

2. Check the result:
   - If `passed` is `true` → this scoped run succeeded. You're done.
   - If `false`:
     - Read `first_failure` — `file`, `line`, `message`, and usually the failing `matcher` with `actual` / `expected`. Open the source at that location.
     - Read `failed_test_ids` for the full failure set.

3. For detailed diagnosis, inspect `assertion_failed`:
   - `matcher` — e.g. `require_eq`, `check_contains` (not generic `require` / `check`)
   - `actual`, `expected`, `file`, `line`, `column`

4. Fix the source, then re-run the **exact same scoped command**.

If `matcher` is `"require"` or `"check"` on a `require_eq` / `check_eq` line, stale test objects are likely — rebuild test TUs (`./tools/CB.sh debug build --jsonl=failures`), not only `tester_assertions.pcm`.

## Triage workflow (build failure)

1. Find `command_end` with `"ok":false` — use the `argv` array to rerun without shell parsing.
2. In failures mode, inspect failed `compile_end` and `command_end` events. Use trace mode when successful per-TU cache/rebuild telemetry is needed.
3. A failed compiler invocation emits `compile_end` with `ok:false`; CB joins the remaining workers before emitting one failed `build_end` and exiting.
4. Rebuild: `./tools/CB.sh debug build --jsonl=failures`, then re-run tests.

## Event reference (stdout)

### Correlation

Filter `run_id=<cb>` or `parent_run_id=<cb>` to correlate `list` → `build` → `test`.

| Field | On | Meaning |
|-------|-----|---------|
| `run_id` | Every event | Session id for the emitting process (32-char hex) |
| `parent_run_id` | `test_runner` events only | CB’s `run_id`, passed via `TESTER_PARENT_RUN_ID` when CB spawns the child |
| `config` on `run_start` | `test_runner` when spawned by CB | CB build config (`debug` / `release`), via `TESTER_CONFIG` |
| `pid` | Every event | OS process id (`test_runner` differs from CB) |
| `ts_unix_ms` | Every event | Unix timestamp (ms) |

### Test catalogue (`test --list --jsonl=failures`)

| Event | Use |
|-------|-----|
| `test_list_start` | Catalogue start (`tags_filter`) |
| `registered_test` | Per test: `id`, `name`, `file`, `line`, `column`, `tags[]`, `depends_on[]`, `priority` |
| `test_list_summary` | `registered_total`, `matched_total`, `tags_filter` |

### Test phase

| Event | Use |
|-------|-----|
| `run_start` / `run_end` | `run_start` is always emitted; `run_end` is trace-only |
| `assertion_failed` | Failures and trace modes (`matcher`, `actual`, `expected`, optional `message`) |
| `assertion_passed` | Trace mode |
| `test` | Failed tests in failures mode; every test in trace mode |
| `summary` | `tests_ok`/`tests_total`, `failed_test_ids`, `first_failure` |
| `exception` | Uncaught exceptions (`exception_type`, `message`, `file`, `line`) |
| `eof` | End of JSONL stream |

### Build phase (CB)

| Event | Use |
|-------|-----|
| `list_start` | TU inventory start (`config`, `include_tests`, `include_examples`, `source_dir`) |
| `unit` | Per translation unit (`path`, `module`, `kind`, `imports[]`, `level`, `has_main`, `is_test`, `is_modular`) |
| `list_summary` | Inventory totals (`units_total`, `main_count`, `test_count`, `max_level`) |
| `build_start` / `build_end` | Whole build; compact modes add aggregate compile/link/cache/failure counts |
| `command_start` / `command_end` | Every command in trace; failed commands only in failures |
| `profile_changed` | Once per build when object-cache profile mismatches (`reason`, `profile_diff`) |
| `cache_status` | `cache status` subcommand (`object_cache_path`, `profile_match`, entry counts, `current_profile`) |
| `cache_invalidate_end` | `cache invalidate` subcommand (`object_cache_removed`, `executable_cache_removed`, `compiler_stamp_removed`) |
| `compile_start` | Per TU in trace mode |
| `compile_end` | Per TU in trace; failed compilations only in failures |
| `link_end` | Per executable in trace; failed links only in failures |
| `cb_error` | CB fatal/diagnostic |

**`unit.is_test`:** `true` for `*.test.c++` / `*.test.c++m`, or when a path segment is exactly `test/` or `tests/`. `false` for sources under a `tester/` framework tree (library modules, not project tests) — including nested paths like `deps/xson/deps/tester/`. Does not match the substring `test` inside names such as `tester` or `test_exception_bug`.

## Example agent loop

```text
1. ./tools/CB.sh debug build --jsonl=failures
2. ./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'
3. Parse the last `summary` → check `passed`
4. If false: follow triage workflow (`first_failure` + `assertion_failed`) → edit → re-run the same scoped command
```

## C++ style (`tools/` and `tester/`)

Prefer the **standard library** over hand-rolled loops and iterator idioms. The project targets **C++23**. Follow [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) for best practices except project naming/style — see [CONTRIBUTING.md — Code Style](CONTRIBUTING.md#code-style).

### Implementation policy (standard C++ only)

Code under `tools/` (CB) and `tester/` must use **ISO C++ and the standard library** — not POSIX-specific APIs — except where noted below.

| Area | Rule |
|------|------|
| **Subprocesses** | `std::system` only (`<cstdlib>`) until the standard ships a better process API. No `popen`, `fork`, `execve`, or `posix_spawn`. Build a `string_list argv`; **`invoke_shell(argv)`** is the sole `join_argv` → `system()` boundary (compile, link, test_runner). |
| **Child output** | Shell redirect to a stamp/temp file, then `std::ifstream` / `std::getline` (e.g. `cache/compiler-version.txt`, `details/selftest_spawn.h++`). |
| **External toolchain** | CB invokes installed `clang++` / `lld` as external programs; that is not a deviation — the constraint applies to **our** source, not which compiler you install. |
| **Stack traces (exception)** | `test_runner.c++` uses `<execinfo.h>` (`backtrace`, `backtrace_symbols_fd`) on `SIGSEGV` / `SIGABRT`. glibc / macOS only — not portable ISO C++. No standard equivalent today. |

Do not add new POSIX-only paths when a `std::system` + file-read pattern or pure stdlib code suffices.

**Associative containers** — use `contains` / `at`, not `find(...) != end()`:

```cpp
// map / flat_map lookup
if (cache.contains(key))
    use(cache.at(key));

// heterogeneous lookup when keys are compared via string_view
using fields = std::flat_map<std::string, std::string, std::less<>>;
if (fields.contains(key))   // key may be std::string_view
    return fields.at(key);
```

**Strings** — use `std::string` / `std::string_view` member algorithms:

```cpp
if (text.contains("needle")) { ... }
if (name.contains(':')) { ... }
```

**Sequences** — use `<algorithm>` / `<ranges>`:

```cpp
std::ranges::contains(tags, required);
std::ranges::find_if(units, predicate);
std::ranges::sort(tokens);
std::ranges::set_difference(new_tokens, old_tokens, std::back_inserter(added));
```

**Joining / formatting** — do **not** use index loops (`for (i = 0; i < n; ++i)` + delimiter checks). For plain `string_list` joins (no empty elements), use **`std::views::join_with`** + **`std::ranges::to<std::string>`**. Use **`std::ranges::fold_left`** only when each element needs transformation:

```cpp
// string_list → delimited text (flags, diff summaries)
return flags | std::views::join_with(" "sv) | std::ranges::to<std::string>();

// shell command: quote each arg, then join (non-empty argv — contract at invoke_shell)
return argv
    | std::views::transform([](const std::string& arg) { return shell_quote(arg); })
    | std::views::join_with(" "sv)
    | std::ranges::to<std::string>();

// JSON string arrays: cb::output::jsonl::join_json_strings (transform(escape) + join_with(','))
os << '[' << cb::output::jsonl::join_json_strings(values) << ']';
```

Prefer **`views::join_with` / `ranges::to`** for delimiter joins (including **`join_argv`**: `transform(shell_quote)` then `join_with(' ')`; **`join_json_strings`**: `transform(quote)` then `join_with(',')`). Use **`fold_left`** only when accumulation is not a plain per-element transform + join (e.g. **`collapse_whitespace`**). Not ad-hoc index loops or one-off join helpers.

**Splitting / parsing** — prefer **`std::views::split`** + **`std::views::transform`** + **`std::ranges::to<Container>`** for delimited fields CB owns. Pair with symmetric read/write helpers (`append_profile_field` / `parse_profile_field`), not ad-hoc parsers with silent `continue` on every segment:

```cpp
// external flag text → string_list (collapse whitespace, drop empty tokens)
return normalized
    | std::views::split(' ')
    | std::views::transform([](auto&& part) { return std::string_view{part}; })
    | std::views::filter([](std::string_view t) { return not t.empty(); })
    | std::views::transform([](std::string_view t) { return std::string{t}; })
    | std::ranges::to<string_list>();

// object-cache profile → flat_map (split on tab; key/value on first '=' only)
return profile
    | std::views::split('\t')
    | std::views::transform([](auto&& part) { return parse_profile_field(std::string_view{part}); })
    | std::ranges::to<profile_fields>();
```

`parse_profile_field` uses **`string_view::find('=')`** (first delimiter only) — not `views::split('=')` — because profile values such as `compile` may contain `=`.

**Object-cache profile values** — tab-delimited `key=value` fields; values are stored verbatim (no percent-encoding). Invariant: values CB writes must not contain `'\t'`, `'\n'`, `'\r'`, or `'%'` (paths, flag lists, version lines satisfy this). For two-field cache entry lines (`path\tticks`), `string_view::find('\t')` is fine — no view pipeline needed.

Do **not** add defensive parsers or legacy upgrade paths for on-disk formats that **only CB writes**; fresh builds use `clean` / `cache invalidate`. Trust the writer contract; invalidate the whole cache on header mismatch instead of skipping bad segments.

**Subprocess I/O** — see [Implementation policy](#implementation-policy-standard-c-only) above. All toolchain commands go through `invoke_shell(argv)`; probes/self-tests use stamp/temp file + `std::ifstream`. Test env uses process-lifetime `std::array<std::string, 2>` storage with `putenv` + `invoke_shell` (not shell env prefixes); populate all strings before calling `putenv`, which retains their pointers. Never `popen` / `fork` / `exec`.

**CB flag argv** — store toolchain flags as `string_list` (`compile_flags`, `link_flags`, `cpp_flags`, `module_flags`); argv builders extend lists with `append_range` (literal groups as `string_list{…}`). Parse external flag **text** only at boundaries (`cb::detail::parse_external_flag_text` in `main` for `--compile-flags` / `--link-flags`; same helper when diffing serialized profile `compile`/`cpp` fields). Serialize lists to the object-cache profile with `detail::flags_profile_string` (`views::join_with(' ')` + `ranges::to<std::string>`). Parse with `collapse_whitespace` then `views::split(' ')` + `filter` non-empty + `ranges::to<string_list>` — symmetric with the writer; not full POSIX shell word-splitting (C++26 `split_when` would cover predicate delimiters without the collapse step).

**`cb.c++` namespaces** — sibling `cb::jsonl_observer()` / `cb::console_observer()` accessors expose the built-in observers; each observer owns its streams, lock, and format-specific lifecycle. Build code publishes command, compile, link, cache, list, and lifecycle events directly through `cb::output::notify` and the `cb::output::observer` contract in `cb-observer.h++`; it does not select a format. `main` installs the observer selected by CLI options, while activation and finalization happen through the observer contract. Shared profile-diff and source-inventory models also live under `cb::output`; channel-specific formatting/serialization stays in `cb::output::console::observer` and `cb::output::jsonl::observer`. `cb::detail` is for compute helpers (`shell_quote`, `join_argv`, profile/flag parsing, path/TU scan); `cb::translation_unit` and `cb::build_system` at top level; anonymous namespace + `main` for CLI argv parsing only.

**When custom code is fine** — topological sort, module graph walks, percent-encoding, and domain-specific cache logic. Do not reimplement `set_difference`, substring search, map membership, or **delimiter-join loops** by hand.

## Do not

- Infer pass/fail from exit code alone — read `summary.passed` or `run_end.passed`
- Parse stderr as structured JSONL
- Use an unfiltered full-suite run as the default fix loop — scope with `--tags='\[self\]'` for framework work
- Expect `summary.passed: true` on standalone `./tools/CB.sh debug test` (demo failures are `[.demo]` hidden tags)
- Run `[.tag]` probe fixtures unless explicitly selected (they are hidden by default)
- Use index loops to join/format delimited strings — use `std::views::join_with` + `std::ranges::to<std::string>`, `std::ranges::fold_left` (when fold state is not transform-then-join), or `join_json_strings`
- Multiply one-off helper functions when a std algorithm or existing join helper already covers the case
- Reimplement std join/parse/search when this section already covers the case

## More detail

- Event fields and examples: [README.md — JSONL sections](README.md#jsonl-assertion-events)
- Improvement backlog: [docs/tester-improvements.md](docs/tester-improvements.md)