# Tester Assertions – Possible Enhancements

Recent review of `deps/tester/tester/tester-assertions.c++m` highlighted a few capability gaps compared to well-rounded testing frameworks. Consider tracking/implementing the following improvements:

1. **Approximate comparisons**
   - ✅ `check_near` / `require_near` now wrap `floating_point_equal` with per-call epsilon control.
   - Explore extending approximate comparisons to user-defined types (e.g., vectors of floats) so tolerance-based checks are easy to express.

2. **Container and range assertions**
   - Add helpers for sequence equality with readable diffs (e.g., highlight first mismatch).
   - Optional: basic matchers like “contains element”, “starts with”, or “is permutation”.

3. **String-focused assertions**
   - Helpers for equality with case/locale options, prefix/suffix/substring checks, and regex matches.

4. **Predicate / matcher API**
   - Expose lightweight matchers so users can compose checks declaratively without hand-writing boilerplate lambdas.

5. **Death/termination tests**
   - Support verifying that code terminates via abort/signal or logs a fatal error, optionally capturing stderr/stdout for diagnostics.

These additions would close the feature gap with mainstream test frameworks while building on the existing `check`/`require` infrastructure.***

