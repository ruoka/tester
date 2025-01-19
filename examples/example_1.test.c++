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
                        require_throws([]{throw 1;});
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
