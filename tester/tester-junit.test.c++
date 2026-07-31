// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#include "details/selftest_spawn.h++"
#include "details/xml.h++"

import std;
import tester;

namespace tester::selftest::junit {

namespace {

auto temp_report_path(std::string_view stem)
{
    return std::filesystem::temp_directory_path()
        / (std::string{stem} + "_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + ".xml");
}

auto remove_quietly(const std::filesystem::path& path)
{
    auto ec = std::error_code{};
    std::filesystem::remove(path, ec);
}

} // namespace

auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;
    using tester_selftest::run_test_runner;
    using tester_selftest::read_file_text;

    test_case("test_case [self] xml escape handles markup and controls") = []
    {
        using ::xml::escape;

        require_eq(escape("plain"), std::string{"plain"});
        require_eq(escape("a&b"), std::string{"a&amp;b"});
        require_eq(escape("<tag>"), std::string{"&lt;tag&gt;"});
        require_eq(escape("\"q\" 's'"), std::string{"&quot;q&quot; &apos;s&apos;"});
        require_eq(escape("line\nbreak"), std::string{"line\nbreak"});
        // C0 controls other than tab/LF/CR are not Char in XML 1.0.
        require_eq(escape(std::string_view{"\x01"}), std::string{"\xef\xbf\xbd"});
        require_eq(escape(std::string_view{"\xff"}), std::string{"\xef\xbf\xbd"});
    };

    test_case("test_case [self] junit report writes alongside jsonl") = []
    {
        if(std::getenv("TESTER_SELFTEST_JUNIT") != nullptr)
        {
            require_eq(1, 1);
            return;
        }

        const auto report = temp_report_path("tester_junit_parallel");
        remove_quietly(report);

        const auto result = run_test_runner({
            "--jsonl=failures",
            "--junit=" + report.string(),
            "junit report writes alongside jsonl"},
            "TESTER_SELFTEST_JUNIT=1 ");

        require_eq(result.exit_code, 0);
        require_true(result.stdout_text.contains("\"type\":\"summary\""));
        require_true(result.stdout_text.contains("\"type\":\"eof\""));
        require_true(std::filesystem::exists(report));

        const auto xml = read_file_text(report);
        remove_quietly(report);

        require_true(xml.contains("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
        require_true(xml.contains("<testsuites "));
        require_true(xml.contains("<testsuite "));
        require_true(xml.contains("junit report writes alongside jsonl"));
        require_true(xml.contains("failures=\"0\""));
        require_true(xml.contains("errors=\"0\""));
        require_true(xml.contains("name=\"tags\""));
    };

    test_case("test_case [self] junit maps assertion failures and errors") = []
    {
        const auto report = temp_report_path("tester_junit_failures");
        remove_quietly(report);

        const auto result = run_test_runner({
            "--jsonl=failures",
            "--junit=" + report.string(),
            "--tags=[.junit-probe]"});

        require_neq(result.exit_code, 0);
        require_true(result.stdout_text.contains("\"type\":\"summary\""));
        require_true(std::filesystem::exists(report));

        const auto xml = read_file_text(report);
        remove_quietly(report);

        require_true(xml.contains("<failure "));
        require_true(xml.contains("type=\"check_eq\"") || xml.contains("type=\"require_eq\""));
        require_true(xml.contains("<error "));
        require_true(xml.contains("std::runtime_error") || xml.contains("runtime_error"));
        require_true(xml.contains("escaped the junit probe"));
    };

    test_case("test_case [.junit-probe] assertion failure") = []
    {
        check_eq(1, 2);
    };

    test_case("test_case [.junit-probe] uncaught exception") = []
    {
        throw std::runtime_error{"escaped the junit probe"};
    };

    test_case("test_case [self] junit empty filter emits a synthetic failure") = []
    {
        const auto report = temp_report_path("tester_junit_empty");
        remove_quietly(report);

        const auto result = run_test_runner({
            "--jsonl=failures",
            "--junit=" + report.string(),
            "--tags=[.junit-no-such-tag-xyz]"});

        require_neq(result.exit_code, 0);
        require_true(std::filesystem::exists(report));

        const auto xml = read_file_text(report);
        remove_quietly(report);

        require_true(xml.contains("no tests matched filter"));
        require_true(xml.contains("failures=\"1\""));
        require_true(xml.contains("type=\"empty_filter\""));
    };

    test_case("test_case [self] xunit-xml alias writes the same report") = []
    {
        if(std::getenv("TESTER_SELFTEST_XUNIT") != nullptr)
        {
            require_eq(1, 1);
            return;
        }

        const auto report = temp_report_path("tester_xunit_alias");
        remove_quietly(report);

        const auto result = run_test_runner({
            "--jsonl=summary",
            "--xunit-xml=" + report.string(),
            "xunit-xml alias writes the same report"},
            "TESTER_SELFTEST_XUNIT=1 ");

        require_eq(result.exit_code, 0);
        require_true(std::filesystem::exists(report));
        const auto xml = read_file_text(report);
        remove_quietly(report);
        require_true(xml.contains("<testsuites "));
        require_true(xml.contains("xunit-xml alias writes the same report"));
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::junit
