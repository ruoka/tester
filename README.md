# Tester – a C++23 module-based testing framework

## Introduction

Tester is a lightweight, macro-free testing framework built entirely with C++23 modules. It provides both traditional unit tests and behaviour-driven helpers, offering expressive assertions and clean syntax without preprocessor tricks. The focus is on readability, ease of refactoring, and straightforward integration into larger projects.

## Features

- Pure C++23 module interface – no headers to include.
- Unit (`tester::basic::test_case`) and BDD (`tester::behavior_driven_development::scenario`) styles.
- Rich assertion set (`require_eq`, `require_nothrow`, `require_throws_as`, etc.).
- Tag-based filtering through the supplied `test_runner` executable.
- Supports standalone builds as well as embedding in a parent mono-repo.

## Repository layout

```
tester/
├── tester/          – framework modules
├── examples/        – sample tests & demo programs
├── deps/            – optional support libraries (std, net, xson)
└── tools/           – helper utilities (e.g. core_pc.c++)
```

## Building

### Standalone checkout

```bash
git clone https://github.com/ruoka/tester.git
cd tester
git submodule update --init --recursive  # pulls deps/std, deps/net, …
make module                                # builds libtester.a and modules
make run_examples                          # (optional) compiles & runs demos
make tests                                 # (optional) builds test_runner
make tools                                 # (optional) builds utilities in tools/
```

Artefacts are emitted under `build/` (`build/pcm` for modules, `build/obj` for objects, `build/lib` for archives, `build/bin` for executables).

### Embedded as a submodule

When `tester` lives under another project’s `deps/` directory, invoke the framework via the parent build (e.g. `make module` at the parent root). Paths are automatically adjusted so the submodules share the parent’s `build/` tree.

## Writing tests

### Simple unit test

```c++
module foo;
import tester;

namespace foo {
auto test_set() {
    using tester::basic::test_case;
    using namespace tester::assertions;

    test_case("Module foo's unit tests") = [] {
        require_eq(foo::x, 1);
        require_eq(foo::y, 2);
    };
    return 0;
}

const auto test_registrar = test_set();
} // namespace foo
```

### Behaviour-driven example

```c++
#include <stdexcept>
import tester;

using namespace tester::behavior_driven_development;
using namespace tester::assertions;

auto feature() {
    scenario("Happy path") = [] {
        given("Customer wants to buy food") = [] {
            when("they visit a restaurant") = [] {
                then("they place an order") = [] {
                    require_true(true);
                    require_nothrow([] {});
                };
            };
        };
    };

    scenario("Failure path") = [] {
        when("service is unavailable") = [] {
            then("the client sees an error") = [] {
                require_throws([] { throw std::runtime_error{"boom"}; });
            };
        };
    };

    return 0;
}

const auto test_registrar = feature();
```

## Running tests

Build the supplied runner (`make tests`) and drive it with tags:

```bash
build/bin/test_runner                  # run everything
build/bin/test_runner --list           # list registered cases
build/bin/test_runner --tags=simulator
build/bin/test_runner --tags=[acceptor]
```

The runner prints results, failures, and aggregate statistics, and returns a non-zero exit code when any scenario fails.

## Utilities

`tools/core_pc.c++` is a small utility that dumps register state from a POSIX core file. Build it via the new `tools` target:

```bash
make tools
build/bin/tools/core_pc /path/to/core
```

## Make targets summary

- `make module` – build modules and `libtester.a`.
- `make run_examples` – compile and execute the sample programs in `examples/`.
- `make tests` – build the standalone `test_runner`.
- `make tools` – build helper binaries under `build/bin/tools/`.
- `make clean` – remove the `build/` directory.

## License

Tester is released under the MIT license. See [LICENSE](LICENSE) for full text.

