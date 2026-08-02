# CB vs CMake + Ninja — build-time notes

Wall-clock comparison of **`tools/cb`** (direct, no `CB.sh` bootstrap) against this repo’s **CMake + Ninja** path. Reproduce with:

```bash
./tools/bench-cb-vs-ninja.sh                      # CB two-phase (default) vs Ninja -v
./tools/bench-cb-vs-ninja.sh --modules=one-phase  # CB one-phase vs Ninja -v
./tools/bench-cb-vs-ninja.sh --quiet-ninja        # Ninja progress only
./tools/bench-cb-vs-ninja.sh --modules=one-phase --jobs=4
```

Scenarios: cold full rebuild (after `clean` / `rm -rf` build tree), no-op rebuild,
touch one test TU, touch a widely imported module interface.
The script exports an empty `CB_STD_CACHE_DIR` by default, so every cold sample includes
the std module compile. Supply a non-empty directory explicitly to measure clean builds
that reuse CB's shared std cache.

## macOS / Clang 23 reference

Re-measured on `b4cc860` (tester tip with per-TU BMI maps, edge scheduler, and
shared-std cache support; cold rows still disable the shared std cache via the
script default). Each mode is a full `./tools/bench-cb-vs-ninja.sh` run: three
cold builds, five no-op builds, and three runs of each touch scenario. The Ninja
column beside each CB mode is from that mode’s complete run.

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
| Date | 2026-08-02 |

### Results (mean wall-clock)

| Scenario | CB two-phase | Ninja (paired) | ratio | CB one-phase | Ninja (paired) | ratio |
|----------|-------------:|---------------:|------:|-------------:|---------------:|------:|
| Cold full | 8.17 s | 8.21 s | 1.00× | 8.03 s | 7.92 s | 1.01× |
| Cold + configure | 8.17 s | 8.95 s | 0.91× | 8.03 s | 8.60 s | 0.93× |
| No-op | 391 ms | 56 ms | 6.93× | 299 ms | 57 ms | 5.22× |
| Touch one test TU | 2.91 s | 2.63 s | 1.10× | 2.86 s | 2.61 s | 1.10× |
| Touch module interface | 5.36 s | 5.50 s | 0.97× | 5.92 s | 5.57 s | 1.06× |

Cold builds are now essentially tied with verbose Ninja (~1.00–1.01×). Two-phase
wins the module-interface touch (0.97×) via early BMI publication; one-phase is
slightly faster on cold/no-op/one-TU but slower on that module-touch sample.
No-op remains dominated by CB’s per-invocation rediscovery (~0.3–0.4 s vs ~56 ms).

`CB.sh` overhead on a warm no-op is ~20 ms once `tools/cb` is already built —
negligible next to CB’s scan/cache check.

## Linux / Clang 21

Measured on the per-TU project-BMI mapping branch. Each full-script result is the
mean of three cold builds, five no-op builds, and three runs of each touch scenario.
The Ninja column beside each CB mode comes from that mode's complete benchmark run.

### Setup

| Knob | Value |
|------|--------|
| Host | Linux x86_64, 4 cores, 4 jobs |
| Config | Debug (`-O0 -g3`) |
| Compiler | Ubuntu Clang 21.1.8 from `/usr/lib/llvm-21` |
| CMake / Ninja | CMake 4.0.3 / Ninja 1.11.1 |
| CB | `tools/cb`, direct, per-TU `-fmodule-file=` project BMI mappings |
| Ninja | `cmake --build … --parallel 4 --verbose` |
| Scope | Standalone tree, examples included (36 TUs) |

### Results (mean wall-clock)

| Scenario | CB two-phase | Ninja (paired) | ratio | CB one-phase | Ninja (paired) | ratio |
|----------|-------------:|---------------:|------:|-------------:|---------------:|------:|
| Cold full | 12.24 s | 9.27 s | 1.32× | 13.00 s | 9.32 s | 1.40× |
| Cold + configure | 12.24 s | 9.52 s | 1.29× | 13.00 s | 9.57 s | 1.36× |
| No-op | 185 ms | 22 ms | 8.32× | 184 ms | 22 ms | 8.30× |
| Touch one test TU | 3.59 s | 3.66 s | 0.98× | 3.56 s | 3.61 s | 0.99× |
| Touch module interface | 7.39 s | 6.55 s | 1.13× | 7.05 s | 6.54 s | 1.08× |

One-phase was **4.6% faster** for the module-touch rebuild, but **6.2% slower**
for the cold build; no-op and the one-TU rebuild were unchanged. Alternating four
cold runs of each mode to remove run-order bias confirmed the cold result:
11.95 s two-phase versus 12.54 s one-phase (**5.0% slower**).

On the current macOS / Clang 23 snapshot, one-phase is slightly faster on cold and
no-op, but two-phase wins the module-interface touch — the opposite of a blanket
“one-phase always wins” claim. Clang 22+ also writes reduced BMIs by default while
this Clang 21 build does not receive that part of the one-phase benefit. The
benchmark does not isolate reduced BMIs as the sole cause, so module mode should
be measured on the actual deployment toolchain rather than assumed universally
faster.

The host exposes four physical cores. Raising both builders to `--jobs=8`
oversubscribed it: CB cold and module-touch builds slowed by 4.8% and 7.6%;
Ninja slowed by 13.5% and 18.2%. Four jobs is the useful ceiling on this runner.

### Edge scheduler and shared-std follow-up

The scheduling/cache branch was compared directly with its directory-pruning parent
(`d37ccc3`) on the same Linux / Clang 21 host, still at four jobs. These shorter
directional runs use two cold builds, three no-op builds, and two touch builds per
mode; the shared-std row was separately seeded before both clean-build samples.

| Scenario | two-phase before | two-phase after | change | one-phase before | one-phase after | change |
|----------|-----------------:|----------------:|-------:|-----------------:|----------------:|-------:|
| Cold full, std cache disabled | 11.70 s | 8.83 s | −24.5% | 12.16 s | 9.75 s | −19.8% |
| No-op | 184 ms | 158 ms | −14.1% | 183 ms | 161 ms | −12.0% |
| Touch one test TU | 3.57 s | 3.57 s | −0.1% | 3.63 s | 3.63 s | −0.1% |
| Touch module interface | 7.16 s | 6.16 s | −14.0% | 6.99 s | 7.16 s | +2.5% |

With the machine-local std cache enabled, a one-phase clean build fell again from
9.75 s to **7.90 s**. That is 0.84× the paired Ninja cold build (9.37 s), while the
strictly cold two-phase scheduler result was already 0.94× its paired Ninja build.
The cache is not included in the ordinary cold rows: `bench-cb-vs-ninja.sh` disables
it by default.

The main two-phase gain is the intended one: importers are released after provider
precompile rather than after provider object generation and a whole dependency-level
barrier. One-phase has no intermediate BMI publication point, but still benefits on
cold builds from edge readiness, bounded workers, and overlapping source discovery
with std work. Its module-touch result did not improve in this small sample, so the
two-phase touch result is the evidence for early BMI publication rather than a claim
that every rebuild shape speeds up.

## How to read the gap

- **No-op:** Ninja checks a persistent `build.ninja` graph (mtime / dirty edges). CB rediscovers TUs, re-reads `import` lines, and consults its object cache every run. That rediscovery dominates the warm CB invocation (~0.16 s on the improved Linux build, ~0.3–0.4 s on the current macOS snapshot).
- **Cold / touch:** Both spend most of the time in `clang++`. One-phase removes CB’s second modular compile step, while two-phase can overlap a provider object with its importers. On current macOS / Clang 23, cold builds are parity with Ninja; module-touch still favours two-phase.
- **Ninja `-v` vs quiet:** Full command-line logging did not change wall-clock meaningfully when redirected to files. Quiet Ninja no-op was also ~56 ms.
- **Add/delete sources:** CB always rescans. This repo’s `CMakeLists.txt` uses `GLOB … CONFIGURE_DEPENDS`, so Ninja re-checks globs and CMake regenerates the graph when matching files appear or disappear — no hand `cmake` for ordinary glob hits.

Numbers are single-machine snapshots (2026-08-02), not a CI gate. Re-run the script after toolchain or tree changes.
