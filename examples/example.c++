import tester;
import foo;

int main()
{
    tester::test_runner tr;
    tr.print_test_cases();
    tr.run_tests();
    tr.print_test_results();
    tr.print_test_statistics();
    return 0;
}
