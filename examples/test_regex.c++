import std;
import tester;

namespace regex_test {

using namespace std::literals;
using namespace tester::behavior_driven_development;
using namespace tester::assertions;

auto feature()
{
    scenario("Regex pattern 'scenario.*Happy' matches Happy path scenario") = []
    {
        auto runner = std::make_shared<tester::runner>("scenario.*Happy");
        given("Test name 'scenario -> Happy path scenario'") = [runner]
        {
            when("Checking if it matches pattern 'scenario.*Happy'") = [runner]
            {
                then("It should match") = [runner]
                {
                    auto test_name = "scenario -> Happy path scenario"sv;
                    require_true(runner->included(test_name));
                };
            };
        };
    };

    scenario("Regex pattern 'scenario.*Failure' matches Failure path scenario") = []
    {
        auto runner = std::make_shared<tester::runner>("scenario.*Failure");
        given("Test name 'scenario -> Failure path scenario'") = [runner]
        {
            when("Checking if it matches pattern 'scenario.*Failure'") = [runner]
            {
                then("It should match") = [runner]
                {
                    auto test_name = "scenario -> Failure path scenario"sv;
                    require_true(runner->included(test_name));
                };
            };
        };
    };

    scenario("Regex pattern '^scenario.*path' matches both Happy and Failure path scenarios") = []
    {
        auto runner = std::make_shared<tester::runner>("^scenario.*path");
        given("Test names 'scenario -> Happy path scenario' and 'scenario -> Failure path scenario'") = [runner]
        {
            when("Checking if they match pattern '^scenario.*path'") = [runner]
            {
                then("Both should match") = [runner]
                {
                    auto happy_name = "scenario -> Happy path scenario"sv;
                    auto failure_name = "scenario -> Failure path scenario"sv;
                    require_true(runner->included(happy_name));
                    require_true(runner->included(failure_name));
                };
            };
        };
    };

    scenario("Regex pattern '.*integration.*' matches API integration test") = []
    {
        auto runner = std::make_shared<tester::runner>(".*integration.*");
        given("Test name 'scenario -> API integration test'") = [runner]
        {
            when("Checking if it matches pattern '.*integration.*'") = [runner]
            {
                then("It should match") = [runner]
                {
                    auto test_name = "scenario -> API integration test"sv;
                    require_true(runner->included(test_name));
                };
            };
        };
    };

    scenario("Regex pattern '.*CRUD.*' matches CRUD operations test") = []
    {
        auto runner = std::make_shared<tester::runner>(".*CRUD.*");
        given("Test name 'scenario -> CRUD operations test'") = [runner]
        {
            when("Checking if it matches pattern '.*CRUD.*'") = [runner]
            {
                then("It should match") = [runner]
                {
                    auto test_name = "scenario -> CRUD operations test"sv;
                    require_true(runner->included(test_name));
                };
            };
        };
    };

    scenario("Simple substring pattern 'Happy' matches Happy path scenario") = []
    {
        auto runner = std::make_shared<tester::runner>("Happy");
        given("Test name 'scenario -> Happy path scenario'") = [runner]
        {
            when("Checking if it matches simple pattern 'Happy'") = [runner]
            {
                then("It should match") = [runner]
                {
                    auto test_name = "scenario -> Happy path scenario"sv;
                    require_true(runner->included(test_name));
                };
            };
        };
    };

    scenario("Regex pattern 'scenario.*Happy' does not match Failure path scenario") = []
    {
        auto runner = std::make_shared<tester::runner>("scenario.*Happy");
        given("Test name 'scenario -> Failure path scenario'") = [runner]
        {
            when("Checking if it matches pattern 'scenario.*Happy'") = [runner]
            {
                then("It should not match") = [runner]
                {
                    auto test_name = "scenario -> Failure path scenario"sv;
                    require_false(runner->included(test_name));
                };
            };
        };
    };

    return 0;
}

const auto test_registrar = feature();

}

