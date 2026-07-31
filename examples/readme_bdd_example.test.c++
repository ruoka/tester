// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

import std;
import tester;

using namespace tester::behavior_driven_development;
using namespace tester::assertions;

namespace ordering {
    struct order {
        bool submitted = false;
        void submit() { submitted = true; }
    };
}

auto readme_bdd_feature()
{
    using ordering::order;

    scenario("Customer places an order") = [] {
        // A step runs where it is written, while this body is still on the stack, so it can
        // hold the scenario's own state by reference and each step sees the one before it.
        auto o = order{};
        given("a draft order") = [&] {
            when("the customer confirms") = [&] {
                o.submit();
                then("the order is marked as submitted") = [&] {
                    require_true(o.submitted);
                    require_nothrow([&]{ o.submit(); });
                };
            };
        };
    };

    scenario("Submission fails") = [] {
        given("a faulty payment gateway") = [] {
            then("submitting raises an error") = [] {
                require_throws([] { throw std::runtime_error{"gateway down"}; });
            };
        };
    };

    return 0;
}

const auto _ = readme_bdd_feature();

