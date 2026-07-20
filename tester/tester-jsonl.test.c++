// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#include "details/selftest_spawn.h++"

import std;
import tester;

namespace tester::selftest::jsonl {

auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;
    using tester_selftest::run_test_runner;

    test_case("test_case [self] jsonl catalogue events") = []
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--list",
            "--tags=[self]"});

        require_eq(result.exit_code, 0);
        require_true(result.stdout_text.contains("\"type\":\"test_list_start\""));
        require_true(result.stdout_text.contains("\"type\":\"registered_test\""));
        require_true(result.stdout_text.contains("\"type\":\"test_list_summary\""));
        require_true(result.stdout_text.contains("\"type\":\"eof\""));
        require_true(result.stdout_text.contains("\"tags\":[\"self\"]"));
    };

    test_case("test_case [self] jsonl run_start metadata") = []
    {
        if(std::getenv("TESTER_SELFTEST_SPAWNED") != nullptr)
        {
            require_eq(1, 1);
            return;
        }

        const auto result = run_test_runner({
            "--jsonl=trace",
            "jsonl run_start metadata"},
            "TESTER_SELFTEST_SPAWNED=1 TESTER_CONFIG=debug ");

        require_eq(result.exit_code, 0);
        require_true(result.stdout_text.contains("\"type\":\"run_start\""));
        require_true(result.stdout_text.contains("\"cwd\":"));
        require_true(result.stdout_text.contains("\"argv\":["));
        require_true(result.stdout_text.contains("\"config\":\"debug\""));
        require_true(result.stdout_text.contains("\"type\":\"case\""));
        require_true(result.stdout_text.contains("\"type\":\"assertion_passed\""));
        require_true(result.stdout_text.contains("\"type\":\"test\""));
        require_true(result.stdout_text.contains("\"type\":\"run_end\""));
    };

    test_case("test_case [self] jsonl summary mode") = []
    {
        if(std::getenv("TESTER_SELFTEST_SUMMARY") != nullptr)
        {
            require_eq(1, 1);
            return;
        }

        const auto result = run_test_runner({
            "--jsonl=summary",
            "jsonl summary mode"},
            "TESTER_SELFTEST_SUMMARY=1 ");

        require_eq(result.exit_code, 0);
        require_true(result.stdout_text.contains("\"type\":\"run_start\""));
        require_true(result.stdout_text.contains("\"type\":\"summary\""));
        require_true(result.stdout_text.contains("\"type\":\"eof\""));
        require_false(result.stdout_text.contains("\"type\":\"case\""));
        require_false(result.stdout_text.contains("\"type\":\"test\""));
        require_false(result.stdout_text.contains("\"type\":\"assertion_"));
        require_false(result.stdout_text.contains("\"type\":\"run_end\""));
    };

    test_case("test_case [self] jsonl assertion_failed message shape") = []
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--tags=[.jsonl-probe]"});

        require_neq(result.exit_code, 0);
        require_true(result.stdout_text.contains("\"type\":\"assertion_failed\""));
        require_true(result.stdout_text.contains("\"matcher\":\"check_nothrow\""));
        require_true(result.stdout_text.contains("\"message\":\"expected no exception\""));
        require_true(result.stdout_text.contains("\"type\":\"test\""));
        require_false(result.stdout_text.contains("\"success\":true"));
    };

    test_case("test_case [.jsonl-probe] check_nothrow failure") = []
    {
        check_nothrow([] { throw std::runtime_error{"probe"}; });
        require_eq(1, 1);
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::jsonl