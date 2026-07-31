# Changelog

Notable changes to the public surface defined in [`docs/release-policy.md`](docs/release-policy.md):
the exported `tester` API, the `test_runner` and CB command lines, and the JSONL contract.
Internal refactoring is not listed — roughly half the commits since `v1.0.0` reorganize
`tools/cb.c++` and the observer layer without changing behaviour, and the git log covers them.

Versions follow the rules in the release policy. `Unreleased` accumulates until the policy's
release criteria are met.

## Unreleased

This section covers everything after `v1.0.0`, which predates the JSONL contract, the
observer layer, CI, the test suites under `tests/`, and CB's cache model. Consumers pin a
commit on `main`; there is no supported tag yet.

### Added

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
  included it (`header_stale`), and an unusable depfile rebuilds rather than guessing
  (`depfile_unusable`).
- **Cache inspection**: `cache status` reports all four cached artefacts and whether each
  matches the current toolchain profile; `cache invalidate` removes them.
- **`--jobs=N`** to bound concurrent compile and link processes.
- **Diagnostics in events**: a failed compile, link, or command carries the toolchain
  output in `diagnostics.text` (with the full capture at `diagnostics.path`), so a failure
  needs no second build to read.
- **MCP stdio bridge** ([`tools/cb_mcp.py`](tools/cb_mcp.py)) wrapping the canonical CB
  commands for editors and agents.
- **Test suites**: `[self]` contract tests for the framework, CB behaviour tests
  ([`tests/cb/smoke.sh`](tests/cb/smoke.sh)), MCP checks, and schema validation, all run by
  CI on Linux for `debug` and `release`.
- **`AGENTS.md`** as the automation contract, and hidden `[.tag]` fixtures so intentional
  failure demos stay out of unfiltered runs.

### Changed

- **Nested BDD steps run eagerly**, at the point of assignment inside the parent's frame,
  so `given` / `when` / `then` bodies capturing parent locals by reference are safe. They
  previously ran after the parent returned.
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

## v1.0.0 — 27 November 2025

A GitHub **pre-release**, kept for history and unsupported. It predates everything above,
including the JSONL contract, the observer layer, and CI. No changelog was maintained at
the time and none is reconstructed here; read the git log if you need its state. The
version number will not be reused — the first supported release takes a new one.
