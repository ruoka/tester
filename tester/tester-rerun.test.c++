// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#include "details/selftest_spawn.h++"

import std;
import tester;

namespace tester::selftest::rerun {

auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;
    using tester_selftest::find_event;
    using tester_selftest::field;
    using tester_selftest::run_test_runner;

    test_case("test_case [.rerun-probe] passes") = []
    {
        require_eq(1, 1);
    };

    test_case("test_case [.rerun-probe] fails") = []
    {
        check_eq(1, 2);
    };

    {
        using tester::behavior_driven_development::given;
        using tester::behavior_driven_development::scenario;
        using tester::behavior_driven_development::when;

        scenario("scenario [.rerun-probe] a scenario whose step fails") = []
        {
            given("a scenario with a failing step") = []
            {
                when("a step that fails") = []
                {
                    check_eq(1, 2);
                };
            };
        };
    }

    static constexpr auto passes_id = "test_case -> test_case [.rerun-probe] passes";
    static constexpr auto fails_id = "test_case -> test_case [.rerun-probe] fails";
    static constexpr auto scenario_id = "scenario -> scenario [.rerun-probe] a scenario whose step fails";
    static constexpr auto rerun_ids =
        R"("rerun_ids":["test_case -> test_case [.rerun-probe] fails","scenario -> scenario [.rerun-probe] a scenario whose step fails"])";

    const auto summary_of = [](const tester_selftest::spawn_result& result)
    {
        const auto summary = find_event(result.stdout_text, "summary");
        require_false(summary.empty());
        return summary;
    };

    test_case("test_case [self] rerun_ids names each failed top-level case once") = [summary_of]
    {
        const auto result = run_test_runner({"--jsonl=failures", "--tags=[.rerun-probe]"});

        require_eq(result.exit_code, 1);
        const auto summary = summary_of(result);
        require_eq(field(summary, "passed"), std::string{"false"});
        // The step's own id is in failed_test_ids, but only its scenario can be selected to run it.
        require_true(summary.contains("a step that fails"));
        require_true(summary.contains(rerun_ids));
        require_false(summary.contains(passes_id));
    };

    test_case("test_case [self] --test-id reruns exactly the cases in rerun_ids") = [summary_of]
    {
        const auto result = run_test_runner({
            "--jsonl=trace",
            std::string{"--test-id="} + fails_id,
            std::string{"--test-id="} + scenario_id});

        require_eq(result.exit_code, 1);
        const auto summary = summary_of(result);
        require_true(summary.contains(rerun_ids));
        require_true(result.stdout_text.contains("a step that fails"));
        require_false(result.stdout_text.contains(passes_id));
    };

    test_case("test_case [self] --test-id selects a hidden case and ANDs with --tags") = [summary_of]
    {
        const auto alone = run_test_runner({"--jsonl=failures", std::string{"--test-id="} + passes_id});
        require_eq(alone.exit_code, 0);
        const auto summary = summary_of(alone);
        require_eq(field(summary, "passed"), std::string{"true"});
        require_eq(field(summary, "tests_total"), std::string{"1"});
        require_true(summary.contains(R"("rerun_ids":[])"));

        const auto narrowed = run_test_runner({
            "--jsonl=failures",
            "--tags=[self]",
            std::string{"--test-id="} + passes_id});
        require_eq(narrowed.exit_code, 1);
        require_eq(field(summary_of(narrowed), "tests_total"), std::string{"0"});
    };

    test_case("test_case [self] --test-id lists only the named case") = []
    {
        const auto result = run_test_runner({"--jsonl=failures", "--list", std::string{"--test-id="} + scenario_id});

        require_eq(result.exit_code, 0);
        require_eq(field(find_event(result.stdout_text, "test_list_summary"), "matched_total"), std::string{"1"});
    };

    test_case("test_case [.first-failure-throw-probe] throws without asserting") = []
    {
        throw std::runtime_error{"probe threw"};
    };

    test_case("test_case [self] first_failure carries the failed assertion") = [summary_of]
    {
        // Recorded in every mode: summary mode writes no assertion_failed events, so first_failure
        // is the only place the matcher and operands appear there.
        for(const auto* mode : {"--jsonl=failures", "--jsonl=summary"})
        {
            const auto result = run_test_runner({mode, std::string{"--test-id="} + fails_id});
            const auto first = field(summary_of(result), "first_failure");
            require_true(first.contains(R"("matcher":"check_eq","actual":1,"expected":2)"));
            require_true(first.contains("tester-rerun.test.c++"));
            require_true(first.contains(R"("column":)"));
        }

        // A test that threw has no failed assertion, so first_failure points at the test itself.
        const auto threw = run_test_runner({"--jsonl=failures", "--tags=[.first-failure-throw-probe]"});
        const auto first = field(summary_of(threw), "first_failure");
        require_true(first.contains("throws without asserting"));
        require_false(first.contains(R"("matcher":)"));
    };

    test_case("test_case [self] --tags and a filter argument cannot both be given") = []
    {
        // They fill one filter, so taking the last one would silently drop the other.
        for(const auto& args : std::vector<std::vector<std::string>>{
                {"--jsonl=failures", "--tags=[self]", "some filter"},
                {"--jsonl=failures", "some filter", "--tags=[self]"},
                {"--jsonl=failures", "one", "two"}})
        {
            const auto result = run_test_runner(args);
            require_eq(result.exit_code, 1);
            require_true(result.stderr_text.contains("same filter"));
            require_true(find_event(result.stdout_text, "run_start").empty());
        }
    };

    test_case("test_case [self] --test-id rejects an id no case has") = []
    {
        // A step id is not selectable either: steps are registered only while their case runs.
        for(const auto* id : {"test_case -> no such test", "when -> a step that fails"})
        {
            const auto result = run_test_runner({"--jsonl=failures", std::string{"--test-id="} + id});

            require_false(result.signaled);
            require_eq(result.exit_code, 1);
            require_true(result.stderr_text.contains("Unknown test id"));
            require_true(result.stderr_text.contains(id));
            require_true(find_event(result.stdout_text, "summary").empty());
        }

        const auto empty = run_test_runner({"--jsonl=failures", "--test-id="});
        require_eq(empty.exit_code, 1);
        require_true(empty.stderr_text.contains("--test-id expects a test id"));
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::rerun
