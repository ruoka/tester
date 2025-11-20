import readme_unit_example;
import tester;

namespace readme_unit_example {
    auto register_tests()
    {
        using tester::basic::test_case;
        using namespace tester::assertions;

        test_case("foo::add handles signed math") = [] {
            require_eq(add(2, 2), 4);
            require_eq(add(-5, 3), -2);
            check_eq(add(0, 0), 0); // non-fatal variant
        };

        test_case("foo::add with floating-point inputs") = [] {
            require_eq(0.3, 0.1 + 0.2);         // default epsilon path
            check_near(0.3, 0.1 + 0.2, 1e-9);   // explicit tolerance
            require_near(0.0, add(1.0, -1.0));  // fatal variant
        };

        return 0;
    }

    const auto _ = register_tests();
} // namespace readme_unit_example

