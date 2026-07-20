// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#include "details/selftest_spawn.h++"

import std;
import tester;

namespace tester::selftest::runner {

namespace {

auto& dependency_run_counter()
{
    static auto value = 0;
    return value;
}

struct recording_observer final : output::observer
{
    int assertions = 0;

    void assertion(const output::assertion_event&) override
    {
        ++assertions;
    }
};

// Registered only in spawned probe processes so normal [self] runs stay cycle-free.
auto register_dependency_cycle_probe()
{
    if(std::getenv("TESTER_DEPENDS_ON_CYCLE_PROBE") == nullptr)
        return 0;

    using tester::basic::test_case;
    using tester::basic::test_order;

    test_case("test_case [.depends-on-cycle-probe] cycle a",
              test_order{.priority = 0, .depends_on = {"self_cycle_b"}, .id = "self_cycle_a"}) = []
    {
    };

    test_case("test_case [.depends-on-cycle-probe] cycle b",
              test_order{.priority = 0, .depends_on = {"self_cycle_a"}, .id = "self_cycle_b"}) = []
    {
    };

    return 0;
}

const auto _cycle_probe = register_dependency_cycle_probe();

} // namespace

auto register_tests()
{
    using tester::basic::test_case;
    using tester::basic::test_order;
    using namespace tester::assertions;
    using tester_selftest::run_test_runner;

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

    test_case("test_case [self][order] cyclic depends_on fails without crashing") = []
    {
        const auto result = run_test_runner(
            {"--tags=[.depends-on-cycle-probe]"},
            "TESTER_DEPENDS_ON_CYCLE_PROBE=1 ");

        // system() status: normal exit keeps the low 7 bits clear; signals do not.
        require_eq(result.exit_code & 0x7f, 0);
        require_neq(result.exit_code, 0);
    };

    test_case("test_case [self] summary records passing run") = []
    {
        require_true(true);
        require_eq(0, 0);
    };

    test_case("test_case [self] output observers receive assertion events") = []
    {
        auto recorder = recording_observer{};
        output::observe(recorder);
        check_eq(1, 1);
        output::unobserve(recorder);
        require_eq(recorder.assertions, 1);
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::runner