# Agent instructions — tester

Guidance for AI agents and automation using this repo’s JSONL output.

## Golden rule

Use **`--jsonl`**. Parse **stdout only** (one JSON object per line, `schema: "tester-jsonl"`). Treat **stderr** as human/CB wrapper logs — do not parse it for pass/fail.

## Canonical commands

```bash
# Translation-unit inventory (modules, imports, compile levels)
./tools/CB.sh debug list --jsonl

# Scoped test run (preferred while fixing)
./tools/CB.sh debug test --jsonl --jsonl-output=always --tags='\[module\]'

# Full suite (examples included in standalone tester)
./tools/CB.sh debug test --jsonl --jsonl-output=always

# Test catalogue (machine-readable)
./tools/CB.sh debug test --list --jsonl

# List registered tests (human)
./tools/CB.sh debug test --list

# Build with compile telemetry
./tools/CB.sh debug build --jsonl
```

**Tag syntax:** bracket tags must be escaped in shell: `--tags='\[dashboard\]'`. Substring/regex filters also work: `--tags='Test case 3'`.

**Flags:**
- `--jsonl` — machine-readable stdout for CB and (on `test`) test_runner
- `--jsonl-output=always` — emit `assertion_passed` as well as `assertion_failed` (default: failures only)

## Triage workflow (test failure)

1. Find the last `summary` or `run_end` on stdout.
2. If `passed` is `false`:
   - Read `first_failure` → open `file` at `line`, use `message`
   - Read `failed_test_ids` for the full failure set
3. For diagnosis, grep stdout for `assertion_failed` (or `assertion_passed` when using `always`):
   - `matcher` — e.g. `require_eq`, `check_contains` (not generic `require` / `check`)
   - `actual`, `expected`, `file`, `line`, `column`
4. Fix the source, then re-run the **same** tagged command.

If `matcher` is `"require"` or `"check"` on a `require_eq` / `check_eq` line, stale test objects are likely — rebuild test TUs (`./tools/CB.sh debug build --jsonl`), not only `tester_assertions.pcm`.

## Triage workflow (build failure)

1. Find `command_end` with `"ok":false` — use the `argv` array to rerun without shell parsing.
2. Check `compile_end` events: `cache_hit:false` means that translation unit recompiled; `cache_hit:true` means incremental skip.
3. Rebuild: `./tools/CB.sh debug build --jsonl`, then re-run tests.

## Event reference (stdout)

### Correlation

Filter `run_id=<cb>` or `parent_run_id=<cb>` to correlate `list` → `build` → `test` in one `CB … --jsonl` invocation.

| Field | On | Meaning |
|-------|-----|---------|
| `run_id` | Every event | Session id for the emitting process (32-char hex) |
| `parent_run_id` | `test_runner` events only | CB’s `run_id`, passed via `TESTER_PARENT_RUN_ID` when CB spawns the child |
| `config` on `run_start` | `test_runner` when spawned by CB | CB build config (`debug` / `release`), via `TESTER_CONFIG` |
| `pid` | Every event | OS process id (`test_runner` differs from CB) |
| `ts_unix_ms` | Every event | Unix timestamp (ms) |

### Test catalogue (`test --list --jsonl`)

| Event | Use |
|-------|-----|
| `test_list_start` | Catalogue start (`tags_filter`) |
| `registered_test` | Per test: `id`, `name`, `file`, `line`, `column`, `tags[]`, `depends_on[]`, `priority` |
| `test_list_summary` | `registered_total`, `matched_total`, `tags_filter` |

### Test phase

| Event | Use |
|-------|-----|
| `run_start` / `run_end` | Run boundaries; `run_start` has `cwd`, `argv`, `config` (from `TESTER_CONFIG` when CB spawns the child), `env` (curated test-relevant vars when set, e.g. `NET_DISABLE_NETWORK_TESTS`, `CURSOR_SANDBOX`), `passed`, `duration_ms` on `run_end` |
| `assertion_failed` | Always on failed assertions |
| `assertion_passed` | With `--jsonl-output=always` |
| `test` | Per-test rollup (`success`, `output`, assertion counts) |
| `summary` | `tests_ok`/`tests_total`, `failed_test_ids`, `first_failure` |
| `exception` | Uncaught exceptions |
| `eof` | End of JSONL stream |

### Build phase (CB)

| Event | Use |
|-------|-----|
| `list_start` | TU inventory start (`config`, `include_tests`, `include_examples`, `source_dir`) |
| `unit` | Per translation unit (`path`, `module`, `kind`, `imports[]`, `level`, `has_main`, `is_test`, `is_modular`) |
| `list_summary` | Inventory totals (`units_total`, `main_count`, `test_count`, `max_level`) |
| `build_start` / `build_end` | Whole build |
| `command_start` / `command_end` | Subprocesses (`cmd` + `argv`) |
| `compile_end` | Per TU (`source_path`, `cache_hit`, `rebuild_reason` when `cache_hit:false`, paths) |
| `cb_error` | CB fatal/diagnostic |

**`unit.is_test`:** `true` for `*.test.c++` / `*.test.c++m`, or when a path segment is exactly `test/` or `tests/`. `false` for sources under a `tester/` framework tree (library modules, not project tests) — including nested paths like `deps/xson/deps/tester/`. Does not match the substring `test` inside names such as `tester` or `test_exception_bug`.

## Example agent loop

```text
1. ./tools/CB.sh debug build --jsonl
2. ./tools/CB.sh debug test --jsonl --jsonl-output=always --tags='\[module\]'
3. Parse last summary → passed?
4. If false: first_failure + assertion_failed → edit → goto 1 or 2
```

## Do not

- Infer pass/fail from exit code alone — read `summary.passed` or `run_end.passed`
- Parse ANSI-colored stderr as structured data
- Run the full suite on every iteration unless explicitly asked — scope with `--tags`

## More detail

- Event fields and examples: [README.md — JSONL sections](README.md#jsonl-assertion-events)
- Improvement backlog: [docs/tester-improvements.md](docs/tester-improvements.md)