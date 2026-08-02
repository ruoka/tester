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

## macOS / Clang 23 reference

These are the original measurements; they remain useful as a newer-toolchain comparison.

### Setup

| Knob | Value |
|------|--------|
| Host | macOS arm64, 8 jobs |
| Config | Debug (`-O0 -g`) |
| Compiler | Clang 23 trunk from `/usr/local/llvm` |
| CB | `tools/cb` with the same argv/env `CB.sh` would `exec` (absolute `-I`, `--include-examples`, `SDKROOT`, `LDFLAGS`) |
| Ninja | `cmake --build … --verbose` → `ninja -v` (full command lines; output captured to temp files) |
| Scope | Standalone tree, examples included (~36 TUs) |

### Results (mean wall-clock)

| Scenario | CB two-phase | CB one-phase | Ninja `-v` | one-phase / Ninja |
|----------|-------------:|-------------:|-----------:|------------------:|
| Cold full | 10.31 s | 9.54 s | 7.83 s | 1.22× |
| Cold + configure | 10.31 s | 9.54 s | 8.53 s | 1.12× |
| No-op | 469 ms | 385 ms | 56 ms | 6.8× |
| Touch one test TU | 3.09 s | 2.90 s | 2.69 s | 1.08× |
| Touch module interface | 6.62 s | 6.10 s | 5.49 s | 1.11× |

`--modules=one-phase` (single `-c -fmodule-output=` per modular TU) is clearly faster than the default two-phase path on this tree. Compile-heavy gaps to Ninja shrink to roughly **8–22%**; no-op stays dominated by CB’s per-invocation work.

`CB.sh` overhead on a warm no-op is ~20 ms once `tools/cb` is already built — negligible next to CB’s scan/cache check.

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

That differs from the macOS / Clang 23 result, where one-phase won every
compile-heavy scenario. Clang 22+ also writes reduced BMIs by default while this
Clang 21 build does not receive that part of the one-phase benefit. The benchmark
does not isolate reduced BMIs as the sole cause, so module mode should be measured
on the actual deployment toolchain rather than assumed to be universally faster.

The host exposes four physical cores. Raising both builders to `--jobs=8`
oversubscribed it: CB cold and module-touch builds slowed by 4.8% and 7.6%;
Ninja slowed by 13.5% and 18.2%. Four jobs is the useful ceiling on this runner.

## How to read the gap

- **No-op:** Ninja checks a persistent `build.ninja` graph (mtime / dirty edges). CB rediscovers TUs, re-reads `import` lines, and consults its object cache every run. That rediscovery dominates the warm CB invocation (~0.18 s on Linux, ~0.4 s on the macOS snapshot).
- **Cold / touch:** Both spend most of the time in `clang++`. One-phase removes CB’s second modular compile step, but whether that pays off depends on the compiler and rebuild shape; scheduling and process orchestration account for the remaining gap.
- **Ninja `-v` vs quiet:** Full command-line logging did not change wall-clock meaningfully when redirected to files. Quiet Ninja no-op was also ~56 ms.
- **Add/delete sources:** CB always rescans. This repo’s `CMakeLists.txt` uses `GLOB … CONFIGURE_DEPENDS`, so Ninja re-checks globs and CMake regenerates the graph when matching files appear or disappear — no hand `cmake` for ordinary glob hits.

Numbers are single-machine snapshots (2026-08-02), not a CI gate. Re-run the script after toolchain or tree changes.
