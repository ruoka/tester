// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

import tester;
import std;

namespace ordering_example {

using namespace tester::behavior_driven_development;
using namespace tester::assertions;

auto test_set()
{
    // Example 1: Priority-based ordering
    // Lower priority numbers run first
    scenario("High priority test runs first", test_order{.priority = 1, .depends_on = {}, .id = "test1"}) = []
    {
        then("this runs after priority 0 tests") = []
        {
            require_true(true);
        };
    };

    scenario("Low priority test runs first", test_order{.priority = 0, .depends_on = {}, .id = "test2"}) = []
    {
        then("this runs before priority 1 tests") = []
        {
            require_true(true);
        };
    };

    // Example 2: Dependency-based ordering
    // test_b depends on test_a, so test_a runs first
    scenario("Independent test", test_order{.priority = 0, .depends_on = {}, .id = "test_a"}) = []
    {
        then("runs first") = []
        {
            require_true(true);
        };
    };

    scenario("Dependent test", test_order{.priority = 0, .depends_on = {"test_a"}, .id = "test_b"}) = []
    {
        then("runs after test_a") = []
        {
            require_true(true);
        };
    };

    // Example 3: Combined priority and dependencies
    // Even though test_d has lower priority, test_c runs first due to dependency
    scenario("Dependency overrides priority", test_order{.priority = 10, .depends_on = {}, .id = "test_c"}) = []
    {
        then("runs first despite lower priority") = []
        {
            require_true(true);
        };
    };

    scenario("Depends on test_c", test_order{.priority = 5, .depends_on = {"test_c"}, .id = "test_d"}) = []
    {
        then("runs after test_c even though it has lower priority") = []
        {
            require_true(true);
        };
    };

    return 0;
}

const auto test_registrar = test_set();

}

