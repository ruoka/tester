# C++ module-based testing framework with C++23 features

##Introduction

This C++20 module-based testing framework leverages C++23 features for macro-free unit and BDD testing with familiar, easy-to-use assertions, lambdas, and a small footprint, promoting improved maintainability.

## Makefile

make clean deps run_examples

## Simple example

```c++
module foo;
import tester;

namespace foo
{
    auto test_set()
    {
        using tester::basic::test_case;
        using namespace tester::assertions;
    
        test_case("Module foo's unit tests") = []
        {
            require_eq(foo::x, 1); // exported form the module foo
            require_eq(foo::y, 2); // internal to the module foo
        };
    
        return 0;
    };
    
    const auto test_registrar = test_set();
}

```

## BDD example
```c++
#include <stdexcept>
import tester;

namespace example_1 {

using namespace tester::behavior_driven_development;
using namespace tester::assertions;

auto feature()
{
    scenario("Test case 1a") = []
    {
        given("Customer wants to buy food") = []
        {
            when("customers goes to a restaurant") = []
            {
                then("customer makes an order") = []
                {
                    and_then("food and drinks are delivered to the customer") = []
                    {
                        require_eq(123.45, 123.45);
                        require_neq(123.34, 456.876);
                        require_lt(123.34, 234.56);
                        require_lteq(123.34, 234.56);
                        require_gt(123.45, 123.4);
                        require_gteq(123.45, 123.4);
                        require_true(123.45 != 123.4);
                        require_false(123.45 == 123.4);
                    };
                };
            };

            when("customers calls WOLT") = []
            {
                and_when("WOLT driver is available") = []
                {
                    then("food and drinks are delievred to his home door") = []
                    {
                        require_false(true);
                        require_true(false);
                        require_nothrow([]{});
                        require_throw([]{throw 1;});
                    };
                };
            };
        };
    };

    scenario("Test case 1b") = []
    {
        given("Customer wants to drink only beer") = []
        {
            when("customers goes to a restaurant") = []
            {
                then("customer gets wasted") = []
                {
                    require_gteq(2,2);
                    require_gteq(3,2);
                    require_lteq(2.2,2.2);
                    require_lteq(2.2,3.2);
                    require_lteq(2.2,2);
                    require_lteq(2.2,3);
                    throw std::runtime_error{"TestException!"};
                };
            };
        };
    };

    return 0;
}

const auto test_registrar = feature();

}
```
## Test runner

```c++
import std;
import tester;

int main()
{
    auto tr = tester::runner{tags};
    tr.print_test_cases();
    tr.run_tests();
    tr.print_test_results();
    tr.print_test_failures();
    tr.print_test_statistics();
    return 0;
}
```
