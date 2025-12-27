// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincomplete-umbrella"
#include <csignal>
#pragma clang diagnostic pop
#include <execinfo.h>
#include <unistd.h>
#include "../tools/jsonl-signal-safe.h++"
import std;
import tester;

static volatile sig_atomic_t g_jsonl_enabled = 0;
using namespace std::literals;
// Schema is defined in jsonl-format.h++ as jsonl_util::jsonl_context<std::ostream>::schema
// We duplicate it here as a string literal to avoid header inclusion conflicts with the std module
static constexpr auto g_schema = "tester-jsonl"sv;
static auto g_schema_buf = std::array<char, 64>{};
static auto g_schema_len = std::size_t{0};

constexpr auto usage =
R"(test_runner [--help] [--list] [--tags=<tag>] [--output=<human|jsonl>] [--slowest=<N>]
            [--jsonl-output=<never|failures|always>] [--jsonl-output-max-bytes=<N>] [--result]
            [<tags>]
Examples:
  test_runner
  test_runner --list
  test_runner --output=jsonl --jsonl-output=failures --slowest=10
  test_runner --tags=scenario("My test")
  test_runner --tags=[acceptor]
  test_runner --tags="scenario.*Happy"
  test_runner --tags="test_case.*CRUD"
  test_runner --tags="scenario.*path"
  test_runner --tags="^scenario.*test$"
)";

static auto parse_usize(std::string_view sv) -> std::optional<std::size_t>
{
    if(sv.empty()) return std::nullopt;
    std::size_t value = 0;
    for(const char ch : sv)
    {
        if(ch < '0' || ch > '9') return std::nullopt;
        value = value * 10u + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

int main(int argc, char** argv)
{
    // Initialize schema used by crash handler (kept in a fixed buffer so the handler stays async-signal-safe).
    g_schema_len = std::min(g_schema.size(), g_schema_buf.size() - 1);
    std::copy_n(g_schema.begin(), g_schema_len, g_schema_buf.begin());
    g_schema_buf[g_schema_len] = '\0';

    auto crash_handler = [](int signal)
    {
        if(g_jsonl_enabled)
        {
            jsonl_util::signal_safe::emit_crash_event_jsonl(
                STDOUT_FILENO,
                STDERR_FILENO,
                static_cast<unsigned>(signal),
                g_schema_buf.data(),
                g_schema_len,
                /*version=*/1,
                /*emit_result_line=*/true);
        }

        void* frames[64];
        auto count = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
        std::_Exit(signal);
    };
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);

    auto command_line = std::span(argv, argc);
    auto arguments = command_line.subspan(1);

    auto list_only = false;
    auto tags = std::string_view{};
    auto output = std::string_view{"human"};
    auto result_line = false;
    auto slowest = std::size_t{0};
    auto jsonl_output = std::string_view{"failures"};
    auto jsonl_output_max_bytes = std::size_t{16384};

    for(std::string_view option : arguments)
    {
        if(option == "--help")
        {
            std::cout << usage << std::endl;
            return 0;
        }

        if(option == "--list")
        {
            list_only = true;
            continue;
        }

        if(option.starts_with("--tags="))
        {
            tags = option.substr(std::string_view{"--tags="}.size());
            continue;
        }

        if(option.starts_with("--output="))
        {
            output = option.substr(std::string_view{"--output="}.size());
            continue;
        }

        if(option == "--result")
        {
            result_line = true;
            continue;
        }

        if(option.starts_with("--slowest="))
        {
            auto value = option.substr(std::string_view{"--slowest="}.size());
            slowest = parse_usize(value).value_or(0);
            continue;
        }

        if(option.starts_with("--jsonl-output="))
        {
            jsonl_output = option.substr(std::string_view{"--jsonl-output="}.size());
            continue;
        }

        if(option.starts_with("--jsonl-output-max-bytes="))
        {
            auto value = option.substr(std::string_view{"--jsonl-output-max-bytes="}.size());
            jsonl_output_max_bytes = parse_usize(value).value_or(16384);
            continue;
        }

        if(option.starts_with("-"))
        {
            std::clog << "Unknown option: " << option << std::endl;
            std::cout << usage << std::endl;
            return 1;
        }

        tags = option;
    }

    try
    {
        auto tr = tester::runner{tags};
        tr.set_output_format(output);
        tr.set_result_line(result_line);
        tr.set_slowest(slowest);
        tr.set_jsonl_output(jsonl_output);
        tr.set_jsonl_output_max_bytes(jsonl_output_max_bytes);

        g_jsonl_enabled = (output == "jsonl" || output == "JSONL");

        if(list_only)
        {
            tr.print_test_cases();
            return 0;
        }

        // In JSONL mode, keep stdout machine-parseable: don't emit the human test list.
        if(!g_jsonl_enabled)
            tr.print_test_cases();
        tr.run_tests();
        tr.print_test_results();
        tr.print_test_failures();
        tr.print_test_statistics();
        return tr.all_tests_passed() ? 0 : 1;
    }
    catch(const tester::assertions::assertion_failure& ex)
    {
        std::clog << "Unhandled assertion failure: " << ex.what() << std::endl;
        return 1;
    }
    catch(const std::exception& ex)
    {
        std::clog << "Unhandled exception: " << ex.what() << std::endl;
        return 1;
    }
    catch(...)
    {
        std::clog << "Unknown exception occurred" << std::endl;
        return 1;
    }
}
