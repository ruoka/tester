#include <iostream>
import foo;

int main()
{
    std::cout << "foo::x = " << foo::x << std::endl;
    std::cout << "foo::bar::x = " << foo::bar::x << std::endl;
    return 0;
}
