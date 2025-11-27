# C++ Builder & Tester – a C++23 module-based build system and testing framework

## Introduction

C++ Builder & Tester is a lightweight, macro-free testing framework built entirely with C++23 modules, combined with a powerful single-file C++ build system. It provides both traditional unit tests and behaviour-driven helpers, offering expressive assertions and clean syntax without preprocessor tricks. The focus is on readability, ease of refactoring, and straightforward integration into larger projects.

## Features

- Pure C++23 module interface – no headers to include.
- Unit (`tester::basic::test_case`) and BDD (`tester::behavior_driven_development::scenario`) styles.
- Rich assertion set (`require_eq`, `require_nothrow`, `require_throws_as`, etc.).
- Tag-based filtering through the supplied `test_runner` executable with regex pattern support.
- Supports standalone builds as well as embedding in a parent mono-repo.

## Key Benefits

C++ Builder & Tester offers unique advantages for modern C++ development:

- **Pure C++ Language**: Build and test management using only C++ – no need for heavy external build systems (CMake, Bazel, etc.), test frameworks with macros, configuration languages (YAML, TOML, etc.), or other programming languages (Python, Lua, etc.). Everything you need is in standard C++.

- **Fast and Lightweight**: Optimized for speed in workflows and Docker containers. Single-file build system with minimal overhead, fast startup times, and efficient incremental builds with intelligent caching.

- **Full Control**: Complete transparency and control over what the build system and test framework does. Single-file implementation means you can read, understand, and modify the entire build logic. No black boxes or hidden abstractions.

- **Zero External Dependencies**: No build tool installation required beyond your C++ compiler. The build system compiles itself on first use and is ready to go.

- **Module-Aware**: Native understanding of C++23 modules with automatic dependency resolution, topological sorting, and proper module interface/implementation handling.

- **Self-Contained**: Everything in one file (`cb.c++`) – easy to version control, embed in projects, or customize for specific needs. No complex directory structures or scattered configuration files.

- **CI/CD Friendly**: Perfect for continuous integration pipelines. Fast builds, clear output, and easy integration into GitHub Actions, GitLab CI, or any containerized environment.

- **No Learning Curve**: If you know C++, you can understand and modify the build system. No need to learn Makefile syntax, CMake scripting, or other domain-specific languages.

- **Cross-Platform**: Works seamlessly on Linux and macOS with automatic OS detection and appropriate compiler flag handling.

## Improvement ideas

Ongoing enhancement notes live in [`docs/tester-improvements.md`](docs/tester-improvements.md). Whether you consume C++ Builder & Tester inside Fixer or as a standalone dependency, start there to see the current backlog and proposed assertion features.

## Repository layout

```
tester/
├── tester/          – framework modules
├── examples/        – sample tests & demo programs
├── tools/           – helper utilities (e.g. core_pc.c++, cb.c++ - the C++ Builder)
├── docs/            – design notes and improvement backlog
├── config/          – compiler configuration (Makefile support)
└── build-*/         – generated artifacts (per host OS), ignored by git
```

## Assertion reference

The core assertion namespace (`tester::assertions`) ships with matching `check_*` (non-fatal) and `require_*` (fatal) helpers:

- Equality & ordering: `check_eq`, `check_neq`, `check_lt`, `check_lteq`, `check_gt`, `check_gteq` (+ `require_*` counterparts). Floating-point paths automatically use epsilon-based comparison.
- Boolean helpers: `check_true`, `check_false`, `require_true`, `require_false`.
- Exception helpers: `check_nothrow`, `check_throws`, `check_throws_as`, `require_nothrow`, `require_throws`, `require_throws_as`.
- Messaging utilities: `succeed`, `failed`, `warning` for explicit pass/fail annotations.

## Building

C++ Builder & Tester provides two build systems:
1. **C++ Builder (`cb.c++`)** - Modern single-file build system (recommended)
2. **Makefile** - Traditional build system (for compatibility with other projects)

### Using C++ Builder (Recommended)

The C++ Builder is located in `tools/cb.c++` and can be invoked via `tools/CB.sh`:

```bash
# Build in debug mode (includes examples and tests)
./tools/CB.sh debug build

# Build in release mode
./tools/CB.sh release build

# Build and run tests (automatically includes examples)
./tools/CB.sh debug test

# Clean build artifacts
./tools/CB.sh debug clean

# List all translation units
./tools/CB.sh debug list

# Include examples explicitly (excluded by default)
./tools/CB.sh debug --include-examples build

# Show help
./tools/CB.sh --help
```

The C++ Builder automatically:
- Detects your OS and compiler
- Resolves `std.cppm` path from LLVM installation
- Handles module dependencies and incremental builds
- Includes examples when running tests or when building standalone

Artifacts land in `build-<os>-<config>/` (e.g., `build-linux-debug/pcm`, `build-linux-debug/obj`, `build-linux-debug/bin`).

### Using Makefile (Legacy)

For projects that still use Makefile-based builds:

```bash
git clone https://github.com/ruoka/tester.git
cd tester
make module                                # builds libtester.a and modules
make run_examples                          # (optional) compiles & runs demos
make tests                                 # (optional) builds test_runner
make tools                                 # (optional) builds utilities in tools/
```

Artifacts land in `build-<os>/` (e.g., `build-linux/pcm`, `build-linux/obj`, `build-linux/lib`, `build-linux/bin`). Override `BUILD_DIR` or `PREFIX` if you need a custom layout.

### Embedded as a submodule

When C++ Builder & Tester lives under another project's `deps/` directory:

- **Using C++ Builder**: The parent project's build system (e.g., `tools/CB.sh` in fixer) will automatically detect and build tester. Examples are excluded by default when building as a submodule, but included when running tests.

- **Using Makefile**: Invoke the framework via the parent build (e.g., `make module` at the parent root). Paths automatically map to the parent's `build-<os>/` tree, so every submodule shares the same artifacts.

## Writing tests

### Simple unit test

```c++
module foo;
import tester;

namespace foo {
int add(int lhs, int rhs) { return lhs + rhs; }

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

    return 0;
}

const auto _ = register_tests();
} // namespace foo
```

### Behaviour-driven example

**Important:** When using nested test cases (`given`/`when`/`then`), nested lambdas execute later, after the parent `scenario` lambda returns. Therefore, nested lambdas must capture parent-scope variables **by value** (e.g., `[o]` or using `std::shared_ptr`), not by reference (`[&]`). Capturing by reference will result in dangling references and undefined behavior.

```c++
#include <stdexcept>
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

auto feature()
{
    using ordering::order;

    scenario("Customer places an order") = [] {
        // Use shared_ptr to safely share state across nested test cases
        // Nested lambdas (given/when/then) execute later, after the scenario
        // lambda returns, so they must capture by value, not by reference
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

const auto _ = feature();
```

## Running tests

### Using C++ Builder

```bash
# Build and run all tests (automatically includes examples)
./tools/CB.sh debug test

# Build and run tests with filter
./tools/CB.sh debug test "scenario.*Happy"
```

### Using Makefile

Build the supplied runner (`make tests`) and drive it with tags:

```bash
build-linux/bin/test_runner                  # run everything (replace build-linux with your BUILD_DIR)
build-linux/bin/test_runner --list           # list registered cases
build-linux/bin/test_runner --tags=simulator # simple substring matching
build-linux/bin/test_runner --tags=[acceptor] # bracket format
build-linux/bin/test_runner --tags="scenario.*Happy" # regex pattern matching
build-linux/bin/test_runner --tags="test_case.*CRUD" # regex for test cases
build-linux/bin/test_runner --tags="^scenario.*path" # regex with anchors
```

The tag selector supports both simple substring matching (for backward compatibility) and regular expression patterns. Invalid regex patterns automatically fall back to substring matching. The runner prints results, failures, and aggregate statistics, and returns a non-zero exit code when any scenario fails.

## Utilities

### C++ Builder

`tools/core_pc.c++` is a small utility that dumps register state from a POSIX core file. Build it using C++ Builder:

```bash
./tools/CB.sh debug build
build-<os>-debug/bin/tools/core_pc /path/to/core
```

### Makefile

```bash
make tools
build-<os>/bin/tools/core_pc /path/to/core
```

## Build System Reference

### C++ Builder Commands

- `./tools/CB.sh debug build` – Build in debug mode (includes tests)
- `./tools/CB.sh release build` – Build in release mode (optimized, no tests)
- `./tools/CB.sh debug test [filter]` – Build and run tests (optional filter)
- `./tools/CB.sh debug clean` – Remove build directories
- `./tools/CB.sh debug list` – List all translation units
- `./tools/CB.sh debug --include-examples build` – Include examples directory
- `./tools/CB.sh --help` – Show help message

### Makefile Targets

- `make help` – list the available targets and configuration knobs.
- `make module` – build modules and `libtester.a`.
- `make run_examples` – compile and execute the sample programs in `examples/`.
- `make tests` – build the standalone `test_runner`.
- `make tools` – build helper binaries under `${BUILD_DIR}/bin/tools/`.
- `make clean` – remove `${BUILD_DIR}/bin`, `${BUILD_DIR}/lib`, and submodule stamps while preserving `std.pcm`.
- `make mostlyclean` – drop only `${BUILD_DIR}/obj` so incremental rebuilds stay fast.

## License

C++ Builder & Tester is released under the MIT license. See [LICENSE](LICENSE) for full text.

