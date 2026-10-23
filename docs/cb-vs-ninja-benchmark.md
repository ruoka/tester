# CB vs CMake + Ninja vs Make — build-time notes

Wall-clock comparison of **`tools/cb`** (direct, no `CB.sh` bootstrap) against this repo's **CMake + Ninja** path and, optionally, the **Makefile** path. Reproduce with:

```bash
./tools/bench-cb-vs-ninja.sh                      # CB two-phase (default) vs Ninja -v
./tools/bench-cb-vs-ninja.sh --with-make          # … plus `make DEBUG=1 tests`
./tools/bench-cb-vs-ninja.sh --modules=one-phase  # CB one-phase vs Ninja -v
./tools/bench-cb-vs-ninja.sh --cb-only            # CB scenarios only (revision comparisons)
./tools/bench-cb-vs-ninja.sh --quiet-ninja        # Ninja progress only
./tools/bench-cb-vs-ninja.sh --modules=one-phase --jobs=4
./tools/run-linux-bench.sh                        # two-phase in .devcontainer (from macOS host)
./tools/run-linux-bench.sh --with-make            # options are forwarded to the script
```

Scenarios: cold full rebuild (after `clean` / `rm -rf` build tree), no-op rebuild,
touch one test TU (`tester/tester-assertions.test.c++`), touch a widely imported module
interface (`tester/tester-assertions.c++m`). The script exports an empty `CB_STD_CACHE_DIR`
by default, so every cold sample includes the std module compile. Supply a non-empty
directory explicitly to measure clean builds that reuse CB's shared std cache.

Each run is three cold builds, five no-op builds, and three runs of each touch scenario.
Ninja figures are always from the same run as the CB figures beside them.

## Flag parity

Since 2026-10-23, [`CMakeLists.txt`](../CMakeLists.txt) mirrors CB's compile and link flags
(`clang_driver::initialize_flags` in `tools/cb.c++`): `-O0 -g3` for Debug, `-O3 -DNDEBUG` for
Release, lld for linking, the same warning, module, and libc++ header flags. Before that, the
CMake path followed `config/compiler.mk` — `-g` instead of `-g3`, `-fno-omit-frame-pointer`,
the platform linker instead of lld on macOS — so CB and Ninja were compiling different amounts
of debug information and linking with different linkers. The August numbers below the fold were
taken that way and are not like-for-like.

The **Makefile keeps `config/compiler.mk`'s flags**, which parent repos share, so Make rows are
not flag-for-flag comparable; the gap they show is far larger than any flag difference.

## macOS / Clang 23

| Knob | Value |
|------|--------|
| Host | macOS arm64, 8 jobs |
| Config | Debug (`-O0 -g3` on CB and Ninja) |
| Compiler | Clang 23 trunk from `/usr/local/llvm` |
| CMake / Ninja | CMake 4.1.2 / Ninja 1.13.2 |
| CB | `tools/cb` with the same argv/env `CB.sh` would `exec` (absolute `-I`, `--include-examples`, `SDKROOT`, `LDFLAGS`); `-fprebuilt-module-path` plus `-fmodule-file=std=…` |
| Ninja | `cmake --build … --verbose` → `ninja -v` (full command lines; output captured to temp files) |
| Make | `make DEBUG=1 -j8 tests` |
| Scope | Standalone tree, examples included (36 TUs plus `std`) |
| Date | 2026-10-23, `238a525` plus the flag-parity change |

Mean wall-clock. The two-phase and Make columns come from one `--with-make` run; the one-phase
columns from a separate `--modules=one-phase` run.

| Scenario | CB two-phase | Ninja | ratio | Make | CB one-phase | Ninja | ratio |
|----------|-------------:|------:|------:|-----:|-------------:|------:|------:|
| Cold full | 9.23 s | 9.41 s | 0.98× | 22.06 s | 7.85 s | 7.98 s | 0.98× |
| Cold + configure | 9.23 s | 10.23 s | 0.90× | 22.06 s | 7.85 s | 8.67 s | 0.91× |
| No-op (median) | 62 ms | 55 ms | 1.13× | 118 ms | 61 ms | 54 ms | 1.13× |
| Touch one test TU | 2.87 s | 2.91 s | 0.98× | 19.46 s | 2.60 s | 2.68 s | 0.97× |
| Touch module interface | 5.66 s | 6.98 s | 0.81× | 18.80 s | 5.50 s | 5.69 s | 0.97× |

A second two-phase run the same day (without Make) gave 1.07× cold, 0.86× test touch, and
0.99× module touch — the same machine moves CB-vs-Ninja ratios by 10–15% between runs, so on
macOS the honest reading is **parity within noise** on every compile-bound scenario. Ninja
keeps a small, consistent no-op lead (55 ms vs 62 ms median; CB's own `duration_ms` is
~30 ms, the rest is process startup). The absolute times differ between the two-phase and
one-phase runs for Ninja too (9.41 s vs 7.98 s cold), which is host load, not module mode.

## Linux / Clang 21 (`.devcontainer`)

Measured in the tester [`.devcontainer`](../.devcontainer) image via `./tools/run-linux-bench.sh`,
which builds the image if needed and writes `.bench-ab/cb-vs-ninja-bench-linux.*`. It sets
`CB_BIN` to a container-local path so the Linux ELF does not overwrite a host `tools/cb` on the
bind mount. The equivalent manual run:

```bash
docker build -t tester-dev-trixie:latest -f .devcontainer/Dockerfile .devcontainer
docker run --rm -v "$PWD:/work" -w /work \
  -e CC=clang-21 -e CXX=clang++-21 \
  -e LLVM_PREFIX=/usr/lib/llvm-21 \
  -e CXX_COMPILER=/usr/bin/clang++-21 \
  -e STD_CPPM=/usr/lib/llvm-21/share/libc++/v1/std.cppm \
  -e LDFLAGS='-Wl,--push-state,--no-as-needed -lc++ -lc++abi -lc++experimental -Wl,--pop-state -pthread -ldl' \
  -e CB_BIN=/tmp/cb-linux \
  tester-dev-trixie:latest \
  ./tools/bench-cb-vs-ninja.sh --jobs=4
```

| Knob | Value |
|------|--------|
| Host | Linux aarch64 (Docker Desktop on the macOS host above), 8 CPUs visible, `--jobs=4` |
| Config | Debug (`-O0 -g3` on CB and Ninja) |
| Compiler | Debian Clang 21.1.8 (`clang++-21` → `/usr/lib/llvm-21`) |
| CMake / Ninja | CMake 4.1.2 / Ninja 1.12.1 |
| Make | `make DEBUG=1 -j4 tests` |
| Scope | Standalone tree, examples included (36 TUs plus `std`) |
| Date | 2026-10-23, `238a525` plus the flag-parity change |

CB vs Ninja is from a run without Make, which was the steadier of the two; Make is from the
`--with-make` run, whose CB/Ninja ratios are given in the prose below.

| Scenario | CB two-phase | Ninja | ratio | Make |
|----------|-------------:|------:|------:|-----:|
| Cold full | 14.15 s | 15.65 s | 0.90× | 17.61 s |
| Cold + configure | 14.15 s | 16.09 s | 0.88× | 17.61 s |
| No-op (mean) | 32 ms | 38 ms | 0.85× | 59 ms |
| Touch one test TU | 4.80 s | 4.39 s | 1.09× | 14.23 s |
| Touch module interface | 11.30 s | 11.97 s | 0.94× | 18.00 s |

The `--with-make` run was noisier — Ninja cold ranged 16.8–21.3 s, module touch 12.6–22.0 s —
and put CB at 0.74× cold, 1.00× test touch, and 0.72× module touch. Taken together, CB holds a
modest lead on Linux for cold and module-touch builds and is level on a single-TU touch; no-op is
a wash at ~30–40 ms. One-phase was not re-measured on Linux. Clang 21 does not default to the
reduced BMI path that Clang 22+ uses for one-phase, so module mode should be measured on the
deployment toolchain rather than assumed universally faster.

## Why Make is slow here

The Makefile is a correct build but not an incremental one for modules. Every `.pcm` and `.o`
rule depends on `$(module_depfile)` (`build-make-*/bmi/modules.dep`), and that file depends on
every source. Touching any source therefore (1) reruns `clang-scan-deps` plus
`scripts/parse_module_deps.py` once per source in a serial shell loop, and (2) leaves every BMI
and object older than the regenerated depfile, so the whole project recompiles. That is why a
one-file test touch (19.5 s on macOS) costs nearly as much as a cold build (22.1 s). The serial
scan itself is ~2.9 s on macOS (`make DEBUG=1 deps` from scratch); the rest of Make's cold-build
gap to CB and Ninja (~13 s) has not been broken down. Two changes would address it, and neither
is done today: rewrite `modules.dep` only when its contents change (or make it order-only, since
the per-unit rules it contains already carry the real edges), which stops the full recompile;
and scan into one `.deps` file per source, so an edit rescans one file instead of all of them.

## How to read the gap

- **No-op:** Ninja checks a persistent `build.ninja` graph (mtime / dirty edges). CB still rediscovers TUs and consults its object cache every run, but after memoizing analyzer decisions, replacing `std::regex` lexical stripping, and cutting redundant filesystem work, that path is no longer the dominant cost — warm no-op is within ~10–15% of Ninja, and most of the residual is process startup. Watch CB's own `duration_ms` on `*_detail` rows (~30 ms on macOS) when a wall-clock sample spikes.
- **Cold / touch:** Both spend almost all the time in `clang++`. With flags matched, CB and Ninja run essentially the same compiler work, so differences come from scheduling. Two-phase CB publishes a provider BMI before its object finishes, so importers can start earlier; that is the plausible source of CB's module-touch lead where one shows up, but at 36 TUs it is the same order as run-to-run noise. CMake's separate configure step (0.5–0.9 s) is the other visible cost on the Ninja side.
- **Ninja `-v` vs quiet:** Full command-line logging did not change wall-clock meaningfully when redirected to files. Quiet Ninja no-op was also ~56 ms on macOS.
- **Add/delete sources:** CB always rescans. This repo's `CMakeLists.txt` uses `GLOB … CONFIGURE_DEPENDS`, so Ninja re-checks globs and CMake regenerates the graph when matching files appear or disappear — no hand `cmake` for ordinary glob hits.

Numbers are single-machine snapshots, not a CI gate. Re-run the script after toolchain or tree changes.

## What CB offers beyond wall-clock

At this tree's size, speed alone is not a reason to choose CB over CMake + Ninja: compile-bound
scenarios are at parity within noise on macOS and modestly ahead on Linux, and Ninja keeps a
small no-op edge. The reasons to pick CB are elsewhere:

- **No configure step or build description.** Discovery from file suffixes and `import` lines
  replaces `CMakeLists.txt`; a new source directory under an include root is picked up without
  editing anything. CMake needs a glob per directory and a configure pass (0.5–0.9 s here).
- **Rebuilds explain themselves.** Every recompile carries a structured `rebuild_reason`
  (`bmi_stale` with the module, `header_stale` with the header path, `profile_change`, …), and
  a flag or toolchain change produces one `profile_changed` event with a token diff. The profile
  includes the compiler binary's size and mtime, so swapping a compiler in place is noticed.
  Ninja's closest equivalent, `ninja -d explain`, is free text, and it only sees command-line
  changes.
- **One event stream from build to test.** Compiler diagnostics arrive in
  `diagnostics.text` on the failed `compile_end`, build and test events share a `run_id`, and
  `test --list`, JUnit, and the MCP bridge sit on the same tool. With CMake + Ninja, CI and
  agents scrape logs instead.
- **Early BMI publication.** Two-phase mode releases importers when a provider's `.pcm` is
  written, and compiles its `.o` as a separate job. CMake with Clang emits both from one step,
  so importers wait for the object. The effect is small at 36 TUs and grows with deeper import
  chains.
- **Shared `std` cache.** A machine-local cache restores `std.pcm` / `std.o` after `clean`
  (disabled in these benchmarks); CMake recompiles `std` per build tree.
- **Self-bootstrapping embed.** One C++ file plus three observer headers, compiled by
  `CB.sh.core` on first use — no CMake or Ninja install in a parent repo.

What CMake + Ninja keeps:

- **Compiler-accurate scanning.** `clang-scan-deps` sees exactly what the compiler sees. CB's
  regex scanner over-approximates `#ifdef` branches, reads only a 1,000-line preamble, and
  refuses two sources with the same basename.
- **A faster no-op** from a persisted graph, and breadth CB does not attempt: install rules,
  packaging, cross-compilation, other compilers, IDE integration.
- **Less code to own.** `tools/cb.c++` is ~5,600 lines plus ~1,400 lines of observer headers,
  verified mainly by a ~60 s smoke suite; the CMake path is one ~190-line file.

A middle path, if scale or scanner accuracy ever becomes the constraint: keep CB's discovery,
cache reasons, and JSONL, and have it emit a `build.ninja` for Ninja to execute.

## Earlier results (August 2026, flags not matched)

On tester v2.2 (`60c5c79`) with CB at `-O0 -g3` and the CMake path still at
`config/compiler.mk`'s `-O0 -g`, two-phase CB measured 0.87× cold, 0.77× test touch, and
0.84× module touch against Ninja on macOS, and 0.92× / 0.79× / 0.84× on Linux. Those leads
largely disappeared once both sides compiled with the same flags; they are kept here for
history, not as a current comparison.

## Class-boundary refactor A/B check

The same Linux x86_64 / Clang 21 process compared the pre-refactor CB (`27f8f8b`) with
the class-based namespaces (`3bddc84`) using `--cb-only --modules=one-phase --jobs=4`.
The order was counterbalanced (old/new/new/old), with two complete runs per revision:
six cold samples, thirty no-op samples, and six samples for each touch scenario.
`CB_STD_CACHE_DIR` remained empty, and Ninja was deliberately not rerun.

| Scenario | Before | Class-based | Change |
|----------|-------:|------------:|-------:|
| Cold full | 9.674 s | 9.692 s | +0.18% |
| No-op | 158.27 ms | 157.20 ms | −0.67% |
| Touch one test TU | 3.541 s | 3.552 s | +0.32% |
| Touch module interface | 7.054 s | 7.056 s | +0.03% |

No scenario shows a measurable regression: bootstrap 95% intervals for the percentage
change all include zero (cold −0.72%…+1.13%, no-op −1.83%…+0.99%, test touch
−0.73%…+1.49%, module touch −0.45%…+0.54%). The class boundaries do not transfer
container ownership: cache and graph inputs are passed by `const&`, while the scanner's
translation-unit vector is return-elided or moved. The no-op row, where compiler work
cannot hide orchestration overhead, is slightly faster within noise.

### Final helper-ownership cleanup

The follow-up compared `20ded84` with the final source/cache helper ownership at `a7cfef4`.
Both binaries ran the same `/workspace` checkout and build tree; this matters because running
one revision from `/tmp` produced a large filesystem-location bias. The order was again
old/new/new/old with `--cb-only --modules=one-phase --jobs=4`: six samples per cold/touch
scenario and ten no-op samples per revision.

| Scenario | Before | Final ownership | Change |
|----------|-------:|----------------:|-------:|
| Cold full | 9.642 s | 9.639 s | −0.03% |
| No-op | 156.9 ms | 156.4 ms | −0.32% |
| Touch one test TU | 3.555 s | 3.511 s | −1.24% |
| Touch module interface | 7.031 s | 7.048 s | +0.24% |

All changes are within ±1.3%, with the no-op path unchanged within sub-millisecond noise.
Moving naming, exclusion and lexical cleaning onto the source classes, and profile/depfile
logic onto the cache classes, therefore adds no measurable orchestration cost. A final accessor
follow-up moves the serialized profile string out of a temporary instead of copying it.
