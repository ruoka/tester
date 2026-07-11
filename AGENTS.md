# Agent instructions — tester

Guidance for AI agents and automation using this repo’s JSONL output.

## Golden rule

Use **`--jsonl`**. Parse **stdout only** (one JSON object per line, `schema: "tester-jsonl"`). Treat **stderr** as human/CB wrapper logs — do not parse it for pass/fail.

## Canonical commands

```bash
# Scoped test run (preferred while fixing)
./tools/CB.sh debug test --jsonl --jsonl-output=always --tags='\[module\]'

# Full suite (examples included in standalone tester)
./tools/CB.sh debug test --jsonl --jsonl-output=always

# List registered tests (human today; JSONL list is planned)
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

### Test phase

| Event | Use |
|-------|-----|
| `run_start` / `run_end` | Run boundaries, `passed`, `duration_ms` |
| `assertion_failed` | Always on failed assertions |
| `assertion_passed` | With `--jsonl-output=always` |
| `test` | Per-test rollup (`success`, `output`, assertion counts) |
| `summary` | `tests_ok`/`tests_total`, `failed_test_ids`, `first_failure` |
| `exception` | Uncaught exceptions |
| `eof` | End of JSONL stream |

### Build phase (CB)

| Event | Use |
|-------|-----|
| `build_start` / `build_end` | Whole build |
| `command_start` / `command_end` | Subprocesses (`cmd` + `argv`) |
| `compile_end` | Per translation unit (`source_path`, `cache_hit`, paths) |
| `cb_error` | CB fatal/diagnostic |

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