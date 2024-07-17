#include<compare>
#include <iostream>
import tester;

namespace example_1 {

using namespace tester::behavior_driven_development;
using namespace tester::assertions;

auto test_set()
{
    scenario("Test case 1a") = []
    {
        given("Customer wants to buy food") = []
        {
            when("customers goes to a restaurant") = []
            {
                // throw std::runtime_error{"TestException!"};

                then("customer makes an order") = []
                {
                    and_then("food and drinks are delivered to the customer") = []
                    {
                        std::clog << std::boolalpha << require_eq(123.34,456.876) << std::endl;
                        std::clog << std::boolalpha << require_eq(123.34,123.34) << std::endl;
                        std::clog << std::boolalpha << require_neq(123.34,456.876) << std::endl;
                        std::clog << std::boolalpha << require_neq(123.34,123.34) << std::endl;
                    };
                };
            };

            when("customers calls WOLT") = []
            {
                and_when("WOLT driver is available") = []
                {
                    // throw std::runtime_error{"TestException!"};

                    then("food and drinks are delievred to his home door") = []
                    {
                        require_false(true);
                        require_true(false);
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

const auto test_registrar = test_set();

}
