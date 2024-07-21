import std;
import tester;

int main(int argc, char** argv)
{
    auto tests = std::string_view{};
    if(argc > 1) tests = argv[1];

    auto tr = tester::test_runner{tests};
    tr.print_test_cases();
    tr.run_tests();
    tr.print_test_results();
    tr.print_test_failures();
    tr.print_test_statistics();
    return 0;
}
