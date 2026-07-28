#!/usr/bin/env python3
# Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
# SPDX-License-Identifier: MIT
# See the LICENSE file in the project root for full license text.
"""
Validate that tester/CB JSONL on stdout is parseable and structurally correct.

The C++ self tests can check that escape() produces valid UTF-8, but only an
external consumer can prove the emitted *stream* is loadable the way an agent or
CI job would load it. This runs the canonical commands (including a probe that
deliberately emits non-UTF-8 assertion data) and asserts, for every line:

  - the bytes decode as UTF-8
  - json.loads accepts the line
  - the required envelope fields are present
  - the per-type required fields are present
  - the line validates against docs/jsonl-schema.json (when jsonschema is
    installed; skipped with a notice otherwise, so CI works without it)

Usage:
  ./tests/jsonl/validate.py [--cb PATH] [--jsonl] [--require-schema]

Exit status is 0 only when every line of every command validates.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "docs" / "jsonl-schema.json"

ENVELOPE_FIELDS = ("type", "schema", "version", "pid", "ts_unix_ms")

# Fields an agent is documented to rely on, per event type. Absent types are only
# checked for the envelope.
REQUIRED_FIELDS = {
    "summary": ("tests_ok", "tests_total", "assertions_ok", "assertions_total",
                "passed", "failed_test_ids"),
    "run_start": ("mode", "tags", "cwd"),
    "run_end": ("mode", "passed", "duration_ms", "failed_test_ids"),
    "test": ("id", "name", "file", "line", "success", "assertions_ok", "assertions_total"),
    "assertion_failed": ("matcher", "actual", "expected", "file", "line", "column"),
    "assertion_passed": ("matcher", "actual", "expected", "file", "line", "column"),
    "exception": ("exception_type", "message", "file", "line"),
    "registered_test": ("id", "name", "file", "line", "tags", "depends_on", "priority"),
    "test_list_summary": ("registered_total", "matched_total", "tags_filter"),
    "build_start": ("config", "include_tests", "include_examples"),
    "build_end": ("ok", "duration_ms"),
    "compile_end": ("source_path", "object_path", "ok", "cache_hit"),
    "link_end": ("executable_path", "ok", "cache_hit"),
    "command_end": ("ok", "exit_code", "duration_ms"),
    "list_summary": ("units_total", "main_count", "test_count", "max_level"),
    "unit": ("path", "kind", "imports", "level", "has_main", "is_test", "is_modular"),
    "cache_status": ("object_cache_path", "profile_match", "current_profile"),
    "crash": ("pid", "signal"),
}

# (label, argv tail). Non-zero exit is expected for the probe cases.
COMMANDS = [
    ("build", ["debug", "build", "--jsonl=failures"]),
    ("list", ["debug", "list", "--jsonl=failures"]),
    ("cache_status", ["debug", "cache", "status", "--jsonl=failures"]),
    ("test_list", ["debug", "test", "--list", "--jsonl=failures"]),
    ("test_self", ["debug", "test", "--jsonl=failures", "--tags=[self]"]),
    ("test_trace", ["debug", "test", "--jsonl=trace", "--tags=[self][harness]"]),
    ("test_summary", ["debug", "test", "--jsonl=summary", "--tags=[self]"]),
    # Deliberately emits invalid UTF-8 in assertion operands.
    ("test_utf8_probe", ["debug", "test", "--jsonl=failures", "--tags=[.jsonl-utf8-probe]"]),
]


class Failure(Exception):
    pass


def load_schema_validator(required: bool):
    """Return a jsonschema validator, or None when the library is unavailable."""
    try:
        from jsonschema import Draft202012Validator
    except ImportError:
        if required:
            raise Failure("jsonschema is not installed but --require-schema was given")
        print("note: jsonschema not installed, schema checks skipped", file=sys.stderr)
        return None

    schema = json.loads(SCHEMA_PATH.read_text())
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema)


def validate_stream(label: str, raw: bytes, validator=None) -> int:
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise Failure(f"{label}: stdout is not valid UTF-8: {exc}") from exc

    lines = 0
    for number, line in enumerate(text.splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith("{"):
            raise Failure(f"{label}:{number}: non-JSONL line on stdout: {line[:80]!r}")

        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            raise Failure(f"{label}:{number}: {exc}: {line[:120]!r}") from exc

        missing = [f for f in ENVELOPE_FIELDS if f not in event]
        if missing:
            raise Failure(f"{label}:{number}: missing envelope fields {missing}")
        if event["schema"] != "tester-jsonl":
            raise Failure(f"{label}:{number}: unexpected schema {event['schema']!r}")

        required = REQUIRED_FIELDS.get(event["type"], ())
        missing = [f for f in required if f not in event]
        if missing:
            raise Failure(
                f"{label}:{number}: {event['type']} missing fields {missing}")

        if validator is not None:
            errors = sorted(validator.iter_errors(event), key=lambda e: e.path)
            if errors:
                detail = "; ".join(
                    f"{'/'.join(str(p) for p in e.path) or '<root>'}: {e.message}"
                    for e in errors[:3])
                raise Failure(f"{label}:{number}: schema: {detail}")
        lines += 1

    if lines == 0:
        raise Failure(f"{label}: no JSONL events on stdout")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cb", default=str(REPO_ROOT / "tools" / "CB.sh"))
    parser.add_argument("--jsonl", action="store_true",
                        help="emit machine-readable results on stdout")
    parser.add_argument("--require-schema", action="store_true",
                        help="fail instead of skipping when jsonschema is missing")
    args = parser.parse_args()

    failures: list[str] = []
    total_lines = 0

    try:
        validator = load_schema_validator(args.require_schema)
    except Failure as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1

    for label, tail in COMMANDS:
        proc = subprocess.run(
            [args.cb, *tail],
            cwd=str(REPO_ROOT),
            capture_output=True,
            env={**os.environ},
            check=False,
        )
        try:
            total_lines += validate_stream(label, proc.stdout, validator)
            print(f"ok   {label}", file=sys.stderr)
        except Failure as exc:
            failures.append(str(exc))
            print(f"FAIL {exc}", file=sys.stderr)

    if args.jsonl:
        print(json.dumps({
            "type": "jsonl_validate_summary",
            "commands": len(COMMANDS),
            "lines": total_lines,
            "failures": len(failures),
            "passed": not failures,
        }))

    if failures:
        print(f"FAILED: {len(failures)} of {len(COMMANDS)} commands", file=sys.stderr)
        return 1

    print(f"OK: {len(COMMANDS)} commands, {total_lines} events validated", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
