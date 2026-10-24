// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#include "details/jsonl.h++"
#include "details/selftest_spawn.h++"
#include <pthread.h>
#include <signal.h>

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

    // A shell that does not exec its last command reports a signal death as 128 + n.
    const auto died_by = [](const tester_selftest::spawn_result& result, int signal)
    {
        return (result.signaled and result.signal == signal) or result.exit_code == 128 + signal;
    };

    test_case("test_case [self] jsonl crash event names the crashing test") = [died_by]
    {
        constexpr auto parent = std::string_view{"0123456789abcdef0123456789abcdef"};
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--tags=[.jsonl-crash-probe]"},
            "TESTER_PARENT_RUN_ID=" + std::string{parent});

        // The runner must die by the signal, not exit with its number: exit 6 is
        // indistinguishable from a test runner that chose to return 6.
        require_true(died_by(result, SIGABRT));

        const auto crash = tester_selftest::find_event(result.stdout_text, "crash");
        require_false(crash.empty());
        require_eq(tester_selftest::field(crash, "schema"), std::string{"\"tester-jsonl\""});
        require_eq(tester_selftest::field(crash, "version"), std::string{"2"});
        require_false(tester_selftest::field(crash, "ts_unix_ms").empty());
        require_eq(tester_selftest::field(crash, "signal"), std::to_string(SIGABRT));
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

    test_case("test_case [self] jsonl crash on a --jobs worker names that worker's test") = [died_by]
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--jobs=2",
            "--tags=[.jsonl-crash-jobs-probe]"});

        require_true(died_by(result, SIGSEGV));
        const auto crash = tester_selftest::find_event(result.stdout_text, "crash");
        require_eq(tester_selftest::field(crash, "signal"), std::to_string(SIGSEGV));
        // The tab in the name is a control character, so it must arrive escaped.
        require_true(tester_selftest::field(crash, "test_id").contains(R"(segv\u0009probe)"));
    };

    test_case("test_case [.jsonl-crash-jobs-probe] crash jobs probe finishes") = []
    {
        require_eq(1, 1);
    };

    test_case("test_case [.jsonl-crash-jobs-probe] segv\tprobe") = []
    {
        std::raise(SIGSEGV);
    };

    test_case("test_case [self] console crash and timeout write no JSONL") = [died_by]
    {
        const auto crash = run_test_runner({"--tags=[.jsonl-crash-probe]"});
        require_true(died_by(crash, SIGABRT));
        require_false(crash.stdout_text.contains("\"type\":"));

        const auto timeout = run_test_runner({"--timeout=1", "--tags=[.jsonl-timeout-probe]"});
        require_true(died_by(timeout, SIGALRM));
        require_false(timeout.stdout_text.contains("\"type\":"));
        require_true(timeout.stderr_text.contains("TIMEOUT: "));
        require_true(timeout.stderr_text.contains("timeout probe hangs"));
    };

    const auto check_timeout = [died_by](const tester_selftest::spawn_result& result, std::string_view hung)
    {
        require_true(died_by(result, SIGALRM));

        const auto timeout = tester_selftest::find_event(result.stdout_text, "timeout");
        require_false(timeout.empty());
        require_eq(tester_selftest::field(timeout, "version"), std::string{"2"});
        require_eq(tester_selftest::field(timeout, "signal"), std::to_string(SIGALRM));
        require_eq(tester_selftest::field(timeout, "timeout_ms"), std::string{"1000"});
        require_true(std::stoull(tester_selftest::field(timeout, "elapsed_ms")) >= 1000);
        require_true(tester_selftest::field(timeout, "test_id").contains(hung));
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
            "--tags=[.jsonl-timeout-probe]"}),
            "timeout probe hangs");
    };

    test_case("test_case [self] jsonl timeout from TESTER_TIMEOUT, sequential") = [check_timeout]
    {
        check_timeout(run_test_runner({
            "--jsonl=failures",
            "--tags=[.jsonl-timeout-probe]"},
            "TESTER_TIMEOUT=1"),
            "timeout probe hangs");
    };

    test_case("test_case [self] jsonl timeout in a step names the step") = [check_timeout]
    {
        // A step has no deadline of its own: it runs inside the scenario's, and the event
        // names the step that was running when that one expired.
        check_timeout(run_test_runner({
            "--jsonl=failures",
            "--timeout=1",
            "--tags=[.jsonl-timeout-step-probe]"}),
            "a step that never returns");
    };

    test_case("test_case [self] jsonl timeout falls back to exit 124 when the hung thread cannot report") = []
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--timeout=1",
            "--tags=[.jsonl-timeout-fallback-probe]"},
            "TESTER_TIMEOUT_GRACE_MS=200");

        require_false(result.signaled);
        require_eq(result.exit_code, 124);
        const auto timeout = tester_selftest::find_event(result.stdout_text, "timeout");
        require_true(tester_selftest::field(timeout, "test_id").contains("blocks SIGALRM"));
        require_eq(tester_selftest::field(timeout, "timeout_ms"), std::string{"1000"});
        require_true(result.stderr_text.contains("TIMEOUT: "));
    };

    test_case("test_case [self] jsonl --timeout leaves a passing run alone") = []
    {
        // The watchdog must neither fire on cases that finish nor hold up exit: the run still
        // ends with summary and eof, and promptly.
        for(const auto& [args, env] : std::vector<std::pair<std::vector<std::string>, std::string>>{
                {{"--jsonl=failures", "--jobs=2", "--timeout=30", "--tags=[.jsonl-timeout-pass-probe]"}, ""},
                {{"--jsonl=failures", "--tags=[.jsonl-timeout-pass-probe]"}, "TESTER_TIMEOUT=30"}})
        {
            const auto started = std::chrono::steady_clock::now();
            const auto result = run_test_runner(args, env);
            const auto elapsed = std::chrono::steady_clock::now() - started;

            require_false(result.signaled);
            require_eq(result.exit_code, 0);
            require_eq(tester_selftest::event_field(result.stdout_text, "summary", "passed"), std::string{"true"});
            require_false(tester_selftest::find_event(result.stdout_text, "eof").empty());
            require_true(tester_selftest::find_event(result.stdout_text, "timeout").empty());
            require_true(elapsed < std::chrono::seconds{10});
        }
    };

    test_case("test_case [self] jsonl --timeout=0 lifts TESTER_TIMEOUT") = []
    {
        const auto result = run_test_runner({
            "--jsonl=failures",
            "--timeout=0",
            "--tags=[.jsonl-timeout-slow-probe]"},
            "TESTER_TIMEOUT=1");

        require_eq(result.exit_code, 0);
        require_eq(tester_selftest::event_field(result.stdout_text, "summary", "passed"), std::string{"true"});
    };

    test_case("test_case [self] jsonl rejects a malformed timeout") = []
    {
        const auto flag = run_test_runner({"--jsonl=failures", "--timeout=soon", "--tags=[.jsonl-timeout-pass-probe]"});
        require_eq(flag.exit_code, 1);
        require_true(flag.stderr_text.contains("--timeout expects"));

        const auto env = run_test_runner({"--jsonl=failures", "--tags=[.jsonl-timeout-pass-probe]"}, "TESTER_TIMEOUT=soon");
        require_eq(env.exit_code, 1);
        require_true(env.stderr_text.contains("TESTER_TIMEOUT expects"));
    };

    test_case("test_case [.jsonl-timeout-probe] timeout probe finishes") = []
    {
        require_eq(1, 1);
    };

    test_case("test_case [.jsonl-timeout-probe] timeout probe hangs") = []
    {
        std::this_thread::sleep_for(std::chrono::seconds{60});
    };

    test_case("test_case [.jsonl-timeout-pass-probe] quick one") = []
    {
        require_eq(1, 1);
    };

    test_case("test_case [.jsonl-timeout-pass-probe] quick two") = []
    {
        require_eq(2, 2);
    };

    test_case("test_case [.jsonl-timeout-slow-probe] outlives one second") = []
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    };

    test_case("test_case [.jsonl-timeout-fallback-probe] blocks SIGALRM and hangs") = []
    {
        // With the signal blocked the handler never runs, so only the watchdog can report.
        auto mask = ::sigset_t{};
        sigemptyset(&mask);
        sigaddset(&mask, SIGALRM);
        ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
        std::this_thread::sleep_for(std::chrono::seconds{60});
    };

    {
        using tester::behavior_driven_development::given;
        using tester::behavior_driven_development::scenario;
        using tester::behavior_driven_development::when;

        scenario("scenario [.jsonl-timeout-step-probe] a scenario whose step hangs") = []
        {
            given("a scenario under --timeout") = []
            {
                when("a step that never returns") = []
                {
                    std::this_thread::sleep_for(std::chrono::seconds{60});
                };
            };
        };
    }

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::jsonl