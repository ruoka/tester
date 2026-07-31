# Technical Review — Tester and CB

**Review scope:** local repository implementation, documentation, CI configuration, and
public GitHub repository context.
**Date:** 31 July 2026 (supersedes the 29 July review, which scored 7.2)
**Assessment:** **8.0 / 10** — 7.5 as first assessed on this date, then 8.0 once the
high-priority findings were fixed in response and re-verified. See
[Score](#score) for what each step was and what caps it below 8.5.

## Executive Summary

Tester is a coherent C++23 modules-first testing framework for teams willing to
standardize on its toolchain and workflow. Its strongest differentiators are the
macro-free API, careful assertion semantics, a structured JSONL event contract, and
CB's capable incremental module build pipeline. The implementation shows substantial
systems-engineering attention in numeric comparison, failure attribution, observer
separation, depfile-aware caching, and diagnostic telemetry.

The two registration hazards that opened the previous review are closed. A nested
`given` / `when` / `then` step now runs at its assignment, inside the body that
declared it, so `[&]` captures are as valid as in any ordinary lambda call and the
name-based heuristic that used to approximate step order is gone. Assertion
comparison has also been hardened through a series of narrow fixes: wrappers are
unwrapped before mixed-signedness comparison, chrono durations keep a floating
common type, composite (pair/tuple) values compare member-wise, and non-finite floats
are quoted in JSONL so the stream still parses.

Ordering metadata is no longer forgiving either: a duplicate test id and a `depends_on`
id nothing registered now stop the run with a message naming the id, and bracket tags
are parsed once at registration and stored on the case instead of being read back out
of the rendered display name. What is left is classification — whether a registration
is a scheduled case or a nested step is still decided by looking for `scenario` or
`test_case` in the registering function's name.

The remaining weaknesses are maturity rather than correctness. CB deliberately scans
sources with regexes, keeps its whole pipeline in one 3,200-line translation unit, and is
verified thoroughly but only from the outside — 39 of its 41 smoke cases run a real
compiler. Adoption is constrained by the Clang 21 / libc++ module requirement,
POSIX-specific crash handling, automation on Linux only, and the absence of a release or
compatibility policy. Tester is a good fit for focused module-native
projects and AI-assisted development today, but is not a broad replacement for Catch2,
doctest, or GoogleTest.

## What Changed Since the Previous Review

| Area | Change |
|------|--------|
| BDD lifetime | Steps execute eagerly at assignment ([`tester/tester-engine.c++m:40`](../tester/tester-engine.c++m)); reference captures are safe |
| Step ordering | Run order comes from the call stack; the keyword-search insertion heuristic is deleted |
| Slot modelling | `test_slot` is a two-alternative `std::variant` (registered case vs owned step), so the impossible states are unrepresentable |
| Comparison semantics | Upstream fixes #37–#44: optional/expected/`expected<void>`, reference wrappers, time points, cross-period chrono, heterogeneous unscoped enums |
| JSONL validity | Infinities and NaN are emitted as quoted strings; the schema validator now covers the whole `[self]` suite |
| CB caching | Four cache artifacts are reported and invalidated uniformly, `std.pcm` / `std.o` report as an ordinary compile unit, and log paths derive from the unit |
| CB scanning | Line splicing is a single regex by policy; the framework does not attempt to be a preprocessor |
| CI accuracy | The gating self-test job reads `summary.passed` correctly. The earlier blanket criticism of CI event checks was too broad: only the non-gating `test-examples` job still queried the obsolete `result` event |

## What Changed In Response To This Review

The high-priority findings were fixed while the review was open, which is why they read
as done below rather than as recommendations:

| Area | Change |
|------|--------|
| Metadata strictness | Duplicate ids and unknown `depends_on` ids stop the run with the id in the message, joining the cycle check that was already fatal |
| Tag storage | `test_case::tags` is filled once at registration; the filter, the hidden-tag check and `registered_test` read it instead of re-parsing the name |
| CI honesty | The redundant `test-examples` job is retired and its one real check — `[.demo]` must still fail — gates inside `build-and-test`; the synthetic `eof` step became a real standalone run; static analysis is labelled advisory with its output uploaded |
| Platform statement | `README.md` says which platform CI verifies on every push, which one is verified on demand, and that Windows is unsupported |
| Release practice | [`docs/release-policy.md`](release-policy.md) defines the public surface, the versioning rules and the criteria for a release; [`CHANGELOG.md`](../CHANGELOG.md) records public-surface changes as they land |
| Second build path | The Makefile is documented as an alternative to CB rather than as legacy, and running it is a release criterion; the dead code its warnings exposed is removed |
| Document accuracy | `CONTRIBUTING.md` rewritten around both build paths and the checks CI gates; `cb.md`'s make targets corrected against the Makefile; `AGENTS.md` now covers the abort that produces no `summary`; the `[acceptor]` example is gone from the runner's `--help`; every cross-document link and heading anchor checked |

## Verified Signals

Measured on this working tree, debug config, on arm64 macOS with a locally built trunk
Clang (23.0.0git) — not the documented Clang 21 Linux configuration that CI exercises:

| Check | Result |
|-------|--------|
| `test --tags='\[self\]'` | 58 / 58 tests, 433 / 433 assertions |
| `test` (standalone, unfiltered) | 146 / 146 tests, 484 / 484 assertions |
| `test --tags='\[.demo\]'` | fails by design: 15 / 42 tests, `passed: false` |
| `tests/cb/smoke.sh` | 245 checks pass (~79 s; it drives many real builds) |
| `tests/mcp/smoke.sh` | 9 checks pass |
| `tests/jsonl/validate.py` | 9 commands, 827 events schema-validated |
| Warm no-op build | 516 ms, 36 compile units all cache hits (35 sources plus `std`) |
| Cold build after `clean` | 16.2 s for the same 36 units, everything rebuilt |
| `make tests` then `--tags='[self]'` | builds into `build-darwin/`, 58 / 58 tests, warning-free after the two dead-code removals |

Sizes: framework and its tests ≈ 6,600 lines; CB ≈ 4,500 lines, of which
[`tools/cb.c++`](../tools/cb.c++) is 3,202.

## Strengths

### Framework and API

- The public surface is deliberately small: `import tester;` exports the framework
  partitions while keeping the publisher an internal import
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

### Architecture and implementation

- The framework's format-neutral observer contract separates run and assertion events
  from console and JSONL presentation
  ([`tester/tester-observer.c++m`](../tester/tester-observer.c++m)); observers are
  registered by name and selected from the CLI in the composition root
  ([`tester/test_runner.c++`](../tester/test_runner.c++)).
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
  triaged without rerunning the build.
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

What is left is the classification heuristic:

- Whether a registration becomes a scheduled case or a nested step is decided by
  searching the registering function's name for `scenario` or `test_case`. Two
  consequences follow: a `test_case` written inside a running case is promoted to a
  top-level case reached later by the run loop, and a BDD step written where no run is
  active degrades to an ordinary case instead of being rejected.
- `test_order` is accepted only by `scenario` and `test_case`, so priority and
  dependencies never apply to steps. That is defensible, but it is a rule readers must
  infer from overload sets.

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
units), but both are shape-dependent rather than bounded.

CB is tested, but only from the outside. [`tests/cb/smoke.sh`](../tests/cb/smoke.sh) is
thorough — 41 cases, 245 checks, covering cache profiles, every rebuild reason, strict
argument handling, compile/link/test failures, and a dozen scanner edge cases — yet 39 of
those cases drive a real build against a temporary project, so the suite costs ~79 s and
no internal function can be asked a question on its own. Two cases stop at `list` and
need no compiler; the scanner cases could mostly do the same.

### Maturity and portability

- Supported usage depends on Clang 21 and libc++'s `std.cppm`; the macOS setup is a
  locally built LLVM, and Xcode or Homebrew toolchains are explicitly unsupported
  ([`README.md`](../README.md)).
- The runner's crash tracing uses POSIX `<execinfo.h>` and handles `SIGSEGV` and
  `SIGABRT` only, so a Windows port would need a second implementation before anything
  else. `README.md` now says plainly that Windows is unsupported.
- Mature-framework capabilities remain absent: fixtures and setup/teardown,
  parameterization or generators, matcher combinators, death tests, timeouts, repeats,
  shuffling, sharding, runtime skip, and third-party reporter formats such as JUnit XML.
  Filtering, dependency ordering, hidden tags, and BDD nesting are the substitutes.
- Release discipline is now *stated* but not yet *exercised*:
  [`docs/release-policy.md`](release-policy.md) defines the public surface, the semantic
  versioning rules (including that raising the minimum compiler is breaking), the
  deprecation window, and the criteria a release must meet — one of which is this review
  rating the repository at or close to 9 / 10. [`CHANGELOG.md`](../CHANGELOG.md) carries
  an `Unreleased` section covering everything after the November 2025 pre-release, so a
  consumer reads a summary instead of 119 commit subjects. What remains is the artefact:
  no supported tag, so the pin is still a commit. Deliberate for a project that says so
  out loud, and stated as such in the README.
- Backlog discipline, by contrast, is good:
  [`docs/tester-improvements.md`](tester-improvements.md) tracks roughly a hundred
  implemented items against about sixty proposed ones, grouped by subsystem, and there
  are no open issues. The one open pull request duplicates a fix already on `main`,
  which is the normal cost of bot-filed reports.

### CI and documentation consistency

Every job now either gates or says it does not. The self-test and standalone-suite steps
read the last `summary` / `run_end` and fail on `passed != true`; a further step requires
the `[.demo]` selection to both exit non-zero and report `passed: false`; the CB, MCP,
and schema-validation jobs each assert a structured success marker; and static analysis
is named `static-analysis (advisory)`, explains why in a comment, and uploads its
findings instead of discarding them to `/dev/null`
([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)).

Getting there removed a job rather than repairing it. `test-examples` existed to build
and run "with examples", but [`tools/CB.sh`](../tools/CB.sh) sets
`CB_INCLUDE_EXAMPLES_MODE=always`, so every standalone invocation already includes
`examples/` — the job was installing a toolchain and rebuilding the same 36 units to run
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
invoked from a parent `make`, and its runner passes the `[self]` suite 58/58 — verified on
macOS while writing this. `README.md` had it labelled *legacy*, which was wrong and is
fixed. Its real limits are that it has no object cache, no build telemetry, and no CI
coverage, so it is checked by running it — which is now a release criterion, because a
full rebuild there surfaced two dead templates in the console observer and an unused
lambda capture in `tester:utils` that incremental CB builds had stopped re-emitting. One
target is broken rather than merely unverified: `make tools` globs `tools/*.c++`, so it
fails on macOS at `core_pc.c++`'s `<elf.h>`, and since CB never scans `tools/`, that
utility has no build path anyone exercises.

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
CB's single-file implementation is defensible for portability, vendoring, and
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
tests, and ecosystem integration.

BDD is no longer the ergonomic hazard it was. A step's captures are as valid as any
lambda invoked in the scope that wrote it, which is the rule a C++ reader already
applies. The residual surprises are quieter: nesting rules and `test_order`
availability both depend on which wrapper you called, and that mapping lives in
overload sets rather than in the API's shape.

### Implementation quality

The engine's accounting is careful: assertions consumed by steps are subtracted so a
parent reports its own, a non-fatal failure still fails the test, and captured output
is handed back to the enclosing case rather than lost. CB's structured rebuild
reasons, bounded diagnostics with a path to the full capture, decoded process status
(distinguishing a signalled toolchain crash from a non-zero exit), and RAII observer
scopes are all above-average choices.

The intentional trade-offs should stay visible: regex scanning, shell-mediated process
invocation, recursive staleness evaluation, and module-file flags attached broadly to
compiler invocations. Two small deviations from CB's own "one `std::system` boundary"
rule remain — the compiler probe and the compiler-version stamp call `system` directly
rather than through `invoke_shell`. The probe is also the one place a value reaches the
shell unquoted: a bare `CXX` or `LLVM_CXX` name is interpolated into
`command -v <name>` without `shell_quote`. The blast radius is the user's own
environment, so this is hygiene rather than a vulnerability, but it is exactly what the
single-boundary rule exists to prevent.

### AI-agent friendliness

This remains the project's clearest differentiator. Failures mode gives agents a small
deterministic event set; trace mode exists when telemetry is actually wanted. The test
catalogue and the MCP bridge remove filename and filter guessing, and `AGENTS.md`
states the parsing contract precisely enough to follow mechanically. The recent
non-finite float fix matters more here than its size suggests: a single unparseable
line breaks a strict JSONL consumer, and the schema validator now covers the whole
`[self]` suite rather than a subset, which is what caught it.

### Production readiness

Credible for a controlled Clang 21 environment and a good candidate for internal or
embedded use where CB's assumptions are accepted. Not yet broadly production-ready as
a portable library product, because platform coverage, release discipline, and
ecosystem integration remain incomplete.

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
4. Reconsider the classification heuristic, which is the last piece of the registration
   story that reads the wrong input: a case's kind is decided from the registering
   function's name, so a step written outside a run silently becomes a case.
5. ~~Say which platforms are verified and how.~~ Done: [`README.md`](../README.md) now
   states that both platforms run the same suites, that Linux runs them on every push
   while macOS runs them on demand, and that Windows is unsupported. A hosted macOS lane
   is blocked on the platform, not declined: no clang on a macOS runner builds C++23
   modules for this project today, and it is planned for when one does.

### Medium priority

1. Let CB be tested without invoking a compiler. Coverage is not the gap — 41 smoke cases
   and 245 checks are thorough — but 39 of those cases run a real build, including the
   pure scanner cases that `list` alone could answer, as two cases already demonstrate.
   Scanner questions answered through `list` and a cache model reachable without
   compiling would make the suite fast enough to run per edit; asking a function like
   `splice_physical_lines` directly needs the extraction below first.
2. Replace per-level thread fan-out with a bounded worker pool, and memoize staleness
   results for the duration of one build pass. Both are cheap and remove
   shape-dependent behavior.
3. ~~Rewrite CONTRIBUTING with current artifact paths, both build paths, and which one CI
   gates; fix the `[acceptor]` example.~~ Done, together with the `cb.md`, `README.md`, and
   `AGENTS.md` corrections listed above. What remains is `make tools` on macOS.
4. ~~Publish versioning, compatibility, and changelog practices for submodule
   consumers.~~ Done: [`docs/release-policy.md`](release-policy.md) and a maintained
   [`CHANGELOG.md`](../CHANGELOG.md). What is left is the tag itself, which the policy
   gates on this review — see the note under Score.
5. Route the compiler probe and version stamp through `invoke_shell` so the
   single-boundary rule holds without exceptions.

### Low priority / strategic

1. Add fixtures, parameterization, and composable matchers only where real adopter
   needs justify the complexity; the current API's coherence is an asset.
2. Offer an opt-in compiler-assisted dependency scanner (`clang-scan-deps`) for
   projects that exceed the documented regex envelope, rather than growing the regexes.
3. Add JUnit or SARIF adapters only when integration demand warrants; JSONL should stay
   the native interface.

## Score

| Score | When | What moved it |
|-------|------|---------------|
| 7.2 | 29 July | Baseline: BDD steps could outlive the captures they were written against, and their run order rested on a name-search heuristic |
| 7.5 | 31 July, as reviewed | Both registration hazards closed, comparison semantics hardened by eight narrow fixes, JSONL valid for non-finite floats and schema-validated across the whole `[self]` suite, CB cache reporting uniform |
| **8.0** | 31 July, after the response | Ordering metadata refuses what it cannot honour, tags are data on the case, every CI job either gates or says it is advisory, and the supported-platform story is written down |

Each step was verified rather than asserted: 58 self tests with 433 assertions, 146
tests standalone, 245 CB smoke checks, 9 MCP checks, and 827 JSONL events validated
against the schema.

Three things cap it below 8.5, in order of weight:

1. **CB is verifiable only by compiling.** 39 of 41 smoke cases invoke a real toolchain,
   so the build system's own correctness costs ~79 s to confirm and no internal function
   can be questioned directly. This is the one cap that is entirely within reach today.
2. **Framework surface.** No fixtures, parameterization, matcher combinators, death
   tests, timeouts, repeats, shuffling, or JUnit-style reporters. Deliberate, but it is
   what a team comparing against Catch2 or GoogleTest will weigh.
3. **No tag to pin.** The compatibility half of this is now answered —
   [`docs/release-policy.md`](release-policy.md) states the public surface and the
   versioning rules, and [`CHANGELOG.md`](../CHANGELOG.md) records changes as they land
   rather than at release time. What is left cannot be closed by documentation: there is
   no version to depend on. It is also self-referential, because the policy makes this
   review's score a release criterion — the score cannot rise by releasing, and the
   release waits on the score. Treat it as a bootstrap dependency that resolves in one
   step once the two caps above are addressed, not as a defect to fix separately.

An 8.5 does not require becoming Catch2. It requires that CB can be trusted without a
minute and a half of compiling, and that the framework's remaining gaps are ones a
consumer chooses to live with rather than discovers.

## Overall Assessment

Tester is a strong specialist tool with better-than-average internal engineering,
diagnostics, and agent-oriented workflows for its maturity. The movement from 7.2 to 8.0
is closure, not polish: the framework no longer has a hazard that turns idiomatic C++
into undefined behavior, no longer accepts ordering metadata it cannot honour, and no
longer has CI steps that report success by construction.

It is still not maturity-equivalent to established general-purpose frameworks, for the
three reasons above plus the toolchain constraint that Clang 21 and libc++ modules
impose. The recommended path remains targeted hardening rather than a rewrite — preserve
the focused modules-first identity while removing the remaining places where the
framework infers what the author meant instead of being told, of which the registration
classification heuristic is now the last one inside the framework itself.
