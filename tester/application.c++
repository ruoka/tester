#include <iostream>
import foo;

int main()
{
    std::cout << "foo::x = " << foo::x << std::endl;
    std::cout << "foo::bar::y = " << foo::bar::y << std::endl;
    return 0;
}
