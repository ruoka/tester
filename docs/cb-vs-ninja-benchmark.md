# CB vs CMake + Ninja — build-time notes

Wall-clock comparison of **`tools/cb`** (direct, no `CB.sh` bootstrap) against this repo’s **CMake + Ninja** path. Reproduce with:

```bash
./tools/bench-cb-vs-ninja.sh                      # CB two-phase (default) vs Ninja -v
./tools/bench-cb-vs-ninja.sh --modules=one-phase  # CB one-phase vs Ninja -v
./tools/bench-cb-vs-ninja.sh --quiet-ninja        # Ninja progress only
```

## Setup (measured)

| Knob | Value |
|------|--------|
| Host | macOS arm64, 8 jobs |
| Config | Debug (`-O0 -g`) |
| Compiler | Clang 23 trunk from `/usr/local/llvm` |
| CB | `tools/cb` with the same argv/env `CB.sh` would `exec` (absolute `-I`, `--include-examples`, `SDKROOT`, `LDFLAGS`) |
| Ninja | `cmake --build … --verbose` → `ninja -v` (full command lines; output captured to temp files) |
| Scope | Standalone tree, examples included (~36 TUs) |

Scenarios: cold full rebuild (after `clean` / `rm -rf` build tree), no-op rebuild, touch one test TU, touch a widely imported module interface.

## Results (mean wall-clock)

| Scenario | CB two-phase | CB one-phase | Ninja `-v` | one-phase / Ninja |
|----------|-------------:|-------------:|-----------:|------------------:|
| Cold full | 10.31 s | 9.54 s | 7.83 s | 1.22× |
| Cold + configure | 10.31 s | 9.54 s | 8.53 s | 1.12× |
| No-op | 469 ms | 385 ms | 56 ms | 6.8× |
| Touch one test TU | 3.09 s | 2.90 s | 2.69 s | 1.08× |
| Touch module interface | 6.62 s | 6.10 s | 5.49 s | 1.11× |

`--modules=one-phase` (single `-c -fmodule-output=` per modular TU) is clearly faster than the default two-phase path on this tree. Compile-heavy gaps to Ninja shrink to roughly **8–22%**; no-op stays dominated by CB’s per-invocation work.

`CB.sh` overhead on a warm no-op is ~20 ms once `tools/cb` is already built — negligible next to CB’s scan/cache check.

## How to read the gap

- **No-op:** Ninja checks a persistent `build.ninja` graph (mtime / dirty edges). CB rediscovers TUs, re-reads `import` lines, and consults its object cache every run. That rediscovery is most of the ~0.4 s.
- **Cold / touch:** Both spend most of the time in `clang++`. One-phase removes CB’s second modular compile step; remaining difference is scheduling and process orchestration, not a different compiler.
- **Ninja `-v` vs quiet:** Full command-line logging did not change wall-clock meaningfully when redirected to files. Quiet Ninja no-op was also ~56 ms.
- **Add/delete sources:** CB always rescans. This repo’s `CMakeLists.txt` uses `GLOB … CONFIGURE_DEPENDS`, so Ninja re-checks globs and CMake regenerates the graph when matching files appear or disappear — no hand `cmake` for ordinary glob hits.

Numbers are a single-machine snapshot (2026-08-02), not a CI gate. Re-run the script after toolchain or tree changes.
