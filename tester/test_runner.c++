import std;
import tester;

constexpr auto usage =
R"(test_runner [--help] [--list] [--tags=<tag>] [<tags>]
Examples:
  test_runner
  test_runner --list
  test_runner --tags=scenario("My test")
)";

int main(int argc, char** argv)
{
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
