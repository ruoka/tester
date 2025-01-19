#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <iostream>
import tester;

namespace example_2 {

auto feature()
{
    using namespace std::literals;
    using namespace tester::behavior_driven_development;
    using namespace tester::assertions;

    scenario("Test case 2a") = []
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
                        require_eq(123.34,456.876);
                        require_eq(123.34,123.34);
                        require_neq(123.34,456.876);
                        require_neq(123.34,123.34);
                    };
                };
            };

            when("customers calls WOLT") = []
            {
                and_when("WOLT driver is available") = []
                {
                    then("food and drinks are delievred to his home door") = []
                    {
                        require_eq(1.23,1.23);
                        require_eq(123,123);
                        require_eq("123","123");
                        require_eq("123"s,"123"s);
                        require_eq("123"sv,"123"sv);
                        require_eq("123"s,"123"sv);
                    };
                };
            };
        };
    };

    scenario("Test case 2b") = []
    {
        given("Customer wants to drink only beer") = []
        {
            when("customers goes to a restaurant") = []
            {
                throw std::runtime_error{"TestException!"};

                then("customer gets wasted") = []
                {
                    require_gt(33,33);
                };
            };
        };
    };

    return 0;
}

const auto test_registrar = feature();

}
