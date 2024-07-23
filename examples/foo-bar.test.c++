module foo;
import :bar;
import tester;
import std;

namespace foo::bar {

auto test_set()
{
    using tester::basic::test_case;
    using tester::basic::section;
    using namespace tester::assertions;

    test_case("Module partition foo:bar's unit tests") = []
    {
        require_eq(foo::bar::x, 1.1); // exported form the module foo:bar
        require_eq(foo::bar::y, 2.2); // internal to the module foo:bar
        require_eq(foo::x, 1); // exported form the module foo
        require_eq(foo::y, 2); // internal to the module foo

        section("foo:bar's test section") = []
        {
            char* prt = nullptr;
            require_eq(prt, nullptr);
            require_neq(prt, "blah");
        };

        section("test nothrow") = []
        {
            require_nothrow([]{});
            require_nothrow([]{throw std::exception{};});
        };

        section("Test throws") = []
        {
            require_throws([]{throw std::exception{};});
            require_throws([]{});
        };

        section("Test throw") = []
        {
            require_throws_as([]{throw std::out_of_range{"test"};}, std::out_of_range{"test"});
            require_throws_as([]{throw std::runtime_error{"test"};}, std::out_of_range{"test"});
        };
    };

    return 0;
};

const auto test_registrar = test_set();

}
