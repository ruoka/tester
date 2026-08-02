# CB vs CMake + Ninja — build-time notes

Wall-clock comparison of **`tools/cb`** (direct, no `CB.sh` bootstrap) against this repo’s **CMake + Ninja** path on **tester v2.2** (`60c5c79` / tag `v2.2.0`: per-TU BMI maps, edge-driven scheduling, shared-std cache). Reproduce with:

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
| Date | 2026-08-02 |

### Results (mean wall-clock)

| Scenario | CB two-phase | Ninja (paired) | ratio | CB one-phase | Ninja (paired) | ratio |
|----------|-------------:|---------------:|------:|-------------:|---------------:|------:|
| Cold full | 8.17 s | 8.21 s | 1.00× | 8.03 s | 7.92 s | 1.01× |
| Cold + configure | 8.17 s | 8.95 s | 0.91× | 8.03 s | 8.60 s | 0.93× |
| No-op | 391 ms | 56 ms | 6.93× | 299 ms | 57 ms | 5.22× |
| Touch one test TU | 2.91 s | 2.63 s | 1.10× | 2.86 s | 2.61 s | 1.10× |
| Touch module interface | 5.36 s | 5.50 s | 0.97× | 5.92 s | 5.57 s | 1.06× |

Cold builds are essentially tied with verbose Ninja (~1.00–1.01×). Two-phase wins the
module-interface touch (0.97×) via early BMI publication; one-phase is slightly faster
on cold/no-op/one-TU but slower on that module-touch sample. No-op remains dominated by
CB’s per-invocation rediscovery (~0.3–0.4 s vs ~56 ms).

`CB.sh` overhead on a warm no-op is ~20 ms once `tools/cb` is already built —
negligible next to CB’s scan/cache check.

## Linux / Clang 21 (`.devcontainer`)

Re-measured in the tester [`.devcontainer`](../.devcontainer) image (Debian trixie,
Clang 21 + libc++, CMake from Kitware). Run from a sibling checkout:

```bash
docker build -t tester-dev-trixie:latest -f .devcontainer/Dockerfile .devcontainer
docker run --rm -v "$PWD:/work" -w /work \
  -e CC=clang-21 -e CXX=clang++-21 \
  -e LLVM_PREFIX=/usr/lib/llvm-21 \
  -e CXX_COMPILER=/usr/bin/clang++-21 \
  -e STD_CPPM=/usr/lib/llvm-21/share/libc++/v1/std.cppm \
  -e LDFLAGS='-Wl,--push-state,--no-as-needed -lc++ -lc++abi -lc++experimental -Wl,--pop-state -pthread -ldl' \
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
| CB | `tools/cb`, direct, per-TU `-fmodule-file=` project BMI mappings |
| Ninja | `cmake --build … --parallel 4 --verbose` |
| Scope | Standalone tree, examples included (36 TUs) |
| Date | 2026-08-02 |

### Results (mean wall-clock)

| Scenario | CB two-phase | Ninja (paired) | ratio | CB one-phase | Ninja (paired) | ratio |
|----------|-------------:|---------------:|------:|-------------:|---------------:|------:|
| Cold full | 10.52 s | 11.63 s | 0.90× | 12.16 s | 12.00 s | 1.01× |
| Cold + configure | 10.52 s | 11.98 s | 0.88× | 12.16 s | 12.35 s | 0.98× |
| No-op | 377 ms | 28 ms | 13.3× | 371 ms | 29 ms | 12.8× |
| Touch one test TU | 3.81 s | 3.84 s | 0.99× | 3.88 s | 4.01 s | 0.97× |
| Touch module interface | 7.68 s | 8.42 s | 0.91× | 9.19 s | 8.44 s | 1.09× |

Two-phase is ahead of verbose Ninja on cold and module-touch (0.90× / 0.91×); one-phase
is roughly tied on cold and one-TU but slower on the module-interface rebuild. No-op is
still rediscovery-bound (~0.37 s vs ~28 ms). Clang 21 does not default to the reduced
BMI path that Clang 22+ uses for one-phase, so module mode should be measured on the
deployment toolchain rather than assumed universally faster.

With a machine-local std cache enabled (`CB_STD_CACHE_DIR` non-empty), a clean CB build
can skip recompiling `std` after `clean`; that path is intentionally excluded from the
cold rows above.

## How to read the gap

- **No-op:** Ninja checks a persistent `build.ninja` graph (mtime / dirty edges). CB rediscovers TUs, re-reads `import` lines, and consults its object cache every run. That rediscovery dominates the warm CB invocation (~0.37 s on this Linux snapshot, ~0.3–0.4 s on macOS).
- **Cold / touch:** Both spend most of the time in `clang++`. One-phase removes CB’s second modular compile step; two-phase can publish a provider BMI before its object finishes so importers start earlier. On v2.2, two-phase is the stronger cold/module-touch shape on Linux Clang 21; macOS Clang 23 is near parity either way, with two-phase still favoured on module-touch.
- **Ninja `-v` vs quiet:** Full command-line logging did not change wall-clock meaningfully when redirected to files. Quiet Ninja no-op was also ~56 ms on macOS.
- **Add/delete sources:** CB always rescans. This repo’s `CMakeLists.txt` uses `GLOB … CONFIGURE_DEPENDS`, so Ninja re-checks globs and CMake regenerates the graph when matching files appear or disappear — no hand `cmake` for ordinary glob hits.

Numbers are single-machine snapshots (2026-08-02), not a CI gate. Re-run the script after toolchain or tree changes.
