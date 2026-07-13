// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

import std;
import tester;

using namespace std::string_literals;

namespace tester::examples::string_assertions {

auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;

    test_case("StringContains_Substring") = [] {
        auto str = "Hello, world!"s;
        
        // Should pass
        require_contains(str, "world");
        require_contains(str, "Hello");
        require_contains(str, "o, wo");
        check_contains(str, "!");
    };

    test_case("StringContains_Character") = [] {
        auto str = "Hello, world!"s;
        
        // Should pass
        require_contains(str, 'H');
        require_contains(str, 'o');
        check_contains(str, '!');
        
        // Should fail
        check_contains(str, 'z');
    };

    test_case("StringContains_CaseSensitive") = [] {
        auto str = "Hello, World!"s;
        
        // Should pass
        require_contains(str, "Hello");
        require_contains(str, "World");
        
        // Should fail - case sensitive
        check_contains(str, std::string{"hello"});
        check_contains(str, std::string{"world"});
    };

    test_case("StringStartsWith_Prefix") = [] {
        auto str = "Hello, world!"s;
        
        // Should pass
        require_starts_with(str, "Hello");
        require_starts_with(str, "H");
        check_starts_with(str, "Hello,");
        
        // Should fail
        check_starts_with(str, std::string{"hello"});
        check_starts_with(str, std::string{"world"});
    };

    test_case("StringEndsWith_Suffix") = [] {
        auto str = "Hello, world!"s;
        
        // Should pass
        require_ends_with(str, "world!");
        require_ends_with(str, "!");
        check_ends_with(str, "d!");
        
        // Should fail
        check_ends_with(str, std::string{"world"});
        check_ends_with(str, std::string{"Hello"});
    };

    test_case("StringHasSubstr_Alias") = [] {
        auto str = "Hello, world!"s;
        
        // require_has_substr is an alias for require_contains
        require_has_substr(str, "world");
        check_has_substr(str, "Hello");
        
        // Should fail
        check_has_substr(str, "xyz");
    };

    test_case("StringAssertions_EmptyString") = [] {
        auto empty = ""s;
        
        // Should fail - empty string doesn't contain anything
        check_contains(empty, std::string{"a"});
        check_contains(empty, 'a');
        check_starts_with(empty, std::string{"a"});
        check_ends_with(empty, std::string{"a"});
        
        // Should pass - empty string starts and ends with empty
        require_starts_with(empty, "");
        require_ends_with(empty, "");
    };

    test_case("StringAssertions_StringView") = [] {
        auto str = "Hello, world!"s;
        auto sv = std::string_view{str};
        
        // Should work with string_view
        require_contains(sv, "world");
        require_starts_with(sv, "Hello");
        require_ends_with(sv, "!");
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::examples::string_assertions

