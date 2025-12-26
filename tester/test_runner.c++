// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincomplete-umbrella"
#include <csignal>
#pragma clang diagnostic pop
#include <execinfo.h>
#include <unistd.h>
import std;
import tester;

static volatile sig_atomic_t g_jsonl_enabled = 0;
using namespace std::literals;
static constexpr auto g_default_schema = "tester-jsonl"sv;
static auto g_schema_buf = std::array<char, 64>{};
static auto g_schema_len = std::size_t{0};

constexpr auto usage =
R"(test_runner [--help] [--list] [--tags=<tag>] [--output=<human|jsonl>] [--slowest=<N>]
            [--jsonl-output=<never|failures|always>] [--jsonl-output-max-bytes=<N>] [--result]
            [--schema=<name>]
            [<tags>]
Examples:
  test_runner
  test_runner --list
  test_runner --output=jsonl --schema=ydb-cb-tester-jsonl --jsonl-output=failures --slowest=10
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
    g_schema_len = std::min(g_default_schema.size(), g_schema_buf.size() - 1);
    std::copy_n(g_default_schema.begin(), g_schema_len, g_schema_buf.begin());
    g_schema_buf[g_schema_len] = '\0';

    auto crash_handler = [](int signal)
    {
        if(g_jsonl_enabled)
        {
            // Emit a machine-readable crash line to stdout (keep it JSONL).
            // NOTE: Use only async-signal-safe functions here.
            char buf[256];
            int n = 0;

            auto append_cstr = [&](const char* s)
            {
                while(*s && n < static_cast<int>(sizeof(buf)))
                    buf[n++] = *s++;
            };

            auto append_u = [&](unsigned v)
            {
                char tmp[32];
                int m = 0;
                do { tmp[m++] = static_cast<char>('0' + (v % 10)); v /= 10; } while(v);
                while(m-- > 0 && n < static_cast<int>(sizeof(buf)))
                    buf[n++] = tmp[m];
            };

            append_cstr("{\"type\":\"crash\",\"schema\":\"");
            for(std::size_t i = 0; i < g_schema_len && n < static_cast<int>(sizeof(buf)); ++i)
                buf[n++] = g_schema_buf[i];
            append_cstr("\",\"version\":1,\"pid\":");
            append_u(static_cast<unsigned>(::getpid()));
            append_cstr(",\"signal\":");
            append_u(static_cast<unsigned>(signal));
            append_cstr("}\n");
            ::write(STDOUT_FILENO, buf, static_cast<size_t>(n));

            const char result[] = "RESULT: passed=false crashed=true\n";
            ::write(STDERR_FILENO, result, sizeof(result) - 1);
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
    auto schema = std::string_view{"tester-jsonl"};
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

        if(option.starts_with("--schema="))
        {
            schema = option.substr(std::string_view{"--schema="}.size());
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
        tr.set_schema(schema);
        tr.set_result_line(result_line);
        tr.set_slowest(slowest);
        tr.set_jsonl_output(jsonl_output);
        tr.set_jsonl_output_max_bytes(jsonl_output_max_bytes);

        g_jsonl_enabled = (output == "jsonl" || output == "JSONL");
        if(!schema.empty())
        {
            // Keep crash handler simple: accept only safe schema characters and cap length.
            auto ok = true;
            for(const char ch : schema)
            {
                const auto safe =
                    (ch >= 'a' && ch <= 'z') ||
                    (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '_' || ch == '.' || ch == ':';
                if(!safe) { ok = false; break; }
            }
            if(!ok)
            {
                std::clog << "Invalid --schema (allowed: [A-Za-z0-9._:-])" << std::endl;
                return 1;
            }
            g_schema_len = std::min(schema.size(), g_schema_buf.size() - 1);
            std::copy_n(schema.begin(), g_schema_len, g_schema_buf.begin());
            g_schema_buf[g_schema_len] = '\0';
        }

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
