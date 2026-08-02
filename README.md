# Tester — C++23 module testing (with a built-in builder)

[![CI](https://github.com/ruoka/tester/actions/workflows/ci.yml/badge.svg)](https://github.com/ruoka/tester/actions/workflows/ci.yml)

A macro-free C++23 testing framework built entirely with modules. Write tests with `import tester;`, run them with one command, and get structured JSONL output for CI and AI agents. A single-file module-aware builder (CB) ships with the repo so you do not need CMake on day one.

```c++
import readme_unit_example;
import tester;

namespace readme_unit_example {
auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;

    test_case("foo::add handles signed math") = [] {
        require_eq(add(2, 2), 4);
        require_eq(add(-5, 3), -2);
    };

    return 0;
}

const auto _ = register_tests();
} // namespace readme_unit_example
```

```bash
./tools/CB.sh debug test --tags='readme_unit'
```

## Table of Contents

- [Why Tester](#why-tester)
- [Quick Start](#quick-start)
- [Writing Tests](#writing-tests)
- [Running Tests](#running-tests)
- [Built-in Builder (CB)](#built-in-builder-cb)
- [JSONL & Automation](#jsonl--automation)
- [How Tester Compares](#how-tester-compares)
- [Requirements](#requirements)
- [Versioning & Releases](#versioning--releases)
- [Repository Layout](#repository-layout)
- [Assertion Reference](#assertion-reference)
- [Utilities](#utilities)
- [Troubleshooting](#troubleshooting)
- [Architecture](#architecture)
- [License](#license)
- [Related Resources](#related-resources)

## Why Tester

Most test frameworks assume headers, macros, and a separate build system. Tester is built for C++23 modules:

- **Modules-first** — `import tester;`, no `TEST()` macros, no generated registration boilerplate
- **Readable assertions** — `require_eq`, `require_throws_as`, `require_container_eq`, and matching non-fatal `check_*` variants
- **Unit tests and BDD** — `test_case` for straightforward tests; `scenario` / `given` / `when` / `then` for behaviour-driven style
- **Tag-based filtering** — bracket tags (`[self]`, `[api]`), hidden tags (`[.probe]` — Catch2-style, excluded unless explicitly selected), substrings, or regex via `test_runner`
- **Machine-readable output** — JSONL on stdout for agents, dashboards, and CI triage
- **Built-in builder** — CB resolves module dependencies, caches incrementally, and compiles in parallel

Tester embeds as a git submodule (`deps/tester`) in larger projects — see [Embedding & tester resolution](#embedding--tester-resolution) for how `CB.sh` finds it. For a public multi-module consumer using tester and CB, see [YarDB](https://github.com/ruoka/YarDB). Framework contract tests live in `tester/*.test.c++` under the `[self]` tag. The `examples/` directory holds demos; assertion-failure showcases use the hidden tag `[.demo]` (excluded from default runs — use `--tags=[.demo]` to execute them).

## Quick Start

```bash
git clone https://github.com/ruoka/tester.git   # no submodules; a plain clone is complete
cd tester

# Framework contract tests (CI gate)
./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'

# Clean rebuild + full suite (JSONL on stdout — good CI entry point)
./tools/CB.sh ci --jsonl=failures

# Build
./tools/CB.sh debug build

# Run all registered tests (includes examples when standalone)
./tools/CB.sh debug test
```

The `[self]` suite exercises JSONL catalogue events, `run_start` metadata, tag filtering, and `depends_on` ordering. CI requires `summary.passed` (or `run_end.passed`) to be `true`.

## Writing Tests

Tests live in `*.test.c++` (or `*.test.c++m`) files. Register cases from a function called at namespace scope (`const auto _ = register_tests();`). Put bracket tags in the case name, e.g. `test_case("my feature [api] does X")`; they are parsed once at registration and are what `--tags` filters on.

Ordering metadata is checked before the first test runs. A `test_order{.id = …}` must be unique, and every `depends_on` entry must name an id that exists — a duplicate id, an unknown dependency, or a cycle stops the run with a message naming the id, rather than quietly dropping the ordering constraint.

### Unit test (module + test file)

Code under test in a module interface:

```c++
export module readme_unit_example;

namespace readme_unit_example {
export int add(int lhs, int rhs) { return lhs + rhs; }
}
```

Tests in a separate translation unit:

```c++
import readme_unit_example;
import tester;

namespace readme_unit_example {
auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;

    test_case("foo::add handles signed math") = [] {
        require_eq(add(2, 2), 4);
        require_eq(add(-5, 3), -2);
        check_eq(add(0, 0), 0); // non-fatal variant
    };

    test_case("foo::add with floating-point inputs") = [] {
        require_eq(0.3, 0.1 + 0.2);         // default epsilon path
        check_near(0.3, 0.1 + 0.2, 1e-9);   // explicit tolerance
        require_near(0.0, add(1.0, -1.0));  // fatal variant
    };

    test_case("foo::add with container assertions") = [] {
        auto results = std::vector<int>{add(1, 2), add(3, 4), add(5, 6)};
        require_container_eq(results, std::vector<int>{3, 7, 11});
    };

    return 0;
}

const auto _ = register_tests();
} // namespace readme_unit_example
```

When the test TU is part of a named module, use a module declaration at the top (see `examples/foo.test.c++`).

### Behaviour-driven test

A step (`given` / `when` / `then` and their `and_` forms, or `section` in a `test_case`) runs the moment it is assigned — inside the body that declared it, before that body reaches its next line. Capturing enclosing locals by reference (`[&]`) is therefore safe, and each step sees what the steps before it did. Every step is still reported as a test case of its own, listed after the case it belongs to, with the assertions it made itself.

```c++
import std;
import tester;

using namespace tester::behavior_driven_development;
using namespace tester::assertions;

namespace ordering {
struct order {
    bool submitted = false;
    void submit() { submitted = true; }
};
}

auto readme_bdd_feature()
{
    using ordering::order;

    scenario("Customer places an order") = [] {
        auto o = order{};
        given("a draft order") = [&] {
            when("the customer confirms") = [&] {
                o.submit();
                then("the order is marked as submitted") = [&] {
                    require_true(o.submitted);
                    require_nothrow([&]{ o.submit(); });
                };
            };
        };
    };

    scenario("Submission fails") = [] {
        given("a faulty payment gateway") = [] {
            then("submitting raises an error") = [] {
                require_throws([] { throw std::runtime_error{"gateway down"}; });
            };
        };
    };

    return 0;
}

const auto _ = readme_bdd_feature();
```

Working copies: `examples/readme_unit_example.*`, `examples/readme_bdd_example.test.c++`.

## Running Tests

```bash
# Build and run all registered tests
./tools/CB.sh debug test

# Filter by tag (escape brackets in shell)
./tools/CB.sh debug test --tags='\[self\]'

# Substring or regex filter
./tools/CB.sh debug test "scenario.*Happy"

# List registered tests (human)
./tools/CB.sh debug test --list

# Machine-readable catalogue
./tools/CB.sh debug test --list --jsonl=failures
```

Pass test_runner options directly (CB recognizes them):

```bash
./tools/CB.sh debug test --jsonl=trace --slowest=10
./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'
./tools/CB.sh debug test --jsonl=failures --junit=report.xml --tags='\[self\]'
./tools/CB.sh debug test --jobs=4 --tags='\[self\]'   # parallel top-level tests (+ compile jobs)
./tools/CB.sh debug test --result   # stable RESULT: line on stderr in JSONL mode
```

**Tag filtering:**
- **Bracket tags** — `test_case("… [api] …")` then `--tags='\[api\]'`
- **Hidden tags** — `test_case("… [.integration] …")` is skipped on an unfiltered run; pass `--tags='\[.integration\]'` (or a matching substring) to run it
- **Substring** — `--tags=simulator` matches any test name containing `simulator`
- **Regex** — `--tags="scenario.*Happy"`; invalid regex falls back to substring matching

The runner prints human-readable results on stderr (stdout in human mode), returns non-zero when any test fails, and emits JSONL on stdout with `--jsonl[=summary|failures|trace]`. `--junit=<path>` (alias `--xunit-xml=`) also writes a JUnit-compatible XML report; it is additive, so it can run next to JSONL.

### Makefile runner (alternative to CB)

CB is not required. The bundled `Makefile` builds the same library and runner with
`clang-scan-deps` for module ordering, into `build-make-<os>-<config>/` — useful if your
project already builds with make, or as a second opinion when a CB result looks wrong.
Default config is `release` (`-O3`); `DEBUG=1` selects `debug`:

```bash
make tests                                       # build-make-<os>-release/bin/test_runner
build-make-darwin-release/bin/test_runner --tags='[self]'   # …-linux-… on Linux
build-make-darwin-release/bin/test_runner --list
make DEBUG=1 tests                               # build-make-<os>-debug/…
make run_tests TEST_TAGS='--tags=[self]'         # build and run in one step
```

It also works embedded: invoked from a parent `make`, it picks up `../../config/compiler.mk` and the parent's `PREFIX`. What it does not have is CB's object cache or JSONL build telemetry. CI gates `make tests` / `make run_tests` with `[self]` on every push (`makefile-build-and-test`); CB remains the primary path and also surfaces compiler warnings from units it compiles.

### CMake + Ninja build (alternative to CB)

The bundled [`CMakeLists.txt`](CMakeLists.txt) builds the same library and runner through
CMake's own module scanner, so a project that already uses CMake can consume tester without
adopting CB or make. It is also the worked example to copy from if you are wiring C++23
modules into your own `CMakeLists.txt`:

```bash
cmake -S . -B build-cmake-darwin-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake-darwin-debug                     # …/test_runner at the tree root
cmake --build build-cmake-darwin-debug --target run_tests      # [self] suite
cmake --build build-cmake-darwin-debug --target run_all_tests  # everything, examples included
```

Build trees are named `build-cmake-<os>-<config>/` so they sit beside CB's
`build-<os>-<config>/` and Make's `build-make-<os>-<config>/` without colliding.

It needs **CMake 4.x and Ninja** — CI and the dev container both install 4.1.2, and module
scanning needs the Ninja generator. `std` is compiled from libc++'s own
`share/libc++/v1/std.cppm` under `LLVM_PREFIX`, exactly as
[`config/compiler.mk`](config/compiler.mk) does it, rather than through CMake's own
`import std` support; that keeps the toolchain assumptions identical across all three build
paths. Point `-DLLVM_PREFIX=` at your LLVM if it is not `/usr/local/llvm` (macOS) or
`/usr/lib/llvm-21` (Linux). Build type maps the way `config/compiler.mk` does (`Release`
optimizes, `Debug` drops to `-O0`); `-DTESTER_STATIC=ON` mirrors `STATIC=1`.

CI gates this path for `Debug` and `Release` on every push (`cmake-ninja-build-and-test`),
warning-free, with `[self]` and the standalone suite. What it does not have, like make, is
CB's object cache or JSONL build telemetry.

## Built-in Builder (CB)

Tester ships with **CB** (`tools/cb.c++`), a module-aware build system in a single file. CB discovers translation units, topologically sorts module imports, compiles in parallel, and caches object files incrementally. **CB is the default path for standalone clones**; parent repos embed tester under `deps/tester` and build through their own `tools/CB.sh` wrapper — [YarDB](https://github.com/ruoka/YarDB) is the public reference layout. For design rationale and comparison with CMake, Make, and other tools, see [`docs/cb.md`](docs/cb.md). Implementation prefers **standard C++**; subprocesses go through `posix_spawn` (Apple's libc serializes `std::system`), and crash stack traces in `test_runner` are the POSIX `<execinfo.h>` exception — see [AGENTS.md — Implementation policy](AGENTS.md#implementation-policy-standard-c-only).

```bash
./tools/CB.sh debug build          # compile project + tests
./tools/CB.sh release build        # optimized; tests off by default
./tools/CB.sh release build --build-tests   # compile tests without running
./tools/CB.sh debug test
./tools/CB.sh ci --jsonl=failures  # clean + test; JSONL-first CI entry point
./tools/CB.sh debug list           # human TU inventory; writes compile_commands.json + graph.json
./tools/CB.sh debug list --jsonl=failures   # machine-readable inventory (+ those files)
./tools/CB.sh debug clean
./tools/CB.sh debug clean --tests     # drop test objects + test_runner only
./tools/CB.sh debug cache status      # inspect object-cache profile
./tools/CB.sh debug cache invalidate  # drop cache indexes only (lighter than clean)
./tools/CB.sh debug --include-examples build
./tools/CB.sh --help
```

`ci` is a shortcut for clean-then-test under the default `debug` config. Prefer
`--jsonl=failures` (or `--jsonl=summary`) so agents and CI parse stdout; wrapper logs stay
on stderr.

Artifacts land in `build-<os>-<config>/` (`pcm/`, `obj/`, `bin/`, `cache/`). Object-cache profile format and invalidation: [`docs/cb.md` — Object cache profile](docs/cb.md#object-cache-profile). When embedded as a submodule, examples are excluded from default builds; standalone `./tools/CB.sh debug test` includes them. Use `--include-examples` to build demos explicitly.

### Embedding & tester resolution

Parent repos do not rebuild this tree's `tools/CB.sh` — they keep a thin `tools/CB.sh` that sources [`tools/CB.sh.core`](tools/CB.sh.core) and sets include paths (see [`tools/CB.sh.template`](tools/CB.sh.template)). `CB.sh.core` resolves **where `cb.c++` comes from** in this order:

1. **`CB_TESTER_ROOT`** — if already set and `$CB_TESTER_ROOT/tools` exists (explicit override).
2. **`$CB_PROJECT_ROOT/deps/tester`** — the usual git submodule checkout.
3. **`$CB_PROJECT_ROOT/../tester`** — a sibling clone (handy for local multi-repo worktrees).
4. **Standalone / in-tree** — `$CB_TOOLS_DIR/cb.c++` (this repository's own wrapper).
5. **`CB_FETCH_DEPS=1`** — only if nothing above was found: shallow-clone `ruoka/tester` into `deps/tester` (branch `CB_TESTER_BRANCH`, default `main`). Without the flag, missing tester is a hard error with the paths that were checked.

`CB_AUTO_SUBMODULES` (optional) can `git submodule update --init` listed paths before a build; it does not change the resolution order above.

**Nested `deps/*/deps/tester` copies** (e.g. `deps/xson/deps/tester`) are for that child package when *it* builds alone. The parent's `CB.sh` uses only the first-level `deps/tester` (or sibling / `CB_TESTER_ROOT`). Do **not** copy or rewrite tester docs inside those nested trees to “fix” a lagging pin — the nested tree is a submodule checkout; bump its pointer (and the parent's `deps/tester`) to the commit that has the docs and behaviour you want. More layout notes: [`docs/cb.md` — Embedded](docs/cb.md#embedded-depstester-in-a-parent-repo).

**Not using CB?** Two alternatives build the same library and runner: the `Makefile` with `clang-scan-deps` ([Makefile runner](#makefile-runner-alternative-to-cb), target table in [`docs/cb.md`](docs/cb.md#make-an-alternative-in-this-repo)) and `CMakeLists.txt` with the Ninja generator ([CMake + Ninja build](#cmake--ninja-build-alternative-to-cb)). CI gates both.

## JSONL & Automation

Tester emits **JSONL on stdout** (`schema: "tester-jsonl"`) for test runs and CB JSONL for builds. Parse stdout only; treat stderr as human wrapper logs.

Every line is valid UTF-8 and valid JSON regardless of what the test data contains: invalid byte sequences in assertion operands are replaced with U+FFFD rather than passed through. The event contract is published as JSON Schema 2020-12 in [`docs/jsonl-schema.json`](docs/jsonl-schema.json); [`tests/jsonl/validate.py`](tests/jsonl/validate.py) runs the canonical commands and validates every emitted line against it.

**For AI agents and automation**, start with [`AGENTS.md`](AGENTS.md). For a practical assessment and token-efficient workflow, see [Recommendation for AI Coding Agents](docs/ai-agent-recommendation.md).

**MCP (Model Context Protocol):** stdio bridge [`tools/cb_mcp.py`](tools/cb_mcp.py) exposes `cb_list` / `cb_build` / `cb_test` / `cb_test_list` / `cb_cache_status` for Cursor and other IDEs. Config: [`.cursor/mcp.json`](.cursor/mcp.json). Smoke: `./tests/mcp/smoke.sh`. See [AGENTS.md — MCP bridge](AGENTS.md#mcp-bridge-cursor--ides).

### Canonical commands

```bash
./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'  # agent/debug loop
./tools/CB.sh debug test --jsonl=summary --tags='\[self\]'   # CI aggregate
./tools/CB.sh ci --jsonl=failures                            # clean + full suite (JSONL-first CI)
./tools/CB.sh debug test --list --jsonl=failures             # test catalogue
./tools/CB.sh debug build --jsonl=trace                       # full compile telemetry
./tools/CB.sh debug list --jsonl=failures                     # TU inventory + compile_commands.json + graph.json
```

**Unified JSONL modes:**
- `--jsonl` / `--jsonl=failures` — aggregate rollups plus actionable failures (default)
- `--jsonl=summary` — lifecycle and final aggregates only
- `--jsonl=trace` — every build/test event, including passing assertions
- `--jsonl-output-max-bytes=N` — cap captured failed-test output

Escape bracket tags in shell: `--tags='\[self\]'`.

### Test catalogue (`test --list --jsonl=failures`)

| Event | Purpose |
|-------|---------|
| `test_list_start` | Catalogue start (`tags_filter`) |
| `registered_test` | Per test: `id`, `name`, `file`, `line`, `column`, `tags[]`, `depends_on[]`, `priority` |
| `test_list_summary` | `registered_total`, `matched_total`, `tags_filter` |

### Assertion events

| Event | When emitted |
|-------|----------------|
| `assertion_failed` | In `failures` and `trace` modes |
| `assertion_passed` | In `trace` mode |

Fields: `test_id`, `matcher`, `actual`, `expected`, `file`, `line`, `column`, optional `message`.

`matcher` is the public wrapper name (e.g. `require_eq`), not the generic `check`/`require` hub. If you see `"matcher":"require"` on a `require_eq` line, rebuild test objects — template matchers are instantiated in `*.test.c++` translation units.

**Event ordering:** assertion events (`assertion_failed` / `assertion_passed`) and `exception` stream **during** execution as each case runs (after that case’s `case` event when the mode emits `case`). Per-case `test` records are batched at finalize time — after every selected case has finished — then `summary`, then `run_end` (trace), then `eof`. A `test` line is not emitted immediately after that case’s last assertion.

Trace mode emits all test events: `run_start`, `run_end`, `case`, `test`, `message`, `exception`, `summary`, and `eof`. Failures mode suppresses passing cases/tests and duplicate `run_end`; summary mode emits only lifecycle and aggregate events.

- `run_start` — `cwd`, structured `argv`, `config` (via `TESTER_CONFIG` when CB spawns the child), `env` for curated vars when set
- `exception` — demangled `exception_type`, `message`, `file`, `line`
- `summary` / `run_end` — `failed_test_ids`, `first_failure`

**Correlation:** filter `run_id=<cb>` or `parent_run_id=<cb>` to tie `list` → `build` → `test` from one JSONL invocation.

### CB build JSONL

| Event | Purpose |
|-------|---------|
| `build_start` / `build_end` | Whole build; compact modes add compile/link/cache/failure totals |
| `command_start` / `command_end` | Every subprocess in trace; failed commands only in failures |
| `profile_changed` | Object-cache profile mismatch (`reason: "profile_change"`, `profile_diff`) — see [object cache profile](docs/cb.md#object-cache-profile) |
| `cache_status` | `cache status` inspection |
| `cache_invalidate_end` | `cache invalidate` result |
| `compile_start` | Per TU in trace mode |
| `compile_end` | Per TU in trace; failed compilations only in failures |
| `link_end` | Per executable in trace; failed links only in failures |
| `list_start` / `unit` / `list_summary` | TU inventory (`module`, `imports[]`, `level`, `is_test`, …) |

## How Tester Compares

| | **Tester** | **Catch2 / doctest** | **Google Test + CMake** |
|---|------------|----------------------|-------------------------|
| **C++23 modules** | Native (`import tester`) | Header / macro based | Header / macro based |
| **Module internals** | Native — `*.test.c++` as `module foo;` sees non-exported names | Not native; friends, test-only exports, or headers | Same workarounds (public API / `friend` / test builds) |
| **Macros** | None | Many (`TEST_CASE`, `SECTION`, `REQUIRE`, `SCENARIO`, …) | Many (`TEST`, `TEST_F`, `EXPECT_*`, `ASSERT_*`, …) |
| **Build system** | CB included; Makefile and `CMakeLists.txt` optional | Bring your own | CMake typical |
| **Compile time** | Modules avoid per-TU header reparse; PCM / `std` module cost on cold builds | Header-heavy (Catch2 often costly; doctest lighter) | Header includes per TU; usually moderate |
| **JSONL output** | First-class (`--jsonl`) | No | No (XML/JUnit via adapters) |
| **JUnit XML** | First-class (`--junit=`, with JSONL) | Native `--reporter junit` | gtest XML / adapters |
| **Test catalogue API** | `test --list --jsonl` | `--list-tests` (text) | GTest filters (text) |
| **BDD style** | Yes — nested `scenario` / `given` / `when` / `then` (run at assignment) | Yes — `SCENARIO` / `GIVEN` / `WHEN` / `THEN` | Via adapters |
| **Nested tests** | Yes — `section` in `test_case`; BDD steps nest; nested `test_case`/`scenario` run later | `SECTION` / `SUBCASE` nesting | No section model; fixtures / `TEST_P` instead |
| **Parallel tests** | In-process `--jobs=N` (default 1); respects `depends_on` | In-process serial; parallel via shards / CTest processes | Process-level (`gtest_parallel`, CTest `-j`) |
| **Parallel builds** | CB `--jobs=N` (default hardware concurrency) | Bring your own | CMake / Ninja `-j` typical |
| **Threads in a test** | Allowed; TLS execution context + locked observers | Allowed; reporters need care | Allowed; death tests / fixtures have rules |
| **Maturity** | Young, focused | Very mature | Very mature |

Tester fits module-native projects that want minimal glue and agent-friendly output. Large existing GTest/Catch codebases may not be worth migrating.

## Requirements

**Minimum toolchain: Clang 21** with libc++ modules (`std.cppm`). Newer Clang is fine and expected — macOS development typically uses a locally built trunk (today Clang 23), while Linux CI and the dev container pin Clang 21. The project does not require that exact version; 21 is the floor CI proves on every push.

Neither alternative build path is needed for CB: the `Makefile` additionally wants `clang-scan-deps` (ships with the toolchain), and `CMakeLists.txt` wants CMake 4.x with Ninja. The dev container has all three.

Both platforms run the same checks — the `[self]` suite (via CB, via the Makefile runner, and via the CMake + Ninja build), the CB and MCP smoke tests, and JSONL schema validation. CI runs them on Linux with Clang 21 on every push; on macOS they are run locally against a locally built LLVM, because no clang available on a hosted macOS runner builds C++23 modules yet. A macOS lane will be added once one does. Windows is not supported. Test steps also emit `--junit=` reports (uploaded as artifacts); gate suites are summarized in the job summary via `test-summary/action`.

### Linux
- Clang **21 or newer** (`clang++-21` in CI and the dev container)
- Matching LLVM libc++ with `std.cppm` (CI: `/usr/lib/llvm-21/share/libc++/v1/std.cppm`)
- libc++ development libraries for that toolchain

### macOS
- Locally built LLVM/clang at `/usr/local/llvm` — [`docs/clang-modules-macos.md`](docs/clang-modules-macos.md) (based on [LLVM Getting Started](https://llvm.org/docs/GettingStarted.html)); typically newer than the Linux CI pin
- Homebrew `llvm` is unsupported: exception unwinding fails on Apple Silicon ([#92121](https://github.com/llvm/llvm-project/issues/92121), [#168287 comment](https://github.com/llvm/llvm-project/issues/168287#issuecomment-3712718691))
- Xcode system clang does not fully support C++23 modules

### Windows
- Not supported: no toolchain path and no CI coverage

### Optional environment variables (build bootstrap)
- `LLVM_PATH` — override path to `std.cppm`
- `CXX` — override C++ compiler
- `CB_INCLUDE_FLAGS` — override include paths for `tools/CB.sh`

Test runner output is configured via CLI options, not environment variables.

## Versioning & Releases

**Current release: [`v2.2.0`](https://github.com/ruoka/tester/releases/tag/v2.2.0)** — Clang 21 + libc++ modules, JSONL, and CB with edge-driven module scheduling, bounded workers, dependency-scoped BMI mappings, and a shared standard-module cache. CMake + Ninja and Make remain supported alternative build paths. Prior supported tags: [`v2.1.3`](https://github.com/ruoka/tester/releases/tag/v2.1.3), [`v2.1.2`](https://github.com/ruoka/tester/releases/tag/v2.1.2), [`v2.1.1`](https://github.com/ruoka/tester/releases/tag/v2.1.1), [`v2.1.0`](https://github.com/ruoka/tester/releases/tag/v2.1.0), [`v2.0.0`](https://github.com/ruoka/tester/releases/tag/v2.0.0). The November 2025 `v1.0.0` GitHub pre-release is historical and unsupported.

Pin a tag or an explicit commit as a submodule. Between tags, `main` is CI-gated but carries no compatibility promise. Prefer aligning **every** nested `deps/tester` in a parent tree to the same SHA (see [YarDB versioning](https://github.com/ruoka/YarDB/blob/master/docs/versioning.md)).

[`docs/release-policy.md`](docs/release-policy.md) states what counts as public API, how versions are numbered, what breaks a consumer, and the criteria a release has to meet. [`CHANGELOG.md`](CHANGELOG.md) records public-surface changes.

## Repository Layout

```
tester/
├── tester/              # Framework modules + [self] contract tests (*.test.c++)
├── examples/            # Sample tests & demos ([.demo] = hidden intentional failures)
├── tools/
│   ├── cb.c++           # C++ Builder (single-file build system)
│   ├── CB.sh            # Per-repo bootstrap wrapper
│   └── core_pc.c++      # Core file analysis utility
├── docs/                # Design notes and improvement backlog
├── AGENTS.md            # JSONL agent guide
├── config/              # Compiler configuration (Makefile support)
├── Makefile             # Alternative build path (clang-scan-deps ordering)
├── CMakeLists.txt       # Alternative build path (CMake + Ninja, C++23 modules example)
└── build-*/             # Generated artifacts (gitignored)
```

## Assertion Reference

Namespace `tester::assertions` — matching `check_*` (non-fatal) and `require_*` (fatal) pairs:

### Equality & Ordering
- `check_eq`, `check_neq`, `check_lt`, `check_lteq`, `check_gt`, `check_gteq`
- `require_eq`, `require_neq`, `require_lt`, `require_lteq`, `require_gt`, `require_gteq`
- Floating-point: automatic epsilon comparison; `check_near` / `require_near` for explicit tolerance. The default tolerance is floored at four times the type's machine epsilon, so `float` is compared sensibly rather than against an unreachable `1e-9`
- `check_neq` is the negation of `check_eq`, so a floating-point pair inside the tolerance is *not* unequal. Ordering (`lt`, `lteq`, `gt`, `gteq`) stays exact
- Signed against unsigned operands are compared as mathematical values via `std::cmp_*`, so `check_eq(-1, 4294967295u)` fails and `check_lt(-1, 1u)` passes. Comparing through `std::common_type_t` would convert the signed operand and invert both answers
- This covers every integer operand, not only `int` against `unsigned`. `bool`, the character types and unscoped enumerations are promoted first, since `std::cmp_*` does not accept them as written, so `check_eq(char{-1}, 4294967295u)` fails as well while `check_eq('a', 97u)` and `check_eq(true, 1u)` pass. An enumeration compared against its own type keeps whatever comparison the type provides
- A `std::pair`, `std::tuple` or other tuple-like operand is compared member by member by those same rules, and ordering is lexicographic over them, so `check_eq(std::pair{std::string::npos, 0}, std::pair{-1L, 0})` fails and `check_lt(std::pair{-1, 0}, std::pair{4294967295u, 0})` passes. Heterogeneous composites have a common type too, and converting through it converted every member. A composite member is walked again, and the floating-point epsilon reaches a member
- A tuple-like operand is reported as its members, `(18446744073709551615, 0)`, in both channels, and each member by its own rule — a character member reads `'a' (97)` inside the composite too
- A character operand is reported by value: `97` in JSONL, `'a' (97)` on the console. The character alone would put a raw byte where the schema promises a number, and for a negative `char` it would hide the value that was compared
- An infinity or a NaN is reported as a quoted string, `"inf"`, `"-inf"`, `"nan"`, since JSON has no numeric form for either; finite operands stay numbers. Bare, `"actual":inf` made the whole line unparseable, and `null` would have parsed while losing which of the three it was

### Boolean
- `check_true`, `check_false`, `require_true`, `require_false`

### Exceptions
- `check_nothrow`, `check_throws`, `require_nothrow`, `require_throws`
- `check_throws_as<ExceptionType>(callable)`, `require_throws_as<ExceptionType>(callable)`
- A derived exception satisfies a base-class expectation
- Passing an exception *instance* (`require_throws_as(callable, E{"..."})`) is deprecated — the value was always discarded; name the type instead

### Containers & strings
- `check_container_eq`, `require_container_eq`
- `check_contains`, `require_contains` (string or container element)
- `check_starts_with`, `require_starts_with`, `check_ends_with`, `require_ends_with`
- Elements are compared and reported by the same rules as `check_eq`, so signedness and the floating-point epsilon behave identically inside a container, and an element with no stream inserter — a `std::pair`, say — is compared and shown as its members rather than failing to compile: `check_contains(std::vector<std::size_t>{std::string::npos}, -1)` fails, while `require_container_eq(std::vector<double>{0.1 + 0.2}, std::vector<double>{0.3})` passes

### Messaging
- `succeed`, `failed`, `warning`

## Utilities

```bash
./tools/CB.sh debug build
build-<os>-debug/bin/tools/core_pc /path/to/core
```

`tools/core_pc.c++` dumps register state from a POSIX core file.

## Troubleshooting

**`std.cppm not found`** — install LLVM 21 or set `LLVM_PATH`; or `./tools/CB.sh /path/to/std.cppm debug build`

**Compiler not found** — set `CXX` / `LLVM_CXX`; need Clang 21 or newer with libc++ modules (CI uses 21; macOS usually a newer local build)

**Module dependency errors** — `./tools/CB.sh debug clean && ./tools/CB.sh debug build`; check submodules

**Examples not building** — use `--include-examples` when embedded as a submodule

**Tests not running** — build first; files need `.test.c++` extension; verify `build-<os>-debug/bin/test_runner` exists

**Tag filtering** — quote regex: `--tags="scenario.*Happy"`; escape brackets: `--tags='\[self\]'`

**Stale JSONL matchers** — rebuild test TUs after editing `tester-assertions.c++m`

## Architecture

**Testing framework** — global registration (`const auto _ = …`), automatic discovery of `*.test.c++` registrations, tag/regex filtering, `depends_on` ordering, rich assertions with source locations.

**Observers** — `tester:observer` defines the format-neutral event contract and the registry, and each part of the framework publishes its own events through `notify()`: the runner reports the run lifecycle, catalogue and aggregates, the engine reports tests and exceptions, and the assertion matchers build and report assertion events. Nobody selects a destination. `test_runner.c++` is a non-module composition root (`import tester;`) that registers the built-in console and JSONL sinks by name, selects one primary sink from the CLI, then may `observe()` additional sinks (JUnit XML via `--junit=`) so machine streams and CI reports run together. The three built-in observer partitions are re-exported from `tester` for that purpose. Additional observers derive from `tester::output::observer` and use `register_observer()` / `select_observer()` / `observe()`; runner and assertion code do not change.

**CB** — parses module dependencies, topological sort, incremental PCM/object caching, parallel compilation, executable linking with module awareness.

## License

MIT — see [LICENSE](LICENSE).

## Related Resources

- [docs/cb.md](docs/cb.md) — C++ Builder design, workflows, and comparison with CMake/Make
- [AGENTS.md](AGENTS.md) — JSONL automation guide for CI and AI agents
- [docs/ai-agent-recommendation.md](docs/ai-agent-recommendation.md) — practical benefits and token-efficient agent workflow
- [docs/tester-improvements.md](docs/tester-improvements.md) — improvement backlog
- [docs/release-policy.md](docs/release-policy.md) — public API surface, versioning rules, and release criteria
- [CHANGELOG.md](CHANGELOG.md) — notable changes to the public surface
- [YarDB](https://github.com/ruoka/YarDB) — public reference project using tester + CB (P1204R0 layout)
- [P1204R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html) — canonical C++ project structure