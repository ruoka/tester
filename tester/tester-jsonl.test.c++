// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#include "details/jsonl.h++"
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

    // ========================================================================
    // Escaping. jsonl::escape lives in a plain header, so it is testable
    // directly — no child process needed.
    // ========================================================================

    test_case("test_case [self] escape handles the mandatory JSON escapes") = []
    {
        using ::jsonl::escape;

        require_eq(escape("plain"), std::string{"plain"});
        require_eq(escape("quote\"here"), std::string{"quote\\\"here"});
        require_eq(escape("back\\slash"), std::string{"back\\\\slash"});
        require_eq(escape("line\nbreak"), std::string{"line\\nbreak"});
        require_eq(escape("tab\there"), std::string{"tab\\there"});
        require_eq(escape("\b\f\r"), std::string{"\\b\\f\\r"});
        // Other C0 controls use the \u form; DEL is legal unescaped in JSON.
        require_eq(escape(std::string_view{"\x01\x1f"}), std::string{"\\u0001\\u001f"});
        require_eq(escape(std::string_view{"\x7f"}), std::string{"\x7f"});
        // An embedded NUL is data, not a terminator.
        require_eq(escape(std::string_view{"a\0b", 3}), std::string{"a\\u0000b"});
    };

    test_case("test_case [self] escape preserves well-formed UTF-8") = []
    {
        using ::jsonl::escape;

        const auto two_byte = std::string{"caf\xc3\xa9"};             // café
        const auto three_byte = std::string{"\xe2\x82\xac"};          // €
        const auto four_byte = std::string{"\xf0\x9f\x92\xa9"};       // U+1F4A9
        require_eq(escape(two_byte), two_byte);
        require_eq(escape(three_byte), three_byte);
        require_eq(escape(four_byte), four_byte);
    };

    test_case("test_case [self] escape replaces invalid UTF-8") = []
    {
        using ::jsonl::escape;

        const auto replacement = std::string{"\xef\xbf\xbd"};

        // Lone continuation byte, bare high byte, and a truncated 3-byte sequence.
        require_eq(escape(std::string_view{"\x80"}), replacement);
        require_eq(escape(std::string_view{"\xff"}), replacement);
        require_eq(escape(std::string_view{"\xe2\x82"}), replacement + replacement);
        // Latin-1 text: one replacement per invalid byte, valid bytes untouched.
        require_eq(escape(std::string_view{"caf\xe9"}), std::string{"caf"} + replacement);
        // Overlong encoding of '/' and a UTF-16 surrogate are both rejected.
        require_eq(escape(std::string_view{"\xc0\xaf"}), replacement + replacement);
        require_eq(escape(std::string_view{"\xed\xa0\x80"}), replacement + replacement + replacement);
        // Valid text after an invalid byte still survives.
        require_eq(escape(std::string_view{"\xff\xc3\xa9"}), replacement + "\xc3\xa9");
    };

    test_case("test_case [self] non-utf8 assertion data still yields parseable JSONL") = []
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--tags=[.jsonl-utf8-probe]"});

        require_eq(result.exit_code, 1);

        // Every emitted byte must belong to a valid UTF-8 sequence, or downstream
        // json.loads fails on the whole stream — including the project's own MCP
        // bridge. Scan first and assert once: a per-byte assertion would add
        // thousands of entries to the run's assertion count.
        auto invalid_at = std::optional<std::size_t>{};
        for(std::size_t index = 0; index < result.stdout_text.size();)
        {
            const auto length = ::jsonl::utf8_sequence_length(result.stdout_text, index);
            if(length == 0)
            {
                invalid_at = index;
                break;
            }
            index += length;
        }
        require_false(invalid_at.has_value());

        // The invalid operand still has to be reported, not dropped.
        require_true(result.stdout_text.contains("\"type\":\"assertion_failed\""));
        require_true(result.stdout_text.contains("\"matcher\":\"check_eq\""));
    };

    test_case("test_case [.jsonl-utf8-probe] invalid utf8 in assertion data") = []
    {
        check_eq(std::string{"caf\xe9 \xff\xfe"}, std::string{"expected"});
    };

    test_case("test_case [self] non-finite float operands are quoted, not bare") = []
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--tags=[.jsonl-nonfinite-probe]"});

        require_eq(result.exit_code, 1);

        // JSON has no infinity and no NaN. Written as numbers they made `"actual":inf`,
        // which json.loads rejects — and it rejects the line, not the token, so one float
        // assertion cost a consumer the whole event. Quoted, the value survives and the
        // line parses; dropping it or writing null would parse and say nothing.
        const auto first = tester_selftest::find_event(result.stdout_text, "assertion_failed", 0);
        require_eq(tester_selftest::field(first, "actual"), std::string{"\"inf\""});
        require_eq(tester_selftest::field(first, "expected"), std::string{"1"});

        const auto second = tester_selftest::find_event(result.stdout_text, "assertion_failed", 1);
        require_eq(tester_selftest::field(second, "actual"), std::string{"\"-inf\""});

        const auto third = tester_selftest::find_event(result.stdout_text, "assertion_failed", 2);
        require_eq(tester_selftest::field(third, "actual"), std::string{"\"nan\""});

        // A finite operand stays a number: quoting everything would make every arithmetic
        // operand a string for the sake of three values.
        const auto fourth = tester_selftest::find_event(result.stdout_text, "assertion_failed", 3);
        require_eq(tester_selftest::field(fourth, "actual"), std::string{"0.5"});
    };

    test_case("test_case [.jsonl-nonfinite-probe] non-finite float operands") = []
    {
        const auto infinity = std::numeric_limits<double>::infinity();

        check_eq(infinity, 1.0);
        check_eq(-infinity, 1.0);
        check_eq(std::nan(""), 1.0);
        check_eq(0.5, 1.0);
    };

    test_case("test_case [self] jsonl crash event names the crashing test") = []
    {
        constexpr auto parent = std::string_view{"0123456789abcdef0123456789abcdef"};
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--tags=[.jsonl-crash-probe]"},
            "TESTER_PARENT_RUN_ID=" + std::string{parent});

        // The runner must die by the signal, not exit with its number: exit 6 is
        // indistinguishable from a test runner that chose to return 6. A shell that does not
        // exec its last command reports the signal as 128 + n instead.
        constexpr auto sigabrt = 6;
        require_true((result.signaled and result.signal == sigabrt) or result.exit_code == 128 + sigabrt);

        const auto crash = tester_selftest::find_event(result.stdout_text, "crash");
        require_false(crash.empty());
        require_eq(tester_selftest::field(crash, "schema"), std::string{"\"tester-jsonl\""});
        require_eq(tester_selftest::field(crash, "version"), std::string{"2"});
        require_false(tester_selftest::field(crash, "ts_unix_ms").empty());
        require_eq(tester_selftest::field(crash, "signal"), std::to_string(sigabrt));
        require_eq(tester_selftest::field(crash, "parent_run_id"), "\"" + std::string{parent} + "\"");
        require_eq(
            tester_selftest::field(crash, "run_id"),
            tester_selftest::field(tester_selftest::find_event(result.stdout_text, "run_start"), "run_id"));
        require_true(tester_selftest::field(crash, "test_id").contains(R"(crash \"probe\")"));
    };

    test_case(R"(test_case [.jsonl-crash-probe] crash "probe")") = []
    {
        std::abort();
    };

    // SIGALRM is 14 on Linux and macOS; a shell that does not exec its last command reports
    // 128 + 14 instead.
    constexpr auto sigalrm = 14;
    const auto check_timeout = [](const tester_selftest::spawn_result& result)
    {
        require_true((result.signaled and result.signal == sigalrm) or result.exit_code == 128 + sigalrm);

        const auto timeout = tester_selftest::find_event(result.stdout_text, "timeout");
        require_false(timeout.empty());
        require_eq(tester_selftest::field(timeout, "version"), std::string{"2"});
        require_eq(tester_selftest::field(timeout, "signal"), std::to_string(sigalrm));
        require_eq(tester_selftest::field(timeout, "timeout_ms"), std::string{"1000"});
        require_true(std::stoull(tester_selftest::field(timeout, "elapsed_ms")) >= 1000);
        require_true(tester_selftest::field(timeout, "test_id").contains("timeout probe hangs"));
        require_false(tester_selftest::field(timeout, "run_id").empty());
        require_true(tester_selftest::find_event(result.stdout_text, "summary").empty());
        require_true(result.stderr_text.contains("TIMEOUT: "));
    };

    test_case("test_case [self] jsonl timeout names the hung test under --jobs") = [check_timeout]
    {
        // The quick case runs beside the hung one and must not be blamed.
        check_timeout(run_test_runner({
            "--jsonl=failures",
            "--jobs=2",
            "--timeout=1",
            "--tags=[.jsonl-timeout-probe]"}));
    };

    test_case("test_case [self] jsonl timeout from TESTER_TIMEOUT, sequential") = [check_timeout]
    {
        check_timeout(run_test_runner({
            "--jsonl=failures",
            "--tags=[.jsonl-timeout-probe]"},
            "TESTER_TIMEOUT=1"));
    };

    test_case("test_case [.jsonl-timeout-probe] timeout probe finishes") = []
    {
        require_eq(1, 1);
    };

    test_case("test_case [.jsonl-timeout-probe] timeout probe hangs") = []
    {
        std::this_thread::sleep_for(std::chrono::seconds{60});
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::jsonl