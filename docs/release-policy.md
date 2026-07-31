# Release Policy

What counts as public, how versions are numbered, what breaks a consumer, and what has to
be true before a release is cut.

## Current status: pre-release

Nothing is released yet, deliberately. The `v1.0.0` tag (27 November 2025) is a GitHub
*pre-release* that predates the JSONL contract, CB's cache model, and the current
assertion semantics; it is kept for history and is not supported. It will not be reused —
the first supported release gets a new version number.

Until then, `main` is the revision to consume, pinned by the parent repository:

```bash
git submodule add https://github.com/ruoka/tester deps/tester
git -C deps/tester checkout <commit>   # pin deliberately; nothing auto-tracks main
```

`main` is expected to be green — every push runs the framework contract tests, the CB and
MCP smoke suites, and JSONL schema validation — but it carries no compatibility promise
between commits. What changed is in [`CHANGELOG.md`](../CHANGELOG.md) under `Unreleased`,
which is maintained now rather than reconstructed at release time.

## What is public

| Surface | Contract |
|---------|----------|
| `import tester;` | The partitions re-exported by [`tester/tester.c++m`](../tester/tester.c++m) — `data`, `utils`, `basic`, `runner`, `assertions`, `behavior_driven_development`, `observer` — plus the `tester::bdd` alias, `set_slowest`, and `set_run_argv` |
| `test_runner` CLI | The flags in `--help` and under [Running Tests](../README.md#running-tests) |
| Test JSONL | Event types and fields in [`docs/jsonl-schema.json`](jsonl-schema.json) (`schema: "tester-jsonl"`) |
| CB CLI | Subcommands and flags documented in [README](../README.md) and [`AGENTS.md`](../AGENTS.md) |
| CB JSONL | The build-phase events in the same schema file |
| `Makefile` | The alternative to CB for projects that build with make: the `module`, `tests`, `run_tests`, `examples`, and `clean` targets, the `build-<os>/{pcm,obj,lib,bin}` layout, and the `CXX` / `CXXFLAGS` / `PREFIX` / `TEST_TAGS` variables it reads from `config/compiler.mk` or a parent's |
| `tools/CB.sh` environment | `LLVM_PATH`, `CXX`, `CB_INCLUDE_FLAGS`, and the `CB_*` variables a parent repo sets |

Everything else may change in any release without notice: the internals of
`tester:engine`, the structure of [`tools/cb.c++`](../tools/cb.c++),
anything under `build-*/cache/` (only CB writes those files, and the documented fix for a
format change is `cache invalidate`), `tester/details/`, and the hidden `[.tag]` probe
fixtures the self-tests spawn.

## Versioning

Semantic versioning, applied to the public surface above.

| Bump | Means |
|------|-------|
| MAJOR | Something public was removed or changed meaning: an exported name, a CLI flag, a JSONL field or event type, or the minimum toolchain |
| MINOR | Something was added: a matcher, an event, a flag, a field — existing usage keeps working unchanged |
| PATCH | A fix that leaves every documented behaviour intact |

While the version is `0.x`, MINOR may break; PATCH may not. After `1.0.0`, breaking
changes wait for a MAJOR.

**JSONL.** Consumers must ignore unknown fields and unknown event types — adding either is
MINOR, which is why a new event never forces a schema version bump. The `version` field
increments only when an existing field disappears or changes meaning, and that is MAJOR.
The rule agents rely on — parse stdout, read the last `summary`, never infer pass/fail
from the exit code alone — is part of the contract, not an implementation detail.

**Toolchain.** Every release states its minimum compiler. Raising it is MAJOR after
`1.0.0`: a consumer cannot adopt the release without upgrading its own toolchain, which is
a breaking change whatever the source compatibility.

## Deprecation

A public name that is going away is first marked deprecated, with its replacement named in
the same place, kept working for at least one MINOR, and removed only in a MAJOR. The
instance form of `check_throws_as` / `require_throws_as` — which discarded the value and
existed only to deduce the type — is the standing example: it still compiles, it warns, and
it names `check_throws_as<E>(callable)` in the warning.

## Release criteria

A release is cut when all of these hold. There is no schedule — the criteria decide.

1. **CI green on Linux** for both `debug` and `release`: the `[self]` suite, the full
   standalone suite, the failure-demo gate, JSONL schema validation, CB smoke, MCP
   smoke, and the Makefile lane (`make tests` + `[self]`, warning-free).
2. **The same suites run by hand on macOS**, against a locally built LLVM, until a
   hosted runner ships a clang that can build C++23 modules and the lane becomes
   automatic.
3. **`make tests` builds and its runner passes**, warning-free. CI gates this on every
   push (`makefile-build-and-test`); a release re-checks it on a clean tree. CB also
   surfaces compiler warnings from units it compiles (`diagnostics` on successful
   `compile_end` / `link_end` in `--jsonl=failures`), so Make is a second path rather
   than the only place `-Wall` noise is visible.
4. **[`docs/repository-technical-review.md`](repository-technical-review.md) rates the
   repository at or close to 9 / 10.** This is the deliberate gate: the project stays
   unreleased while its own review can name weaknesses a consumer would inherit. The
   review is re-run against the tree, with its claims verified rather than carried
   forward. *(Currently 8.0.)*
5. **No High priority item open** in that review, and nothing above Low left in the
   priority sketch in [`docs/tester-improvements.md`](tester-improvements.md).
6. **Documentation agrees with the code.** README, `AGENTS.md`, `CONTRIBUTING.md`, and
   `docs/` are checked against the current CLI, event names, flags, and paths — verified,
   not assumed. A release is the point where a stale command costs somebody else time.
7. **[`CHANGELOG.md`](../CHANGELOG.md) is current.** Every change to the public surface
   lands in `Unreleased` with the change, not afterwards; cutting a release renames that
   section to the version and its date.
8. **The minimum toolchain is stated** in the README requirements and matches the release
   notes.

## Cutting a release

```bash
# 1. Verify — all of it, on a clean tree
./tools/CB.sh debug clean
./tools/CB.sh debug test --jsonl=failures --tags='\[self\]'   # framework contract
./tools/CB.sh debug test --jsonl=failures                     # standalone suite, examples included
./tools/CB.sh release build --build-tests --jsonl=failures    # tests must compile optimized too
./tests/cb/smoke.sh                                           # CB behaviour, real compiler
./tests/mcp/smoke.sh                                          # MCP bridge
./tests/jsonl/validate.py --require-schema                    # every event against the schema
make clean && make run_tests TEST_TAGS='--tags=[self]'        # the non-CB path, warning-free

# 2. Rename CHANGELOG.md's Unreleased section to the version + date;
#    update README requirements if the toolchain moved

# 3. Tag on main, annotated
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z

# 4. Publish notes: highlights, breaking changes, minimum compiler, JSONL schema version
gh release create vX.Y.Z --title vX.Y.Z --notes-file release-notes.md
```

Release notes state, in this order: breaking changes and what to do about them, new
capabilities, fixes, the minimum compiler, and the JSONL `version`. A consumer should be
able to decide from the notes alone whether the upgrade is mechanical.

## Support window

The newest release only. No backports before `1.0.0`, and no separate embargo process for
security-relevant fixes — they ship in the next release, and if that is urgent the release
is cut early rather than patched sideways.
