# C++ Builder (CB)

CB (`tools/cb.c++`) is the module-aware build system that ships with tester. It is a single C++23 source file that discovers translation units, resolves module dependencies, compiles in parallel, and links binaries — with no CMake or Makefile required for day-one use.

This document explains **what CB is for**, **when to use it**, and **how it compares** to Make, CMake, and other build tools. For day-to-day commands, see the [Built-in Builder](../README.md#built-in-builder-cb) section in the README. For JSONL compile telemetry and agent workflows, see [AGENTS.md](../AGENTS.md). For planned enhancements, see [tester-improvements.md §4](tester-improvements.md#4-c-builder-cbc).

---

## What CB does

CB is optimized for **pure C++23 module projects** that follow ruoka layout conventions:

- **Discovery** — scans `*.c++m`, `*.cppm`, `*.c++`, `*.cpp`, `*.impl.c++`, `*.test.c++`, and `*.test.c++m` under configured include roots (`supported_suffixes` in `cb.c++`)
- **Module graph** — scans source preambles for `import` / `export module` and builds a dependency graph (no `clang-scan-deps` in `cb.c++`, which keeps scanning compiler-independent). Continued lines are spliced first, as translation phase 2 does — one substitution over the whole preamble, raw-string bodies included — and then comments, string literals and `#if 0` bodies are stripped before matching, so a commented-out `import` does not become a graph edge and a `#if \` / `0` region is as dead here as it is to the compiler; genuine `#ifdef` branches are over-approximated (every branch contributes an edge) because resolving them would require a preprocessor
- **Topological compile order** — compiles module interfaces and partitions before importers; emits PCM files under `build-<os>-<config>/pcm/`
- **Incremental caching** — skips recompilation when source timestamps and transitive PCM dependencies are unchanged (`cache_hit`, `rebuild_reason` in JSONL); compile cache invalidated when the **toolchain profile** changes (flags, compiler path, `std.cppm`, …); link step skipped when object signature unchanged
- **Parallel builds** — compiles independent translation units concurrently
- **Test integration** — auto-discovers `*.test.c++`, links `test_runner`, forwards `test` / `--tags` / `--list` to the framework
- **JSONL telemetry** — unified summary/failures/trace modes; trace includes `compile_end` and structured `argv`

Artifacts land in `build-<os>-<config>/` (`pcm/`, `obj/`, `bin/`, `cache/`). Examples: `build-linux-debug/`, `build-darwin-release/`.

On first run, **`CB.sh.core` bootstraps CB itself** — it compiles `tools/cb.c++` into `build-*/bin/cb`. No separate build-tool install beyond a capable `clang++` and `std.cppm`.

### Implementation: standard C++ only

`tools/cb.c++` and `tools/cb-*.h++` use **ISO C++23 and the standard library** — not POSIX APIs in the build path.

- **Subprocesses:** `posix_spawn` + `waitpid` through `cb::process::runner::invoke_shell(argv)` (still `/bin/sh -c` so capture redirects stay shell syntax). The runner also owns status decoding, diagnostics capture and failed-command messages. Apple's libc serializes `std::system`, which caps parallel compiles on macOS; `posix_spawn` does not. No `popen`, and no ad-hoc `fork` / `execve` outside that class.
- **Probes / capture:** redirect child stdout to a file (`compiler-version.txt`, self-test temp files), read with `std::ifstream`.
- **Invoked toolchain:** `clang++` and `lld` are external programs; calling them through `process::runner` is expected.
- **Algorithms:** prefer `std::views::join_with`, `std::ranges::to`, `std::views::split`, `std::ranges::set_difference`, etc. over index loops and one-off helpers — see [AGENTS.md — C++ style](../AGENTS.md).

The **test runner** is separate: crash **stack traces** in `test_runner.c++` use `<execinfo.h>` (`backtrace`, `backtrace_symbols_fd`) — POSIX/glibc/macOS only, not ISO C++. That is the deliberate exception; see [AGENTS.md — Implementation policy](../AGENTS.md#implementation-policy-standard-c-only).

---

## Key strengths

Condensed from the original project pitch — why teams pick CB over wiring CMake + CTest for ruoka-style repos:

- **Pure C++** — build orchestration lives in `cb.c++`; no CMake scripting, Makefile generation, YAML/TOML, or helper languages
- **Single-file transparency** — the ~3,300-line orchestrator remains in one place, with presentation split into focused console/JSONL observers
- **Zero config** — conventions (`*.c++m`, `import` lines, co-located `*.test.c++`) replace `CMakeLists.txt`
- **Fast incremental loops** — object timestamp cache, link signature cache, transitive PCM staleness; suited to Docker/CI where rebuild time matters
- **No extra learning curve** — if you know C++ modules and can read `cb.c++`, you understand the build; no second DSL
- **Cross-platform** — automatic OS detection (`build-<os>-<config>/`), Linux CI on Clang 21 (minimum) and macOS on a locally built LLVM (often newer) with per-repo `CB.sh` tuning
- **Self-contained embed** — one file to vendor; parent repos add a thin `CB.sh` config block (`tools/CB.sh.template`)
- **CI-friendly** — `./tools/CB.sh ci`, GitHub Actions badge, JSONL on stdout for agents; human wrapper logs on stderr

---

## When CB is a good fit

CB works well when:

- The project is **modules-first** (`.c++m` interfaces, co-located `.test.c++`)
- You want **zero external build configuration** — clone, `./tools/CB.sh debug test`, done
- The repo is **standalone** (tester itself) or **embedded as `deps/tester`** with a thin per-repo `CB.sh` wrapper
- You value **readability and control** — the entire build logic lives in one file you can read and modify
- **CI and AI agents** need machine-parseable compile/test output on stdout
- Incremental rebuild speed matters more than exotic target graphs

ruoka projects use this model — [YarDB](https://github.com/ruoka/YarDB) is the **public reference** for a multi-module app with `deps/tester`, `deps/net`, and `deps/xson`. Parent repos source shared bootstrap logic from `tools/CB.sh.core` and customize include paths, examples mode, and sandbox hooks in a small `tools/CB.sh` config block (see `tools/CB.sh.template`).

**CB also handles automatically:**

- OS and compiler detection (with cross-OS binary rebuild in `CB.sh.core`)
- `std.cppm` resolution from the LLVM install (overridable via `LLVM_PATH` or as the first CLI argument: `./tools/CB.sh /path/to/std.cppm debug build`)
- Module interface **and** implementation units (`.c++m` / `.cppm`, `.impl.c++`)
- Examples inclusion policy (`CB_INCLUDE_EXAMPLES_MODE`: `always` in standalone tester, `never` in most parent repos, examples still run on standalone `test`)

---

## When to use something else

CB is **not** trying to replace CMake, Bazel, or Meson for general-purpose builds. Reach for another tool when you need:

| Need | Why CB falls short |
|------|-------------------|
| Multi-language monorepos (C++, Rust, Protobuf codegen, …) | CB compiles C++ module TUs only |
| Install rules, packaging, CPack, distro `.deb` / `.rpm` | No install/export target model |
| IDE project generation beyond `compile_commands.json` / `graph.json` (presets, CMake export) | `list` writes both for clangd and the module graph; full IDE/CMake export stays out of scope |
| Cross-compilation matrices (Android, embedded, many triples) | Single-host `debug` / `release` configs |
| FetchContent / vcpkg / Conan dependency ecosystems | Dependencies are git submodules + include dirs |
| Complex conditional target graphs | No CMake-style generator expressions |
| Mature ecosystem integrations (CTest, sanitizers as first-class configs) | `asan` / `coverage` configs are backlog items |

For a large existing CMake codebase, migrating to CB is usually not worth it — consume tester from the bundled [`CMakeLists.txt`](../CMakeLists.txt) instead (see [CMake + Ninja](#cmake--ninja-the-other-alternative-in-this-repo) below). For a **new module-native C++23 library** in the ruoka style, CB removes a layer of tooling.

---

## Compared to other build tools

Honest positioning — each tool has a sweet spot.

| Concern | **CB** | **CMake** | **Make** (this repo's alternative) | **Ninja** | **Bazel** |
|---------|--------|-----------|------------------------|-----------|-----------|
| **C++23 module PCM ordering** | Built-in topological sort | Possible; manual or generator-dependent | Generated rules — `clang-scan-deps` p1689 here | Backend only; needs generator | Rules + toolchains |
| **Zero config for this layout** | Yes — convention over configuration | No — `CMakeLists.txt` required | Partial — existing `Makefile` | No — needs build file | No — `BUILD` files |
| **Standalone clone → build → test** | One command | Several steps + generator | `make` targets vary | Via CMake/etc. | `bazel test` setup |
| **Submodule embed** | `CB.sh.core` wrapper pattern | Per-parent project | Per-parent project | Via parent generator | Workspace rules |
| **Incremental compile cache** | Object + link cache, PCM staleness | ccache / compiler cache | Timestamp rules | Same as generator | Hermetic cache |
| **Parallel compilation** | Yes | Yes (with generator) | `-j` | Yes | Yes |
| **Test runner integration** | First-class `CB.sh test` | CTest adapter | Separate `make tests` | Via CTest | `bazel test` |
| **Agent/CI JSONL telemetry** | `compile_end`, `list --jsonl` | Adapters / custom | None | None | Event protocol |
| **Read/modify entire build logic** | ~single file (`cb.c++`) | Scattered CMake + scripts | Makefiles + rules | Build graph file | Starlark + rules |
| **Ecosystem & maturity** | Young, focused | Very mature | Mature, low-level | Mature backend | Mature at scale |

### Make (an alternative in this repo)

The `Makefile` is a supported second path, not a leftover: it orders modules with `clang-scan-deps` (p1689 output parsed by [`scripts/parse_module_deps.py`](../scripts/parse_module_deps.py)), builds the same library and runner, and works standalone or invoked from a parent `make`. What it lacks is CB's object cache and build telemetry. CI gates `make tests` / `make run_tests` with `[self]` for both `debug` (`DEBUG=1`) and `release` on every push (`makefile-build-and-test`); [release-policy.md](release-policy.md) still requires a clean Make rebuild before a release.

| Target | Purpose |
|--------|---------|
| `make module` | Build modules and `libtester.a` |
| `make tests` | Build `${BUILD_DIR}/bin/test_runner` |
| `make run_tests` | Build and run it; pass flags as `TEST_TAGS='--tags=[self]'` |
| `make run_examples` | Compile and run `examples/` demos (the default goal) |
| `make tools` | Build utilities under `${BUILD_DIR}/bin/tools/` — Linux only: it globs `tools/*.c++`, and `core_pc.c++` includes `<elf.h>` |
| `make deps` | Regenerate the module dependency graph only |
| `make mostlyclean` | Drop `${BUILD_DIR}/obj` and `${BUILD_DIR}/pcm`, `std.pcm` included |
| `make clean` | The above plus `bin/` and `lib/` |
| `make dump` | Print every file-scope make variable, for debugging the configuration |

**Build-tree names** (so the three paths never collide):

| Path | Directory |
|------|-----------|
| CB (primary) | `build-<os>-<config>/` — e.g. `build-darwin-debug` |
| Make | `build-make-<os>-<config>/` — default `release`; `DEBUG=1` → `debug` |
| CMake | `build-cmake-<os>-<config>/` — e.g. `build-cmake-linux-release` |

`<os>` is `uname -s` lowercased. Override Make with `BUILD_DIR` or `PREFIX`. A `make` build has no cache, so every unit is compiled; CB surfaces the same class of warnings on units it compiles (`diagnostics` when clang prints anything, including successful steps in `--jsonl=failures`). A CB build after `cache invalidate` recompiles everything. After `clean`, project units are cold but CB can restore the matching `std.pcm` / `std.o` from its shared machine-local cache.

When tester is embedded as a submodule, the **parent** Makefile/CB entry point owns paths — submodules typically share the parent's build tree rather than a separate tester build root.

### CMake

CMake excels at portable project configuration, dependency fetching, install trees, and IDE integration. CB deliberately avoids a second configuration language: discovery and conventions replace `CMakeLists.txt`. The trade-off is less flexibility outside the ruoka module layout.

`list` writes `compile_commands.json` at the project root for clangd (one entry per scanned source, using the same argv builders as a real compile) and `graph.json` (schema `cb-graph`) with the same unit inventory JSONL streams as `unit` / `list_summary`. Both are gitignored; regenerate with `./tools/CB.sh debug list` after the TU set or flags change. Full CMake export (targets, presets, install/export sets) stays out of scope.

### CMake + Ninja (the other alternative in this repo)

[`CMakeLists.txt`](../CMakeLists.txt) is the third supported path: CMake's own scanner orders the modules, Ninja runs the build, and the result is the same library and `test_runner`. It exists for two audiences — projects that already build with CMake and would rather not adopt CB, and anyone looking for a worked example of C++23 modules under CMake, since the interesting parts (`FILE_SET CXX_MODULES`, a `std` module target built from libc++'s own source, a static library plus a runner that must keep its self-registering test objects) are all exercised here rather than sketched.

```bash
cmake -S . -B build-cmake-darwin-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake-darwin-debug --target run_tests      # [self]
cmake --build build-cmake-darwin-debug --target run_all_tests  # examples included
```

| Constraint | Why |
|------------|-----|
| CMake 4.x | One supported line, pinned to 4.1.2 in CI and the dev container; older releases are untested |
| Ninja generator | Module dependency scanning; Make generators do not do it |
| `std` built from libc++ source | `${LLVM_PREFIX}/share/libc++/v1/std.cppm` is compiled as an ordinary module target, the way `STD_LLVM_PREFIX` is used in `config/compiler.mk`. CMake can supply `std` itself, but it finds the source via `clang++ -print-file-name=libc++.modules.json`, and apt.llvm.org installs a second copy of that manifest under `/usr/lib/<triple>` whose relative `source-path` only resolves from `${LLVM_PREFIX}/lib` — clang answers with the broken copy and the configure dies on a missing `/lib/share/libc++/v1/std.cppm` |
| Module set is hand-maintained | The globs pick up files, but the `FILE_SET` wiring is not derived from `import` lines the way CB's graph is |

Like Make, it has no object cache and no build telemetry, and it has no `list`-equivalent inventory. CI gates it warning-free for `Debug` and `Release` with `[self]` and the standalone suite (`cmake-ninja-build-and-test`); the dev container ships the pinned CMake and Ninja.

Wall-clock comparison of `tools/cb` against this CMake + Ninja path (including `--modules=one-phase`): [`docs/cb-vs-ninja-benchmark.md`](cb-vs-ninja-benchmark.md). Reproduce with `./tools/bench-cb-vs-ninja.sh`.

### Ninja / Meson / Bazel

- **Ninja** — a fast backend, not a project descriptor. CB invokes the compiler directly; there is no separate "generate then ninja" step. The bundled `CMakeLists.txt` above is where Ninja does get used, as CMake's backend.
- **Meson** — similar role to CMake with a different DSL; same trade-offs for module-native zero-config repos.
- **Bazel** — strong for hermetic multi-language monorepos at scale; heavy for a single-file test framework submodule.

---

## Standalone vs submodule workflows

### Standalone (clone tester)

```bash
git clone --recursive https://github.com/ruoka/tester.git
cd tester
./tools/CB.sh debug test
```

- `tools/CB.sh` includes `tester/` sources and **examples on `test` runs** (`CB_INCLUDE_EXAMPLES_MODE=always`)
- CB compiles itself (`cb.c++` → `build-*/bin/cb`) on first invocation via `CB.sh.core`
- Framework contract tests: `./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'`

### Embedded (`deps/tester` in a parent repo)

Parent projects provide their own `tools/CB.sh`. Clone [YarDB](https://github.com/ruoka/YarDB) for a working public example (`deps/tester`, co-located `*.test.c++`, `tools/CB.sh` over `CB.sh.core`):

- Sources shared logic from `deps/tester/tools/CB.sh.core` (or a nested copy)
- Sets `CB_INCLUDE_DIRS` to the parent library tree **and** tester
- Typically sets `CB_INCLUDE_EXAMPLES_MODE=never` — tester examples are not built by default
- May enable sandbox hooks (e.g. `CB_SANDBOX_DISABLE_NETWORK_TESTS` in net-based repos)

Tester is a **dependency**, not the build entry point. The parent's wrapper owns include paths, link flags, and submodule auto-init.

**Nested copies:** some repos embed tester twice (e.g. `deps/tester` and `deps/xson/deps/tester`). The parent's bootstrap resolves only first-level `deps/tester`, a sibling `../tester`, `CB_TESTER_ROOT`, or `CB_FETCH_DEPS=1` — order and variables are in [README — Embedding & tester resolution](../README.md#embedding--tester-resolution). Nested trees lag until their own submodule pointer moves; bump that pointer (do not paste newer docs or patches into the nested checkout). See also [tester-improvements.md §8](tester-improvements.md#8-submodule--monorepo-consumption).

---

## Commands (quick reference)

```bash
./tools/CB.sh debug build              # compile project + tests
./tools/CB.sh release build            # optimized; tests off unless --build-tests
./tools/CB.sh debug test               # build and run tests
./tools/CB.sh debug test "substring"   # positional filter on test id
./tools/CB.sh debug test --tags='\[self\]'   # filter by bracket tag
./tools/CB.sh debug test --list --jsonl=failures  # test catalogue
./tools/CB.sh debug list --jsonl=failures         # translation-unit inventory
./tools/CB.sh debug build --jsonl=trace            # full compile telemetry
./tools/CB.sh debug build --jobs=4                 # cap concurrent compiles/links
./tools/CB.sh debug build --modules=one-phase      # one clang++ step per modular unit
./tools/CB.sh ci --jsonl=summary                    # aggregate CI entry point
./tools/CB.sh debug clean
./tools/CB.sh debug clean --tests   # drop test objects + test_runner only
./tools/CB.sh --help
```

Pass `std.cppm` as the **first** argument when auto-detection fails: `./tools/CB.sh /path/to/std.cppm debug build`. A `.cppm` path that does not exist is an error, not a silently ignored argument.

**Unknown arguments exit `2`.** CB validates its whole argument list, so a typo such as `--tag=` (for `--tags=`) fails with a message instead of silently building or testing something you did not ask for.

**`--modules=<two-phase|one-phase>`** picks how a modular interface, partition, or `std.cppm` becomes a `.pcm` and a `.o`. Default `two-phase` runs `clang++ --precompile` and then compiles the resulting BMI. Its edge-driven scheduler publishes that BMI immediately, so importers can start while clang compiles the provider object. `one-phase` asks for both artefacts from a single `-c -fmodule-output=<pcm>` read of the source: the source is parsed once instead of twice, and Clang 22+ writes a [reduced BMI](https://clang.llvm.org/docs/StandardCPlusPlusModules.html#reduced-bmi) by default (on this tree, project BMIs dropped from 38 MB to 29 MB and a cold build from 19.2 s to 17.7 s).

The mode is a profile field, so switching it recompiles every unit rather than mixing BMIs the two schemes do not produce identically. Everything else is unchanged: the BMI lands at the same path, `-fmodule-file=` wiring, staleness reasons, and `clean` do not care which command wrote it. Under two-phase, `object_missing` / `object_stale` reuse the existing BMI for the object step only when that BMI is still fresh versus imports and textual headers (an import PCM newer than the unit's own PCM is `pcm_stale` and re-precompiles); one-phase has no object-only shortcut — those reasons re-read the source with `-fmodule-output=` (a reduced BMI is not a valid `pcm → .o` input).

**`--jobs=N`** bounds concurrent compile and link processes. Without it CB uses `hardware_concurrency()`; the cap exists because each `clang++` invocation on a module-heavy TU can peak at hundreds of megabytes. CB uses a bounded worker pool rather than creating one thread per translation unit. When `--jobs=` is set on a `test` invocation, CB also forwards it to `test_runner` (runner default remains `1` = sequential).

CB forwards common `test_runner` flags without `--`: `--tags=`, `--list`, `--jsonl[=summary|failures|trace]`, `--jsonl-output-max-bytes=…`, `--slowest=…`, `--jobs=…`, `--junit=<path>`, and `--xunit-xml=<path>`.

Environment variables for **bootstrap** (not test output): `LLVM_PATH`, `CXX`, `CB_INCLUDE_FLAGS`. See [Requirements](../README.md#requirements) in the README. macOS toolchain: [clang-modules-macos.md](clang-modules-macos.md) ([LLVM build docs](https://llvm.org/docs/GettingStarted.html)).

---

## Object cache profile

`build-<os>-<config>/cache/object-cache.txt` starts with a readable **profile header** (not a hash). `cb::cache::profile` owns its byte-stable serialization, shared-`std` key and field diff; `cb::cache::object_store` owns the path, loaded entries, persistence, status and invalidation. It compares the full profile string on load; a mismatch clears the in-memory object cache and sets `rebuild_reason: "profile_change"` on every recompiled TU.

**Format:** `format=cb-object-cache-v3` (tab-separated `key=value` fields after the `profile\t` prefix).

| Field | Meaning |
|-------|---------|
| `config` | `debug` or `release` |
| `static_link` | `0` / `1` |
| `module_phases` | `two-phase` / `one-phase` (see `--modules=`) |
| `llvm` | LLVM prefix derived from `std.cppm` |
| `cxx` | Resolved `clang++` path (`LLVM_CXX` / `CXX` override or `llvm/bin/clang++`) |
| `cxx_sig` | Compiler binary `size:mtime_ns` (detects toolchain binary swaps) |
| `clang_ver` | First line of `clang++ --version` (probed once per CB run through `process::runner`, with `cache::compiler_stamp` composing `storage_file` for the write/read path) |
| `std_cppm` | Canonical path to `std.cppm` with content signature (`path@size:mtime_ns`) |
| `compile` / `cpp` | Effective compile / per-TU C++ flags (includes `--compile-flags`) |

Legacy caches without a `profile\t` header still load; the header is rewritten on the next save. Bumping `format` or adding fields intentionally invalidates old caches once.

Cache classes compose `cache::storage_file` for path ownership, first-line reads, invalidation, and checked replacement. Replacement delegates to the shared `detail::write_atomic_file` primitive and atomically renames only after a complete flush; generated inventory files use the same primitive directly. A write or rename failure fails the build rather than promoting a partial file.

**Value encoding:** profile field values are stored verbatim (no percent-encoding). CB writes only values that cannot contain tab, newline, or `%` (paths, flag lists, and version lines satisfy this).

### Header dependencies

The object cache tracks module imports and source mtimes, neither of which sees a textual `#include`. CB therefore compiles with `-MMD -MF <object>.d` — emitted by the step that actually reads the source (`--precompile` for two-phase modular units, `-c` otherwise) — and `cb::cache::analyzer` parses the depfile and compares each prerequisite's mtime against the object. A newer header yields `rebuild_reason: "header_stale"` with the header in `rebuild.trigger_path`. A project header the depfile still names but that is missing or unreadable yields `rebuild_reason: "header_missing"` with that path in `rebuild.trigger_path` — a cache hit would keep shipping an object built against content that is gone.

A depfile that cannot be read — missing, unreadable, or lacking the `target:` prefix — yields `rebuild_reason: "depfile_unusable"` with the `.d` path in `rebuild.trigger_path`. It is the only record of a unit's textual includes, so "no parseable prerequisites" cannot be read as "no headers": that would hold a cache hit while ignoring every header edit until the source itself changed. Since `-MMD` writes a depfile even for a unit that includes nothing, this fires once after an upgrade or a wiped `obj/` and then settles.

Prerequisites are filtered to the project tree. Toolchain headers change as a unit and are already covered by the `cxx_sig` and `clang_ver` profile fields, so scanning them would add thousands of `stat` calls per build for no additional coverage.

**Human logs** (stderr, non-JSONL): `Object cache profile changed; invalidating compile cache (compile: + -DFOO)`.

**Inspect cache:** `./tools/CB.sh debug cache status` (human) or `… cache status --jsonl` (`cache_status` event). Both describe all four files under `cache/` — object cache, executable cache, `std-module-profile.txt`, `compiler-version.txt` — with entry counts and profile matches where those exist. `cache status` reports the compiler stamp as present even straight after an invalidate: it has to probe `clang++ --version` to know the current profile, and that probe is what writes the stamp.

**Invalidate indexes:** `./tools/CB.sh debug cache invalidate` removes all four files `cache status` reports — lighter than `clean`; artifacts in `obj/` / `pcm/` remain. JSONL: `cache_invalidate_end`, one flag per file. Removing `std-module-profile.txt` is what makes the next build rebuild `std.pcm`.

**Clean test artefacts only:** `./tools/CB.sh debug clean --tests` removes test TU objects/PCMs and `bin/test_runner`, and drops their object-/executable-cache entries. App and library objects stay, so the next build recompiles tests without a full cold rebuild.

**Std module:** `std.pcm` and `std.o` compile through the same reporting path as project units — one `compile_start` / `compile_end` pair for module `std`, with a `rebuild_reason` and a `cache_hit`, counted in `compile_total` and `rebuild_summary`. Their reasons are the ones a modular unit uses: `own_pcm_missing`, `profile_change`, `own_pcm_stale` rebuild the pcm and then the object; `object_missing` and `object_stale` (including when the object lags its own PCM after a partial two-phase compile) reuse the pcm that is already there. A failure attaches the compiler's own output as `diagnostics`, as any other compile does.

Successful std artefacts are also published to a full-profile-keyed, machine-local cache (`$XDG_CACHE_HOME/cb/std-module`, or `$HOME/.cache/cb/std-module`). A build after `clean` hard-links or copies the matching pair into the local build tree and reports the std compile as a cache hit. `cache invalidate` still forces recompilation: CB only hydrates when the local PCM is missing, never when its local profile was invalidated. Set `CB_STD_CACHE_DIR` to an alternate directory; set it to an empty value to disable sharing for a truly cold benchmark. The key covers the compiler and its binary/version signature, std source signature, config, module mode, and compile flags; project include paths are intentionally excluded because std compilation does not use them.

**Scanner scope:** the module graph is scanned with regular expressions, so a source that needs a tokenizer to read is out of scope. The known case is [lex.pptoken]'s reversion of phase-2 splices inside a raw-string body: honouring it means deciding whether an `R"(` opens a literal or is text inside a comment, which is lexical state rather than a pattern. A raw string whose body ends a line with `)\` is therefore read as closing one line early and may contribute a phantom edge — the same over-approximation `#ifdef` branches get. A comment or literal that merely mentions `R"(` is harmless, which is the likelier text and the reason the trade goes this way.

**Smoke tests:** `./tests/cb/smoke.sh` (also in CI `cb-smoke` job). Coverage includes `profile_header`, `cache_hit`, `link_cache_hit`, `clean_tests`, `parallel_main_link`, `compile_start`, `source_stale`, `header_stale`, `header_missing`, `depfile_unusable`, `strict_arguments`, `source_list`, `compile_commands`, `graph_json`, `compile_failure`, `compile_warning`, `modular_compile_warning`, `link_failure`, `test_link_failure`, `link_rebuild_reason`, `implementation_pcm`, scanner/inventory list-only cases, `rebuild_summary`, `test_lifecycle`, `cache_invalidate`, `profile_change`, `cache_status`, `std_module_reported`, `jsonl_modes`, `jsonl_failure_mode`.

**Optional follow-up:** `cache prune` for disk/orphan cleanup — backlog only; see [tester-improvements.md §4.4](tester-improvements.md#44-cache-maintenance-optional--add-if-operational-issues-appear).

---

## JSONL and correlation

Build-phase JSONL events share `run_id` with test-phase events when CB spawns `test_runner`. Trace mode includes per-command and per-TU events; compact modes retain aggregate `build_end`. Filter by `run_id` or `parent_run_id` to correlate build and test phases.

Each `build` or `test` invocation emits exactly one `build_start` / `build_end` pair. For `test`, that pair covers source compilation, ordinary links, and the `test_runner` link; `test_start` follows it.

Full event reference and triage workflow: [AGENTS.md](../AGENTS.md).

Useful compile/link fields for debugging stale builds:

- `compile_start` / `compile_end` — paired per TU. `compile_end.duration_ms` is wall time from compile start to finish (0 on cache hit). When `cache_hit: false`, both carry short `rebuild_reason` plus structured `rebuild` (`kind`, optional `module` / `pcm_path` / `object_path` / `trigger_path` / `hint` / `message` / `see_event`). `compile_start` also repeats `message` at the top level for log skimmers.
- `build_end.rebuild_summary` — per-kind compile rebuild counts plus `top_modules` (modules most often cited by PCM reasons). Present in every JSONL mode when any TU rebuilt.
- `profile_changed` — emitted **once** when the profile header mismatches (`reason: "profile_change"`, optional `profile_diff`). Scalars use `{"old":"…","new":"…"}`; `compile` / `cpp` use `{"added":[…],"removed":[…]}` (sorted token diff via `std::ranges::set_difference` on `flags::codec` tokens).
- `cache_hit: false` + `rebuild_reason: "profile_change"` on each recompiled TU — correlate with the single `profile_changed` event (`rebuild.see_event: "profile_changed"`); do not expect `profile_diff` on each `compile_end`.
- `link_end` — per executable after link or skip (`executable_path`, `cache_hit`, `ok`, `duration_ms`, `signature`). Skipped links emit `cache_hit: true` with `duration_ms: 0` and the same `signature` that made the link a hit. Relinks add `rebuild_reason` / `rebuild` (`missing_executable`, `not_in_cache`, `object_changed`, `link_flags_changed`, …).
- `rebuild_reason: "not_in_cache"` — first compile of this source for the current config (distinct from an edit)
- `rebuild_reason: "source_stale"` — TU source newer than cached object
- `rebuild_reason: "header_stale"` — an `#include`d project header is newer than the object; the header is in `rebuild.trigger_path` (see [Header dependencies](#header-dependencies))
- `rebuild_reason: "header_missing"` — a project header named by the depfile is missing or unreadable; the header is in `rebuild.trigger_path`
- `rebuild_reason: "depfile_unusable"` — the compiler `.d` is missing, unreadable, or malformed, so header freshness is unknown; the `.d` path is in `rebuild.trigger_path`
- `rebuild_reason: "pcm_stale"` — imported PCM (including an implementation unit's implicit interface PCM) newer than this object; module and trigger source are in `rebuild`

Example rebuild object:

```json
"rebuild_reason": "pcm_stale",
"rebuild": {
  "kind": "pcm_stale",
  "module": "sample",
  "pcm_path": "build-darwin-debug/pcm/sample.pcm",
  "object_path": "build-darwin-debug/obj/sample.impl.o",
  "trigger_path": "sample.c++m",
  "hint": "Imported PCM newer than this object; recompile follows module graph.",
  "message": "Rebuilding sample.impl.c++ because PCM sample is newer than the object (import graph)"
}
```

### Build failures

A failed compile or link is not just `ok: false`. The command's stdout and stderr are redirected to a per-target capture file and read back, so `compile_end`, `link_end` and `command_end` carry a `diagnostics` object:

```json
"ok": false,
"diagnostics": {
  "text": "hello.c++:1:21: error: use of undeclared identifier 'undefined_symbol_here'\n…",
  "path": "build-darwin-debug/obj/hello.o.log",
  "bytes": 220,
  "truncated": false
}
```

`text` is capped at 8 KiB with `truncated` recording whether it was cut; `path` always points at the full capture. A consumer restricted to stdout no longer has to rerun the build to find out what broke. The human (non-JSONL) path is unchanged: the diagnostic is still printed to stderr.

`command_end` and `test_end` also report a decoded process status. `waitpid` returns a wait status — 256 for a child that exited 1, 11 for one killed by `SIGSEGV` — so `process::runner` decodes it once at the shell boundary into `exit_code`, `signaled` and `signal`, keeping the raw value as `wait_status`. A test runner killed by a signal is reported as a crash rather than as an assertion failure.

Example `profile_diff` fragment (on `profile_changed` only):

```json
"reason": "profile_change",
"profile_diff": {
  "compile": { "added": ["-DCB_SMOKE_FLAG=1"], "removed": [] }
}
```

---

## Architecture (brief)

**`tools/cb.c++`** — parses the module graph, coordinates the owning cache classes, schedules parallel compiles, invokes `clang++` with `-fmodule-file=` flags, handles module interfaces and `.impl.c++` units, links executables (including `test_runner` with discovered test objects), and writes `compile_commands.json` / `graph.json` from `list`.

**`tools/CB.sh.core`** — bootstraps the `cb` binary, resolves `std.cppm`, handles cross-OS rebuild detection, JSONL-safe logging to stderr, and forwards args to `cb`.

**`tools/CB.sh`** (per repo) — thin config: include dirs, examples mode, sandbox env, extra link flags.

### Output observers

| File | Role |
|------|------|
| `cb-observer.h++` | Shared event models plus the `cb::output::observer` contract and observer registry |
| `cb-jsonl_observer.h++` | `cb::output::jsonl::observer` serialization, JSONL context, stream, and output lock |
| `cb-console_observer.h++` | `cb::output::console::observer` human formatting, stream, and output lock |
| `cb::output::notify` | Publishes build events directly to installed observers |
| `cb::output::{build,compile,link,test}_scope` | RAII pairing for lifecycle events, timing and step diagnostics |
| `cb::source::translation_unit` / `scanner` | Source identity, collection, exclusion, lexical cleaning, dependency edges and topological order |
| `cb::flags::codec` | Symmetric whitespace-normalized conversion between flag text and `string_list` |
| `cb::toolchain::clang_driver` | LLVM discovery/identity, Clang/libc++ flags, compile plans and compile/link argv construction |
| `cb::cache::storage_file` | Composed cache-file path, first-line read, atomic replacement and invalidation operations |
| `cb::cache::profile` / `analyzer` | Profile serialization/key/diff and recursive object/BMI/header freshness analysis |
| `cb::cache::object_store` | Object-cache path, entry snapshot, profile mismatch, persistence, status and invalidation |
| `cb::cache::link_store` | Executable signatures, relink decisions, parallel-safe updates, persistence, status and invalidation |
| `cb::cache::standard_module_store` | Local `std` profile/rebuild decision and shared-machine-cache hydrate/publish storage |
| `cb::cache::compiler_stamp` | Compiler-version stamp path, read, status and invalidation |
| `cb::process::runner` | Sole child-process boundary; shell quoting/argv joining, capture/status decoding and reported step execution |
| `cb::execution::run_workers` | Shared thread-group creation and join lifecycle; callers retain scheduling and failure policy |
| `cb::execution::worker_pool` | Bounded fixed-range job claiming with join-before-rethrow failure handling |
| `cb::cli::options` / `parser` | Strict argv parsing, test-runner forwarding, and projection into semantic build settings |
| `cb::build_system` | Settings-driven graph, artifact, cache, scheduling, execution, reporting and action orchestration |
| `cb::detail` | Shared filesystem/path primitives: directory joining, component-aware path predicates, weak canonicalization with normalized absolute fallback, atomic replacement and remove-if-present |

`build_system` consumes one `build_system::settings` value and derives its orchestration defaults. `cli::options::build_settings()` is the composition-layer adapter; action, output, and test-runner-only CLI state never enters the build engine. The concrete `clang_driver` receives compiler-facing settings and owns LLVM discovery, toolchain identity, include/default/module/link flags and command construction. It returns semantic compile plans; `build_system` executes those steps, publishes provider readiness, and owns source-graph, artifact, cache and reporting policy. There is deliberately no abstract driver or virtual interface before a second compiler exists. Cache maps and decisions stay in `cb::cache`, cache-file mechanics stay in composed `storage_file` values, flag text conversion stays in `cb::flags`, process mechanics stay in `cb::process`, generic independent-job concurrency stays in `cb::execution`, and event pairing stays in `cb::output`. `main` registers built-in observers by name and selects one from the parsed `output_name`.

### Ranges idioms (`cb.c++`)

CB uses C++23 range pipelines instead of hand-written accumulation loops where the standard library covers the case:

| Task | Pattern |
|------|---------|
| Join `string_list` with separator | `items \| std::views::join_with(sep) \| std::ranges::to<std::string>()` |
| Split delimited text → `vector` | `text \| std::views::split(delim) \| … \| std::ranges::to<string_list>()` |
| Split profile → `flat_map` | `text_ \| std::views::split('\t') \| std::views::transform(parse_field) \| std::ranges::to<field_map>()` |
| Shell-safe command string | `argv \| transform(shell_quote) \| views::join_with(' ') \| ranges::to<std::string>()` (private `process::runner::join_argv`; non-empty `argv` contract) |

`cache::profile::parse_field` splits on the **first** `=` only (`find`, not `views::split('=')`) because values like `compile` may contain `=`. Agent-oriented detail: [AGENTS.md](../AGENTS.md).

---

## Design principles

1. **Convention over configuration** — file extensions and `import` lines define the graph; no generated build files to maintain.
2. **Single-file transparency** — no hidden macros; read `cb.c++` to understand behaviour.
3. **Modules are first-class** — PCM ordering and staleness are core, not bolted on.
4. **Test and build in one tool** — `CB.sh test` is the primary developer loop.
5. **Machine-readable output** — JSONL for agents and CI; human logs on stderr.

---

## See also

- [README — Built-in Builder](../README.md#built-in-builder-cb) — quick start and commands
- [README — Makefile runner](../README.md#makefile-runner-alternative-to-cb) — a non-CB path
- [README — CMake + Ninja build](../README.md#cmake--ninja-build-alternative-to-cb) — the other one
- [cb-vs-ninja-benchmark.md](cb-vs-ninja-benchmark.md) — measured build times (`tools/bench-cb-vs-ninja.sh`)
- [CONTRIBUTING.md](../CONTRIBUTING.md) — all three build paths and the checks a change has to pass
- [YarDB](https://github.com/ruoka/YarDB) — public reference project (`deps/tester` + parent `tools/CB.sh`)
- [AGENTS.md](../AGENTS.md) — JSONL events, triage, correlation
- [tester-improvements.md §4–§5](tester-improvements.md#4-c-builder-cbc) — CB backlog and bootstrap scripts
- [P1204R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html) — canonical C++ project structure (co-located tests)
