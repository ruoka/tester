// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

module;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincomplete-umbrella"
#include <csignal>
#pragma clang diagnostic pop
#include <execinfo.h>
#include <unistd.h>
module tester;
import std;
import :console_observer;
import :jsonl_observer;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmain-attached-to-named-module"
using namespace std::literals;

namespace {

auto append_cstr(char* buffer, std::size_t size, std::size_t capacity, const char* text)
{
    while(*text && size < capacity)
        buffer[size++] = *text++;
    return size;
}

auto append_unsigned(char* buffer, std::size_t size, std::size_t capacity, unsigned value)
{
    auto temporary = std::array<char, 32>{};
    auto count = std::size_t{};
    do
    {
        temporary[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    while(value);

    while(count > 0 && size < capacity)
        buffer[size++] = temporary[--count];
    return size;
}

void emit_crash_event(
    int stdout_fd,
    int stderr_fd,
    unsigned signal_number,
    const char* schema,
    std::size_t schema_length,
    int version,
    bool emit_result_line)
{
    auto buffer = std::array<char, 256>{};
    auto size = std::size_t{};

    size = append_cstr(buffer.data(), size, buffer.size(), "{\"type\":\"crash\",\"schema\":\"");
    for(auto index = std::size_t{}; index < schema_length && size < buffer.size(); ++index)
        buffer[size++] = schema[index];
    size = append_cstr(buffer.data(), size, buffer.size(), "\",\"version\":");
    size = append_unsigned(buffer.data(), size, buffer.size(), static_cast<unsigned>(version));
    size = append_cstr(buffer.data(), size, buffer.size(), ",\"pid\":");
    size = append_unsigned(buffer.data(), size, buffer.size(), static_cast<unsigned>(::getpid()));
    size = append_cstr(buffer.data(), size, buffer.size(), ",\"signal\":");
    size = append_unsigned(buffer.data(), size, buffer.size(), signal_number);
    size = append_cstr(buffer.data(), size, buffer.size(), "}\n");

    (void)::write(stdout_fd, buffer.data(), size);

    if(emit_result_line)
    {
        constexpr char result[] = "RESULT: passed=false crashed=true\n";
        (void)::write(stderr_fd, result, sizeof(result) - 1);
    }
}

} // namespace

// Schema is defined in jsonl.h++ as jsonl::jsonl_context<std::ostream>::schema
// We duplicate it here as a string literal to avoid header inclusion conflicts with the std module
static constexpr auto g_schema = "tester-jsonl"sv;
static auto g_schema_buf = std::array<char, 64>{};
static auto g_schema_len = std::size_t{0};

constexpr auto usage =
R"(test_runner [--help] [--list] [--tags=<tag>]
            [--jsonl[=<summary|failures|trace>]] [--slowest=<N>]
            [--jsonl-output-max-bytes=<N>] [--result]
            [<tags>]
Examples:
  test_runner
  test_runner --list
  test_runner --list --jsonl
  test_runner --jsonl=failures --tags=[self]
  test_runner --jsonl=trace --slowest=10
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

    static volatile std::sig_atomic_t jsonl_crash_output{};

    auto crash_handler = [](int signal)
    {
        if(jsonl_crash_output)
        {
            emit_crash_event(
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
    auto output_name = std::string_view{"console"};
    auto result_line = false;
    auto slowest = std::size_t{0};
    auto jsonl_mode = tester::output::jsonl::jsonl_mode::failures;
    auto output_max_bytes = std::size_t{16384};

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

        if(option == "--jsonl")
        {
            output_name = "jsonl";
            continue;
        }

        if(option.starts_with("--jsonl="))
        {
            const auto mode = option.substr(std::string_view{"--jsonl="}.size());
            if(mode == "summary")
                jsonl_mode = tester::output::jsonl::jsonl_mode::summary;
            else if(mode == "failures")
                jsonl_mode = tester::output::jsonl::jsonl_mode::failures;
            else if(mode == "trace")
                jsonl_mode = tester::output::jsonl::jsonl_mode::trace;
            else
            {
                std::clog << "Unknown JSONL mode: " << mode << std::endl;
                return 1;
            }
            output_name = "jsonl";
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

        if(option.starts_with("--jsonl-output-max-bytes="))
        {
            auto value = option.substr(std::string_view{"--jsonl-output-max-bytes="}.size());
            output_max_bytes = parse_usize(value).value_or(16384);
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
        // Use the public API of the tester module.
        // It handles its own internal configuration.
        tester::set_run_argv(argc, argv);
        tester::set_slowest(slowest);
        tester::output::register_observer(
            "jsonl",
            tester::output::jsonl::observer_instance(std::cout, std::clog, jsonl_mode, output_max_bytes));
        tester::output::register_observer(
            "console",
            tester::output::console::observer_instance(std::clog, std::clog, result_line));
        if(not tester::output::select_observer(output_name))
        {
            std::clog << "Unknown output observer: " << output_name << '\n';
            return 1;
        }
        jsonl_crash_output = output_name == "jsonl";

        auto tr = tester::runner{tags};

        if(list_only)
        {
            tr.print_test_cases();
            return 0;
        }

        tr.run_tests();
        tr.print_test_results();
        tr.print_test_failures();
        tr.print_test_statistics();
        tester::publisher::emit_output_eof();
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

#pragma clang diagnostic pop
