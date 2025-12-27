# C++ Builder & Tester – a C++23 module-based build system and testing framework

A lightweight, macro-free testing framework built entirely with C++23 modules, combined with a powerful single-file C++ build system. Everything you need for building and testing modern C++ projects – in pure C++.

## Table of Contents

- [Introduction](#introduction)
- [Features](#features)
- [Key Benefits](#key-benefits)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [Repository Layout](#repository-layout)
- [Building](#building)
- [Writing Tests](#writing-tests)
- [Running Tests](#running-tests)
- [Assertion Reference](#assertion-reference)
- [Utilities](#utilities)
- [Build System Reference](#build-system-reference)
- [Troubleshooting](#troubleshooting)
- [License](#license)

## Introduction

C++ Builder & Tester is a lightweight, macro-free testing framework built entirely with C++23 modules, combined with a powerful single-file C++ build system. It provides both traditional unit tests and behaviour-driven helpers, offering expressive assertions and clean syntax without preprocessor tricks. The focus is on readability, ease of refactoring, and straightforward integration into larger projects.

**Why C++ Builder & Tester?**
- Write build logic and tests in pure C++ – no external build tools or configuration languages
- Single-file implementation (`cb.c++`) – understand and modify everything
- Zero external dependencies beyond your C++ compiler
- Fast incremental builds with intelligent caching
- Native C++23 module support with automatic dependency resolution

## Features

### Testing Framework
- **Pure C++23 module interface** – no headers to include, just `import tester;`
- **Dual testing styles**: Unit tests (`tester::basic::test_case`) and BDD (`tester::behavior_driven_development::scenario`)
- **Rich assertion set**: `require_eq`, `require_nothrow`, `require_throws_as`, and more
- **Tag-based filtering**: Filter tests using regex patterns via `test_runner`
- **Floating-point comparisons**: Automatic epsilon-based comparison for floating-point values

### Build System
- **Single-file implementation**: Everything in `cb.c++` (~1300 lines)
- **Incremental builds**: Intelligent caching of object files and executable signatures
- **Parallel compilation**: Utilizes multiple CPU cores automatically
- **Module-aware**: Automatic dependency resolution and topological sorting
- **Zero configuration**: Works out of the box with sensible defaults

### Integration
- **Standalone builds**: Works as a standalone project
- **Submodule support**: Seamlessly embeds in parent projects (e.g., as `deps/tester`)
- **Cross-platform**: Linux and macOS support with automatic OS detection

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

## Requirements

### Linux
- Clang 20 (`clang++-20`)
- LLVM 20 installation (for `std.cppm`)
- libc++ development libraries

### macOS
- **Requires locally built LLVM/clang** (not Homebrew)
- **LLVM installation at `/usr/local/llvm`**
- System clang from Xcode doesn't fully support C++23 modules
- You must build LLVM from source and install it to `/usr/local/llvm`

### Optional: Environment Variables (build bootstrap)
- `LLVM_PATH`: Override path to `std.cppm` (defaults to OS-specific locations)
- `CXX`: Override C++ compiler (defaults to OS-specific clang++)
- `CB_INCLUDE_FLAGS`: Override include paths for `tools/CB.sh` (space-separated)

**Note:** test runner output (human vs JSONL, slowest list, etc.) is configured via `test_runner` CLI options (see [Running tests](#running-tests)), not environment variables.

## Quick Start

Get up and running in minutes:

```bash
# Clone the repository
git clone --recursive https://github.com/ruoka/tester.git
cd tester

# Build (debug mode, includes examples)
./tools/CB.sh debug build

# Run tests
./tools/CB.sh debug test
```

**Note**: If you didn't clone with `--recursive`, initialize submodules first:
```bash
git submodule update --init --recursive
```

## Improvement ideas

Ongoing enhancement notes live in [`docs/tester-improvements.md`](docs/tester-improvements.md). Whether you consume C++ Builder & Tester inside Fixer or as a standalone dependency, start there to see the current backlog and proposed assertion features.

## Repository Layout

```
tester/
├── tester/          # Framework modules (testing framework implementation)
├── examples/        # Sample tests & demo programs
├── tools/           # Helper utilities
│   ├── cb.c++       # C++ Builder (single-file build system)
│   ├── CB.sh        # Bootstrap script for C++ Builder
│   └── core_pc.c++  # Core file analysis utility
├── docs/            # Design notes and improvement backlog
├── config/          # Compiler configuration (Makefile support)
└── build-*/         # Generated artifacts (per host OS), ignored by git
    ├── pcm/         # Precompiled module files
    ├── obj/         # Object files
    ├── bin/         # Executables
    └── cache/       # Build cache
```

## Assertion Reference

The core assertion namespace (`tester::assertions`) provides matching `check_*` (non-fatal) and `require_*` (fatal) helpers:

### Equality & Ordering
- `check_eq`, `check_neq`, `check_lt`, `check_lteq`, `check_gt`, `check_gteq`
- `require_eq`, `require_neq`, `require_lt`, `require_lteq`, `require_gt`, `require_gteq`
- **Floating-point**: Automatically uses epsilon-based comparison
- **Custom tolerance**: `check_near`, `require_near` with explicit tolerance

### Boolean Helpers
- `check_true`, `check_false` (non-fatal)
- `require_true`, `require_false` (fatal, stops test execution on failure)

### Exception Helpers
- `check_nothrow`, `check_throws`, `check_throws_as` (non-fatal)
- `require_nothrow`, `require_throws`, `require_throws_as` (fatal)
- `require_throws_as<ExceptionType>(callable)` – verifies specific exception type

### Container Assertions
- `check_container_eq`, `require_container_eq` – Compare two containers element-by-element with readable diff messages
  - Provides clear error messages showing first mismatch and highlighting differences
  - Works with any container type (vectors, arrays, etc.)

### String Assertions
- `check_contains`, `require_contains` – Check if a string contains a substring or character
- `check_has_substr`, `require_has_substr` – Alias for `contains` (substring check)
- `check_starts_with`, `require_starts_with` – Check if a string starts with a prefix
- `check_ends_with`, `require_ends_with` – Check if a string ends with a suffix
- Supports string literals, `std::string`, `std::string_view`, and `const char*`

### Container Element Checks
- `check_contains(container, element)`, `require_contains(container, element)` – Check if a container contains a specific element
  - Works with any container type (overloaded with string version)

### Messaging Utilities
- `succeed(message)` – Explicit success annotation
- `failed(message)` – Explicit failure annotation
- `warning(message)` – Warning message (doesn't fail test)

**Usage**: All assertions are in the `tester::assertions` namespace. Use `using namespace tester::assertions;` for convenience.

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
- Supports parallel compilation for faster builds
- Caches object files and executable signatures for incremental builds

**Build Output**: Artifacts land in `build-<os>-<config>/`:
- `build-<os>-<config>/pcm/` – Precompiled module files
- `build-<os>-<config>/obj/` – Object files
- `build-<os>-<config>/bin/` – Executables
- `build-<os>-<config>/cache/` – Build cache (object timestamps, executable signatures)

Example: `build-linux-debug/`, `build-darwin-release/`

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

### Embedded as a Submodule

When C++ Builder & Tester lives under another project's `deps/` directory:

**Using C++ Builder**:
- The parent project's build system (e.g., `tools/CB.sh` in fixer) automatically detects and builds tester
- Examples are **excluded by default** when building as a submodule
- Examples are **included automatically** when running tests (`./tools/CB.sh debug test`)
- Include examples explicitly: `./tools/CB.sh debug --include-examples build`

**Using Makefile**:
- Invoke the framework via the parent build (e.g., `make module` at the parent root)
- Paths automatically map to the parent's `build-<os>/` tree
- All submodules share the same build artifacts

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

    test_case("foo::add with container assertions") = [] {
        auto results = std::vector<int>{add(1, 2), add(3, 4), add(5, 6)};
        require_container_eq(results, std::vector<int>{3, 7, 11});
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

# Pass options through to test_runner (everything after "--" is forwarded):
./tools/CB.sh debug test -- --list
./tools/CB.sh debug test -- --tags="scenario.*Happy"

# JSONL output (stdout), with human logs on stderr:
./tools/CB.sh debug test -- --output=jsonl --schema=tester-jsonl --jsonl-output=always --slowest=10

# Emit a stable RESULT: line (on stderr in JSONL mode):
./tools/CB.sh debug test -- --output=jsonl --result
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

**Tag Filtering**:
- **Simple substring matching**: `--tags=simulator` matches any test containing "simulator"
- **Regex patterns**: `--tags="scenario.*Happy"` uses regex matching
- **Bracket format**: `--tags=[acceptor]` for exact substring match
- **Automatic fallback**: Invalid regex patterns fall back to substring matching

The runner prints results, failures, and aggregate statistics, and returns a non-zero exit code when any test fails.

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
- `./tools/CB.sh release build --build-tests` – Build tests in release mode without running them (useful for CI)
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

## Troubleshooting

### Build Issues

**`std.cppm not found`**:
- Ensure LLVM is installed and `std.cppm` exists at the expected path
- Set `LLVM_PATH` environment variable to point to `std.cppm`
- Pass `std.cppm` path as first argument: `./tools/CB.sh /path/to/std.cppm debug build`

**Compiler not found**:
- Set `CXX` environment variable to point to your clang++ compiler
- Ensure clang++ supports C++23 modules (Clang 19+)

**Module dependency errors**:
- Clean and rebuild: `./tools/CB.sh debug clean && ./tools/CB.sh debug build`
- Check that all submodules are initialized: `git submodule update --init --recursive`

**Examples not building**:
- Examples are excluded by default when building as a submodule
- Use `--include-examples` flag: `./tools/CB.sh debug --include-examples build`
- Examples are automatically included when running tests: `./tools/CB.sh debug test`

### Test Issues

**Tests not running**:
- Ensure tests are built: `./tools/CB.sh debug build`
- Check that test files have `.test.c++` extension
- Verify `test_runner` exists: `ls build-<os>-debug/bin/test_runner`

**Tag filtering not working**:
- Use quotes for regex patterns: `--tags="scenario.*Happy"`
- Check regex syntax if using complex patterns
- Invalid regex automatically falls back to substring matching

## Architecture

### Build System (`cb.c++`)

The C++ Builder is a single-file build system (~1300 lines) that:
- Parses C++23 module dependencies automatically
- Performs topological sorting of translation units
- Supports incremental builds with timestamp caching
- Handles both module interfaces and implementations
- Manages precompiled module (PCM) files
- Links executables with proper module dependencies

### Testing Framework

The testing framework provides:
- **Registration system**: Tests register themselves via global constructors
- **Test discovery**: Automatic discovery of test files (`.test.c++` extension)
- **Test execution**: Runs all registered tests or filtered subsets
- **Assertion framework**: Rich set of assertions with clear error messages
- **BDD support**: Behavior-driven development with `scenario`/`given`/`when`/`then`

## License

C++ Builder & Tester is released under the MIT license. See [LICENSE](LICENSE) for full text.

## Related Resources

- [Improvement Ideas](docs/tester-improvements.md) – Current backlog and proposed features
- [P1204R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html) – Canonical Project Structure for C++ projects
