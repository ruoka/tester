# Technical Review — Tester and CB

**Review scope:** local repository implementation, documentation, CI configuration, and public GitHub repository context.  
**Date:** 29 July 2026  
**Assessment:** **7.2 / 10**

## Executive Summary

Tester is a coherent C++23 modules-first testing framework for teams willing to
standardize on its toolchain and workflow. Its strongest differentiators are the
macro-free API, careful assertion semantics, structured JSONL event contract, and
CB's capable incremental module build pipeline. The implementation demonstrates
substantial systems-engineering attention in numeric comparison, failure
attribution, observer separation, depfile-aware caching, and diagnostic telemetry.

The principal risks are architectural rather than cosmetic: tags remain embedded in
display names, and CB deliberately uses an approximate source scanner. The two
registration risks this review opened with — BDD steps outliving the captures they
were written against, and their placement in the run order resting on name-based
heuristics — are fixed: a step runs at its assignment, inside the body that declared
it. Wider adoption is also constrained by the Clang 21/libc++ module
requirement, POSIX-specific crash handling, Linux-only CI coverage, and the lack of
a release/versioning policy. Tester is a good fit for focused module-native projects
and AI-assisted development today, but is not yet a broad replacement for mature
framework ecosystems such as Catch2, doctest, or GoogleTest.

## Strengths

### Framework and API

- The public surface is deliberately small: `import tester;` exports the framework
  partitions while retaining the publisher implementation as an internal import
  ([`tester/tester.c++m`](../tester/tester.c++m)).
- The `require_*` / `check_*` split is easy to understand and maps cleanly to fatal
  and non-fatal failure behavior.
- Assertion semantics go beyond basic equality: mixed signedness, floating point,
  tuple-like values, containers, strings, and exception types receive considered
  behavior and reporting ([`tester/tester-assertions.c++m`](../tester/tester-assertions.c++m)).
- Source locations are retained without requiring test-registration or assertion
  macros.
- `test_order` exposes test identifiers, dependencies, and priority directly
  ([`tester/tester-basic.c++m`](../tester/tester-basic.c++m)).

### Architecture and implementation

- The framework's format-neutral observer interface separates run and assertion
  events from console and JSONL presentation
  ([`tester/tester-observer.c++m`](../tester/tester-observer.c++m)).
- CB has a clear conceptual pipeline: translation-unit discovery, module graph
  construction, cache/rebuild decisions, compilation/linking, and observers.
- CB caching is substantive rather than superficial. It accounts for cache profiles,
  source/object timestamps, depfiles, headers in the project tree, PCMs, transitive
  imports, and executable signatures ([`tools/cb.c++`](../tools/cb.c++)).
- JSONL carries actionable source locations, matcher identity, expected/actual
  values, failed test IDs, and build diagnostics. The schema and validation script
  make this a usable automation contract
  ([`docs/jsonl-schema.json`](jsonl-schema.json),
  [`tests/jsonl/validate.py`](../tests/jsonl/validate.py)).

### Documentation and automation

- [`AGENTS.md`](../AGENTS.md) gives concrete command, output-parsing, and
  failure-triage rules rather than generic agent guidance.
- [`docs/ai-agent-recommendation.md`](ai-agent-recommendation.md) correctly
  distinguishes reduced diagnostic context from an unconditional build-speed or
  token-use claim.
- `[self]` framework contracts, CB smoke coverage, MCP smoke coverage, and JSONL
  validation provide strong focused regression signals.

## Weaknesses and Risks

### BDD lifetime safety — fixed

Nested `given` / `when` / `then` lambdas used to be queued and executed after their
parent scenario lambda had returned, so a lambda capturing a parent local by
reference held a dangling reference. That was a high-severity ergonomic risk,
because `[&]` is idiomatic C++ and the undefined behavior appeared only later, when
the deferred body ran.

A step now runs where it is written, while the body that declared it is still on the
stack, so `[&]` is safe and each step observes the ones before it
([`tester/tester-engine.c++m`](../tester/tester-engine.c++m)). Reporting is
unchanged: a step is still its own test case, listed after the case it belongs to,
counting the assertions it made itself.

### Heuristic registration and metadata

Tags are parsed from display names, while unresolved `depends_on` IDs are silently
ignored and duplicate IDs are not rejected. These choices keep the API compact, but
make behavior less predictable as a suite grows.

Nested placement was part of this: a queued step was inserted into the run list at a
position found by searching the pending names for BDD keywords, to approximate the
order in which the steps were written. Running each step at its assignment takes
that order from the call stack instead, so the heuristic is gone.

### CB scanner and scale boundaries

CB's scanner intentionally uses regexes after preprocessing source text. This is a
practical trade-off that is documented and smoke-tested, but it cannot have the
correctness envelope of a compiler parser for all preprocessor and module syntax.

`run_in_parallel` creates one `std::jthread` per unit in a dependency level and then
uses a semaphore to limit active work. This caps concurrent compiler processes, but
very wide module levels can still create excessive thread churn. Recompilation
checks also recursively revisit dependencies without a per-build memoized result.

### Maturity and portability

- Supported usage depends on Clang 21 and libc++'s `std.cppm`; the macOS setup is
  deliberately specialized and Xcode/Homebrew paths are unsupported
  ([`README.md`](../README.md)).
- The runner's crash tracing uses POSIX `<execinfo.h>`, and Windows has no supported
  CI or documented toolchain path.
- Mature-framework capabilities remain absent or incomplete: fixtures,
  parameterization/generators, composable matchers, death tests, timeouts/repeats,
  shuffling, and ecosystem-oriented reporters.
- The repository has no published versioning, compatibility, changelog, or release
  policy for submodule consumers.

### CI and documentation consistency

[`CONTRIBUTING.md`](../CONTRIBUTING.md) is Makefile-centric and refers to older
artifact paths, while README, CI, and `AGENTS.md` establish CB as the primary
workflow. This creates avoidable contributor confusion.

The CI example-test verification queries an obsolete `"result"` event even though
the current machine-readable contract uses `summary` / `run_end`. The job also uses
`continue-on-error`. Static analysis commands end with `|| true`, making the job
informational rather than a quality gate
([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)).

## Detailed Findings by Focus Area

### Architecture and design

The module partition design is appropriate and the framework/CB separation is
generally clean. CB's single-file implementation is defensible for portability,
vendoring, and discoverability, especially because its observer headers isolate
format-specific serialization. Its cost is a large review surface: scanning,
graphing, caching, compiler command construction, linking, CLI parsing, and test
orchestration share one implementation file.

Splitting CB purely to reduce line count is not yet justified. Extraction should
follow clear ownership or testing pressure, such as moving the scanner/cache model
into independently testable components.

### API and ergonomics

The framework is more natural than macro-heavy alternatives for users already
writing module interfaces, and its assertion vocabulary is readable. Catch2 and
doctest remain more immediately convenient for simple tests because their macro
registration has less namespace-scope boilerplate. GoogleTest remains stronger for
fixtures, parameterized tests, adapters, and ecosystem integration.

BDD was appealing syntactically but carried the greatest surprise cost, since its
execution model let parent-scope reference lifetimes escape. Running each step at its
assignment removes that: a step's captures are as valid as any lambda called in the
scope that wrote it, which is the rule a C++ reader already applies.

### Implementation quality

The runner correctly treats non-fatal assertion failures as failed tests; a normal
return is insufficient to mark a test successful. CB's structured rebuild reasons,
bounded diagnostics, process result decoding, and observer lifecycle scopes are
high-quality implementation choices.

The key trade-offs are intentional but should stay visible: regex module scanning,
process invocation through a shell boundary, recursive staleness evaluation, and
all module-file flags being attached broadly to compiler invocations.

### AI-agent friendliness

Tester is particularly strong in this area. JSONL failures mode gives agents a
small, deterministic set of events; trace mode is available only when detailed
telemetry is actually needed. The test catalogue and CB MCP bridge reduce
filename/filter guessing. This is a meaningful differentiator, provided consumers
follow the documented rule to parse stdout only and interpret the final `summary`.

### Production readiness

The project is credible for a controlled Clang 21/Linux environment and a good
candidate for internal or embedded use where CB's assumptions are accepted. It is
not yet broadly production-ready as a portable library product because release
discipline, CI platform coverage, and ecosystem integration remain incomplete.

## Recommendations

### High priority

1. ~~Make BDD lifetime mistakes harder or impossible.~~ Done: a step runs at its
   assignment, so a reference capture cannot outlive what it refers to.
2. Reject duplicate test IDs and unresolved dependency IDs. `test_order` is only
   accepted by `scenario` and `test_case`, so priority and dependencies apply to the
   cases a run schedules and never to the steps inside them.
3. Repair CI event checks to use `summary.passed`, remove or justify
   `continue-on-error`, and make static analysis either actionable or explicitly
   informational.
4. Add a macOS smoke lane, or clearly scope platform support to Linux until that
   coverage exists. Add an explicit Windows support statement.

### Medium priority

1. Replace per-level thread fan-out with a bounded worker model.
2. Memoize `needs_recompile` results for the duration of one build pass.
3. Update CONTRIBUTING to use CB-first commands, current artifact paths, and clear
   standalone versus embedded-project instructions.
4. Make dependency fetching deterministic and tied to stable revisions rather than
   a development branch.
5. Publish versioning, compatibility, changelog, and release practices for
   consumers.

### Low priority / strategic

1. Add fixtures, parameterization, and composable matchers only where real adopter
   needs justify the complexity.
2. Offer an opt-in compiler-assisted dependency scanner if production projects
   exceed CB's documented regex-scanner envelope.
3. Add JUnit or SARIF adapters only when integration demand warrants them; JSONL
   should remain the native interface.

## Overall Assessment

Tester is a strong specialist tool with better-than-average internal engineering,
diagnostics, and agent-oriented workflows for its maturity. It is not yet
maturity-equivalent to established general-purpose frameworks, primarily because of
platform/toolchain constraints, incomplete ecosystem features, and release/CI polish
gaps. The recommended path is targeted hardening rather than a
rewrite: preserve the framework's focused modules-first identity while removing
semantic hazards and improving consumer confidence.
