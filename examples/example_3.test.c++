import tester;

using namespace tester::bdd;
using namespace tester::assertions;

auto example_3()
{
    scenario("Test case 3") = []
    {
        given("Simple test to verify assertions") = [] 
        {
            when("true and false are compared") = []
            {
                then("requiring the values to be equal fails") = []
                {
                    require_eq(true,false);
                    require_eq(false,true);
                };

                then("requiring the values to be not equal succeeds") = []
                {
                    require_neq(true,false);
                    require_neq(false,true);
                };
            };
        };
    };

    return 0;
}

const auto test_registrar = example_3();
