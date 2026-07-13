// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

import std;
import tester;

using namespace std::string_literals;

namespace tester::examples::container_assertions {

auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;

    test_case("ContainerEquality_Vectors") = [] {
        auto vec1 = std::vector<int>{1, 2, 3, 4, 5};
        auto vec2 = std::vector<int>{1, 2, 3, 4, 5};
        
        // Should pass
        require_container_eq(vec1, vec2);
        check_container_eq(vec1, vec2);
    };

    test_case("ContainerEquality_Mismatch") = [] {
        auto vec1 = std::vector<int>{1, 2, 3, 4, 5};
        auto vec2 = std::vector<int>{1, 2, 9, 4, 5};  // Mismatch at index 2
        
        // Should fail - demonstrates error message
        check_container_eq(vec1, vec2);
    };

    test_case("ContainerEquality_DifferentSizes") = [] {
        auto vec1 = std::vector<int>{1, 2, 3};
        auto vec2 = std::vector<int>{1, 2, 3, 4, 5};  // Extra elements
        
        // Should fail - demonstrates size mismatch message
        check_container_eq(vec1, vec2);
    };

    test_case("ContainerContains_Integer") = [] {
        auto vec = std::vector<int>{10, 20, 30, 40, 50};
        
        // Should pass
        require_contains(vec, 30);
        check_contains(vec, 20);
        
        // Should fail
        check_contains(vec, 99);
    };

    test_case("ContainerContains_String") = [] {
        auto vec = std::vector<std::string>{"apple", "banana", "cherry"};
        
        // Should pass
        require_contains(vec, "banana"s);
        check_contains(vec, std::string{"apple"});
        
        // Should fail
        check_contains(vec, "grape"s);
    };

    test_case("ContainerContains_Empty") = [] {
        auto vec = std::vector<int>{};
        
        // Should fail - empty container
        check_contains(vec, 42);
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::examples::container_assertions

