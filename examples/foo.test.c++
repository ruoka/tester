module foo;
import :bar;
import tester;

namespace foo {

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
