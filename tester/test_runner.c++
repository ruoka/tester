import std;
import tester;

int main(int argc, char** argv)
{
    auto tags = std::string_view{};
    if(argc > 1) tags = argv[1];

    auto tr = tester::runner{tags};
    tr.print_test_cases();
    tr.run_tests();
    tr.print_test_results();
    tr.print_test_failures();
    tr.print_test_statistics();
    return 0;
}
