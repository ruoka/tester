// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#include "details/selftest_spawn.h++"

import std;
import tester;

namespace tester::selftest::tags {

auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;
    using tester_selftest::run_test_runner;

    test_case("test_case [self][alpha] tagged alpha") = []
    {
        require_eq(1, 1);
    };

    test_case("test_case [self][beta] tagged beta") = []
    {
        require_eq(2, 2);
    };

    test_case("test_case [self] tag filter alpha only") = []
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--list",
            "--tags=[alpha]"});

        require_eq(result.exit_code, 0);
        require_true(result.stdout_text.contains("\"tags\":[\"self\",\"alpha\"]"));
        require_false(result.stdout_text.contains("\"tags\":[\"self\",\"beta\"]"));

        const auto summary_pos = result.stdout_text.find("\"type\":\"test_list_summary\"");
        require_neq(summary_pos, std::string::npos);
        const auto summary_line = result.stdout_text.substr(summary_pos, 256);
        require_true(summary_line.contains("\"matched_total\":1"));
    };

    test_case("test_case [.hidden-probe] hidden from default run") = []
    {
        require_eq(1, 1);
    };

    test_case("test_case [self] hidden tag excluded unless requested") = []
    {
        const auto default_list = run_test_runner({
            "--jsonl=failures",
            "--list"});

        require_eq(default_list.exit_code, 0);
        require_false(default_list.stdout_text.contains(".hidden-probe"));

        const auto tagged_list = run_test_runner({
            "--jsonl=failures",
            "--list",
            "--tags=[.hidden-probe]"});

        require_eq(tagged_list.exit_code, 0);
        require_true(tagged_list.stdout_text.contains("\"tags\":[\".hidden-probe\"]"));
    };

    test_case("test_case [self] unmatched tag filter fails the run") = []
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--tags=[.no-such-tag-ever]"});

        // system() status: normal exit keeps the low 7 bits clear; signals do not.
        require_eq(result.exit_code & 0x7f, 0);
        require_neq(result.exit_code, 0);

        const auto summary_pos = result.stdout_text.rfind("\"type\":\"summary\"");
        require_neq(summary_pos, std::string::npos);
        const auto summary_end = result.stdout_text.find('\n', summary_pos);
        const auto summary_line = result.stdout_text.substr(
            summary_pos,
            summary_end == std::string::npos ? std::string::npos : summary_end - summary_pos);
        require_true(summary_line.contains("\"passed\":false"));
        require_true(summary_line.contains("\"tests_total\":0"));
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::tags