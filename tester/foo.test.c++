module foo;
import tester;

namespace foo {

auto test_set()
{
    using namespace tester::basic;
    using namespace tester::assertions;

    test_case("nodule foo's unit tests") = []
    {
        require_eq(foo::x, 1);
    };

    return 0;
};

const auto test_registrar = test_set();

}
