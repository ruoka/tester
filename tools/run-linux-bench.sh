#!/usr/bin/env bash
# Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
# SPDX-License-Identifier: MIT
# See the LICENSE file in the project root for full license text.
#
# Run the CB vs Ninja benchmark inside the tester .devcontainer image.
# Intended for a host terminal where Docker Desktop is reachable (Cursor's
# agent sandbox cannot connect to docker.sock — EPERM).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p .bench-ab

DOCKER_BIN="${DOCKER_BIN:-/Applications/Docker.app/Contents/Resources/bin/docker}"
if ! command -v "$DOCKER_BIN" >/dev/null 2>&1; then
  DOCKER_BIN=docker
fi

DOCKER=("$DOCKER_BIN" --context desktop-linux)
"${DOCKER[@]}" info >/dev/null

if ! "${DOCKER[@]}" image inspect tester-dev-trixie:latest >/dev/null 2>&1; then
  "${DOCKER[@]}" build -t tester-dev-trixie:latest -f .devcontainer/Dockerfile .devcontainer
fi

# CB_BIN points at a container-local path so CB.sh writes the Linux ELF there and
# leaves the bind-mounted host Mach-O at tools/cb alone. Do not copy tools/cb onto
# CB_BIN — that replaces the ELF with the host binary and yields Exec format error.
"${DOCKER[@]}" run --rm -v "$ROOT:/work" -w /work \
  -e CC=clang-21 -e CXX=clang++-21 \
  -e LLVM_PREFIX=/usr/lib/llvm-21 \
  -e CXX_COMPILER=/usr/bin/clang++-21 \
  -e STD_CPPM=/usr/lib/llvm-21/share/libc++/v1/std.cppm \
  -e LDFLAGS='-Wl,--push-state,--no-as-needed -lc++ -lc++abi -lc++experimental -Wl,--pop-state -pthread -ldl' \
  -e CB_BIN=/tmp/cb-linux \
  tester-dev-trixie:latest \
  bash -lc 'set -euo pipefail
    ./tools/CB.sh debug build --jsonl=failures >/dev/null
    file /tmp/cb-linux | grep -q ELF
    ./tools/bench-cb-vs-ninja.sh --jobs=4 --results=/tmp/cb-vs-ninja-bench-linux.jsonl | tee /work/.bench-ab/cb-vs-ninja-bench-linux.log
    cp /tmp/cb-vs-ninja-bench-linux.jsonl /work/.bench-ab/cb-vs-ninja-bench-linux.jsonl
  '

echo "Linux results: .bench-ab/cb-vs-ninja-bench-linux.jsonl"
echo "Linux log:     .bench-ab/cb-vs-ninja-bench-linux.log"
