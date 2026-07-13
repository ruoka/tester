// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

import std;
import tester;

namespace tester::selftest::runner {

namespace {

auto& dependency_run_counter()
{
    static auto value = 0;
    return value;
}

} // namespace

auto register_tests()
{
    using tester::basic::test_case;
    using tester::basic::test_order;
    using namespace tester::assertions;

    test_case("test_case [self][order] dependency root",
              test_order{.priority = 0, .depends_on = {}, .id = "self_order_root"}) = []
    {
        dependency_run_counter() = 1;
        require_eq(dependency_run_counter(), 1);
    };

    test_case("test_case [self][order] dependency child",
              test_order{.priority = 0, .depends_on = {"self_order_root"}, .id = "self_order_child"}) = []
    {
        require_eq(dependency_run_counter(), 1);
        dependency_run_counter() = 2;
        require_eq(dependency_run_counter(), 2);
    };

    test_case("test_case [self] summary records passing run") = []
    {
        require_true(true);
        require_eq(0, 0);
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::runner