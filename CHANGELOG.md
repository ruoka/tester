# Changelog

Notable changes to the public surface defined in [`docs/release-policy.md`](docs/release-policy.md):
the exported `tester` API, the `test_runner` and CB command lines, and the JSONL contract.
Internal refactoring is not listed — roughly half the commits since `v1.0.0` reorganize
`tools/cb.c++` and the observer layer without changing behaviour, and the git log covers them.

Versions follow the rules in the release policy. `Unreleased` accumulates until the next
tag; cutting a release renames that section.

## [2.1.0] — 2026-08-02

### Added

- **`cb --modules=<two-phase|one-phase>`** selects how modular units (including `std.cppm`)
  are compiled. The default `two-phase` is the existing `--precompile` to `.pcm` followed by
  `.pcm` to `.o`. `one-phase` emits both from a single `-c -fmodule-output=<pcm>` step, so the
  source is parsed once and Clang 22+ writes reduced BMIs by default. The mode is an
  object-cache profile field (`module_phases`), so switching it reports `profile_change` and
  recompiles rather than reusing BMIs the other scheme produced.
- **CMake + Ninja build path** ([`CMakeLists.txt`](CMakeLists.txt)) — a third way to build the
  same library and `test_runner`, for consumers who already build with CMake and as a worked
  example of C++23 modules under CMake (`FILE_SET CXX_MODULES` plus a `std` module target).
  Targets `run_tests` / `run_all_tests`; `-DTESTER_STATIC=ON` mirrors the Makefile's `STATIC=1`
  and build types map to the same flags `config/compiler.mk` uses. `std` is compiled from
  `${LLVM_PREFIX}/share/libc++/v1/std.cppm` exactly as `config/compiler.mk` does it, rather
  than through CMake's own `import std` support, which resolves the source through a libc++
  manifest that apt.llvm.org installs twice — the copy clang answers with points at a path
  that does not exist, so every Ubuntu configure failed. Requires CMake 4.x and the Ninja
  generator.
- **CMake CI lane** (`cmake-ninja-build-and-test`): configure and build for `Debug` and
  `Release`, failing on any compiler `warning:`, then `[self]` sequentially and at
  `--jobs=$(nproc)`, the unfiltered standalone suite, and the `run_tests` target. The pinned
  CMake install is shared with the dev container.

### Fixed

- **`[self]` suite builds where `char` is unsigned** (ARM Linux). The mixed-signedness cases
  spelled their negative character operand `char{-1}`, which is a narrowing error there and
  could not hold a negative value in any case, so `tester-assertions.test.c++` failed to
  compile. They use `signed char` now — the same promotion and reporting paths, negative on
  every platform.

### Changed

- **CB subprocesses use `posix_spawn`** instead of `std::system`. Apple's libc serializes
  `std::system`, which caps parallel compiles on macOS; `invoke_shell` still runs
  `/bin/sh -c` so capture redirects are unchanged. The implementation policy in
  `AGENTS.md` / `docs/cb.md` follows.
- **Dev container moved to Debian 13 (trixie)** from bookworm, and it now ships `make`,
  Ninja and the pinned CMake so all three build paths run there. Debian in the container
  against Ubuntu in CI stays deliberate — the distros package libc++ differently, and
  bookworm was the one layout that did not reproduce what CI builds on.
- **Technical review** — [`docs/repository-technical-review.md`](docs/repository-technical-review.md)
  amended so the closed “no supported tag” finding reflects [`v2.0.0`](https://github.com/ruoka/tester/releases/tag/v2.0.0);
  remaining caps under 9 are CB extractability and framework-surface choices.

## [2.0.0] — 2026-08-01

First **supported** modules-era release. Minimum toolchain: Clang 21+ with libc++ modules
(`std.cppm`). Prior `v1.0.0` remains an unsupported GitHub pre-release.

### Fixed

- **`[self] observer_instance` under console mode** — the live-sink guard test no longer
  assumes JSONL was already `activate()`d by `--jsonl`. Parents that run without `--jsonl`
  (e.g. nested cryptic CI) enable the sink for the assertion and restore afterward, so the
  case passes in both console and JSONL process modes.

### Changed

- **CI runs `[self]` and the standalone suite under both `--jobs=1` and `--jobs=$(nproc)`**
  (CB and Makefile paths), so sequential and parallel runner modes stay gated.

### Added

- **Parallel top-level tests** via `test_runner --jobs=N` (default `1` = sequential;
  `0` = `hardware_concurrency`). Independent cases at the same dependency wave run
  concurrently; `depends_on` still barriers between waves. Each worker has its own
  `execution_context`; console capture is thread-local. When CB is given `--jobs=N`,
  that cap is also forwarded to `test_runner` (compile default remains CPU count;
  the runner stays sequential unless `--jobs=` is set).
- **JUnit / xUnit XML reports** via `--junit=<path>` (alias `--xunit-xml=`). Additive with
  console or `--jsonl`: JSONL stays on stdout for agents while the XML file feeds CI test
  UIs. CB forwards the flag. Empty tag filters emit a synthetic failing testcase so the
  run does not look green in Jenkins/GitHub Actions. GitHub Actions writes the reports
  beside JSONL and publishes gate-suite results with `test-summary/action`.
- **JSONL output** for both phases, with `--jsonl`, `--jsonl=summary`, `--jsonl=failures`,
  and `--jsonl=trace` selecting how much is emitted. Machine contract published as JSON
  Schema 2020-12 in [`docs/jsonl-schema.json`](docs/jsonl-schema.json), with
  [`tests/jsonl/validate.py`](tests/jsonl/validate.py) validating every line the canonical
  commands emit.
- **`summary` and `first_failure`** as the single place an agent reads a verdict —
  `tests_ok` / `tests_total`, `assertions_ok` / `assertions_total`, `passed`,
  `failed_test_ids`, and the failing file, line, matcher, `actual`, and `expected`.
- **Test catalogue** via `test --list`: `registered_test` per case with `id`, `file`,
  `line`, `tags`, `depends_on`, and `priority`, plus `test_list_summary`.
- **Observer plug-in model** — console and JSONL observers behind one contract, so a
  project can add its own reporter without touching the engine.
- **CB rebuild telemetry**: every non-cached compile or link reports why, as a short
  `rebuild_reason` and a structured `rebuild` naming the module or triggering path.
- **`#include` tracking** through compiler depfiles, so editing a header rebuilds what
  included it (`header_stale`), a missing or unreadable project header named by the
  depfile rebuilds (`header_missing`) instead of keeping a silent cache hit, and an
  unusable depfile rebuilds rather than guessing (`depfile_unusable`).
- **Cache inspection**: `cache status` reports all four cached artefacts and whether each
  matches the current toolchain profile; `cache invalidate` removes them.
- **`link_end.signature`**: the link input stamp on every JSONL `link_end`, including
  skipped links (`cache_hit: true`), so agents can see the cache key without reading
  `executable-cache.txt`.
- **`clean --tests`**: removes only test TU objects / PCMs and `test_runner`, pruning
  matching object- and executable-cache entries, so the next build rebuilds tests
  without a full cold rebuild.
- **`compile_commands.json` from `list`**: writes a clangd compilation database at the
  project root for the active TU set, using the same compile argv builders as a build.
- **`graph.json` from `list`**: writes the module/import inventory (`cb-graph` v1) at the
  project root — the same fields as JSONL `unit` / `list_summary`, for tools that prefer
  one file over parsing the stream.
- **Modular compile capture files**: `--precompile` and pcm→object use distinct `.pcm.log`
  / `.o.log` paths so `diagnostics.path` still names a file that contains the warning text.
- **`--jobs=N`** to bound concurrent compile and link processes.
- **Diagnostics in events**: a failed compile, link, or command carries the toolchain
  output in `diagnostics.text` (with the full capture at `diagnostics.path`), so a failure
  needs no second build to read. Successful steps that printed warnings carry the same
  field, and `--jsonl=failures` still emits those events so `-Wall` noise cannot hide.
- **MCP stdio bridge** ([`tools/cb_mcp.py`](tools/cb_mcp.py)) wrapping the canonical CB
  commands for editors and agents.
- **Test suites**: `[self]` contract tests for the framework, CB behaviour tests
  ([`tests/cb/smoke.sh`](tests/cb/smoke.sh)), MCP checks, and schema validation, all run by
  CI on Linux for `debug` and `release`.
- **Makefile CI lane** (`makefile-build-and-test`): `make tests` (fail on compiler
  `warning:`), then `make run_tests` with `--tags=[self]`. The second build path is
  gated on every push, not only at release time.
- **`AGENTS.md`** as the automation contract, and hidden `[.tag]` fixtures so intentional
  failure demos stay out of unfiltered runs.

### Fixed

- **Live `observer_instance` sinks are not torn down** — after `activate()`, further
  calls return the existing JSONL/console singleton instead of `emplace`-rebuilding
  it. Mid-run emplace reset `jsonl_context::enabled` (silencing `summary`/`eof`) and
  could destroy a mutex `notify` still held; restoring the flag after emplace was not
  enough.

### Changed

- **README documents tester resolution for embedded consumers** — `CB_TESTER_ROOT` →
  `deps/tester` → sibling `../tester` → in-tree `cb.c++` → optional `CB_FETCH_DEPS=1`,
  plus the rule that nested `deps/*/deps/tester` trees are updated by bumping the
  submodule pointer rather than copying docs into the checkout.
- **MCP stdio framing is newline-delimited JSON-RPC** per the MCP transport spec
  (`tools/cb_mcp.py` and `tests/mcp/smoke.sh`). The previous LSP-style
  `Content-Length` headers were a mismatch with the published stdio binding.
- **`runner` owns its tag filter** as a `std::string`, so a temporary or argv view need
  not outlive the runner. Console and JSONL `observer_instance(...)` reconfigure on
  each call only while the sink is inactive (startup multi-setup); a live sink keeps
  its streams/mode and only allows updating console `result_line`. JUnit already
  applied path/state via `configure()`.
- **Registration kind is explicit on each wrapper** (`suite_case` vs `step`), with a
  role string for display names (`"test_case"`, `"given"`, …). Whether a registration
  is scheduled or nested no longer depends on sniffing `source_location::function_name()`.
  The public `test_case` / `scenario` / `section` / BDD entry points are unchanged; a
  step written with no active run still becomes a scheduled case (compatibility).
- **Assertion statistics are atomic**, and parallel `--jobs` accounting is closed:
  `test_statistics` counters use `std::atomic` with `record_assertion` /
  `begin_assertion` / `complete_assertion`; soft asserts from many child threads keep
  accurate totals. Under `--jobs>1`, soft fails that land on the run-wide fallback are
  reified as `<unattributed thread assertion>` so `failed_test_ids` is usable; parallel
  waves spawn at most `jobs` threads per chunk and drop filtered-out cases before
  scheduling; the observer registry is locked around mutate/notify.
- **`test_runner.c++` is a non-module executable** (`import tester;`) rather than a
  `tester` implementation unit with `main`. The built-in `console_observer`,
  `jsonl_observer`, and `junit_observer` partitions are re-exported from
  `export module tester` so the composition root does not need partition imports
  (which are only legal inside the module).
- **Makefile prefers `./tester/` as the source tree** when that directory exists, instead
  of always deriving the project name from the checkout basename. Checkouts whose
  directory is not named `tester` (for example `/workspace`) now build without renaming.
- **`make run_tests` no longer echoes the runner command** onto stdout, so redirecting
  the target with `--jsonl` stays a pure JSONL stream.
- **Execution state is owned by `execution_context`**, not process-wide mutable globals.
  Current test id, nested capture pointers, step assertion counters, results, and
  statistics live on a per-run / per-worker context activated with `execution_scope`.
  The registration catalogue remains shared.
- **Nested BDD steps run at assignment**, inside the parent's frame, so `given` / `when` /
  `then` bodies capturing parent locals by reference are safe. They previously ran after
  the parent returned.
- **Ordering metadata is now refused rather than reinterpreted**: duplicate test ids,
  `depends_on` ids that name no test, and dependency cycles each fail before the first
  test runs, with a message naming the offender.
- **Tags are data on the case**, recorded at registration instead of parsed back out of
  the rendered display name.
- **Assertion operands are taken without copying** (forwarding references), arrays decay
  at the hub, and move-only types are accepted.
- **Failure reports read like the source**: the matcher is named exactly
  (`require_eq`, `check_contains`), characters are reported by value, strings verbatim,
  exception type names demangled (`std::runtime_error`, not `St13runtime_error`), and
  composite operands rendered member-wise.
- **Unknown CB arguments are fatal** (exit `2`), so a typo such as `--tag=` fails loudly
  instead of silently running the whole suite.
- **Process outcomes are decoded**: events carry the child's `exit_code` separately from
  the raw `wait_status`, with `signaled` and `signal` distinguishing a crash from a
  non-zero exit.
- **A tag filter that matches no test fails** instead of reporting an empty success.
- Assertion operands are guaranteed valid UTF-8 in JSONL — invalid bytes become U+FFFD
  rather than passing through.
- **`tester::output::observer` takes event objects and no longer answers questions.** The
  contract changed in five ways, all of them visible only to code that implements an observer;
  the JSONL wire format and the console output are unchanged.
  - `run_start`, `run_end` and the summary take DTOs — `run_info`, `run_verdict` plus the
    `failure_index`, and `run_summary` plus the `failure_index` — instead of seven, six and
    four positional arguments. The summary virtual is `summary`, matching the event it writes,
    rather than `statistics`.
  - `test_catalogue` receives a `catalogue`: the matched tests in registration order and the
    registered total. Filtering is the runner's decision, so an observer renders the selection
    instead of walking the registry and re-applying a predicate.
  - `emit_eof` is `eof`, the only member that was named for the act rather than the event.
  - `captured_output` and `reset_captured_output` are gone. Buffering test output is a separate
    `tester::output::output_capture` interface, registered by the observer that does it — the
    console one — so reading the buffer no longer broadcasts a query to every observer and
    keeps the first non-empty answer.
  - The `test_cases` virtual was removed. Nothing reached it: its only caller was an internal
    forwarder with no callers of its own, and the catalogue goes through `test_catalogue`.

### Deprecated

- Passing an exception *instance* to `check_throws_as` / `require_throws_as`. The value was
  always discarded; name the type instead — `check_throws_as<E>(callable)`. The instance
  form still compiles and warns.

### Fixed

- **Mixed-signedness comparisons** no longer promote through `std::common_type_t` and
  compare wrong values. `check_eq` and friends compare by value across signed and unsigned
  integers, characters, `bool`, unscoped enums, container elements, `pair` and `tuple`
  members, `optional` and `expected` payloads, and `chrono` durations and time points.
- **Non-finite floating point** (`inf`, `-inf`, `nan`) is quoted in JSONL, so a line
  containing one still parses as JSON.
- **`tests_ok`** counts only cases whose assertions all passed, so it can no longer equal
  `tests_total` while `failed_test_ids` is non-empty.
- **Cyclic `depends_on`** is detected without overflowing the stack.
- **The module scanner** no longer reads declarations out of comments, `#if 0` bodies, or
  string literals, and joins backslash-continued lines before scanning.
- **CB module and artefact handling**: dotted module names and global module fragments
  parse, module artefacts map to hyphen stems with collisions reported, project sources
  colliding with reserved `std` artefacts fail fast, `std.pcm` rebuilds when the toolchain
  profile changes, and `std.o` compiles with the project's own flags.
- **Source scanning boundaries**: project `test/` trees are scanned, while vendored
  `deps/*/tester` trees and nested `deps/*/deps` are not.
- **Link and run target exactness**: only the project's own `bin/test_runner` is linked
  and executed, with link decisions snapshotted before parallel workers start.
- **Relative depfile headers** resolve before the staleness check.
- **The MCP bridge** no longer reports a green `build_end` as test success after the runner
  crashed.
- **The console observer takes a lock** for every event, as the JSONL observer already did. A
  test that asserts from a thread it started could interleave two reports in the shared buffer,
  which is both the terminal output and the text a failing test attaches to its result.

## v1.0.0 — 27 November 2025

A GitHub **pre-release**, kept for history and unsupported. It predates everything above,
including the JSONL contract, the observer layer, and CI. No changelog was maintained at
the time and none is reconstructed here; read the git log if you need its state. Superseded
by supported `[2.0.0]`.
