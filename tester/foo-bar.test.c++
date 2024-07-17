module;
#include <exception>
module foo;
import :bar;
import tester;

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

        section("est nothrow") = []
        {
            require_nothrow([]{});
            require_nothrow([]{throw std::exception{};});
        };

        section("Test throw") = []
        {
            require_throw([]{throw std::exception{};});
            require_throw([]{});
        };

    };

    return 0;
};

const auto test_registrar = test_set();

}
