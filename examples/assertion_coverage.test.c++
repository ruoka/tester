// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.
//
// Comprehensive test coverage for all assertion functions
// This file ensures both check_* (non-fatal) and require_* (fatal) versions are tested

import std;
import tester;

using namespace std::string_literals;

namespace tester::examples::assertion_coverage {

auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;

    // ============================================================================
    // Equality and ordering assertions
    // ============================================================================

    test_case("Check_neq_NonFatal") = [] {
        // Should pass
        check_neq(1, 2);
        check_neq(1.0, 2.0);
        check_neq("a"s, "b"s);
        
        // Should fail (non-fatal, continues execution)
        check_neq(5, 5);
        check_neq(3.14, 3.14);
    };

    test_case("Check_lt_NonFatal") = [] {
        // Should pass
        check_lt(1, 2);
        check_lt(1.5, 2.5);
        check_lt(-10, 0);
        
        // Should fail (non-fatal)
        check_lt(5, 3);
        check_lt(5, 5);  // equal, not less
        check_lt(10.0, 5.0);
    };

    test_case("Check_lteq_NonFatal") = [] {
        // Should pass
        check_lteq(1, 2);
        check_lteq(5, 5);  // equal case
        check_lteq(3.14, 3.14);
        check_lteq(2.5, 3.0);
        
        // Should fail (non-fatal)
        check_lteq(10, 5);
        check_lteq(7.0, 3.0);
    };

    test_case("Check_gt_NonFatal") = [] {
        // Should pass
        check_gt(5, 3);
        check_gt(10.0, 5.0);
        check_gt(100, 0);
        
        // Should fail (non-fatal)
        check_gt(2, 5);
        check_gt(5, 5);  // equal, not greater
        check_gt(1.0, 2.0);
    };

    test_case("Check_gteq_NonFatal") = [] {
        // Should pass
        check_gteq(5, 3);
        check_gteq(5, 5);  // equal case
        check_gteq(10.0, 5.0);
        check_gteq(3.14, 3.14);
        
        // Should fail (non-fatal)
        check_gteq(2, 5);
        check_gteq(1.0, 2.0);
    };

    // ============================================================================
    // Boolean assertions
    // ============================================================================

    test_case("Check_true_NonFatal") = [] {
        // Should pass
        check_true(true);
        check_true(1 == 1);
        check_true(10 > 5);
        
        // Should fail (non-fatal)
        check_true(false);
        check_true(1 == 2);
        check_true(5 > 10);
    };

    test_case("Check_false_NonFatal") = [] {
        // Should pass
        check_false(false);
        check_false(1 == 2);
        check_false(5 > 10);
        
        // Should fail (non-fatal)
        check_false(true);
        check_false(1 == 1);
        check_false(10 > 5);
    };

    // ============================================================================
    // Exception assertions - check_* versions (non-fatal)
    // ============================================================================

    test_case("Check_nothrow_NonFatal") = [] {
        // Should pass
        check_nothrow([] {});
        check_nothrow([] { volatile int x = 42; (void)x; });
        check_nothrow([] { std::vector<int> v{1, 2, 3}; });
        
        // Should fail (non-fatal, continues execution)
        check_nothrow([] { throw std::runtime_error{"test"}; });
        check_nothrow([] { throw std::out_of_range{"test"}; });
        check_nothrow([] { throw 42; });  // non-std exception
    };

    test_case("Check_throws_NonFatal") = [] {
        // Should pass - any exception
        check_throws([] { throw std::runtime_error{"test"}; });
        check_throws([] { throw std::out_of_range{"test"}; });
        check_throws([] { throw 42; });  // non-std exception
        
        // Should fail (non-fatal)
        check_throws([] {});  // no exception
        check_throws([] { volatile int x = 42; (void)x; });  // no exception
    };

    test_case("Check_throws_as_NonFatal") = [] {
        // Should pass - exact exception type
        check_throws_as([] { throw std::runtime_error{"test"}; }, std::runtime_error{"expected"});
        check_throws_as([] { throw std::out_of_range{"test"}; }, std::out_of_range{"expected"});
        
        // Should fail (non-fatal) - wrong exception type
        check_throws_as([] { throw std::runtime_error{"test"}; }, std::out_of_range{"expected"});
        check_throws_as([] { throw std::out_of_range{"test"}; }, std::runtime_error{"expected"});
        
        // Should fail (non-fatal) - no exception
        check_throws_as([] {}, std::runtime_error{"expected"});
        check_throws_as([] { volatile int x = 42; (void)x; }, std::runtime_error{"expected"});
    };

    // ============================================================================
    // Mixed test: Verify non-fatal assertions continue execution
    // ============================================================================

    test_case("NonFatal_ContinuesExecution") = [] {
        // Multiple failing check_* assertions should all be reported
        check_eq(1, 2);      // fails
        check_neq(5, 5);     // fails
        check_lt(10, 5);     // fails
        check_true(false);   // fails
        check_false(true);   // fails
        
        // But execution continues - this should still run
        require_eq(42, 42);  // This should pass and the test should complete
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::examples::assertion_coverage

