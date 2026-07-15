# Recommendation for AI Coding Agents

Tester and CB are particularly effective for AI-assisted C++ work because they turn build and test state into a small, deterministic protocol instead of forcing an agent to interpret terminal prose.

## What an Agent Gains

### Less context and fewer tokens

Successful test runs can produce megabytes of assertion and compiler output. With failures or summary mode, an agent normally needs only the final `summary` event:

```json
{"type":"summary","tests_ok":318,"tests_total":318,"passed":true,"failed_test_ids":[]}
```

On failure, `first_failure`, `assertion_failed`, or `command_end` identifies the file, line, matcher, command, and expected/actual values. The agent can read those events instead of ingesting the full log.

This saves model context only when the agent selects the appropriate mode. Trace output is intentionally verbose.

### Faster edit-test loops

CB discovers module dependencies, compiles in dependency order, and reuses its object cache. An agent does not need to understand or regenerate a separate build graph before making a focused change.

Registered test-name and tag filtering also makes it practical to run a small relevant suite first, then the complete project suite only for final verification.

### Deterministic triage

Structured events remove ambiguity:

- `summary.passed` is the test result.
- `first_failure` is the first actionable assertion.
- `failed_test_ids` is the complete failure set.
- `command_end.ok` identifies a failed compiler or linker invocation.
- `compile_end.cache_hit` shows whether the expected translation unit rebuilt.
- `run_id` and `parent_run_id` correlate CB and `test_runner`.

This is more reliable than inferring success from colors, prose, or process exit status alone.

## Recommended Agent Workflow

```bash
# Discover registered tests and their exact names.
./tools/CB.sh debug test --list --jsonl=failures

# Build with machine-readable compile events.
./tools/CB.sh debug build --jsonl=failures

# Run the smallest relevant scope while editing.
./tools/CB.sh debug test "registered test name" --jsonl=failures

# Run the project tag before handoff.
./tools/CB.sh debug test --jsonl=failures --tags='\[project-tag\]'

# Verify the optimized configuration before committing.
./tools/CB.sh release test --jsonl=summary --tags='\[project-tag\]'
```

Parse stdout only. Treat stderr as human-readable wrapper and compiler diagnostics.

For a successful run, read the last `summary`. For a failed run:

1. Read `first_failure` and `failed_test_ids`.
2. Inspect matching `assertion_failed` or `exception` events.
3. For build failures, find `command_end` with `"ok":false`.
4. Fix the source and rerun the same scope.
5. Rebuild test translation units when matcher metadata appears stale.

## Token and Output Guidance

- Use `--jsonl=failures` during routine loops.
- Use `--jsonl=summary` for aggregate-only CI gates.
- Use `--jsonl=trace` only when successful per-command, per-TU, per-test, or assertion events are needed.
- Never send the complete JSONL stream into the model when a summary or failure event answers the question.
- Use `--list --jsonl=failures` before guessing a filename-based filter; filters match registered test names.
- In an embedded project, use the parent project's tag. Do not assume an unfiltered tester run represents the parent suite.
- After a successful build, rerun `test_runner` directly when no source changed and a relink is unnecessary.

## Practical Assessment

In a YarDB maintenance session, CB and tester made repeated C++23 module refactors manageable for an AI agent:

- Incremental builds exposed exactly which module recompiled.
- Targeted engine and HTTP suites completed quickly enough for iterative fixes.
- Full debug and release runs produced large logs, but the agent consumed only two summary events.
- JSONL failures pinpointed a Linux-only `std::common_type` compilation issue after a dependency update.
- Tag-scoped verification avoided intentional tester probe fixtures.

The tools saved time and model context overall. The main costs were large output files when every passing assertion was requested and confusion when documentation suggested filters that were not registered test-name substrings.

## Local Makefile vs CB Benchmark

A local macOS/Clang 21 comparison was run on 15 July 2026. This is one machine and not a universal performance benchmark.

Method:

- Make cold: isolated output directory, `make -j8 ... tests`
- Make warm: repeat the same command without source changes
- CB cold: `./tools/CB.sh debug clean`, then `./tools/CB.sh debug build --jsonl=trace`
- CB warm: repeat the CB build without source changes
- Output size includes stdout and stderr and is used only as a token-pressure proxy

Observed results:

- Make cold: **14.85 s**, **90,386 bytes**, 75 output lines
- Make warm: **0.10 s**, **67 bytes**, 4 output lines
- CB cold: **10.91 s**, **423,876 bytes**, 180 output lines
- CB warm: **0.20 s**, **24,419 bytes**, 76 output lines

CB listed 34 translation units, 2 main programs, and 14 test units. Its cold JSONL contained 34 `compile_end` events and one `link_end` event. The targets are not perfectly identical: Make's `tests` target links the test runner, while standalone CB discovers examples and the CB fixture program as configured units.

For this run, CB's cold build was about 26% faster; Make's no-change build was 0.10 s faster and emitted much less raw output in both cases. CB's agent advantage was structured, queryable telemetry rather than lower output volume. An agent that reads only `build_end`, failed `command_end`, and relevant `compile_end` events consumes little context. An agent that ingests the complete CB JSONL consumes more context than the Make log.

Therefore:

- Do not claim that CB inherently saves tokens compared with Make or Ninja.
- Claim that CB can reduce **diagnostic context** when its JSONL is filtered correctly.
- Prefer failures/summary modes; avoid forwarding trace command events into the model.
- Benchmark build speed independently from agent-debugging efficiency.

## Token-Efficient JSONL Modes

Tester and CB now share one JSONL strategy:

```bash
./tools/CB.sh debug build --jsonl=summary
./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'
./tools/CB.sh debug build --jsonl=trace
```

- `summary` emits lifecycle and enriched aggregate events.
- `failures` adds failed commands, compiles, links, tests, assertions, and exceptions.
- `trace` emits every event and full successful telemetry.
- Bare `--jsonl` means failures mode.
- Explicit test catalogues remain complete in every mode.

On the same standalone tester build used above, failures mode produced **792 bytes / 4 lines** for a successful incremental build, compared with **24,419 bytes / 76 lines** for the previous full warm trace. A successful summary-mode self-test invocation, including CB and child process lifecycle events, produced about **2.2 KiB / 10 lines**.

Potential future improvements include project-relative paths, bounded compiler stderr excerpts with artifact paths, and explicit event selection.

## Recommendation

For AI coding agents working on modern C++ modules, use tester and CB as a structured feedback API:

> Build incrementally, discover tests before filtering, parse summaries instead of logs, and reserve full-suite debug/release runs for final verification.

The combination is most valuable when the repository also provides an `AGENTS.md` that documents its project tag, canonical commands, expected network requirements, and failure-triage rules.
