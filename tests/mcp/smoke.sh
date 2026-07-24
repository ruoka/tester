#!/usr/bin/env bash
# Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
# SPDX-License-Identifier: MIT
# See the LICENSE file in the project root for full license text.
#
# MCP stdio smoke for tools/cb_mcp.py (CB.sh agent bridge).
#
# Usage:
#   ./tests/mcp/smoke.sh [--jsonl]
#
# Requires: python3; prefer a prior `./tools/CB.sh debug build` so cb_test_list is fast.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MCP_PY="${MCP_PY:-${ROOT_DIR}/tools/cb_mcp.py}"
CB_SH="${CB_SH:-${ROOT_DIR}/tools/CB.sh}"

JSONL_MODE=0
FAILURES=0
TESTS_RUN=0

log() { printf '%s\n' "$*" >&2; }
fail() { FAILURES=$((FAILURES + 1)); log "FAIL: $*"; }
jsonl_emit() { [[ "${JSONL_MODE}" -eq 1 ]] || return 0; printf '%s\n' "$1"; }

for arg in "$@"; do
  case "${arg}" in
    --jsonl) JSONL_MODE=1 ;;
    --help|-h)
      echo "usage: smoke.sh [--jsonl]"
      exit 0
      ;;
    *)
      echo "unknown argument: ${arg}" >&2
      exit 2
      ;;
  esac
done

if [[ ! -f "${MCP_PY}" ]]; then
  log "cb_mcp.py not found at ${MCP_PY}"
  exit 1
fi
if [[ ! -f "${CB_SH}" ]]; then
  log "CB.sh not found at ${CB_SH}"
  exit 1
fi

jsonl_emit '{"type":"smoke_start","schema":"mcp-smoke","version":1,"suite":"tester-cb"}'
log "tester CB MCP smoke (root=${ROOT_DIR})"

export JSONL_MODE
export CB_PROJECT_ROOT="${ROOT_DIR}"
export CB_SH
export CB_MCP_DEFAULT_TAGS='[self]'

output="$(
python3 - "${MCP_PY}" <<'PY'
from __future__ import annotations

import json
import os
import subprocess
import sys

MCP_PY = sys.argv[1]
JSONL = os.environ.get("JSONL_MODE", "0") == "1"
failures = 0
checks = 0


def emit(obj: dict) -> None:
    if JSONL:
        print(json.dumps(obj, separators=(",", ":")), flush=True)


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def ok(cond: bool, label: str, detail: str = "") -> None:
    global checks, failures
    checks += 1
    if cond:
        emit({"type": "smoke_assert_passed", "matcher": label})
        return
    failures += 1
    emit({"type": "smoke_assert_failed", "matcher": label, "message": detail or label})
    log(f"FAIL: {label}" + (f" — {detail}" if detail else ""))


def frame(obj: dict) -> bytes:
    data = json.dumps(obj).encode()
    return f"Content-Length: {len(data)}\r\n\r\n".encode() + data


def read_one(buf):
    length = None
    while True:
        line = b""
        while not line.endswith(b"\n"):
            ch = buf.read(1)
            if not ch:
                return None
            line += ch
        if line in (b"\r\n", b"\n"):
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1])
    if length is None:
        return None
    return json.loads(buf.read(length))


proc = subprocess.Popen(
    [sys.executable, MCP_PY],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    env={
        **os.environ,
        "CB_PROJECT_ROOT": os.environ["CB_PROJECT_ROOT"],
        "CB_SH": os.environ["CB_SH"],
        "CB_MCP_DEFAULT_TAGS": "[self]",
    },
)
assert proc.stdin and proc.stdout
nid = 0


def call(method: str, params: dict | None = None) -> dict:
    global nid
    nid += 1
    msg = {"jsonrpc": "2.0", "id": nid, "method": method, "params": params or {}}
    proc.stdin.write(frame(msg))
    proc.stdin.flush()
    reply = read_one(proc.stdout)
    if reply is None:
        raise RuntimeError("MCP closed")
    return reply


call(
    "initialize",
    {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "tester-mcp-smoke", "version": "0"},
    },
)
proc.stdin.write(frame({"jsonrpc": "2.0", "method": "notifications/initialized"}))
proc.stdin.flush()

listed = call("tools/list")
tools = (listed.get("result") or {}).get("tools") or []
names = {t.get("name") for t in tools}
expected = {"cb_list", "cb_build", "cb_test", "cb_test_list", "cb_cache_status"}
ok(len(tools) == 5, "tools_count_5", f"got {len(tools)}")
ok(names == expected, "tools_names", f"got {sorted(names)}")


def tool(name: str, arguments: dict | None = None) -> dict:
    reply = call("tools/call", {"name": name, "arguments": arguments or {}})
    result = reply.get("result") or {}
    if result.get("isError"):
        text = (result.get("content") or [{}])[0].get("text", "error")
        return {"ok": False, "error": text}
    text = (result.get("content") or [{}])[0].get("text", "{}")
    return json.loads(text)


catalogue = tool("cb_test_list", {"tags": "[self]", "jsonl": "failures"})
summary = catalogue.get("summary") or {}
ok(bool(catalogue.get("ok")) or catalogue.get("exit_code") == 0, "cb_test_list_ok", str(catalogue.get("error") or catalogue.get("exit_code")))
ok(
    summary.get("type") == "test_list_summary"
    or summary.get("matched_total") is not None
    or int(catalogue.get("events_count") or 0) > 0,
    "cb_test_list_has_catalogue",
    json.dumps(summary)[:240],
)

inv = tool("cb_list", {"jsonl": "failures"})
ok(
    int(inv.get("events_count") or 0) > 0
    or (inv.get("summary") or {}).get("type") == "list_summary",
    "cb_list_events",
)

cache = tool("cb_cache_status", {"jsonl": "failures"})
ok(cache.get("exit_code") == 0 or cache.get("summary") is not None, "cb_cache_status")

proc.stdin.close()
try:
    proc.wait(timeout=5)
except subprocess.TimeoutExpired:
    proc.kill()
    proc.wait(timeout=3)

print(json.dumps({"type": "smoke_case_stats", "checks": checks, "failures": failures}, separators=(",", ":")), flush=True)
sys.exit(1 if failures else 0)
PY
)" || status=$?
status=${status:-0}

stats_line=""
while IFS= read -r line; do
  [[ -n "${line}" ]] || continue
  if [[ "${line}" == *'"smoke_case_stats"'* ]]; then
    stats_line="${line}"
    continue
  fi
  jsonl_emit "${line}"
done <<<"${output}"

if [[ -n "${stats_line}" ]]; then
  TESTS_RUN="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["checks"])' "${stats_line}")"
  FAILURES="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["failures"])' "${stats_line}")"
elif [[ "${status}" -ne 0 ]]; then
  fail "mcp driver exited ${status} without stats"
fi

passed=$([[ "${FAILURES}" -eq 0 ]] && echo true || echo false)
jsonl_emit "{\"type\":\"smoke_summary\",\"tests_run\":${TESTS_RUN},\"failures\":${FAILURES},\"passed\":${passed}}"
jsonl_emit "{\"type\":\"smoke_end\",\"passed\":${passed},\"suite\":\"tester-cb\"}"

if [[ "${FAILURES}" -ne 0 ]]; then
  log "FAILED: ${FAILURES} assertion(s) (${TESTS_RUN} checks)"
  exit 1
fi
log "OK: all ${TESTS_RUN} checks passed"
