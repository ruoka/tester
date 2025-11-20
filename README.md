# Tester – a C++23 module-based testing framework

## Introduction

Tester is a lightweight, macro-free testing framework built entirely with C++23 modules. It provides both traditional unit tests and behaviour-driven helpers, offering expressive assertions and clean syntax without preprocessor tricks. The focus is on readability, ease of refactoring, and straightforward integration into larger projects.

## Features

- Pure C++23 module interface – no headers to include.
- Unit (`tester::basic::test_case`) and BDD (`tester::behavior_driven_development::scenario`) styles.
- Rich assertion set (`require_eq`, `require_nothrow`, `require_throws_as`, etc.).
- Tag-based filtering through the supplied `test_runner` executable with regex pattern support.
- Supports standalone builds as well as embedding in a parent mono-repo.

## Improvement ideas

Ongoing enhancement notes live in [`docs/tester-improvements.md`](docs/tester-improvements.md). Whether you consume Tester inside Fixer or as a standalone dependency, start there to see the current backlog and proposed assertion features.

## Repository layout

```
tester/
├── tester/          – framework modules
├── examples/        – sample tests & demo programs
├── tools/           – helper utilities (e.g. core_pc.c++)
├── docs/            – design notes and improvement backlog
├── deps/            – optional support libraries (net, xson)
└── build-*/         – generated artifacts (per host OS), ignored by git
```

## Assertion reference

The core assertion namespace (`tester::assertions`) ships with matching `check_*` (non-fatal) and `require_*` (fatal) helpers:

- Equality & ordering: `check_eq`, `check_neq`, `check_lt`, `check_lteq`, `check_gt`, `check_gteq` (+ `require_*` counterparts). Floating-point paths automatically use epsilon-based comparison.
- Boolean helpers: `check_true`, `check_false`, `require_true`, `require_false`.
- Exception helpers: `check_nothrow`, `check_throws`, `check_throws_as`, `require_nothrow`, `require_throws`, `require_throws_as`.
- Messaging utilities: `succeed`, `failed`, `warning` for explicit pass/fail annotations.

## Building

### Standalone checkout

```bash
git clone https://github.com/ruoka/tester.git
cd tester
git submodule update --init --recursive  # pulls deps/net, …
make module                                # builds libtester.a and modules
make run_examples                          # (optional) compiles & runs demos
make tests                                 # (optional) builds test_runner
make tools                                 # (optional) builds utilities in tools/
```

Artifacts land in `build-<os>/` (e.g., `build-linux/pcm`, `build-linux/obj`, `build-linux/lib`, `build-linux/bin`). Override `BUILD_DIR` or `PREFIX` if you need a custom layout.

### Embedded as a submodule

When `tester` lives under another project’s `deps/` directory, invoke the framework via the parent build (e.g., `make module` at the parent root). Paths automatically map to the parent’s `build-<os>/` tree, so every submodule shares the same artifacts.

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

```c++
#include <stdexcept>
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
        order o{};
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

const auto _ = feature();
```

## Running tests

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

`tools/core_pc.c++` is a small utility that dumps register state from a POSIX core file. Build it via the new `tools` target:

```bash
make tools
build-linux/bin/tools/core_pc /path/to/core
```

## Make targets summary

- `make help` – list the available targets and configuration knobs.
- `make module` – build modules and `libtester.a`.
- `make run_examples` – compile and execute the sample programs in `examples/`.
- `make tests` – build the standalone `test_runner`.
- `make tools` – build helper binaries under `${BUILD_DIR}/bin/tools/`.
- `make clean` – remove `${BUILD_DIR}/bin`, `${BUILD_DIR}/lib`, and submodule stamps while preserving `std.pcm`.
- `make mostlyclean` – drop only `${BUILD_DIR}/obj` so incremental rebuilds stay fast.

## License

Tester is released under the MIT license. See [LICENSE](LICENSE) for full text.

