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

constexpr auto usage =
R"(test_runner [--help] [--list] [--tags=<tag>] [<tags>]
Examples:
  test_runner
  test_runner --list
  test_runner --tags=scenario("My test")
  test_runner --tags=[acceptor]
  test_runner --tags="scenario.*Happy"
  test_runner --tags="test_case.*CRUD"
  test_runner --tags="scenario.*path"
  test_runner --tags="^scenario.*test$"
)";

int main(int argc, char** argv)
{
    g_jsonl_enabled = []{
        if(const char* v = std::getenv("TESTER_OUTPUT"))
            return std::string_view{v} == "jsonl" || std::string_view{v} == "JSONL";
        return false;
    }();

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

            append_cstr("{\"type\":\"crash\",\"schema\":\"tester-jsonl\",\"version\":1,\"pid\":");
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

        if(option.starts_with("-"))
        {
            std::cerr << "Unknown option: " << option << std::endl;
            std::cout << usage << std::endl;
            return 1;
        }

        tags = option;
    }

    try
    {
        auto tr = tester::runner{tags};

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
        std::cerr << "Unhandled assertion failure: " << ex.what() << std::endl;
        return 1;
    }
    catch(const std::exception& ex)
    {
        std::cerr << "Unhandled exception: " << ex.what() << std::endl;
        return 1;
    }
    catch(...)
    {
        std::cerr << "Unknown exception occurred" << std::endl;
        return 1;
    }
}
