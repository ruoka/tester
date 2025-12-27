# Tester Assertions – Possible Enhancements

Recent review of `deps/tester/tester/tester-assertions.c++m` highlighted a few capability gaps compared to well-rounded testing frameworks. Consider tracking/implementing the following improvements:

1. **Approximate comparisons**
   - ✅ `check_near` / `require_near` now wrap `floating_point_equal` with per-call epsilon control.
   - Explore extending approximate comparisons to user-defined types (e.g., vectors of floats) so tolerance-based checks are easy to express.

2. **Container and range assertions**
   - ✅ `check_container_eq` / `require_container_eq` now provide sequence equality with readable diffs (highlights first mismatch).
   - ✅ `check_contains(container, element)` / `require_contains(container, element)` for checking if container contains element.
   - Optional: additional matchers like "starts with" (for sequences), or "is permutation".

3. **String-focused assertions**
   - ✅ `check_contains` / `require_contains` for substring checks
   - ✅ `check_has_substr` / `require_has_substr` (alias for contains)
   - ✅ `check_starts_with` / `require_starts_with` for prefix checks
   - ✅ `check_ends_with` / `require_ends_with` for suffix checks
   - Optional: equality with case/locale options and regex matches.

4. **Predicate / matcher API**
   - Expose lightweight matchers so users can compose checks declaratively without hand-writing boilerplate lambdas.

5. **Death/termination tests**
   - Support verifying that code terminates via abort/signal or logs a fatal error, optionally capturing stderr/stdout for diagnostics.

These additions would close the feature gap with mainstream test frameworks while building on the existing `check`/`require` infrastructure.***

## AI-friendly output / automation enhancements

These are focused on making test + build output easier to parse and act on by tools (including AI agents), while keeping human output readable.

1. **Structured assertion events (JSONL)**
   - TODO: Emit a dedicated `assertion_failed` (and optionally `assertion_passed`) JSONL event with structured fields:
     - `matcher`, `actual`, `expected`, `file`, `line`, `column`, `expression`, `message`
   - Currently assertions are only captured in the test's `output` field as text
   - This would make it easier for tools to parse assertion failures without scraping text output

2. **First-failure + failure index**
   - In `summary` / `run_end`, include:
     - `failed_test_ids: [...]`
     - `first_failure: { test_id, file, line, message }`
   - This allows automation to jump directly to the most relevant failure.

3. **Exception metadata**
   - For thrown exceptions, add separate fields (not only embedded text):
     - `exception_type` and `what`

4. **Run correlation ID**
   - Add a shared `run_id` that appears in every CB and tester JSONL record so multi-process streams can be correlated reliably.

5. **CB: per-step build events**
   - Emit per-translation-unit events:
     - `compile_start` / `compile_end` (with `ok`, `duration`, `source_path`, `object_path`, `pcm_path`, `module_name`)
   - Emit per-binary events:
     - `link_start` / `link_end`

6. **CB: structured commands**
   - In addition to the human `command` string, emit an `argv: ["clang++", "..."]` array so a failing invocation can be re-run safely without shell parsing.

7. **CB/tester: attach log artifacts**
   - Write large stderr/stdout to files and emit paths like `stderr_path` / `stdout_path`.
   - Optionally include a small `*_head` snippet in JSONL (bounded by size limits).

