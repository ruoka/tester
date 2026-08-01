#!/usr/bin/env bash
# Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
# SPDX-License-Identifier: MIT
# See the LICENSE file in the project root for full license text.
#
# Regression: Make DEBUG=1 must not pass -fno-pie/-no-pie on Darwin (arm64 rejects
# both). Linux keeps them for resolvable backtraces — same gate as CMakeLists.txt.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "${ROOT}"

fail() { echo "✗ $*" >&2; exit 1; }
pass() { echo "✓ $*"; }

# Structural: assignments of -fno-pie/-no-pie in the DEBUG block must sit under the
# Linux uname guard (comments above the gate may still name the flags).
python3 - <<'PY' || fail "compiler.mk DEBUG pie flags not Linux-gated"
from pathlib import Path
import re
text = Path("config/compiler.mk").read_text()
start = text.index("# Debug override")
end = text.index("ifeq ($(STATIC),1)", start)
block = text[start:end]
linux = block.index("ifeq ($(shell uname -s),Linux)")
assigns = [
    m.start()
    for m in re.finditer(r"^override \w+ \+= .*-(?:fno-pie|no-pie)", block, re.M)
]
if not assigns:
    raise SystemExit("DEBUG block missing override assigns for -fno-pie/-no-pie")
if any(pos < linux for pos in assigns):
    raise SystemExit("pie override assign appears before the Linux guard")
print("structural Linux gate ok")
PY
pass "compiler.mk gates DEBUG -fno-pie/-no-pie under Linux"

# Live dump on this host (shim clang-scan-deps so the include succeeds in thin envs).
shim="$(mktemp -d)"
trap 'rm -rf "${shim}"' EXIT
printf '%s\n' '#!/bin/sh' 'exit 0' > "${shim}/clang-scan-deps-21"
chmod +x "${shim}/clang-scan-deps-21"

dump_mk="$(mktemp)"
trap 'rm -rf "${shim}" "${dump_mk}"' EXIT
cat > "${dump_mk}" <<'EOF'
ifeq ($(MAKELEVEL),0)
include config/compiler.mk
endif
all:; @echo CXXFLAGS=$(CXXFLAGS); echo LDFLAGS=$(LDFLAGS)
EOF

flags="$(PATH="${shim}:${PATH}" make -f "${dump_mk}" DEBUG=1)"
echo "${flags}"

case "$(uname -s)" in
  Linux)
    grep -q -- '-fno-pie' <<<"${flags}" || fail "Linux DEBUG CXXFLAGS missing -fno-pie"
    grep -q -- '-no-pie' <<<"${flags}" || fail "Linux DEBUG LDFLAGS missing -no-pie"
    pass "Linux DEBUG keeps -fno-pie/-no-pie"
    ;;
  Darwin)
    grep -q -- '-fno-pie' <<<"${flags}" && fail "Darwin DEBUG must not pass -fno-pie"
    grep -q -- '-no-pie' <<<"${flags}" && fail "Darwin DEBUG must not pass -no-pie"
    grep -q -- '-rdynamic' <<<"${flags}" || fail "Darwin DEBUG should keep -rdynamic"
    pass "Darwin DEBUG omits -fno-pie/-no-pie"
    ;;
  *)
    pass "skipped live pie assert on $(uname -s)"
    ;;
esac

grep -q -- '-O0' <<<"${flags}" || fail "DEBUG must lower optimization to -O0"
pass "DEBUG sets -O0"
