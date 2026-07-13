# Getting started with Clang modules on macOS

Tester needs a **locally built** LLVM/clang with libc++'s `std.cppm` and reliable C++ exception unwinding. On macOS, `./tools/CB.sh` defaults to:

- Compiler: `/usr/local/llvm/bin/clang++`
- Standard module: `/usr/local/llvm/share/libc++/v1/std.cppm`

Apple's Xcode clang and Homebrew's `llvm` formula are **not** supported for this workflow.

Official LLVM build documentation (upstream reference):

- [Getting Started with the LLVM System](https://llvm.org/docs/GettingStarted.html)
- [CMake](https://llvm.org/docs/CMake.html) — generator options, `LLVM_ENABLE_PROJECTS`, `LLVM_ENABLE_RUNTIMES`, install prefix
- [Clang](https://clang.llvm.org/)

The steps below are a **tester-focused** recipe (install prefix, runtimes, macOS SDK). For the full option matrix and platform notes, use the LLVM docs above.

---

## Why not Homebrew LLVM?

Homebrew LLVM 18+ on Apple Silicon can ship a libc++/unwinder stack where **exceptions are not caught** — the process terminates with `libc++abi: terminating due to uncaught exception` instead of entering a `catch` block. That breaks tester (`require_throws`, `check_throws`, concurrent tests) and ordinary async code.

Upstream tracking:

- [llvm/llvm-project#92121](https://github.com/llvm/llvm-project/issues/92121) — exception handling on macOS ARM
- [llvm/llvm-project#168287](https://github.com/llvm/llvm-project/issues/168287) — `std::async` exceptions cannot be caught (Homebrew LLVM); see [this comment](https://github.com/llvm/llvm-project/issues/168287#issuecomment-3712718691) for Homebrew vs source-build context

The bug is **not limited to C++ modules** — it also affects regular translation units — but tester depends on both **modules** and **working exceptions**, so a unified source build is required.

**Workaround:** build LLVM from source with `libcxx`, `libcxxabi`, and `libunwind` in one tree, install to `/usr/local/llvm`. Homebrew `cmake` and `ninja` are convenient but optional; avoid `brew install llvm` for the compiler/stdlib.

---

## Prerequisites

### 1. Xcode Command Line Tools (required)

Provides the macOS SDK, `git`, and `make` (`/usr/bin/make`):

```bash
xcode-select --install
xcrun --show-sdk-path
```

Command Line Tools **do not** include `cmake` or `ninja`. You need CMake to configure LLVM (see below).

### 2. CMake (required)

Pick one:

| Source | Notes |
|--------|--------|
| [cmake.org download](https://cmake.org/download/) | Works without Homebrew |
| `brew install cmake` | Convenient if you already use Homebrew |

LLVM requires CMake >= 3.20 ([upstream requirements](https://llvm.org/docs/GettingStarted.html#software)).

### 3. Build driver (required) — Ninja or Make

| Driver | How to get it |
|--------|----------------|
| **Ninja** (recommended) | `brew install ninja`, or [release binary](https://github.com/ninja-build/ninja/releases) |
| **Make** | Included with Command Line Tools — use CMake generator `Unix Makefiles` |

Python 3.8+ is also required by LLVM's build ([Getting Started — Software](https://llvm.org/docs/GettingStarted.html#software)); macOS ships a suitable Python.

---

## Build LLVM with libc++ modules

Shallow clone is enough for a current toolchain ([upstream checkout notes](https://llvm.org/docs/GettingStarted.html#getting-the-source-code-and-building-llvm)):

```bash
git clone --depth 1 https://github.com/llvm/llvm-project.git
cd llvm-project
mkdir build && cd build
```

### Option A — Ninja (recommended)

```bash
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld;clang-tools-extra" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DLLVM_TARGETS_TO_BUILD=Native \
  -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
  -DDEFAULT_SYSROOT="$(xcrun --show-sdk-path)" \
  -DCMAKE_INSTALL_PREFIX=/usr/local/llvm \
  -DLLVM_PARALLEL_COMPILE_JOBS="$(sysctl -n hw.ncpu)" \
  -DLLVM_PARALLEL_LINK_JOBS=4

ninja -j"$(sysctl -n hw.ncpu)"
sudo ninja install
```

### Option B — Make (no Ninja, uses CLT `make`)

Same flags, different generator and build commands:

```bash
cmake -G "Unix Makefiles" ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld;clang-tools-extra" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DLLVM_TARGETS_TO_BUILD=Native \
  -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
  -DDEFAULT_SYSROOT="$(xcrun --show-sdk-path)" \
  -DCMAKE_INSTALL_PREFIX=/usr/local/llvm

make -j"$(sysctl -n hw.ncpu)"
sudo make install
```

Add to your shell profile if needed:

```bash
export PATH="/usr/local/llvm/bin:$PATH"
```

First full build may take 30–90 minutes on Apple Silicon.

---

## Verify the toolchain

### `std.cppm` and compiler

```bash
clang++ --version
test -f /usr/local/llvm/share/libc++/v1/std.cppm && echo "std.cppm OK"
```

### Module precompile smoke test

```bash
cat >/tmp/mod_smoke.c++m <<'EOF'
module;
export module smoke;
export int ok() { return 42; }
EOF

clang++ -std=c++23 -stdlib=libc++ \
  -nostdinc++ -isystem /usr/local/llvm/include/c++/v1 \
  -fno-implicit-modules -fno-implicit-module-maps \
  --precompile /tmp/mod_smoke.c++m -o /tmp/smoke.pcm

echo "modules OK"
```

### Exception unwind smoke test

After a source install to `/usr/local/llvm`, this should print `SUCCESS` (not abort). With Homebrew LLVM on macOS ARM it typically fails; see [#168287](https://github.com/llvm/llvm-project/issues/168287).

```bash
cat >/tmp/exn_smoke.cpp <<'EOF'
#include <future>
#include <iostream>
#include <stdexcept>

struct custom_exception : std::runtime_error {
    custom_exception() : std::runtime_error("custom_exception") {}
};

void throw_from_async() {
    auto future = std::async(std::launch::async, [] { throw custom_exception{}; });
    future.get();
}

int main() {
    try {
        throw_from_async();
    } catch (const custom_exception& e) {
        std::cout << "SUCCESS: " << e.what() << '\n';
        return 0;
    }
}
EOF

clang++ -std=c++20 -stdlib=libc++ \
  -nostdinc++ -isystem /usr/local/llvm/include/c++/v1 \
  -L/usr/local/llvm/lib -Wl,-rpath,/usr/local/llvm/lib \
  -lc++ -lc++abi -lunwind \
  /tmp/exn_smoke.cpp -o /tmp/exn_smoke

/tmp/exn_smoke
```

---

## Run tester

```bash
git clone --recursive https://github.com/ruoka/tester.git
cd tester
./tools/CB.sh debug test --jsonl --tags='\[self\]'
```

Optional overrides:

```bash
export CXX=/usr/local/llvm/bin/clang++
export LLVM_PATH=/usr/local/llvm/share/libc++/v1/std.cppm
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `std.cppm not found` | runtimes not installed | `sudo ninja install` or `sudo make install` from the LLVM `build` directory |
| `cmake: command not found` | CMake not installed | [cmake.org/download](https://cmake.org/download/) or `brew install cmake` |
| Module / `import` errors with Xcode clang | wrong compiler | use `/usr/local/llvm/bin/clang++` |
| `terminating due to uncaught exception` with `catch` | Homebrew LLVM unwinder on macOS ARM | source build to `/usr/local/llvm`; see [#92121](https://github.com/llvm/llvm-project/issues/92121), [#168287](https://github.com/llvm/llvm-project/issues/168287#issuecomment-3712718691) |
| CB uses wrong compiler | `CXX` unset or pointing elsewhere | `export CXX=/usr/local/llvm/bin/clang++` |

---

## Advanced: incremental rebuilds

If you already have an LLVM `build` tree and only need to refresh runtimes:

```bash
cd llvm-project/build
ninja cxx cxxabi unwind    # or: make cxx cxxabi unwind
sudo ninja install         # or: sudo make install
```

To build only `clang-tidy` in an existing tree:

```bash
ninja clang-tidy
sudo ninja install-clang-tidy
```

See [Getting Started with the LLVM System](https://llvm.org/docs/GettingStarted.html) for targets, testing (`check-llvm`), and other options.

---

## Linux

CI and dev containers use **Clang 21** from apt.llvm.org (`std.cppm` at `/usr/lib/llvm-21/share/libc++/v1/std.cppm`). See `.devcontainer/` and [README Requirements](../README.md#requirements).