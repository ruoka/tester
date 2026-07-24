#!/usr/bin/env python3
# Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
# SPDX-License-Identifier: MIT
# See the LICENSE file in the project root for full license text.
"""
MCP stdio bridge for the CB.sh / tester agent workflow (stdlib only).

Exposes structured tools that wrap canonical AGENTS.md commands and return
parsed JSONL summaries from stdout (never treat stderr as structured output).

Environment:
  CB_PROJECT_ROOT         Repo root that owns tools/CB.sh (default: parent of this file)
  CB_SH                   Path to CB.sh (default: $CB_PROJECT_ROOT/tools/CB.sh)
  CB_MCP_DEFAULT_CONFIG   Default build config (default: debug)
  CB_MCP_DEFAULT_TAGS     Default --tags for cb_test (default: [self])
  CB_MCP_TIMEOUT_SEC      Subprocess timeout seconds (default: 600)

Wire: JSON-RPC 2.0 with LSP-style Content-Length framing on stdin/stdout.
Log only on stderr — never print to stdout (corrupts MCP framing).
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable

TOOLS_DIR = Path(__file__).resolve().parent
DEFAULT_ROOT = TOOLS_DIR.parent

PROJECT_ROOT = Path(
    os.environ.get("CB_PROJECT_ROOT") or str(DEFAULT_ROOT)
).resolve()
CB_SH = Path(os.environ.get("CB_SH") or str(PROJECT_ROOT / "tools" / "CB.sh")).resolve()
DEFAULT_CONFIG = (os.environ.get("CB_MCP_DEFAULT_CONFIG") or "debug").strip() or "debug"
DEFAULT_TAGS = (os.environ.get("CB_MCP_DEFAULT_TAGS") or "[self]").strip()
TIMEOUT_SEC = int(os.environ.get("CB_MCP_TIMEOUT_SEC") or "600")


def _log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def _parse_jsonl(stdout: str) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict) and obj.get("schema") == "tester-jsonl":
            events.append(obj)
    return events


def _pick_summary(events: list[dict[str, Any]]) -> dict[str, Any] | None:
    # Prefer test outcome events over build_end: a green build_end is emitted
    # before test_runner runs, and must not be treated as the command result when
    # the runner crashes without a `summary` (only a `crash` + CB `test_end`).
    preferred = (
        "summary",
        "run_end",
        "test_end",
        "build_end",
        "list_summary",
        "test_list_summary",
        "cache_status",
    )
    for kind in preferred:
        for event in reversed(events):
            if event.get("type") == kind:
                return event
    return events[-1] if events else None


def _outcome_ok(summary: dict[str, Any] | None, returncode: int, events: list[dict[str, Any]]) -> bool:
    """Derive tool ok from JSONL + exit status (never mask a failed exit with build_end.ok)."""
    if any(event.get("type") == "crash" for event in events):
        return False

    ok = returncode == 0
    if isinstance(summary, dict) and "passed" in summary:
        # Authoritative test aggregate — may disagree with process exit quirks.
        return bool(summary.get("passed"))
    if isinstance(summary, dict) and "ok" in summary:
        # build_end/test_end/cache ok must not report success when the process failed
        # (e.g. test_runner SIGSEGV after a successful build_end).
        return bool(summary.get("ok")) and ok
    return ok


def _run_cb(argv_tail: list[str], *, timeout: int | None = None) -> dict[str, Any]:
    if not CB_SH.is_file():
        return {
            "ok": False,
            "exit_code": 127,
            "argv": [],
            "error": f"CB.sh not found at {CB_SH}",
            "hint": "Set CB_SH or CB_PROJECT_ROOT to a repo with tools/CB.sh",
        }
    argv = [str(CB_SH), *argv_tail]
    try:
        proc = subprocess.run(
            argv,
            cwd=str(PROJECT_ROOT),
            capture_output=True,
            text=True,
            timeout=timeout or TIMEOUT_SEC,
            env={**os.environ, "CB_PROJECT_ROOT": str(PROJECT_ROOT)},
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        events = _parse_jsonl(stdout)
        return {
            "ok": False,
            "exit_code": 124,
            "argv": argv,
            "error": f"timeout after {timeout or TIMEOUT_SEC}s",
            "summary": _pick_summary(events),
            "events_count": len(events),
            "stderr_tail": stderr[-4000:],
        }

    events = _parse_jsonl(proc.stdout or "")
    summary = _pick_summary(events)
    ok = _outcome_ok(summary, proc.returncode, events)

    return {
        "ok": ok,
        "exit_code": proc.returncode,
        "argv": argv,
        "cwd": str(PROJECT_ROOT),
        "summary": summary,
        "events_count": len(events),
        "failed_test_ids": (summary or {}).get("failed_test_ids")
        if isinstance(summary, dict)
        else None,
        "first_failure": (summary or {}).get("first_failure")
        if isinstance(summary, dict)
        else None,
        "stderr_tail": (proc.stderr or "")[-4000:],
    }


def _config(args: dict[str, Any]) -> str:
    return str(args.get("config") or DEFAULT_CONFIG).strip() or DEFAULT_CONFIG


def _jsonl_mode(args: dict[str, Any]) -> str:
    mode = str(args.get("jsonl") or "failures").strip() or "failures"
    if mode not in ("failures", "summary", "trace"):
        raise ValueError("jsonl must be failures, summary, or trace")
    return mode


def _text(result: dict[str, Any]) -> dict[str, Any]:
    return {"content": [{"type": "text", "text": json.dumps(result, indent=2, ensure_ascii=False)}]}


def tool_cb_list(args: dict[str, Any]) -> dict[str, Any]:
    config = _config(args)
    jsonl = _jsonl_mode(args)
    return _text(_run_cb([config, "list", f"--jsonl={jsonl}"]))


def tool_cb_build(args: dict[str, Any]) -> dict[str, Any]:
    config = _config(args)
    jsonl = _jsonl_mode(args)
    argv = [config, "build", f"--jsonl={jsonl}"]
    if args.get("include_examples"):
        argv.insert(1, "--include-examples")
    if args.get("build_tests"):
        argv.append("--build-tests")
    return _text(_run_cb(argv))


def tool_cb_test(args: dict[str, Any]) -> dict[str, Any]:
    config = _config(args)
    jsonl = _jsonl_mode(args)
    tags = str(args.get("tags") if args.get("tags") is not None else DEFAULT_TAGS).strip()
    filt = str(args.get("filter") or "").strip()
    argv = [config, "test", f"--jsonl={jsonl}"]
    if tags:
        # argv list — no shell globbing; pass [self] literally (not \[self\]).
        argv.append(f"--tags={tags}")
    if filt:
        # Substring filter as a trailing positional (CB forwards after test).
        argv.append(filt)
    return _text(_run_cb(argv))


def tool_cb_test_list(args: dict[str, Any]) -> dict[str, Any]:
    config = _config(args)
    jsonl = _jsonl_mode(args)
    tags = str(args.get("tags") or "").strip()
    argv = [config, "test", "--list", f"--jsonl={jsonl}"]
    if tags:
        argv.append(f"--tags={tags}")
    return _text(_run_cb(argv))


def tool_cb_cache_status(args: dict[str, Any]) -> dict[str, Any]:
    config = _config(args)
    jsonl = _jsonl_mode(args)
    return _text(_run_cb([config, "cache", "status", f"--jsonl={jsonl}"]))


TOOLS: dict[str, tuple[dict[str, Any], Callable[[dict[str, Any]], dict[str, Any]]]] = {
    "cb_list": (
        {
            "name": "cb_list",
            "description": (
                "List translation units via ./tools/CB.sh <config> list --jsonl=…. "
                "Returns parsed JSONL (prefer list_summary)."
            ),
            "inputSchema": {
                "type": "object",
                "properties": {
                    "config": {
                        "type": "string",
                        "description": "Build config (default debug)",
                        "default": "debug",
                    },
                    "jsonl": {
                        "type": "string",
                        "enum": ["failures", "summary", "trace"],
                        "default": "failures",
                    },
                },
                "additionalProperties": False,
            },
        },
        tool_cb_list,
    ),
    "cb_build": (
        {
            "name": "cb_build",
            "description": (
                "Build via ./tools/CB.sh <config> build --jsonl=…. "
                "Returns parsed build_end / compile failures from stdout JSONL."
            ),
            "inputSchema": {
                "type": "object",
                "properties": {
                    "config": {"type": "string", "default": "debug"},
                    "jsonl": {
                        "type": "string",
                        "enum": ["failures", "summary", "trace"],
                        "default": "failures",
                    },
                    "include_examples": {"type": "boolean", "default": False},
                    "build_tests": {
                        "type": "boolean",
                        "default": False,
                        "description": "For release builds: compile tests without running",
                    },
                },
                "additionalProperties": False,
            },
        },
        tool_cb_build,
    ),
    "cb_test": (
        {
            "name": "cb_test",
            "description": (
                "Run tests via ./tools/CB.sh <config> test --jsonl=… [--tags=…] [filter]. "
                "Default tags are [self] (standalone tester). In parent repos set "
                "CB_MCP_DEFAULT_TAGS or pass tags=[yardb] / [fixer] / etc. "
                "Returns summary.passed and first_failure when present."
            ),
            "inputSchema": {
                "type": "object",
                "properties": {
                    "config": {"type": "string", "default": "debug"},
                    "jsonl": {
                        "type": "string",
                        "enum": ["failures", "summary", "trace"],
                        "default": "failures",
                    },
                    "tags": {
                        "type": "string",
                        "description": "Tag filter, e.g. [self] or [yardb]",
                        "default": "[self]",
                    },
                    "filter": {
                        "type": "string",
                        "description": "Optional test-name substring filter",
                        "default": "",
                    },
                },
                "additionalProperties": False,
            },
        },
        tool_cb_test,
    ),
    "cb_test_list": (
        {
            "name": "cb_test_list",
            "description": (
                "Catalogue tests via ./tools/CB.sh <config> test --list --jsonl=…. "
                "Returns test_list_summary (and registered tests in events)."
            ),
            "inputSchema": {
                "type": "object",
                "properties": {
                    "config": {"type": "string", "default": "debug"},
                    "jsonl": {
                        "type": "string",
                        "enum": ["failures", "summary", "trace"],
                        "default": "failures",
                    },
                    "tags": {
                        "type": "string",
                        "description": "Optional tag filter",
                        "default": "",
                    },
                },
                "additionalProperties": False,
            },
        },
        tool_cb_test_list,
    ),
    "cb_cache_status": (
        {
            "name": "cb_cache_status",
            "description": "Object-cache profile status via ./tools/CB.sh <config> cache status --jsonl=…",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "config": {"type": "string", "default": "debug"},
                    "jsonl": {
                        "type": "string",
                        "enum": ["failures", "summary", "trace"],
                        "default": "failures",
                    },
                },
                "additionalProperties": False,
            },
        },
        tool_cb_cache_status,
    ),
}


def _read_message() -> dict[str, Any] | None:
    headers: dict[str, str] = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        key, _, value = line.decode("utf-8").partition(":")
        headers[key.strip().lower()] = value.strip()
    length = int(headers.get("content-length", "0"))
    if length <= 0:
        return None
    body = sys.stdin.buffer.read(length)
    if len(body) < length:
        return None
    return json.loads(body.decode("utf-8"))


def _write_message(message: dict[str, Any]) -> None:
    data = json.dumps(message, ensure_ascii=False).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def _handle(msg: dict[str, Any]) -> dict[str, Any] | None:
    method = msg.get("method")
    msg_id = msg.get("id")
    params = msg.get("params") or {}

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "tester-cb", "version": "1.0.0"},
            },
        }

    if method == "notifications/initialized":
        return None

    if method == "ping":
        return {"jsonrpc": "2.0", "id": msg_id, "result": {}}

    if method == "tools/list":
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": {"tools": [spec for spec, _ in TOOLS.values()]},
        }

    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        entry = TOOLS.get(name)
        if entry is None:
            return {
                "jsonrpc": "2.0",
                "id": msg_id,
                "error": {"code": -32601, "message": f"Unknown tool: {name}"},
            }
        try:
            result = entry[1](args)
            return {"jsonrpc": "2.0", "id": msg_id, "result": result}
        except Exception as exc:  # noqa: BLE001 — surface to MCP client
            return {
                "jsonrpc": "2.0",
                "id": msg_id,
                "result": {
                    "content": [{"type": "text", "text": f"error: {exc}"}],
                    "isError": True,
                },
            }

    if msg_id is None:
        return None

    return {
        "jsonrpc": "2.0",
        "id": msg_id,
        "error": {"code": -32601, "message": f"Method not found: {method}"},
    }


def main() -> None:
    _log(f"tester CB MCP (project={PROJECT_ROOT}, cb={CB_SH})")
    while True:
        msg = _read_message()
        if msg is None:
            break
        reply = _handle(msg)
        if reply is not None:
            _write_message(reply)


if __name__ == "__main__":
    main()
