# CB vs CMake + Ninja — build-time notes

Wall-clock comparison of **`tools/cb`** (direct, no `CB.sh` bootstrap) against this repo’s **CMake + Ninja** path on **tester v2.2** (`60c5c79` / tag `v2.2.0`: per-TU BMI maps, edge-driven scheduling, shared-std cache). Reproduce with:

```bash
./tools/bench-cb-vs-ninja.sh                      # CB two-phase (default) vs Ninja -v
./tools/bench-cb-vs-ninja.sh --modules=one-phase  # CB one-phase vs Ninja -v
./tools/bench-cb-vs-ninja.sh --cb-only             # CB scenarios only (revision comparisons)
./tools/bench-cb-vs-ninja.sh --quiet-ninja        # Ninja progress only
./tools/bench-cb-vs-ninja.sh --modules=one-phase --jobs=4
./tools/run-linux-bench.sh                       # two-phase in .devcontainer (from macOS host)
```

Scenarios: cold full rebuild (after `clean` / `rm -rf` build tree), no-op rebuild,
touch one test TU, touch a widely imported module interface.
The script exports an empty `CB_STD_CACHE_DIR` by default, so every cold sample includes
the std module compile. Supply a non-empty directory explicitly to measure clean builds
that reuse CB's shared std cache.

Each mode below is a full `./tools/bench-cb-vs-ninja.sh` run: three cold builds, five
no-op builds, and three runs of each touch scenario. The Ninja column beside each CB
mode is from that mode’s complete run.

## macOS / Clang 23

### Setup

| Knob | Value |
|------|--------|
| Host | macOS arm64, 8 jobs |
| Config | Debug (`-O0 -g3`) |
| Compiler | Clang 23 trunk from `/usr/local/llvm` |
| CMake / Ninja | CMake 4.1.2 / Ninja 1.13.2 |
| CB | `tools/cb` with the same argv/env `CB.sh` would `exec` (absolute `-I`, `--include-examples`, `SDKROOT`, `LDFLAGS`), per-TU `-fmodule-file=` maps |
| Ninja | `cmake --build … --verbose` → `ninja -v` (full command lines; output captured to temp files) |
| Scope | Standalone tree, examples included (~36 TUs) |
| Date | 2026-08-04 (two-phase, after rediscovery/scan/analyzer speedups); one-phase column still 2026-08-02 |

### Results (mean wall-clock)

| Scenario | CB two-phase | Ninja (paired) | ratio | CB one-phase | Ninja (paired) | ratio |
|----------|-------------:|---------------:|------:|-------------:|---------------:|------:|
| Cold full | 8.31 s | 9.56 s | 0.87× | 8.03 s | 7.92 s | 1.01× |
| Cold + configure | 8.31 s | 10.41 s | 0.80× | 8.03 s | 8.60 s | 0.93× |
| No-op | 108 ms | 73 ms | 1.49× | 299 ms | 57 ms | 5.22× |
| Touch one test TU | 4.74 s | 6.12 s | 0.77× | 2.86 s | 2.61 s | 1.10× |
| Touch module interface | 6.52 s | 7.74 s | 0.84× | 5.92 s | 5.57 s | 1.06× |

Two-phase CB leads this Ninja pair on cold, both touch scenarios, and sits near parity on
no-op once process startup settles (samples 2–5 are 72–74 ms wall / ~30 ms inside CB; the
108 ms mean includes a 248 ms first-sample wall outlier with the same ~30 ms internal
`duration_ms`). One-phase numbers were not remeasured on 2026-08-04.

`CB.sh` overhead on a warm no-op is ~20 ms once `tools/cb` is already built — most of the
remaining gap to Ninja is that floor plus process startup, not rediscovery.

## Linux / Clang 21 (`.devcontainer`)

Re-measured in the tester [`.devcontainer`](../.devcontainer) image (Debian trixie,
Clang 21 + libc++, CMake from Kitware). From a macOS host with Docker Desktop:

```bash
./tools/run-linux-bench.sh   # builds the image if needed; writes .bench-ab/cb-vs-ninja-bench-linux.*
```

Or the equivalent manual `docker run` (set `CB_BIN` to a container-local path such as
`/tmp/cb-linux` so the Linux ELF is not overwritten by a host `tools/cb` on the bind mount):

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

### Setup

| Knob | Value |
|------|--------|
| Host | Linux aarch64 (Docker Desktop / linuxkit), 8 CPUs available, `--jobs=4` |
| Config | Debug (`-O0 -g3`) |
| Compiler | Debian Clang 21.1.8 (`clang++-21` → `/usr/lib/llvm-21`) |
| CMake / Ninja | CMake 4.1.2 / Ninja 1.12.1 |
| CB | `tools/cb` (or `CB_BIN`), direct, per-TU `-fmodule-file=` project BMI mappings |
| Ninja | `cmake --build … --parallel 4 --verbose` |
| Scope | Standalone tree, examples included (36 TUs) |
| Date | 2026-08-04 (two-phase, after rediscovery/scan/analyzer speedups); one-phase column still 2026-08-02 |

### Results (mean wall-clock)

| Scenario | CB two-phase | Ninja (paired) | ratio | CB one-phase | Ninja (paired) | ratio |
|----------|-------------:|---------------:|------:|-------------:|---------------:|------:|
| Cold full | 11.16 s | 12.08 s | 0.92× | 12.16 s | 12.00 s | 1.01× |
| Cold + configure | 11.16 s | 12.45 s | 0.90× | 12.16 s | 12.35 s | 0.98× |
| No-op | 37 ms | 31 ms | 1.18× | 371 ms | 29 ms | 12.8× |
| Touch one test TU | 3.36 s | 4.23 s | 0.79× | 3.88 s | 4.01 s | 0.97× |
| Touch module interface | 7.97 s | 9.43 s | 0.84× | 9.19 s | 8.44 s | 1.09× |

Two-phase leads verbose Ninja on cold and both touch scenarios (0.92× / 0.79× / 0.84×).
No-op is within ~20% of Ninja (37 ms vs 31 ms); the old ~12× rediscovery gap is gone.
One-phase numbers were not remeasured on 2026-08-04. Clang 21 does not default to the
reduced BMI path that Clang 22+ uses for one-phase, so module mode should be measured on
the deployment toolchain rather than assumed universally faster.

With a machine-local std cache enabled (`CB_STD_CACHE_DIR` non-empty), a clean CB build
can skip recompiling `std` after `clean`; that path is intentionally excluded from the
cold rows above.

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

## How to read the gap

- **No-op:** Ninja checks a persistent `build.ninja` graph (mtime / dirty edges). CB still rediscovers TUs and consults its object cache every run, but after memoizing analyzer decisions, replacing `std::regex` lexical stripping, and cutting redundant filesystem work, that path is no longer the dominant cost — warm no-op is within ~1.2–1.5× of Ninja, and most of the residual is process startup. Watch CB’s own `duration_ms` on `*_detail` rows (~30 ms on macOS) when a wall-clock sample spikes.
- **Cold / touch:** Both spend most of the time in `clang++`. One-phase removes CB’s second modular compile step; two-phase can publish a provider BMI before its object finishes so importers start earlier. On this refresh, two-phase leads the paired verbose Ninja run on cold and both touch scenarios on macOS and Linux.
- **Ninja `-v` vs quiet:** Full command-line logging did not change wall-clock meaningfully when redirected to files. Quiet Ninja no-op was also ~56 ms on macOS.
- **Add/delete sources:** CB always rescans. This repo’s `CMakeLists.txt` uses `GLOB … CONFIGURE_DEPENDS`, so Ninja re-checks globs and CMake regenerates the graph when matching files appear or disappear — no hand `cmake` for ordinary glob hits.

Numbers are single-machine snapshots (macOS and Linux two-phase refreshed 2026-08-04; one-phase columns 2026-08-02), not a CI gate. Re-run the script after toolchain or tree changes.
