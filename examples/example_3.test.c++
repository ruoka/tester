import tester;
import std;

using namespace tester::bdd;
using namespace tester::assertions;
using namespace std::literals;

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

                and_then("requiring the values to be not equal succeeds") = []
                {
                    require_neq(true,false);
                    require_neq(false,true);
                };
            };
        };

        and_given("another given section") = []
        {
            auto test = "looking good"s;

            when("having many when and then sections") = [=]
            {
                then("it works")= [=]
                {
                    succeed(test);
                };

                and_then("it works")= [=]
                {
                    failed(test);
                };               
            };
            and_when("having another when section") = [=]
            {
                then("it also works")= [=]
                {
                    succeed(test);
                };

                and_then("it also wroks")= [=]
                {
                    failed(test);
                };

                and_then("it also works")= [=]
                {
                    warning(test);
                };
            };        
        };
    };

    return true;
}

const auto test_registrar = example_3();
