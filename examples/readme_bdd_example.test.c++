// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
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
        // Use shared_ptr to safely share state across nested test cases
        // Nested lambdas (given/when/then) execute later, after the scenario
        // lambda returns, so they must capture by value, not by reference
        auto o = std::make_shared<order>();
        given("a draft order") = [o] {
            when("the customer confirms") = [o] {
                o->submit();
                then("the order is marked as submitted") = [o] {
                    require_true(o->submitted);
                    require_nothrow([o]{ o->submit(); });
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

