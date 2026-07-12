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

Tester embeds as a git submodule (`deps/tester`) in larger projects. For a public multi-module consumer using tester and CB, see [YarDB](https://github.com/ruoka/YarDB). Framework contract tests live in `tester/*.test.c++` under the `[self]` tag. The `examples/` directory holds demos — some intentionally fail to show assertion output.

## Quick Start

```bash
git clone --recursive https://github.com/ruoka/tester.git
cd tester

# Framework contract tests (CI gate)
./tools/CB.sh debug test --jsonl --tags='\[self\]'

# Build
./tools/CB.sh debug build

# Run all registered tests (includes examples when standalone)
./tools/CB.sh debug test
```

If you did not clone with `--recursive`:

```bash
git submodule update --init --recursive
```

The `[self]` suite exercises JSONL catalogue events, `run_start` metadata, tag filtering, and `depends_on` ordering. CI requires `summary.passed` (or `run_end.passed`) to be `true`.

## Writing Tests

Tests live in `*.test.c++` (or `*.test.c++m`) files. Register cases from a function called at namespace scope (`const auto _ = register_tests();`). Put bracket tags in the case name, e.g. `test_case("my feature [api] does X")`.

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

**Important:** Nested lambdas (`given` / `when` / `then`) run after the parent `scenario` lambda returns. Capture parent-scope variables **by value** (e.g. `std::shared_ptr`), not by reference (`[&]`), or you will get dangling references.

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
        auto o = std::make_shared<order>();
        given("a draft order") = [o] {
            when("the customer confirms") = [o] {
                o->submit();
                then("the order is marked as submitted") = [o] {
                    require_true(o->submitted);
                    require_nothrow([o]{ o->submit(); });
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
./tools/CB.sh debug test --list --jsonl
```

Pass options through to `test_runner` after `--`, or use CB shortcuts directly:

```bash
./tools/CB.sh debug test -- --output=jsonl --jsonl-output=always --slowest=10
./tools/CB.sh debug test --jsonl --tags='\[self\]'
./tools/CB.sh debug test -- --result   # stable RESULT: line on stderr in JSONL mode
```

**Tag filtering:**
- **Bracket tags** — `test_case("… [api] …")` then `--tags='\[api\]'`
- **Hidden tags** — `test_case("… [.integration] …")` is skipped on an unfiltered run; pass `--tags='\[.integration\]'` (or a matching substring) to run it
- **Substring** — `--tags=simulator` matches any test name containing `simulator`
- **Regex** — `--tags="scenario.*Happy"`; invalid regex falls back to substring matching

The runner prints human-readable results on stderr (stdout in human mode), returns non-zero when any test fails, and emits JSONL on stdout with `--output=jsonl`.

### Makefile runner (legacy)

```bash
make tests
build-linux/bin/test_runner --list
build-linux/bin/test_runner --tags=[acceptor]
build-linux/bin/test_runner --tags="scenario.*Happy"
```

## Built-in Builder (CB)

Tester ships with **CB** (`tools/cb.c++`), a module-aware build system in a single file. CB discovers translation units, topologically sorts module imports, compiles in parallel, and caches object files incrementally. **CB is the default path for standalone clones**; parent repos embed tester under `deps/tester` and build through their own `tools/CB.sh` wrapper — [YarDB](https://github.com/ruoka/YarDB) is the public reference layout. For design rationale and comparison with CMake, Make, and other tools, see [`docs/cb.md`](docs/cb.md).

```bash
./tools/CB.sh debug build          # compile project + tests
./tools/CB.sh release build        # optimized; tests off by default
./tools/CB.sh release build --build-tests   # compile tests without running
./tools/CB.sh debug test
./tools/CB.sh debug list           # human TU inventory
./tools/CB.sh debug list --jsonl   # machine-readable inventory
./tools/CB.sh debug clean
./tools/CB.sh debug --include-examples build
./tools/CB.sh --help
```

Artifacts land in `build-<os>-<config>/` (`pcm/`, `obj/`, `bin/`, `cache/`). When embedded as a submodule, examples are excluded from default builds; standalone `./tools/CB.sh debug test` includes them. Use `--include-examples` to build demos explicitly.

**Makefile (legacy):** `make module`, `make run_examples`, `make tests`, `make tools` — see `Makefile` for targets. Prefer CB for new work.

## JSONL & Automation

Tester emits **JSONL on stdout** (`schema: "tester-jsonl"`) for test runs and CB JSONL for builds. Parse stdout only; treat stderr as human wrapper logs.

**For AI agents and automation**, start with [`AGENTS.md`](AGENTS.md).

### Canonical commands

```bash
./tools/CB.sh debug test --jsonl --jsonl-output=always --tags='\[self\]'  # scoped run
./tools/CB.sh debug test --list --jsonl                                   # test catalogue
./tools/CB.sh debug build --jsonl                                         # compile telemetry
./tools/CB.sh debug list --jsonl                                          # TU inventory
```

**Flags:**
- `--jsonl` — machine-readable stdout for CB and (on `test`) `test_runner`
- `--jsonl-output=always` — emit `assertion_passed` as well as `assertion_failed` (default: failures only)

Escape bracket tags in shell: `--tags='\[self\]'`.

### Test catalogue (`test --list --jsonl`)

| Event | Purpose |
|-------|---------|
| `test_list_start` | Catalogue start (`tags_filter`) |
| `registered_test` | Per test: `id`, `name`, `file`, `line`, `column`, `tags[]`, `depends_on[]`, `priority` |
| `test_list_summary` | `registered_total`, `matched_total`, `tags_filter` |

### Assertion events

| Event | When emitted |
|-------|----------------|
| `assertion_failed` | Every failed assertion (always) |
| `assertion_passed` | Only when `--jsonl-output=always` |

Fields: `test_id`, `matcher`, `actual`, `expected`, `file`, `line`, `column`, optional `message`.

`matcher` is the public wrapper name (e.g. `require_eq`), not the generic `check`/`require` hub. If you see `"matcher":"require"` on a `require_eq` line, rebuild test objects — template matchers are instantiated in `*.test.c++` translation units.

Other test events: `run_start`, `run_end`, `case`, `test`, `message`, `exception`, `summary`, `eof`.

- `run_start` — `cwd`, structured `argv`, `config` (via `TESTER_CONFIG` when CB spawns the child), `env` for curated vars when set
- `exception` — demangled `exception_type`, `message`, `file`, `line`
- `summary` / `run_end` — `failed_test_ids`, `first_failure`

**Correlation:** filter `run_id=<cb>` or `parent_run_id=<cb>` to tie `list` → `build` → `test` from one `./tools/CB.sh … --jsonl` invocation.

### CB build JSONL

| Event | Purpose |
|-------|---------|
| `build_start` / `build_end` | Whole build phase |
| `command_start` / `command_end` | Each subprocess (`cmd` + structured `argv`) |
| `compile_end` | Per TU (`source_path`, `cache_hit`, `rebuild_reason`, paths) |
| `list_start` / `unit` / `list_summary` | TU inventory (`module`, `imports[]`, `level`, `is_test`, …) |

## How Tester Compares

| | **Tester** | **Catch2 / doctest** | **Google Test + CMake** |
|---|------------|----------------------|-------------------------|
| **C++23 modules** | Native (`import tester`) | Header / macro based | Header / macro based |
| **Macros** | None | Some (`TEST_CASE`, etc.) | `TEST()`, `EXPECT_*` |
| **Build system** | CB included; Makefile optional | Bring your own | CMake typical |
| **JSONL output** | First-class (`--jsonl`) | No | No (XML/JUnit via adapters) |
| **Test catalogue API** | `test --list --jsonl` | `--list-tests` (text) | GTest filters (text) |
| **BDD style** | Built-in `scenario`/`given` | Via macros / tags | Via adapters |
| **Maturity** | Young, focused | Very mature | Very mature |

Tester fits module-native projects that want minimal glue and agent-friendly output. Large existing GTest/Catch codebases may not be worth migrating.

## Requirements

### Linux
- Clang 21 (`clang++-21`) — required for CI and dev containers
- LLVM 21 (`std.cppm` at `/usr/lib/llvm-21/share/libc++/v1/std.cppm`)
- libc++-21 development libraries

### macOS
- Locally built LLVM/clang at `/usr/local/llvm` — [`docs/clang-modules-macos.md`](docs/clang-modules-macos.md) (based on [LLVM Getting Started](https://llvm.org/docs/GettingStarted.html))
- Homebrew `llvm` is unsupported: exception unwinding fails on Apple Silicon ([#92121](https://github.com/llvm/llvm-project/issues/92121), [#168287 comment](https://github.com/llvm/llvm-project/issues/168287#issuecomment-3712718691))
- Xcode system clang does not fully support C++23 modules

### Optional environment variables (build bootstrap)
- `LLVM_PATH` — override path to `std.cppm`
- `CXX` — override C++ compiler
- `CB_INCLUDE_FLAGS` — override include paths for `tools/CB.sh`

Test runner output is configured via CLI options, not environment variables.

## Repository Layout

```
tester/
├── tester/              # Framework modules + [self] contract tests (*.test.c++)
├── examples/            # Sample tests & demos (some intentional failures)
├── tools/
│   ├── cb.c++           # C++ Builder (single-file build system)
│   ├── CB.sh            # Per-repo bootstrap wrapper
│   └── core_pc.c++      # Core file analysis utility
├── docs/                # Design notes and improvement backlog
├── AGENTS.md            # JSONL agent guide
├── config/              # Compiler configuration (Makefile support)
└── build-*/             # Generated artifacts (gitignored)
```

## Assertion Reference

Namespace `tester::assertions` — matching `check_*` (non-fatal) and `require_*` (fatal) pairs:

### Equality & Ordering
- `check_eq`, `check_neq`, `check_lt`, `check_lteq`, `check_gt`, `check_gteq`
- `require_eq`, `require_neq`, `require_lt`, `require_lteq`, `require_gt`, `require_gteq`
- Floating-point: automatic epsilon comparison; `check_near` / `require_near` for explicit tolerance

### Boolean
- `check_true`, `check_false`, `require_true`, `require_false`

### Exceptions
- `check_nothrow`, `check_throws`, `check_throws_as`
- `require_nothrow`, `require_throws`, `require_throws_as`
- `require_throws_as<ExceptionType>(callable)`

### Containers & strings
- `check_container_eq`, `require_container_eq`
- `check_contains`, `require_contains` (string or container element)
- `check_starts_with`, `require_starts_with`, `check_ends_with`, `require_ends_with`

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

**Compiler not found** — set `CXX`; use Clang 21+ on Linux, locally built LLVM on macOS

**Module dependency errors** — `./tools/CB.sh debug clean && ./tools/CB.sh debug build`; check submodules

**Examples not building** — use `--include-examples` when embedded as a submodule

**Tests not running** — build first; files need `.test.c++` extension; verify `build-<os>-debug/bin/test_runner` exists

**Tag filtering** — quote regex: `--tags="scenario.*Happy"`; escape brackets: `--tags='\[self\]'`

**Stale JSONL matchers** — rebuild test TUs after editing `tester-assertions.c++m`

## Architecture

**Testing framework** — global registration (`const auto _ = …`), automatic discovery of `*.test.c++` registrations, tag/regex filtering, `depends_on` ordering, rich assertions with source locations.

**CB** — parses module dependencies, topological sort, incremental PCM/object caching, parallel compilation, executable linking with module awareness.

## License

MIT — see [LICENSE](LICENSE).

## Related Resources

- [docs/cb.md](docs/cb.md) — C++ Builder design, workflows, and comparison with CMake/Make
- [AGENTS.md](AGENTS.md) — JSONL automation guide for CI and AI agents
- [docs/tester-improvements.md](docs/tester-improvements.md) — improvement backlog
- [YarDB](https://github.com/ruoka/YarDB) — public reference project using tester + CB (P1204R0 layout)
- [P1204R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html) — canonical C++ project structure