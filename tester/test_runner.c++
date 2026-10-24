// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincomplete-umbrella"
#include <csignal>
#pragma clang diagnostic pop
#include <execinfo.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

import std;
import tester;

using namespace std::literals;

namespace {

using jsonl_observer = tester::output::jsonl::observer;

// The crash line is composed in a signal handler: no allocation, no locks, no streams.
class crash_line
{
public:
    void put(char ch)
    {
        if(m_size < m_buffer.size())
            m_buffer[m_size++] = ch;
    }

    void raw(std::string_view text)
    {
        const auto count = std::min(text.size(), m_buffer.size() - m_size);
        std::ranges::copy(text.substr(0, count), m_buffer.begin() + m_size);
        m_size += count;
    }

    void number(std::uint64_t value)
    {
        const auto [end, error] = std::to_chars(m_buffer.data() + m_size, m_buffer.data() + m_buffer.size(), value);
        if(error == std::errc{})
            m_size = static_cast<std::size_t>(end - m_buffer.data());
    }

    void string(std::string_view text)
    {
        // Escaping can grow a byte to six, and the fields after this one still need room. A cut
        // backs off to a UTF-8 lead byte so a truncated id stays valid UTF-8.
        const auto budget = (m_buffer.size() - m_size - tail_reserve) / 6;
        if(text.size() > budget)
        {
            auto cut = budget;
            while(cut > 0 and (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                --cut;
            text = text.substr(0, cut);
        }

        constexpr auto hex = "0123456789abcdef"sv;
        put('"');
        for(const auto ch : text)
        {
            const auto byte = static_cast<unsigned char>(ch);
            if(ch == '"' or ch == '\\')
            {
                put('\\');
                put(ch);
            }
            else if(byte < 0x20)
            {
                raw("\\u00");
                put(hex[byte >> 4]);
                put(hex[byte & 0xF]);
            }
            else
                put(ch);
        }
        put('"');
    }

    void write(int fd) const
    {
        (void)::write(fd, m_buffer.data(), m_size);
    }

private:
    static constexpr std::size_t tail_reserve = 128;
    std::array<char, 4096> m_buffer{};
    std::size_t m_size = 0;
};

// Copied out of the JSONL observer once it is active, so the handler reads fixed storage rather
// than a std::string another thread may be reassigning.
struct crash_report
{
    std::array<char, 72> run_id{};
    std::array<char, 72> parent_run_id{};

    static void copy(std::array<char, 72>& target, std::string_view value)
    {
        const auto count = std::min(value.size(), target.size() - 1);
        std::ranges::copy(value.substr(0, count), target.begin());
        target[count] = '\0';
    }
};

auto g_crash_report = crash_report{};
volatile std::sig_atomic_t g_jsonl_crash_output = 0;

// Filled by the watchdog before it signals the hung thread, read by whichever of the two writes
// the timeout event. g_reported makes that exactly one of them: atomic_flag is lock-free, so it
// is usable from a signal handler.
struct timeout_report
{
    std::array<char, 4096> test_id{};
    std::uint64_t timeout_ms = 0;
    std::uint64_t elapsed_ms = 0;
};

auto g_timeout_report = timeout_report{};
auto g_reported = std::atomic_flag{};

void emit_event(int signal_number, std::string_view test_id)
{
    const auto timed_out = signal_number == SIGALRM;
    auto line = crash_line{};
    line.raw(timed_out ? "{\"type\":\"timeout\",\"schema\":" : "{\"type\":\"crash\",\"schema\":");
    line.string(jsonl_observer::schema);
    line.raw(",\"version\":");
    line.number(static_cast<std::uint64_t>(jsonl_observer::version));
    line.raw(",\"pid\":");
    line.number(static_cast<std::uint64_t>(::getpid()));
    // clock_gettime is async-signal-safe; std::chrono clocks are not guaranteed to be.
    auto now = ::timespec{};
    if(::clock_gettime(CLOCK_REALTIME, &now) == 0)
    {
        line.raw(",\"ts_unix_ms\":");
        line.number(static_cast<std::uint64_t>(now.tv_sec) * 1000u + static_cast<std::uint64_t>(now.tv_nsec) / 1'000'000u);
    }
    if(const auto run_id = std::string_view{g_crash_report.run_id.data()}; not run_id.empty())
    {
        line.raw(",\"run_id\":");
        line.string(run_id);
    }
    if(const auto parent = std::string_view{g_crash_report.parent_run_id.data()}; not parent.empty())
    {
        line.raw(",\"parent_run_id\":");
        line.string(parent);
    }
    line.raw(",\"signal\":");
    line.number(static_cast<std::uint64_t>(signal_number));
    if(not test_id.empty())
    {
        line.raw(",\"test_id\":");
        line.string(test_id);
    }
    if(timed_out)
    {
        line.raw(",\"timeout_ms\":");
        line.number(g_timeout_report.timeout_ms);
        line.raw(",\"elapsed_ms\":");
        line.number(g_timeout_report.elapsed_ms);
    }
    line.raw("}\n");
    line.write(STDOUT_FILENO);

    const auto result = timed_out ? "RESULT: passed=false timed_out=true\n"sv : "RESULT: passed=false crashed=true\n"sv;
    (void)::write(STDERR_FILENO, result.data(), result.size());
}

void report(int signal_number, std::string_view test_id)
{
    if(g_reported.test_and_set())
        return;

    if(g_jsonl_crash_output)
        emit_event(signal_number, test_id);

    if(signal_number == SIGALRM)
    {
        auto line = crash_line{};
        line.raw("TIMEOUT: ");
        line.raw(test_id.empty() ? "<no test running>"sv : test_id);
        line.raw(" did not finish within ");
        line.number(g_timeout_report.timeout_ms);
        line.raw(" ms (--timeout)\n");
        line.write(STDERR_FILENO);
    }
}

void crash_handler(int signal_number)
{
    // Synchronous faults and abort() are delivered on the thread that raised them, and the
    // watchdog sends SIGALRM to the thread running the hung case, so this is the test (or step)
    // that crashed or hung, also under --jobs.
    report(signal_number, tester::data::current_test_id());

    void* frames[64];
    const auto count = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, count, STDERR_FILENO);

    // Die by the signal itself so the parent's waitpid reports signaled=true. The signal stays
    // blocked while this handler runs; it is delivered with the default action on return.
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}

// --timeout: a thread that watches every running top-level case. C++ cannot stop one thread, so
// an expired case ends the whole run: the watchdog sends SIGALRM to the thread running it, whose
// handler reports the case and prints that thread's stack — where it hangs — before dying by the
// signal. If the handler cannot finish (a wedged stdout, say), the watchdog reports and exits 124.
class timeout_watchdog final : public tester::data::case_watch
{
public:
    timeout_watchdog(std::chrono::milliseconds limit, std::chrono::milliseconds grace)
        : m_limit{limit}
        , m_grace{grace}
    {
        g_timeout_report.timeout_ms = static_cast<std::uint64_t>(limit.count());
        std::signal(SIGALRM, crash_handler);
        tester::data::set_case_watch(this);
    }

    ~timeout_watchdog() override
    {
        tester::data::set_case_watch(nullptr);
    }

    timeout_watchdog(const timeout_watchdog&) = delete;
    timeout_watchdog& operator=(const timeout_watchdog&) = delete;

    void started(std::string_view test_id) override
    {
        auto lock = std::lock_guard{m_mutex};
        m_running.insert_or_assign(std::this_thread::get_id(), running_case{::pthread_self(), clock::now(), std::string{test_id}});
        m_changed.notify_all();
    }

    void finished() override
    {
        auto lock = std::lock_guard{m_mutex};
        m_running.erase(std::this_thread::get_id());
    }

private:
    using clock = std::chrono::steady_clock;

    struct running_case
    {
        ::pthread_t thread;
        clock::time_point started;
        std::string test_id;
    };

    void watch(std::stop_token stop)
    {
        auto lock = std::unique_lock{m_mutex};
        while(not stop.stop_requested())
        {
            // The longest-running case expires first; one that starts later has a later deadline.
            const auto oldest = std::ranges::min_element(m_running, {}, [](const auto& entry) { return entry.second.started; });
            if(oldest == m_running.end())
            {
                m_changed.wait(lock, stop, [&] { return not m_running.empty(); });
                continue;
            }
            if(const auto deadline = oldest->second.started + m_limit; clock::now() < deadline)
            {
                m_changed.wait_until(lock, stop, deadline, [] { return false; });
                continue;
            }
            expire(oldest->second, lock);
        }
    }

    [[noreturn]] void expire(const running_case& hung, std::unique_lock<std::mutex>& lock)
    {
        const auto count = std::min(hung.test_id.size(), g_timeout_report.test_id.size() - 1);
        std::ranges::copy(hung.test_id.substr(0, count), g_timeout_report.test_id.begin());
        g_timeout_report.elapsed_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - hung.started).count());
        ::pthread_kill(hung.thread, SIGALRM);
        lock.unlock();

        std::this_thread::sleep_for(m_grace);
        report(SIGALRM, std::string_view{g_timeout_report.test_id.data()});
        std::_Exit(124);
    }

    std::chrono::milliseconds m_limit;
    std::chrono::milliseconds m_grace;
    std::mutex m_mutex{};
    std::condition_variable_any m_changed{};
    std::map<std::thread::id, running_case> m_running{};
    // Last, so it is joined before the state it watches is destroyed.
    std::jthread m_thread{[this](std::stop_token stop) { watch(stop); }};
};

} // namespace

constexpr auto usage =
R"(test_runner [--help] [--list] [--tags=<tag>]
            [--jsonl[=<summary|failures|trace>]] [--slowest=<N>]
            [--jobs=<N>] [--jsonl-output-max-bytes=<N>] [--result]
            [--junit=<path>] [--xunit-xml=<path>] [--timeout=<seconds>]
            [<tags>]
  --timeout=<seconds>  End the run if one top-level test runs longer (0 = off,
                       the default; TESTER_TIMEOUT sets it too)
Examples:
  test_runner
  test_runner --list
  test_runner --list --jsonl
  test_runner --jsonl=failures --tags=[self]
  test_runner --jsonl=failures --junit=report.xml --tags=[self]
  test_runner --jsonl=trace --slowest=10
  test_runner --jobs=4 --tags=[self]
  test_runner --timeout=60 --jsonl=failures
  test_runner --tags=scenario("My test")
  test_runner --tags=[self][order]
  test_runner --tags="scenario.*Happy"
  test_runner --tags="test_case.*CRUD"
  test_runner --tags="scenario.*path"
  test_runner --tags="^scenario.*test$"
)";

static std::optional<std::size_t> parse_usize(std::string_view sv)
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
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);
    // The first backtrace() can load the unwinder and allocate; do that now rather than in a
    // handler that may have interrupted malloc.
    {
        void* frame[1];
        (void)::backtrace(frame, 1);
    }

    auto command_line = std::span(argv, argc);
    auto arguments = command_line.subspan(1);

    auto list_only = false;
    auto tags = std::string_view{};
    auto output_name = std::string_view{"console"};
    auto result_line = false;
    auto slowest = std::size_t{0};
    // 1 = sequential (default); 0 = hardware_concurrency. Optional CLI override.
    auto jobs = std::optional<std::size_t>{};
    auto jsonl_mode = tester::output::jsonl::jsonl_mode::failures;
    auto output_max_bytes = std::size_t{16384};
    auto junit_path = std::filesystem::path{};
    // --timeout wins over TESTER_TIMEOUT, so a command line can lift a limit the environment sets.
    auto timeout_seconds = std::size_t{0};
    if(const auto* env = std::getenv("TESTER_TIMEOUT"); env != nullptr and env[0] != '\0')
    {
        if(auto parsed = parse_usize(env); parsed.has_value())
            timeout_seconds = *parsed;
        else
        {
            std::clog << "TESTER_TIMEOUT expects a non-negative integer (seconds), got: " << env << std::endl;
            return 1;
        }
    }
    // How long a signalled hung thread gets to report before the watchdog does it and exits 124.
    // Undocumented: it exists so the self tests can reach that path without waiting 5 s.
    auto timeout_grace = std::chrono::milliseconds{5s};
    if(const auto* env = std::getenv("TESTER_TIMEOUT_GRACE_MS"); env != nullptr)
        if(auto parsed = parse_usize(env); parsed.has_value())
            timeout_grace = std::chrono::milliseconds{*parsed};

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

        if(option.starts_with("--jobs="))
        {
            auto value = option.substr(std::string_view{"--jobs="}.size());
            // 0 means hardware_concurrency; reject empty / non-numeric.
            if(auto parsed = parse_usize(value); parsed.has_value())
            {
                jobs = *parsed;
                continue;
            }
            std::clog << "--jobs expects a non-negative integer, got: " << value << std::endl;
            return 1;
        }

        if(option.starts_with("--timeout="))
        {
            auto value = option.substr(std::string_view{"--timeout="}.size());
            if(auto parsed = parse_usize(value); parsed.has_value())
            {
                timeout_seconds = *parsed;
                continue;
            }
            std::clog << "--timeout expects a non-negative integer (seconds), got: " << value << std::endl;
            return 1;
        }

        if(option.starts_with("--jsonl-output-max-bytes="))
        {
            auto value = option.substr(std::string_view{"--jsonl-output-max-bytes="}.size());
            output_max_bytes = parse_usize(value).value_or(16384);
            continue;
        }

        // Additive file sink — may run alongside console or JSONL (not a select_observer).
        if(option.starts_with("--junit=") || option.starts_with("--xunit-xml="))
        {
            const auto prefix = option.starts_with("--junit=")
                ? "--junit="sv
                : "--xunit-xml="sv;
            const auto value = option.substr(prefix.size());
            if(value.empty())
            {
                std::clog << "Missing path for " << prefix.substr(0, prefix.size() - 1) << std::endl;
                return 1;
            }
            junit_path = value;
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
        if(jobs.has_value())
            tester::set_jobs(*jobs);
        auto& jsonl = tester::output::jsonl::observer_instance(std::cout, std::clog, jsonl_mode, output_max_bytes);
        tester::output::register_observer("jsonl", jsonl);
        tester::output::register_observer(
            "console",
            tester::output::console::observer_instance(std::clog, std::clog, result_line));
        if(not tester::output::select_observer(output_name))
        {
            std::clog << "Unknown output observer: " << output_name << '\n';
            return 1;
        }
        // JUnit/xUnit XML is additive: observe() after select_observer so JSONL (or
        // console) keeps the primary stream while the report file is written too.
        if(not junit_path.empty())
        {
            auto& junit = tester::output::junit::observer_instance();
            junit.configure(junit_path);
            tester::output::observe(junit);
        }
        if(output_name == "jsonl")
        {
            crash_report::copy(g_crash_report.run_id, jsonl.run_id());
            crash_report::copy(g_crash_report.parent_run_id, jsonl.parent_run_id());
            g_jsonl_crash_output = 1;
        }

        auto tr = tester::runner{tags};

        if(list_only)
        {
            tr.list_tests();
            return 0;
        }

        {
            auto watchdog = std::optional<timeout_watchdog>{};
            if(timeout_seconds > 0)
                watchdog.emplace(std::chrono::seconds{timeout_seconds}, timeout_grace);
            tr.run_tests();
        }
        tr.report_results();
        tr.report_failures();
        tr.report_summary();
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
