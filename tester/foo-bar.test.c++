module;
#include <exception>
module foo;
import :bar;
import tester;

namespace foo::bar {

auto test_set()
{
    using namespace tester::basic;
    using namespace tester::assertions;

    test_case("Sub-module foo:bar's unit tests") = []
    {
        require_eq(foo::bar::y, 2);

        section("Sub-test") = []
        {
            char* prt = nullptr;
            require_eq(prt, nullptr);
        };

        section("Test no throw") = []
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
