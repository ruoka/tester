#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

STAMP=".devcontainer/bootstrap-stamp"
KEY="$(git rev-parse HEAD)"

echo "=== Toolchain ==="
clang++-21 -v
/usr/lib/llvm-21/bin/clang-scan-deps --version
# The two alternative build paths: make (clang-scan-deps ordering) and CMake + Ninja.
make --version | head -1
cmake --version | head -1
ninja --version

if [[ -f /usr/lib/llvm-21/share/libc++/v1/std.cppm ]]; then
    echo "std.cppm: /usr/lib/llvm-21/share/libc++/v1/std.cppm"
else
    echo "WARNING: std.cppm not found at /usr/lib/llvm-21/share/libc++/v1/std.cppm"
    find /usr/lib/llvm-21 -name "std.cppm" 2>/dev/null || true
fi

if [[ -f "$STAMP" && "$(cat "$STAMP")" == "$KEY" ]]; then
    echo "=== Build ==="
    echo "Bootstrap up to date — skipping build (commit unchanged)"
else
    echo "=== Build ==="
    ./tools/CB.sh debug build
    echo "$KEY" > "$STAMP"
fi

echo "Dev container ready."