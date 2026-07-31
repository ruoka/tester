// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#include "details/selftest_spawn.h++"

import std;
import tester;

namespace tester::selftest::runner {

namespace {

auto& dependency_run_counter()
{
    static auto value = 0;
    return value;
}

struct recording_observer final : output::observer
{
    int assertions = 0;

    void assertion(const output::assertion_event&) override
    {
        ++assertions;
    }
};

// Registered only in spawned probe processes so normal [self] runs stay cycle-free.
auto register_dependency_cycle_probe()
{
    if(std::getenv("TESTER_DEPENDS_ON_CYCLE_PROBE") == nullptr)
        return 0;

    using tester::basic::test_case;
    using tester::basic::test_order;

    test_case("test_case [.depends-on-cycle-probe] cycle a",
              test_order{.priority = 0, .depends_on = {"self_cycle_b"}, .id = "self_cycle_a"}) = []
    {
    };

    test_case("test_case [.depends-on-cycle-probe] cycle b",
              test_order{.priority = 0, .depends_on = {"self_cycle_a"}, .id = "self_cycle_b"}) = []
    {
    };

    return 0;
}

const auto _cycle_probe = register_dependency_cycle_probe();

// Two cases claiming one id, and a case depending on an id nothing registered. Both are
// refused before the first test runs, so they can only be observed from another process.
auto register_metadata_probes()
{
    if(std::getenv("TESTER_DUPLICATE_ID_PROBE") != nullptr)
    {
        using tester::basic::test_case;
        using tester::basic::test_order;

        test_case("test_case [.duplicate-id-probe] first claimant",
                  test_order{.priority = 0, .depends_on = {}, .id = "self_duplicate_id"}) = []
        {
        };

        test_case("test_case [.duplicate-id-probe] second claimant",
                  test_order{.priority = 0, .depends_on = {}, .id = "self_duplicate_id"}) = []
        {
        };
    }

    if(std::getenv("TESTER_UNKNOWN_DEPENDENCY_PROBE") != nullptr)
    {
        using tester::basic::test_case;
        using tester::basic::test_order;

        test_case("test_case [.unknown-dependency-probe] depends on a typo",
                  test_order{.priority = 0, .depends_on = {"self_no_such_id"}, .id = "self_typo_dep"}) = []
        {
        };
    }

    return 0;
}

const auto _metadata_probes = register_metadata_probes();

// A step that fails cannot be verified in-process: the failure would fail this run. Registered
// only in the spawned probe so an ordinary run never sees a deliberate failure.
auto register_failing_step_probe()
{
    if(std::getenv("TESTER_FAILING_STEP_PROBE") == nullptr)
        return 0;

    using namespace tester::assertions;
    using namespace tester::behavior_driven_development;

    scenario("scenario [.failing-step-probe] one step fails") = []
    {
        given("a step that fails and a sibling after it") = []
        {
            // Non-fatal, so the sibling is reached; a fatal one would leave `given` early.
            then("this one fails on purpose") = []
            {
                check_eq(1, 2);
            };

            and_then("this one still runs") = []
            {
                require_true(true);
            };
        };
    };

    return 0;
}

const auto _failing_step_probe = register_failing_step_probe();

} // namespace

auto register_tests()
{
    using tester::basic::test_case;
    using tester::basic::test_order;
    // Named one at a time: both APIs export a test_order, so importing either wholesale would
    // make the one used above ambiguous.
    using tester::behavior_driven_development::given;
    using tester::behavior_driven_development::scenario;
    using tester::behavior_driven_development::then;
    using tester::behavior_driven_development::when;
    using namespace tester::assertions;
    using tester_selftest::run_test_runner;

    test_case("test_case [self][order] dependency root",
              test_order{.priority = 0, .depends_on = {}, .id = "self_order_root"}) = []
    {
        dependency_run_counter() = 1;
        require_eq(dependency_run_counter(), 1);
    };

    test_case("test_case [self][order] dependency child",
              test_order{.priority = 0, .depends_on = {"self_order_root"}, .id = "self_order_child"}) = []
    {
        require_eq(dependency_run_counter(), 1);
        dependency_run_counter() = 2;
        require_eq(dependency_run_counter(), 2);
    };

    test_case("test_case [self][order] cyclic depends_on fails without crashing") = []
    {
        const auto result = run_test_runner(
            {"--tags=[.depends-on-cycle-probe]"},
            "TESTER_DEPENDS_ON_CYCLE_PROBE=1 ");

        // A dependency cycle must be reported as a failure, not a crash.
        require_false(result.signaled);
        require_eq(result.exit_code, 1);
    };

    test_case("test_case [self][order] a duplicate test id fails the run") = []
    {
        const auto result = run_test_runner(
            {"--tags=[.duplicate-id-probe]"},
            "TESTER_DUPLICATE_ID_PROBE=1 ");

        require_false(result.signaled);
        require_eq(result.exit_code, 1);
        // The message has to name the id: which two cases collided is the whole diagnosis.
        require_true(result.stderr_text.contains("Duplicate test id \"self_duplicate_id\""));
    };

    test_case("test_case [self][order] a depends_on id nothing registered fails the run") = []
    {
        const auto result = run_test_runner(
            {"--tags=[.unknown-dependency-probe]"},
            "TESTER_UNKNOWN_DEPENDENCY_PROBE=1 ");

        require_false(result.signaled);
        require_eq(result.exit_code, 1);
        require_true(result.stderr_text.contains("Unknown test id \"self_no_such_id\""));
    };

    test_case("test_case [self] summary records passing run") = []
    {
        require_true(true);
        require_eq(0, 0);
    };

    test_case("test_case [self] output observers receive assertion events") = []
    {
        auto recorder = recording_observer{};
        output::observe(recorder);
        check_eq(1, 1);
        output::unobserve(recorder);
        require_eq(recorder.assertions, 1);
    };

    // A step runs where it is written, so the enclosing frame is still alive: capturing a local
    // by reference is correct, each step observes what the step before it did, and all of them
    // have finished by the time the enclosing body reaches its last line.
    scenario("scenario [self] steps run in the frame that declared them") = []
    {
        auto reached = std::vector<std::string>{};
        auto value = 41;

        given("locals held by reference") = [&]
        {
            reached.push_back("given");
            ++value;

            when("a step mutates one") = [&]
            {
                reached.push_back("when");

                then("the step after it sees the new value") = [&]
                {
                    reached.push_back("then");
                    require_eq(value, 42);
                };
            };
        };

        require_eq(reached, std::vector<std::string>{"given", "when", "then"});
    };

    test_case("test_case [self] a failing step fails itself and not the case it is written in") = []
    {
        const auto result = run_test_runner(
            {"--jsonl=trace", "--tags=[.failing-step-probe]"},
            "TESTER_FAILING_STEP_PROBE=1 ");

        require_eq(result.exit_code, 1);

        // Four cases: the scenario, its given, and the two steps inside that.
        const auto summary = tester_selftest::find_event(result.stdout_text, "summary");
        require_eq(tester_selftest::field(summary, "tests_total"), std::string{"4"});
        require_eq(tester_selftest::field(summary, "tests_ok"), std::string{"3"});

        // The step owns its failure: the scenario and the given it sits in still pass, and the
        // sibling written after it ran rather than being skipped with it.
        const auto failed = tester_selftest::field(summary, "failed_test_ids");
        require_true(failed.contains("then -> this one fails on purpose"));
        require_false(failed.contains("scenario ->"));
        require_false(failed.contains("given ->"));
        require_true(result.stdout_text.contains("and_then -> this one still runs"));
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::runner