# Technical Review — Tester and CB

**Review scope:** local repository implementation, documentation, CI configuration, and
public GitHub repository context.
**Date:** 1 August 2026 (supersedes the earlier 1 August draft at 8.3 and the 31 July
review at 8.0; that review superseded 29 July at 7.2)
**Assessment:** **8.5 / 10** — product surface and CI integration caught up with the
architecture work: JUnit beside JSONL, parallel top-level runs, clangd
`compile_commands.json`, and a gated Makefile lane. See [Score](#score) for the ladder
and what still caps it below 9.

## Executive Summary

Tester is a coherent C++23 modules-first testing framework for teams willing to
standardize on its toolchain and workflow. Its strongest differentiators are the
macro-free API, careful assertion semantics, a structured JSONL event contract, and
CB's capable incremental module build pipeline. The implementation shows substantial
systems-engineering attention in numeric comparison, failure attribution, observer
separation, depfile-aware caching, and diagnostic telemetry.

The registration hazards that opened the mid-July review are closed. Nested
`given` / `when` / `then` steps run at assignment, so `[&]` captures are ordinary
C++; ordering metadata refuses duplicate ids and unknown dependencies; tags are data
on the case; assertion comparison handles mixed signedness, wrappers, composites, and
non-finite floats in JSONL. CI either gates or says it is advisory, and the supported
platforms are written down.

What moved the score past 8.0 is two layers of work. On the framework side, the
`tester:publisher` forwarding layer is gone: engine, assertions, and runner notify
observers directly; lifecycle events take named DTOs; capture is a dedicated
`output_capture`; both shipped sinks lock; and the public module face is a short list of
re-exports (including the built-in observers so `test_runner` can stay a non-module TU).
On the product side, adopters and CI got what they actually ask for:
additive JUnit / xUnit XML (`--junit=`), parallel top-level tests (`--jobs=N` on
`execution_context` workers), `compile_commands.json` from `list` for clangd, warning
text on successful compiles, and a CI job that gates the Makefile path.

The remaining weaknesses are still maturity rather than correctness. CB is still
verified mainly from the outside; there is a release policy and a changelog but no
supported tag to pin. Tester is a good fit for focused module-native projects and
AI-assisted development today, but is not a broad replacement for Catch2, doctest, or
GoogleTest.

## What Changed Since the Previous Review

| Area | Change |
|------|--------|
| BDD lifetime | Steps run at assignment ([`tester/tester-engine.c++m:40`](../tester/tester-engine.c++m)); reference captures are safe |
| Step ordering | Run order comes from the call stack; the keyword-search insertion heuristic is deleted |
| Slot modelling | `test_slot` is a two-alternative `std::variant` (registered case vs owned step), so the impossible states are unrepresentable |
| Comparison semantics | Upstream fixes #37–#44: optional/expected/`expected<void>`, reference wrappers, time points, cross-period chrono, heterogeneous unscoped enums |
| JSONL validity | Infinities and NaN are emitted as quoted strings; the schema validator now covers the whole `[self]` suite |
| CB caching | Four cache artifacts are reported and invalidated uniformly, `std.pcm` / `std.o` report as an ordinary compile unit, and log paths derive from the unit |
| CB scanning | Line splicing is a single regex by policy; the framework does not attempt to be a preprocessor |
| CI accuracy | The gating self-test job reads `summary.passed` correctly. The earlier blanket criticism of CI event checks was too broad: only the non-gating `test-examples` job still queried the obsolete `result` event |

## What Changed In Response To This Review

The high-priority findings were fixed while the 31 July review was open:

| Area | Change |
|------|--------|
| Metadata strictness | Duplicate ids and unknown `depends_on` ids stop the run with the id in the message, joining the cycle check that was already fatal |
| Tag storage | `test_case::tags` is filled once at registration; the filter, the hidden-tag check and `registered_test` read it instead of re-parsing the name |
| CI honesty | The redundant `test-examples` job is retired and its one real check — `[.demo]` must still fail — gates inside `build-and-test`; the synthetic `eof` step became a real standalone run; static analysis is labelled advisory with its output uploaded |
| Platform statement | `README.md` says which platform CI verifies on every push, which one is verified on demand, and that Windows is unsupported |
| Release practice | [`docs/release-policy.md`](release-policy.md) defines the public surface, the versioning rules and the criteria for a release; [`CHANGELOG.md`](../CHANGELOG.md) records public-surface changes as they land |
| Second build path | The Makefile is documented as an alternative to CB rather than as legacy; CI gates `make run_tests` with `[self]` (`makefile-build-and-test`); the dead code its warnings exposed is removed |
| Document accuracy | `CONTRIBUTING.md` rewritten around both build paths and the checks CI gates; `cb.md`'s make targets corrected against the Makefile; `AGENTS.md` now covers the abort that produces no `summary`; the `[acceptor]` example is gone from the runner's `--help`; every cross-document link and heading anchor checked |

## What Changed Since the 8.0 Rating

Architecture, product surface, and CI integration landed after the gates above and
re-verified on this tree (`43a55fc` on `main`, plus local list-only smoke and
`invoke_shell` hardening):

| Area | Change |
|------|--------|
| Publisher dissolved | `tester:publisher` is gone. Assertions, engine, and runner call `notify` themselves; run-scoped state lives in `:data` |
| Observer DTOs | `run_start` / `run_end` / summary take `run_info`, `run_verdict`, `run_summary`; catalogue takes matched cases rather than a filter predicate |
| Capture off broadcast | `output_capture` is a separate interface; the console observer registers it from `activate` |
| Console lock | Every console event takes a mutex, matching the JSONL observer — fixes interleaved buffer writes from test-spawned threads |
| Runner vocabulary | `list_tests`, `report_results`, `report_failures`, `report_summary`; each mode closes the stream; no public `emit_output_eof` |
| Module face | [`tester/tester.c++m`](../tester/tester.c++m) is only `export import` lines (ten partitions, including the three built-in observers); `set_slowest` / `set_run_argv` live in `:runner`, `bdd` in the BDD partition; `test_runner.c++` is a non-module TU that `import tester`s |
| JUnit / xUnit XML | Additive `--junit=` / `--xunit-xml=` observer; runs beside console or JSONL; CB forwards the flag; CI uploads reports and summarizes gate suites with `test-summary/action` (#50 / #51) |
| Parallel top-level tests | `test_runner --jobs=N` (default `1`); ready cases run as dependency waves; per-worker `execution_context`; CB forwards `--jobs` to the runner when set (#52–#54) |
| Child-thread asserts | Run-wide fallback context so a thread a test starts can still report without `std::terminate` (#53) |
| `compile_commands.json` | `list` writes a clangd compilation database at the project root (#47) |
| Warning telemetry | Successful compiles that printed warnings carry `diagnostics`; failures mode still emits those events (#46) |
| Makefile CI gate | `makefile-build-and-test` runs `[self]` on every push (#57) |
| Smoke list-only | Fifteen scanner/inventory cases stop at `list` instead of compiling a fixture |
| Shell boundary | Compiler probe and `--version` stamp go through `invoke_shell` |
| Registration kind | Wrappers pass `suite_case` / `step` plus a role string; no `function_name()` sniffing |

Wire format and console output for the gated suites stayed byte-identical across the
publisher refactor (timestamps and `--slowest` timing noise aside). JUnit is a second
file sink, not a replacement for JSONL.

## Verified Signals

Measured on this working tree, debug config, on arm64 macOS with a locally built trunk
Clang — not the documented Clang 21 Linux configuration that CI exercises:

| Check | Result |
|-------|--------|
| `test --tags='\[self\]'` | 74 / 74 tests, 499 / 499 assertions |
| `test --tags='\[self\]' --jobs=4` | 74 / 74 tests pass (assertion totals may be one lower than sequential where a probe skips a jobs==1-only count check) |
| `test --tags='\[self\]' --junit=…` | JUnit report with 74 `<testcase>` elements beside JSONL |
| `test` (standalone, unfiltered) | includes examples; green when demos stay `[.demo]`-hidden |
| `test --tags='\[.demo\]'` | fails by design (`passed: false`) |
| `tests/cb/smoke.sh` | 247 checks pass (~61 s; 15 of 44 cases are list-only) |
| `tests/mcp/smoke.sh` | 9 checks pass |
| `tests/jsonl/validate.py` | 9 commands, 917 events schema-validated |
| Warm no-op build | sub-second, 36 scanned units plus `std` |
| Cold build after `clean` | ~16–19 s for the same set |
| `make tests` then `--tags='[self]'` | builds into `build-darwin/`, warning-free on the Makefile path |
| `list` | writes `compile_commands.json` (36 entries on this tree) |

Sizes: framework modules and runner (excluding `*.test.c++*`) ≈ 4,900 lines; with
self-tests ≈ 6,900. CB's [`tools/cb.c++`](../tools/cb.c++) is 3,334 lines.

## Strengths

### Framework and API

- The public surface is deliberately small: `import tester;` re-exports the authoring
  partitions plus the built-in observers, and keeps the engine internal
  ([`tester/tester.c++m`](../tester/tester.c++m)).
- The `require_*` / `check_*` split maps cleanly onto fatal and non-fatal failure
  behavior, and a non-fatal failure still fails the test — returning normally is not
  enough to be counted successful ([`tester/tester-engine.c++m`](../tester/tester-engine.c++m)).
- Assertion semantics go well beyond equality: mixed signedness via `std::cmp_*`,
  epsilon-based floating point with exact ordering, wrapper unwrapping, member-wise
  composite comparison, containers, strings, and exception types with demangled names
  ([`tester/tester-assertions.c++m`](../tester/tester-assertions.c++m)).
- Source locations are retained without registration or assertion macros.
- `test_order` exposes identifiers, dependencies, and priority directly
  ([`tester/tester-basic.c++m`](../tester/tester-basic.c++m)).
- A step failure is attributed to the step, not to everything it is written inside,
  and each case reports the assertions it made itself.
- Parallel top-level execution is opt-in and dependency-aware: `--jobs=N` runs ready
  cases as waves, and `depends_on` still barriers between waves.

### Architecture and implementation

- The framework's format-neutral observer contract separates run and assertion events
  from console, JSONL, and JUnit presentation
  ([`tester/tester-observer.c++m`](../tester/tester-observer.c++m)); observers are
  registered by name and selected from the CLI in the composition root
  ([`tester/test_runner.c++`](../tester/test_runner.c++)). Lifecycle events carry
  named DTOs rather than positional argument lists, matching the shape CB already
  used for compile and link. Producers notify directly — there is no publisher
  forwarding layer between the engine and the sinks. Additional sinks attach with
  `observe()` so JUnit can ride beside JSONL without replacing it.
- Capture is not an observation: a failing test's text comes from a dedicated
  `output_capture`, registered by the console observer, instead of a broadcast query
  that kept the first non-empty answer.
- Both shipped text observers lock around every event, so a test that asserts from a
  thread it started cannot interleave two reports in the shared console buffer.
  Run state lives on per-thread `execution_context` with a run-wide fallback for
  child threads that never install a scope.
- CB has a clear conceptual pipeline: translation-unit discovery, module graph
  construction, cache/rebuild decisions, compilation, linking, and observers.
- CB caching is substantive rather than superficial. It accounts for a cache profile
  (compiler version, flags, `std.cppm` signature), source and object timestamps,
  depfile-derived header freshness, PCMs, transitive imports, and executable link
  signatures ([`tools/cb.c++`](../tools/cb.c++)).
- Rebuild decisions are structured rather than textual: every non-hit compile carries
  a `rebuild_kind` plus the module or path that triggered it, and the human sentence
  is composed in the observer ([`tools/cb-observer.h++`](../tools/cb-observer.h++)).
- All toolchain execution funnels through one `std::system` boundary, and failing
  commands carry captured diagnostics with the full log path, so a failure can be
  triaged without rerunning the build. Successful warning output uses the same path.
- JSONL carries actionable source locations, matcher identity, expected/actual values,
  failed test IDs, and build diagnostics. The schema plus validator make this a usable
  automation contract ([`docs/jsonl-schema.json`](jsonl-schema.json),
  [`tests/jsonl/validate.py`](../tests/jsonl/validate.py)).

### Documentation and automation

- [`AGENTS.md`](../AGENTS.md) gives concrete command, output-parsing, and triage rules
  rather than generic agent guidance.
- [`docs/ai-agent-recommendation.md`](ai-agent-recommendation.md) distinguishes reduced
  diagnostic context from an unconditional build-speed or token-use claim.
- `[self]` framework contracts, CB smoke coverage, MCP smoke coverage, and JSONL
  schema validation give strong focused regression signals, and all four run in CI.
- CI emits JUnit beside JSONL and publishes gate-suite results on the job summary.

## Weaknesses and Risks

### Heuristic registration

The forgiving half of this is fixed. Ordering metadata is now validated before the
first test runs: a duplicate id and a `depends_on` id nothing registered each stop the
run with a message naming the id, the way a cycle already did
([`tester/tester-engine.c++m`](../tester/tester-engine.c++m)). Tags are parsed once at
registration and stored on the case, so filtering and the catalogue read data rather
than re-parsing the rendered display name. A description containing brackets is still
read as carrying tags — that is the authoring convention, not a parsing accident — and
moving tags into `test_order` remains open.

Registration kind is explicit on each wrapper (`suite_case` vs `step`): `test_case` /
`scenario` schedule for the run loop; `section` / `given` / `when` / `then` / `and_*`
run as steps of the active case. Display role strings are supplied by the wrappers, so
classification no longer sniffs `source_location::function_name()`. A step written
where no run is active still degrades to a scheduled case — the long-standing
compat behaviour, not a name heuristic. `test_order` remains only on `scenario` and
`test_case` (overload sets), so priority and dependencies never apply to steps.

### CB scanner and scale boundaries

CB scans sources with regexes after splicing lines, stripping comments and literals,
and eliding never-taken `#if` regions. This is an explicit policy, documented in the
source and smoke-tested, not an oversight: files that defeat the regexes are declared
unsupported. The envelope should stay visible, because it is real —
`#ifdef` branches are over-approximated so both sides contribute import edges, the
preamble scan stops after 1,000 lines, and translation units are keyed by basename, so
two `foo.c++` files in different directories are unsupported rather than distinguished.

Concurrency is one `std::jthread` per rebuild job within a dependency level, with a
counting semaphore capping active compilers and `--jobs=N` overriding the default.
Levels are processed sequentially. Very wide levels still create thread churn
proportional to the level's size, and jobs already running continue after a peer
fails — only unstarted work is skipped. Staleness evaluation recurses through imports
without a per-build memo; only the transitive-PCM walk keeps a visited set. At this
repository's scale the cost is invisible (a warm no-op build is half a second for 36
scanned units), but both are shape-dependent rather than bounded. On the test-runner
side, parallel waves now spawn at most `--jobs` threads per chunk, drop filtered-out
cases before scheduling, lock the observer registry around mutate/notify, and reify
orphan soft fails that land on the run-wide fallback under `--jobs>1`.

CB is tested, but only from the outside. [`tests/cb/smoke.sh`](../tests/cb/smoke.sh) is
thorough — 44 cases, 247 checks, covering cache profiles, every rebuild reason, strict
argument handling, compile/link/test failures, warning capture, `compile_commands`, and
a dozen scanner edge cases — yet 29 of those cases still drive a real build or test
against a temporary project, so the suite costs ~61 s and no internal function can be
asked a question on its own. Fifteen cases are list-only: scanner and inventory
questions that used to compile "to prove it still builds" now stop at `list`, which
already runs `scan_and_order`.

### Maturity and portability

- Supported usage depends on **Clang 21 or newer** and libc++'s `std.cppm`. Linux CI
  pins 21; macOS development uses a locally built LLVM that is often newer. Xcode and
  Homebrew toolchains are explicitly unsupported ([`README.md`](../README.md)).
- The runner's crash tracing uses POSIX `<execinfo.h>` and handles `SIGSEGV` and
  `SIGABRT` only, so a Windows port would need a second implementation before anything
  else. `README.md` now says plainly that Windows is unsupported.
- Mature-framework capabilities remain absent: fixtures and setup/teardown,
  parameterization or generators, matcher combinators, death tests, timeouts, repeats,
  shuffling, sharding, and runtime skip. Filtering, dependency ordering, hidden tags,
  BDD nesting, parallel waves, and JUnit beside JSONL are the substitutes that shipped.
- Release discipline is now *stated* but not yet *exercised*:
  [`docs/release-policy.md`](release-policy.md) defines the public surface, the semantic
  versioning rules (including that raising the minimum compiler is breaking), the
  deprecation window, and the criteria a release must meet — one of which is this review
  rating the repository at or close to 9 / 10. [`CHANGELOG.md`](../CHANGELOG.md) carries
  an `Unreleased` section covering everything after the November 2025 pre-release, so a
  consumer reads a summary instead of commit subjects. What remains is the artefact:
  no supported tag, so the pin is still a commit. Deliberate for a project that says so
  out loud, and stated as such in the README.
- Backlog discipline, by contrast, is good:
  [`docs/tester-improvements.md`](tester-improvements.md) tracks roughly a hundred
  implemented items against about sixty proposed ones, grouped by subsystem, and there
  are no open issues.
- Assertion accounting uses `std::atomic` counters on `test_statistics`, with
  `record_assertion` / `begin_assertion` / `complete_assertion` as the update path.
  Soft asserts from test-spawned threads that share a context keep accurate totals;
  under `--jobs>1`, orphans on the run-wide fallback become an explicit
  `<unattributed thread assertion>` failure so triage is never an empty
  `failed_test_ids` list beside `passed: false`.

### CI and documentation consistency

Every job now either gates or says it does not. The self-test and standalone-suite steps
read the last `summary` / `run_end` and fail on `passed != true`; a further step requires
the `[.demo]` selection to both exit non-zero and report `passed: false`; the CB, MCP,
and schema-validation jobs each assert a structured success marker; the Makefile path
gates `[self]`; and static analysis is named `static-analysis (advisory)`, explains why
in a comment, and uploads its findings instead of discarding them to `/dev/null`
([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)). Gate suites also emit
`--junit=` reports summarized on the job page.

Getting there removed a job rather than repairing it. `test-examples` existed to build
and run "with examples", but [`tools/CB.sh`](../tools/CB.sh) sets
`CB_INCLUDE_EXAMPLES_MODE=always`, so every standalone invocation already includes
`examples/` — the job was installing a toolchain and rebuilding the same units to run
the same tests, and the only check unique to it was the one it could not make. That check
is now a step in `build-and-test`. Also gone: a step that wrote a synthetic
`{"type":"eof"}` line for the next step to validate, justified by a comment claiming all
tests live in `examples/` and fail.

One gap remains, and it is blocked rather than open:

1. Every lane is `ubuntu-latest`, so Linux and Clang 21 are the only configuration
   verified automatically. macOS runs the same checks — `[self]`, the CB and MCP smoke
   suites, and schema validation — on the maintainer's locally built clang, but on demand
   rather than on every push. A hosted lane is not a matter of willingness: no clang
   available on a macOS runner builds C++23 modules for this project, since Xcode's does
   not support them fully and Homebrew LLVM fails exception unwinding on Apple Silicon,
   and building LLVM in CI to work around that is hours per run. The lane is intended
   once a module-capable clang ships on macOS; until then `README.md` states which
   platform is verified on every push, which one on demand, and that Windows is
   unsupported.

The Makefile is a genuine second build path, not a leftover: `make tests` builds the
library and runner through `clang-scan-deps` into `build-<os>/`, works standalone or
invoked from a parent `make`, and CI gates that path on every push
(`makefile-build-and-test`). What remains is no object cache and no build telemetry. CB
now surfaces warnings from units it compiles (`diagnostics` on successful steps), so Make
is no longer the only place they show up. One target is broken rather than merely
unverified: `make tools` globs `tools/*.c++`, so it fails on macOS at `core_pc.c++`'s
`<elf.h>`, and since CB never scans `tools/`, that utility has no build path anyone
exercises.

Checking the documents against the code found more drift than the code had. Every item
below was verified and then fixed: [`CONTRIBUTING.md`](../CONTRIBUTING.md) told
contributors to initialize submodules this repository does not have, pointed at
`build/bin/`, which neither build produces, documented an `.impl.c++` extension no file
uses, and never mentioned CB, the `[self]` gate, or the smoke suites — so following it
exactly could not reproduce what CI checks. [`docs/cb.md`](cb.md) listed a `make help`
target that does not exist and described `make clean` as preserving `std.pcm`, when
`mostlyclean` removes the whole pcm directory. `README.md` contradicted itself, calling the
Makefile legacy two sections after documenting it. [`AGENTS.md`](../AGENTS.md) told agents
to parse stdout and ignore stderr without saying that a metadata error stops the run after
`run_start` and puts the reason on stderr only — the one case where its own rule leaves an
agent with nothing. The runner's `--help` still advertised `--tags=[acceptor]`, a tag no
test carries.

## Detailed Findings by Focus Area

### Architecture and design

The module partition design is appropriate and the framework/CB separation is clean.
On the framework side, dissolving `tester:publisher` closed the largest structural
gap relative to CB: both sides now publish through the same pattern — producers own
facts, observers format them, `notify` is the fan-out. The primary module is a pure
re-export list; configuration and the `bdd` alias live in the partitions that use
them. JUnit is a third observer, not a fork of the reporting path.

CB's single-file implementation remains defensible for portability, vendoring, and
discoverability, especially now that its observer headers own all format-specific
serialization and its internal sections are ordered symmetrically (compilation and
linking follow the same work → signature → pass shape). The cost is review surface:
scanning, graphing, caching, command construction, linking, CLI parsing, and test
orchestration share one file, and nothing below the shell level is independently
testable.

Splitting CB purely to reduce line count is still not justified. Extraction should
follow testing pressure — the scanner and the cache model are the two components whose
behavior is intricate enough to deserve direct tests, and they are also the two most
separable.

### API and ergonomics

The framework is more natural than macro-heavy alternatives for users already writing
module interfaces, and its assertion vocabulary reads well. Catch2 and doctest remain
more immediately convenient for simple tests because macro registration carries less
namespace-scope boilerplate; GoogleTest remains stronger for fixtures, parameterized
tests, and ecosystem breadth — though tester now matches the common CI ask for JUnit
XML without giving up JSONL for agents.

BDD is no longer the ergonomic hazard it was. A step's captures are as valid as any
lambda invoked in the scope that wrote it, which is the rule a C++ reader already
applies. The residual surprises are quieter: nesting rules and `test_order`
availability both depend on which wrapper you called, and that mapping lives in
overload sets rather than in the API's shape.

### Implementation quality

The engine's accounting is careful: assertions consumed by steps are subtracted so a
parent reports its own, a non-fatal failure still fails the test, and captured output
is handed back to the enclosing case rather than lost — now through `captured_text`
rather than a poll of every observer. CB's structured rebuild reasons, bounded
diagnostics with a path to the full capture, decoded process status (distinguishing a
signalled toolchain crash from a non-zero exit), and RAII observer scopes are all
above-average choices. The runner's slowest ranking is computed once, stably, and
handed to every sink — the console observer no longer re-sorts from the global results
list using the span's size as a count.

The intentional trade-offs should stay visible: regex scanning, shell-mediated process
invocation, recursive staleness evaluation, and module-file flags attached broadly to
compiler invocations. The compiler probe and version stamp go through `invoke_shell`
like every other toolchain command: bare `CXX` / `LLVM_CXX` names are quoted inside
`sh -c 'command -v …'`, and `clang++ --version` writes `cache/compiler-version.txt`
via the same capture path.

### AI-agent friendliness

This remains the project's clearest differentiator. Failures mode gives agents a small
deterministic event set; trace mode exists when telemetry is actually wanted. The test
catalogue and the MCP bridge remove filename and filter guessing, and `AGENTS.md`
states the parsing contract precisely enough to follow mechanically. The recent
non-finite float fix matters more here than its size suggests: a single unparseable
line breaks a strict JSONL consumer, and the schema validator now covers the whole
`[self]` suite rather than a subset, which is what caught it. JUnit is for humans and
CI UIs; JSONL remains the agent contract.

### Production readiness

Credible for a controlled Clang 21 environment and a good candidate for internal or
embedded use where CB's assumptions are accepted. Not yet broadly production-ready as
a portable library product, because platform coverage, release discipline, and some
ecosystem surface (fixtures, generators) remain incomplete — not because CI cannot read
the results.

## Recommendations

### High priority

1. ~~Make metadata strict instead of forgiving.~~ Done: a duplicate id and an
   unresolved `depends_on` id each stop the run with the id in the message, from the two
   places the silence used to be — the id map and the dependency walk. Two spawned
   probes pin the exit code and the message.
2. ~~Store tags as data at registration.~~ Done: `test_case::tags` is parsed once from
   the description; the filter, the hidden-tag check and `registered_test` read it.
   Authoring tags through `test_order` instead of brackets in the name is still open.
3. ~~Repair or retire the `test-examples` CI job.~~ Retired: `CB.sh` always includes
   `examples/`, so the job duplicated `build-and-test`. Its one real check — the `[.demo]`
   selection must fail — is now a gating step there, and static analysis is advisory by
   decision, named as such, with its output uploaded.
4. ~~Reconsider the classification heuristic.~~ Done: each wrapper passes an explicit
   `registration_kind` and role string; sniffing `function_name()` for
   `scenario` / `test_case` is gone. Public API unchanged. Steps with no active run
   still degrade to scheduled cases for compatibility with existing suites.
5. ~~Say which platforms are verified and how.~~ Done: [`README.md`](../README.md) now
   states that both platforms run the same suites, that Linux runs them on every push
   while macOS runs them on demand, and that Windows is unsupported. A hosted macOS lane
   is blocked on the platform, not declined: no clang on a macOS runner builds C++23
   modules for this project today, and it is planned for when one does.
6. ~~Align the framework observer contract with CB's.~~ Done: positional lifecycle
   arguments became DTOs, `test_catalogue` receives matched cases, capture left the
   broadcast interface, both sinks lock, and `tester:publisher` is gone.

### Medium priority

1. Let CB be tested without invoking a compiler. The easy half is done: fifteen of the
   44 smoke cases are list-only (scanner, inventory, and scan-time refusals), and the
   suite is 247 checks in ~61 s. What remains are the 29 cache/rebuild/link/test cases
   that *are* the compiler, plus asking a function like `splice_physical_lines`
   directly — that still needs the extraction below. This is still the heaviest
   remaining weight under 9.
2. Replace per-level CB compile thread fan-out with a bounded worker pool, and memoize
   staleness results for the duration of one build pass. (Test-runner `--jobs` chunking
   and orphan soft-fail attribution are done.)
3. ~~Rewrite CONTRIBUTING with current artifact paths, both build paths, and which one CI
   gates; fix the `[acceptor]` example.~~ Done, together with the `cb.md`, `README.md`, and
   `AGENTS.md` corrections listed above. What remains is `make tools` on macOS.
4. ~~Publish versioning, compatibility, and changelog practices for submodule
   consumers.~~ Done: [`docs/release-policy.md`](release-policy.md) and a maintained
   [`CHANGELOG.md`](../CHANGELOG.md). What is left is the tag itself, which the policy
   gates on this review — see the note under Score.
5. ~~Route the compiler probe and version stamp through `invoke_shell`.~~ Done: bare
   `LLVM_CXX` / `CXX` names are looked up with a quoted `sh -c 'command -v …'` through
   `invoke_shell`, and `clang++ --version` writes the stamp the same way. Nothing reaches
   `system()` unquoted.
6. ~~Make assertion statistics counters atomic and pin the parallel soft-fail /
   orphan case.~~ Done: counters are `std::atomic`; `[self]` covers concurrent
   child-thread counts and `<unattributed thread assertion>` under `--jobs>1`;
   filtered cases no longer consume parallel chunk slots; the observer registry is
   locked around mutate/notify.
7. ~~Ship a CI-friendly XML reporter beside JSONL.~~ Done: `--junit=` / `--xunit-xml=`
   with self-tests and Actions job-summary integration. SARIF stays out of scope unless
   static-analysis consumers ask for it.

### Low priority / strategic

1. Add fixtures, parameterization, and composable matchers only where real adopter
   needs justify the complexity; the current API's coherence is an asset.
2. Offer an opt-in compiler-assisted dependency scanner (`clang-scan-deps`) for
   projects that exceed the documented regex envelope, rather than growing the regexes.
3. ~~Add JUnit or SARIF adapters only when integration demand warrants.~~ JUnit shipped;
   JSONL stays the native machine interface. SARIF only if advisory analysis needs it.

## Score

| Score | When | What moved it |
|-------|------|---------------|
| 7.2 | 29 July | Baseline: BDD steps could outlive the captures they were written against, and their run order rested on a name-search heuristic |
| 7.5 | 31 July, as reviewed | Both registration hazards closed, comparison semantics hardened by eight narrow fixes, JSONL valid for non-finite floats and schema-validated across the whole `[self]` suite, CB cache reporting uniform |
| 8.0 | 31 July, after the response | Ordering metadata refuses what it cannot honour, tags are data on the case, every CI job either gates or says it is advisory, and the supported-platform story is written down |
| 8.3 | 1 August (interim) | Publisher dissolved; observer DTOs; dedicated capture; console lock; runner reporting vocabulary; primary module is re-exports only |
| **8.5** | 1 August | Interim architecture plus JUnit beside JSONL, parallel `--jobs` / `execution_context`, `compile_commands.json`, warning telemetry, Makefile CI gate, list-only smoke, and `invoke_shell` for the compiler probe |

Re-verified on this tree: 74 self tests with 499 assertions (also green under `--jobs=4`),
917 JSONL events across the nine schema-validation commands. CB smoke is 247 checks
in ~61 s; MCP smoke remains 9.

Why +0.5 from 8.0 (and +0.2 past the interim 8.3): the architecture catch-up alone did
not clear the adopter and CI gaps. JUnit, parallel runs, clangd export, and a gated
second build path do. That is enough for 8.5; it is not yet a release.

Three things still cap it below 9, in order of weight:

1. **CB is verifiable mainly by compiling.** 29 of 44 smoke cases still invoke a real
   toolchain (the rest are list-only), so the build system's own correctness costs ~61 s
   to confirm and no internal function can be questioned directly. This is the one cap
   that is entirely within reach today.
2. **No tag to pin.** The compatibility half is answered —
   [`docs/release-policy.md`](release-policy.md) and [`CHANGELOG.md`](../CHANGELOG.md).
   What is left cannot be closed by documentation: there is no version to depend on.
   The policy still gates a release on this review approaching 9 / 10, so the score and
   the tag remain a bootstrap pair — neither finishes the other alone.
3. **Framework edges inside the shipped surface.** The deliberate absence of fixtures /
   generators / timeouts is what a careful adopter still weighs. Fixtures remain
   strategic.

An 8.5 does not require becoming Catch2. It requires that CI and agents can trust the
results they already get, and that the remaining gaps are ones a consumer chooses to
live with rather than discovers. A path to ~9 / 10 from here is mostly product maturity:
CB extractable enough for direct tests, a supported tag under the existing policy, and
either enough of the missing surface or a clear "won't do" list that adopters are not
surprised.

## Overall Assessment

Tester is a strong specialist tool with better-than-average internal engineering,
diagnostics, and agent-oriented workflows for its maturity. The movement from 7.2 to
8.0 was closure of hazards and dishonest CI; 8.0 to 8.5 is the framework catching up to
CB's publishing discipline and then shipping the integration pieces (JUnit, parallel
runs, clangd, Makefile gate) that make that discipline usable outside the repo.

It is still not maturity-equivalent to established general-purpose frameworks, for the
three reasons above plus the toolchain constraint that Clang 21 and libc++ modules
impose. The recommended path remains targeted hardening rather than a rewrite —
preserve the focused modules-first identity, make CB's own tests fast enough to run per
edit, and cut a supported tag when the policy's bar is met.
