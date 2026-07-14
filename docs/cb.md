# C++ Builder (CB)

CB (`tools/cb.c++`) is the module-aware build system that ships with tester. It is a single C++23 source file that discovers translation units, resolves module dependencies, compiles in parallel, and links binaries — with no CMake or Makefile required for day-one use.

This document explains **what CB is for**, **when to use it**, and **how it compares** to Make, CMake, and other build tools. For day-to-day commands, see the [Built-in Builder](../README.md#built-in-builder-cb) section in the README. For JSONL compile telemetry and agent workflows, see [AGENTS.md](../AGENTS.md). For planned enhancements, see [tester-improvements.md §4](tester-improvements.md#4-c-builder-cbc).

---

## What CB does

CB is optimized for **pure C++23 module projects** that follow ruoka layout conventions:

- **Discovery** — scans `*.c++m`, `*.c++`, `*.impl.c++`, and `*.test.c++` under configured include roots
- **Module graph** — scans source preambles for `import` / `export module` and builds a dependency graph (no `clang-scan-deps` in `cb.c++`)
- **Topological compile order** — compiles module interfaces and partitions before importers; emits PCM files under `build-<os>-<config>/pcm/`
- **Incremental caching** — skips recompilation when source timestamps and transitive PCM dependencies are unchanged (`cache_hit`, `rebuild_reason` in JSONL); compile cache invalidated when the **toolchain profile** changes (flags, compiler path, `std.cppm`, …); link step skipped when object signature unchanged
- **Parallel builds** — compiles independent translation units concurrently
- **Test integration** — auto-discovers `*.test.c++`, links `test_runner`, forwards `test` / `--tags` / `--list` to the framework
- **JSONL telemetry** — `list --jsonl`, `build --jsonl` with `compile_end`, structured `argv`, and `run_id` correlation with test runs

Artifacts land in `build-<os>-<config>/` (`pcm/`, `obj/`, `bin/`, `cache/`). Examples: `build-linux-debug/`, `build-darwin-release/`.

On first run, **`CB.sh.core` bootstraps CB itself** — it compiles `tools/cb.c++` into `build-*/bin/cb`. No separate build-tool install beyond a capable `clang++` and `std.cppm`.

### Implementation: standard C++ only

`tools/cb.c++` and `tools/cb-*.h++` use **ISO C++23 and the standard library** — not POSIX APIs in the build path.

- **Subprocesses:** `std::system` only (until the standard provides something better). No `popen`, `fork`, `execve`, or `posix_spawn`. Build a `string_list argv`; `invoke_shell(argv)` is the sole `join_argv` → `system()` boundary (compile, link, `test_runner`).
- **Probes / capture:** redirect child stdout to a file (`compiler-version.txt`, self-test temp files), read with `std::ifstream`.
- **Invoked toolchain:** `clang++` and `lld` are external programs; calling them via `std::system` is expected.
- **Algorithms:** prefer `std::views::join_with`, `std::ranges::to`, `std::views::split`, `std::ranges::set_difference`, etc. over index loops and one-off helpers — see [AGENTS.md — C++ style](../AGENTS.md).

The **test runner** is separate: crash **stack traces** in `test_runner.c++` use `<execinfo.h>` (`backtrace`, `backtrace_symbols_fd`) — POSIX/glibc/macOS only, not ISO C++. That is the deliberate exception; see [AGENTS.md — Implementation policy](../AGENTS.md#implementation-policy-standard-c-only).

---

## Key strengths

Condensed from the original project pitch — why teams pick CB over wiring CMake + CTest for ruoka-style repos:

- **Pure C++** — build orchestration lives in `cb.c++`; no CMake scripting, Makefile generation, YAML/TOML, or helper languages
- **Single-file transparency** — the ~2400-line orchestrator remains in one place, with presentation split into focused console/JSONL sinks
- **Zero config** — conventions (`*.c++m`, `import` lines, co-located `*.test.c++`) replace `CMakeLists.txt`
- **Fast incremental loops** — object timestamp cache, link signature cache, transitive PCM staleness; suited to Docker/CI where rebuild time matters
- **No extra learning curve** — if you know C++ modules and can read `cb.c++`, you understand the build; no second DSL
- **Cross-platform** — automatic OS detection (`build-<os>-<config>/`), Linux (Clang 21) and macOS (`/usr/local/llvm`) with per-repo `CB.sh` tuning
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
- Module interface **and** implementation units (`.c++m`, `.impl.c++`)
- Examples inclusion policy (`CB_INCLUDE_EXAMPLES_MODE`: `always` in standalone tester, `never` in most parent repos, examples still run on standalone `test`)

---

## When to use something else

CB is **not** trying to replace CMake, Bazel, or Meson for general-purpose builds. Reach for another tool when you need:

| Need | Why CB falls short |
|------|-------------------|
| Multi-language monorepos (C++, Rust, Protobuf codegen, …) | CB compiles C++ module TUs only |
| Install rules, packaging, CPack, distro `.deb` / `.rpm` | No install/export target model |
| IDE project generation (`compile_commands.json`, presets) | Not generated today (see backlog) |
| Cross-compilation matrices (Android, embedded, many triples) | Single-host `debug` / `release` configs |
| FetchContent / vcpkg / Conan dependency ecosystems | Dependencies are git submodules + include dirs |
| Complex conditional target graphs | No CMake-style generator expressions |
| Mature ecosystem integrations (CTest, sanitizers as first-class configs) | `asan` / `coverage` configs are backlog items |

For a large existing CMake codebase, migrating to CB is usually not worth it. For a **new module-native C++23 library** in the ruoka style, CB removes a layer of tooling.

---

## Compared to other build tools

Honest positioning — each tool has a sweet spot.

| Concern | **CB** | **CMake** | **Make** (legacy here) | **Ninja** | **Bazel** |
|---------|--------|-----------|------------------------|-----------|-----------|
| **C++23 module PCM ordering** | Built-in topological sort | Possible; manual or generator-dependent | Manual rules | Backend only; needs generator | Rules + toolchains |
| **Zero config for this layout** | Yes — convention over configuration | No — `CMakeLists.txt` required | Partial — existing `Makefile` | No — needs build file | No — `BUILD` files |
| **Standalone clone → build → test** | One command | Several steps + generator | `make` targets vary | Via CMake/etc. | `bazel test` setup |
| **Submodule embed** | `CB.sh.core` wrapper pattern | Per-parent project | Per-parent project | Via parent generator | Workspace rules |
| **Incremental compile cache** | Object + link cache, PCM staleness | ccache / compiler cache | Timestamp rules | Same as generator | Hermetic cache |
| **Parallel compilation** | Yes | Yes (with generator) | `-j` | Yes | Yes |
| **Test runner integration** | First-class `CB.sh test` | CTest adapter | Separate `make tests` | Via CTest | `bazel test` |
| **Agent/CI JSONL telemetry** | `compile_end`, `list --jsonl` | Adapters / custom | None | None | Event protocol |
| **Read/modify entire build logic** | ~single file (`cb.c++`) | Scattered CMake + scripts | Makefiles + rules | Build graph file | Starlark + rules |
| **Ecosystem & maturity** | Young, focused | Very mature | Mature, low-level | Mature backend | Mature at scale |

### Make (legacy in this repo)

A `Makefile` remains for compatibility. CB is recommended; Make predates module-aware dependency resolution.

| Target | Purpose |
|--------|---------|
| `make help` | List targets and configuration knobs |
| `make module` | Build modules and `libtester.a` |
| `make run_examples` | Compile and run `examples/` demos |
| `make tests` | Build standalone `test_runner` |
| `make tools` | Build utilities under `${BUILD_DIR}/bin/tools/` |
| `make clean` | Remove `bin/`, `lib/`, submodule stamps; **preserves** `std.pcm` |
| `make mostlyclean` | Drop only `${BUILD_DIR}/obj` for a fast incremental rebuild |

Makefile artifacts use `build-<os>/` (e.g. `build-linux/pcm`, `build-linux/lib`). Override layout with `BUILD_DIR` or `PREFIX`.

When tester is embedded as a submodule, the **parent** Makefile/CB entry point owns paths — submodules typically share the parent's `build-<os>/` tree rather than a separate tester build root.

### CMake

CMake excels at portable project configuration, dependency fetching, install trees, and IDE integration. CB deliberately avoids a second configuration language: discovery and conventions replace `CMakeLists.txt`. The trade-off is less flexibility outside the ruoka module layout.

If you need `compile_commands.json` for clangd, that is a known gap — tracked as a proposed (optional) feature in [tester-improvements.md §4.1](tester-improvements.md#41-core-build-system), explicitly marked as conflicting with zero-config philosophy.

### Ninja / Meson / Bazel

- **Ninja** — a fast backend, not a project descriptor. CB invokes the compiler directly; there is no separate "generate then ninja" step.
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
- Framework contract tests: `./tools/CB.sh debug test --jsonl --tags='\[self\]'`

### Embedded (`deps/tester` in a parent repo)

Parent projects provide their own `tools/CB.sh`. Clone [YarDB](https://github.com/ruoka/YarDB) for a working public example (`deps/tester`, co-located `*.test.c++`, `tools/CB.sh` over `CB.sh.core`):

- Sources shared logic from `deps/tester/tools/CB.sh.core` (or a nested copy)
- Sets `CB_INCLUDE_DIRS` to the parent library tree **and** tester
- Typically sets `CB_INCLUDE_EXAMPLES_MODE=never` — tester examples are not built by default
- May enable sandbox hooks (e.g. `CB_SANDBOX_DISABLE_NETWORK_TESTS` in net-based repos)

Tester is a **dependency**, not the build entry point. The parent's wrapper owns include paths, link flags, and submodule auto-init.

**Nested copies:** some repos embed tester twice (e.g. `deps/tester` and `deps/xson/deps/tester`). Submodule pointers can lag; bump the parent pointer after tester fixes. See [tester-improvements.md §8](tester-improvements.md#8-submodule--monorepo-consumption).

---

## Commands (quick reference)

```bash
./tools/CB.sh debug build              # compile project + tests
./tools/CB.sh release build            # optimized; tests off unless --build-tests
./tools/CB.sh debug test               # build and run tests
./tools/CB.sh debug test "substring"   # positional filter on test id
./tools/CB.sh debug test --tags='\[self\]'   # filter by bracket tag
./tools/CB.sh debug test --list --jsonl      # test catalogue
./tools/CB.sh debug list --jsonl       # translation-unit inventory
./tools/CB.sh debug build --jsonl      # compile telemetry
./tools/CB.sh ci --jsonl               # clean, then test (CI entry point)
./tools/CB.sh debug clean
./tools/CB.sh --help
```

Pass `std.cppm` as the **first** argument when auto-detection fails: `./tools/CB.sh /path/to/std.cppm debug build`.

CB forwards common `test_runner` flags without `--`: `--tags=`, `--list`, `--jsonl`, `--jsonl-output=…`, `--slowest=…`.

Environment variables for **bootstrap** (not test output): `LLVM_PATH`, `CXX`, `CB_INCLUDE_FLAGS`. See [Requirements](../README.md#requirements) in the README. macOS toolchain: [clang-modules-macos.md](clang-modules-macos.md) ([LLVM build docs](https://llvm.org/docs/GettingStarted.html)).

---

## Object cache profile

`build-<os>-<config>/cache/object-cache.txt` starts with a readable **profile header** (not a hash). CB compares the full profile string on load; a mismatch clears the in-memory object cache and sets `rebuild_reason: "profile_change"` on every recompiled TU.

**Format:** `format=cb-object-cache-v3` (tab-separated `key=value` fields after the `profile\t` prefix).

| Field | Meaning |
|-------|---------|
| `config` | `debug` or `release` |
| `static_link` | `0` / `1` |
| `llvm` | LLVM prefix derived from `std.cppm` |
| `cxx` | Resolved `clang++` path (`LLVM_CXX` / `CXX` override or `llvm/bin/clang++`) |
| `cxx_sig` | Compiler binary `size:mtime_ns` (detects toolchain binary swaps) |
| `clang_ver` | First line of `clang++ --version` (probed once per CB run via `std::system`, written to `cache/compiler-version.txt`, read with `std::ifstream`) |
| `std_cppm` | Canonical path to `std.cppm` with content signature (`path@size:mtime_ns`) |
| `compile` / `cpp` | Effective compile / per-TU C++ flags (includes `--compile-flags`) |

Legacy caches without a `profile\t` header still load; the header is rewritten on the next save. Bumping `format` or adding fields intentionally invalidates old caches once.

Cache indexes are written through a checked temporary file and atomically renamed only after a complete flush. A write or rename failure fails the build rather than promoting a partial index.

**Value encoding:** profile field values are stored verbatim (no percent-encoding). CB writes only values that cannot contain tab, newline, or `%` (paths, flag lists, and version lines satisfy this).

**Human logs** (stderr, non-JSONL): `Object cache profile changed; invalidating compile cache (compile: + -DFOO)`.

**Inspect cache:** `./tools/CB.sh debug cache status` (human) or `… cache status --jsonl` (`cache_status` event).

**Invalidate indexes:** `./tools/CB.sh debug cache invalidate` removes `object-cache.txt`, `executable-cache.txt`, and `compiler-version.txt` only — lighter than `clean`; artifacts in `obj/` / `pcm/` remain. JSONL: `cache_invalidate_end`.

**Smoke tests:** `./tests/cb/smoke.sh` (also in CI `cb-smoke` job) — `profile_header`, `cache_hit`, `link_cache_hit`, `compile_start`, `cache_invalidate`, `profile_change`, `cache_status`.

**Optional follow-up:** `cache prune` for disk/orphan cleanup — backlog only; see [tester-improvements.md §4.4](tester-improvements.md#44-cache-maintenance-optional--add-if-operational-issues-appear).

---

## JSONL and correlation

Build-phase JSONL events (`build_start`, `compile_start`, `compile_end`, `command_start`, `list_start`, `unit`, …) share `run_id` with test-phase events when CB spawns `test_runner`. Filter by `run_id` or `parent_run_id` to correlate `list` → `build` → `test` from one `./tools/CB.sh … --jsonl` invocation.

Each `build` or `test` invocation emits exactly one `build_start` / `build_end` pair. For `test`, that pair covers source compilation, ordinary links, and the `test_runner` link; `test_start` follows it.

Full event reference and triage workflow: [AGENTS.md](../AGENTS.md).

Useful compile/link fields for debugging stale builds:

- `compile_start` / `compile_end` — paired per TU. `compile_end.duration_ms` is wall time from compile start to finish (0 on cache hit). `rebuild_reason` appears on `compile_start` when recompiling and on `compile_end` when `cache_hit: false`.
- `profile_changed` — emitted **once** when the profile header mismatches (`reason: "profile_change"`, optional `profile_diff`). Scalars use `{"old":"…","new":"…"}`; `compile` / `cpp` use `{"added":[…],"removed":[…]}` (sorted token diff via `std::ranges::set_difference` on shell words).
- `cache_hit: false` + `rebuild_reason: "profile_change"` on each recompiled TU — correlate with the single `profile_changed` event for the diff.
- `link_end` — per executable after link or skip (`executable_path`, `cache_hit`, `ok`, `duration_ms`). Skipped links emit `cache_hit: true` with `duration_ms: 0`.
- `cache_hit: false` + `rebuild_reason: "pcm_stale:tester:assertions"` — transitive PCM invalidation; rebuilds test objects when assertion templates change
- `rebuild_reason: "source_stale"` — TU source newer than cached object
- `rebuild_reason: "pcm_stale:<module>"` — imported PCM, including an implementation unit's implicit interface PCM, is newer than its object

Example `profile_diff` fragment:

```json
"rebuild_reason": "profile_change",
"profile_diff": {
  "compile": { "added": ["-DCB_SMOKE_FLAG=1"], "removed": [] }
}
```

---

## Architecture (brief)

**`tools/cb.c++`** (~2400 lines) — parses the module graph, maintains `object_cache_map` and executable link signature cache, schedules parallel compiles, invokes `clang++` with `-fmodule-file=` flags, handles module interfaces and `.impl.c++` units, links executables (including `test_runner` with discovered test objects).

**`tools/CB.sh.core`** — bootstraps the `cb` binary, resolves `std.cppm`, handles cross-OS rebuild detection, JSONL-safe logging to stderr, and forwards args to `cb`.

**`tools/CB.sh`** (per repo) — thin config: include dirs, examples mode, sandbox env, extra link flags.

### Output sinks

| File | Role |
|------|------|
| `cb-jsonl_sink.h++` | `namespace cb` — shared `object_cache_profile_diff` types; `namespace cb_jsonl` — JSONL event serialization (`write_profile_diff`, `sink::profile_changed`, `cache_status`, …) |
| `cb-console_sink.h++` | Human formatting (`format_profile_diff`, `sink::profile_changed`, `cache_status`, …) |
| `cb::log` (in `cb.c++`) | Routes events that have both human and JSONL forms; suppresses human `log::info` when JSONL is on |
| `cb::detail` | Domain compute only (profile parse/diff, flag token diff) — not presentation |

`build_system` gathers facts; `cb::log` picks the channel for dual-format events; JSONL-only compile/link lifecycle helpers emit telemetry directly. Each sink owns how its output looks. Shared profile field iteration keeps diff computation and both sink formats aligned when fields are added.

### Ranges idioms (`cb.c++`)

CB uses C++23 range pipelines instead of hand-written accumulation loops where the standard library covers the case:

| Task | Pattern |
|------|---------|
| Join `string_list` with separator | `items \| std::views::join_with(sep) \| std::ranges::to<std::string>()` |
| Split delimited text → `vector` | `text \| std::views::split(delim) \| … \| std::ranges::to<string_list>()` |
| Split profile → `flat_map` | `profile \| std::views::split('\t') \| std::views::transform(parse_profile_field) \| std::ranges::to<profile_fields>()` |
| Shell-safe command string | `argv \| transform(shell_quote) \| views::join_with(' ') \| ranges::to<std::string>()` (`join_argv`; non-empty `argv` — contract at `invoke_shell`) |

`parse_profile_field` splits on the **first** `=` only (`find`, not `views::split('=')`) because values like `compile` may contain `=`. Agent-oriented detail: [AGENTS.md](../AGENTS.md).

---

## Design principles

1. **Convention over configuration** — file extensions and `import` lines define the graph; no generated build files to maintain.
2. **Single-file transparency** — no hidden macros; read `cb.c++` to understand behaviour.
3. **Modules are first-class** — PCM ordering and staleness are core, not bolted on.
4. **Test and build in one tool** — `CB.sh test` is the primary developer loop.
5. **Machine-readable output** — JSONL for agents and CI; human logs on stderr.

---

## See also

- [README — Built-in Builder](../README.md#built-in-builder-cb) — quick start, commands, legacy Makefile
- [YarDB](https://github.com/ruoka/YarDB) — public reference project (`deps/tester` + parent `tools/CB.sh`)
- [AGENTS.md](../AGENTS.md) — JSONL events, triage, correlation
- [tester-improvements.md §4–§5](tester-improvements.md#4-c-builder-cbc) — CB backlog and bootstrap scripts
- [P1204R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html) — canonical C++ project structure (co-located tests)
