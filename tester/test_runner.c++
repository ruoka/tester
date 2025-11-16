#include <csignal>
#include <cstdlib>
#include <execinfo.h>
#include <unistd.h>
import std;
import tester;

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
    auto crash_handler = [](int signal)
    {
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

        tr.print_test_cases();
        tr.run_tests();
        tr.print_test_results();
        tr.print_test_failures();
        tr.print_test_statistics();
        return 0;
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
