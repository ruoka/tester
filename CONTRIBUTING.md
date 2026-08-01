# Contributing to Tester

Thanks for your interest. This describes how to build the project, what has to pass before
a change is submitted, and the conventions the code follows.

## Development Setup

### Prerequisites

- **Clang 21 or newer** with C++23 module support and a libc++ `std` module. Trunk Clang
  also works; the CI configuration is Clang 21 on Linux. On macOS you need a locally built
  LLVM — see [`docs/clang-modules-macos.md`](docs/clang-modules-macos.md). Windows is not
  supported.
- **`clang-scan-deps`** (ships with the toolchain) — required only for the Makefile path.
- **CMake 4.x and Ninja** — required only for the `CMakeLists.txt` path. CI and the dev
  container install 4.1.2; older CMake is not tested.
- **Python 3** — for the JSONL schema validator and the MCP bridge.
- **Git.** There are no submodules in this repository; a plain `git clone` is complete.

The repository includes a devcontainer with the Linux toolchain and all three build paths
(CB, make, CMake + Ninja): open it in VS Code and choose "Reopen in Container".

### Build and test with CB (what CI gates)

[CB](docs/cb.md) is the default path. It discovers translation units, orders module
imports, compiles in parallel, and caches objects, so the usual loop is one command:

```bash
git clone https://github.com/ruoka/tester.git
cd tester

./tools/CB.sh debug test --tags='\[self\]'   # framework contract tests
./tools/CB.sh debug test                     # everything, examples included
./tools/CB.sh debug build                    # build only
./tools/CB.sh debug clean                    # start over
```

Artifacts land in `build-<os>-<config>/` (`pcm/`, `obj/`, `bin/`, `cache/`), so the runner
is at `build-<os>-<config>/bin/test_runner` — for example `build-darwin-debug/bin/test_runner`.

Add `--jsonl=failures` to any command for machine-readable output; that is what CI and
agents read, and [`AGENTS.md`](AGENTS.md) documents the contract.

For a clean rebuild-and-run entry point (what many CI jobs want):

```bash
./tools/CB.sh ci --jsonl=failures   # clean + test; JSONL on stdout
```

### Sandbox environment (Cursor / network-heavy consumers)

`tools/CB.sh.core` can skip network tests when a Cursor sandbox is active. The hook is
**opt-in per wrapper** via `CB_SANDBOX_DISABLE_NETWORK_TESTS`:

| Setting | This repo (`tester`) | Typical `net` consumer |
|---------|----------------------|-------------------------|
| `CB_SANDBOX_DISABLE_NETWORK_TESTS` | `0` (off) | `1` (on) |

When the hook is on **and** `CURSOR_SANDBOX` is set **and** `NET_DISABLE_NETWORK_TESTS`
is not already set in the environment, the wrapper exports `NET_DISABLE_NETWORK_TESTS=1`.
An explicit `NET_DISABLE_NETWORK_TESTS=0` (or any set value) is left alone.

This standalone tester tree does not enable the hook — it has no network suite. Parent
repos that do (see `tools/CB.sh.template`) turn it on so sandboxed CI/agent runs do not
hang on live network tests. When either variable is set, `run_start.env` in JSONL records
it so a skipped network suite is explainable from the stream.

### Build and test with make

The `Makefile` is a supported alternative that uses `clang-scan-deps` for module ordering
and needs no CB bootstrap. It has no object cache and no build telemetry. CI gates
`make tests` plus `[self]` on every push; run it locally when you change module structure
or want a second opinion:

```bash
make tests                                     # build-make-<os>-release/bin/test_runner
make DEBUG=1 tests                             # build-make-<os>-debug/…
make run_tests TEST_TAGS='--tags=[self]'       # build and run
make run_examples                              # build and run examples/
make module                                    # modules + lib/libtester.a under the same tree
make mostlyclean                               # drop obj/ and pcm/, std.pcm included
make clean                                     # the above plus bin/ and lib/
```

`make tools` builds `tools/*.c++` into `build-make-<os>-<config>/bin/tools/`, but only on Linux: the glob
picks up `core_pc.c++`, which includes `<elf.h>`. CB does not scan `tools/` at all, so that
utility has no other build path.

Keep builds warning-free. CB attaches compiler warnings to successful `compile_end` /
`link_end` events (`diagnostics` in `--jsonl=failures`); Make has no cache, so a full
`make` rebuild compiles every unit and the CI Makefile job fails on any `warning:`. The
CMake lane below is gated the same way.

### Build and test with CMake + Ninja

[`CMakeLists.txt`](CMakeLists.txt) is the third supported path, for consumers who already
build with CMake and as the worked example of wiring C++23 modules (`FILE_SET CXX_MODULES`
plus `import std`) into a CMake project. Run it when you change the module set, since the
file sets there are maintained by hand rather than discovered:

```bash
cmake -S . -B build-cmake-darwin-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake-darwin-debug                         # …/test_runner
cmake --build build-cmake-darwin-debug --target run_tests      # [self]
cmake --build build-cmake-darwin-debug --target run_all_tests  # everything, examples included
```

`-DTESTER_STATIC=ON` mirrors the Makefile's `STATIC=1`, and the build type maps to the same
flags `config/compiler.mk` uses. The generator has to be Ninja — module scanning needs it —
and adding compiler flags is the one rough edge:

| Attempt | Result |
|---------|--------|
| `CXXFLAGS=…` | Configure fails. CMake detects the standard library *without* it, so `import std` resolves against libstdc++ and stops on a missing `libstdc++.modules.json` |
| `-DCMAKE_CXX_FLAGS=…` | Replaces `CMAKE_CXX_FLAGS_INIT`, dropping `-stdlib=libc++`; same failure |
| `-DCMAKE_CXX_FLAGS_INIT=…` | Silently ignored — the file sets that variable unconditionally |

Edit `CMAKE_CXX_FLAGS_INIT` in `CMakeLists.txt` instead. It is the only place that also
reaches CMake's internal `std` target, and a flag that changes the language configuration
(`-fsigned-char`, say) has to reach it or the compile fails on `std.pcm` being built with a
mismatched configuration.

## What has to pass

Before opening a pull request:

```bash
./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'   # must report passed: true
./tools/CB.sh debug test --jsonl=failures                     # standalone suite
make clean && make run_tests TEST_TAGS='--tags=[self]'        # second path; CI gates this too
cmake --build build-cmake-darwin-debug --target run_tests     # third path; if you changed the module set
./tests/cb/smoke.sh                                           # if you touched tools/
./tests/mcp/smoke.sh                                          # if you touched tools/cb_mcp.py
./tests/jsonl/validate.py --require-schema                    # if you touched any event
```

The `[self]` suite is the gate: it covers registration, tag filtering, `depends_on`
ordering, the assertion matchers, and the JSONL events. A change to framework behaviour
belongs with a `[self]` test that fails without it.

Read the verdict from the last `summary` event's `passed` field, not from the exit code
alone — see [`AGENTS.md`](AGENTS.md#triage-workflow-test-failure).

## Testing conventions

- Tests are co-located with sources as `*.test.c++`, framework tests under `tester/`,
  examples under `examples/`.
- Tag framework tests `[self]` so they run in the gate.
- Bracket tags starting with `.` are hidden from unfiltered runs (`[.demo]` marks the
  intentional failure demos in `examples/`). Select them explicitly when you need them.
- Filter by tag or by name pattern:

```bash
build-darwin-debug/bin/test_runner --tags='[self]'
build-darwin-debug/bin/test_runner --tags='scenario.*Happy'
build-darwin-debug/bin/test_runner --list
```

- Ordering metadata is checked before the first test runs. A duplicate `test_order::id`, a
  `depends_on` id that names no test, or a dependency cycle aborts the run with a message
  naming the offender.

## Assertions

Two families, both macro-free:

- **`require_*`** — fatal: throws `assertion_failure` and ends that test.
- **`check_*`** — non-fatal: records the failure and continues.

```cpp
require_eq(a, b);                       // fatal if a != b
check_eq(a, b);                         // records and continues
require_throws_as<std::runtime_error>([]{ throw std::runtime_error{"x"}; });
```

Name the exception type rather than passing an instance; the instance form is deprecated.
The full list is in [README — Assertion Reference](README.md#assertion-reference).

## Code Style

The project follows the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
with these deviations and additions:

**Naming** — all identifiers are `snake_case`, including types:

- Types: `test_case`, `build_system` (not `TestCase`, `BuildSystem`)
- Functions: `require_eq()`, `check_neq()` (not `requireEq()`)
- Variables: `test_name`, `assertion_count`

**Formatting** — 4 spaces, no tabs. Prefer the spelled operators `not`, `and`, `or` over
`!`, `&&`, `||` in new code.

**Standard library first** — the project targets C++23 and prefers standard algorithms and
views over hand-rolled loops: `contains` over `find(...) != end()`, `views::join_with` over
index loops with delimiter checks, `views::split` over ad-hoc parsers.
[`AGENTS.md`](AGENTS.md#c-style-tools-and-tester) has the full policy with examples,
including the rule that `tools/` and `tester/` use ISO C++ rather than POSIX APIs, with the
documented exceptions (`posix_spawn` for subprocesses, `<execinfo.h>` for crash traces).

**Comments** explain intent, trade-offs, and constraints the code cannot state — not what
the next line does.

## Module Organization

- Module interfaces use `.c++m`, other translation units `.c++`, tests `*.test.c++`.
- Module names are partitions of `tester` (`tester:assertions`, `tester:runner`, …), one
  partition per file, named `tester-<partition>.c++m`.
- Follow [P1204R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html)
  layout: the module tree is `tester/`, tools are in `tools/`, tests live beside sources.
- A partition is internal unless [`tester/tester.c++m`](tester/tester.c++m) re-exports it —
  `:engine` is deliberately not exported. Adding to the exported set is a
  public API change; see [`docs/release-policy.md`](docs/release-policy.md).

## Submitting Changes

1. Branch: `git checkout -b fix/what-it-fixes`
2. Make the change, with a test that fails without it
3. Run the checks above; keep the build warning-free
4. Commit with a message that says why, not what
5. Push and open a pull request

If the change touches the public surface — exported names, either CLI, or the JSONL events
— add an entry to [`CHANGELOG.md`](CHANGELOG.md) under `Unreleased`.

## Where Things Are Documented

| Topic | Document |
|-------|----------|
| Using the framework | [`README.md`](README.md) |
| JSONL contract, agent workflow | [`AGENTS.md`](AGENTS.md), [`docs/jsonl-schema.json`](docs/jsonl-schema.json) |
| CB design, cache model, targets | [`docs/cb.md`](docs/cb.md) |
| Sandbox / `NET_DISABLE_NETWORK_TESTS` | [Sandbox environment](#sandbox-environment-cursor--network-heavy-consumers) above; wrapper table in [`tools/CB.sh.template`](tools/CB.sh.template) |
| Versioning and release criteria | [`docs/release-policy.md`](docs/release-policy.md) |
| Known gaps and planned work | [`docs/tester-improvements.md`](docs/tester-improvements.md) |
| macOS toolchain setup | [`docs/clang-modules-macos.md`](docs/clang-modules-macos.md) |

## Questions

Open an issue on GitHub.
