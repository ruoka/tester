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
    // Under --jobs>1 other workers also notify; count only this case's events.
    std::string filter_id{};
    std::atomic<int> assertions{0};

    void assertion(const output::assertion_event& event) override
    {
        if(not filter_id.empty() and event.test_id != filter_id)
            return;
        assertions.fetch_add(1);
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

// Two cases claiming one id, and a case depending on an id nothing registered. Both are
// refused before the first test runs, so they can only be observed from another process.
auto register_metadata_probes()
{
    if(std::getenv("TESTER_DUPLICATE_ID_PROBE") != nullptr)
    {
        using tester::basic::test_case;
        using tester::basic::test_order;

        test_case("test_case [.duplicate-id-probe] first claimant",
                  test_order{.priority = 0, .depends_on = {}, .id = "self_duplicate_id"}) = []
        {
        };

        test_case("test_case [.duplicate-id-probe] second claimant",
                  test_order{.priority = 0, .depends_on = {}, .id = "self_duplicate_id"}) = []
        {
        };
    }

    if(std::getenv("TESTER_UNKNOWN_DEPENDENCY_PROBE") != nullptr)
    {
        using tester::basic::test_case;
        using tester::basic::test_order;

        test_case("test_case [.unknown-dependency-probe] depends on a typo",
                  test_order{.priority = 0, .depends_on = {"self_no_such_id"}, .id = "self_typo_dep"}) = []
        {
        };
    }

    return 0;
}

const auto _metadata_probes = register_metadata_probes();

// A step that fails cannot be verified in-process: the failure would fail this run. Registered
// only in the spawned probe so an ordinary run never sees a deliberate failure.
auto register_failing_step_probe()
{
    if(std::getenv("TESTER_FAILING_STEP_PROBE") == nullptr)
        return 0;

    using namespace tester::assertions;
    using namespace tester::behavior_driven_development;

    scenario("scenario [.failing-step-probe] one step fails") = []
    {
        given("a step that fails and a sibling after it") = []
        {
            // Non-fatal, so the sibling is reached; a fatal one would leave `given` early.
            then("this one fails on purpose") = []
            {
                check_eq(1, 2);
            };

            and_then("this one still runs") = []
            {
                require_true(true);
            };
        };
    };

    return 0;
}

const auto _failing_step_probe = register_failing_step_probe();

// Parallel runner probe: two independent sleeps must overlap under --jobs>1, and a
// depends_on child must still see the root's write. Hidden unless the env is set.
auto register_parallel_probe()
{
    if(std::getenv("TESTER_PARALLEL_PROBE") == nullptr)
        return 0;

    using tester::basic::test_case;
    using tester::basic::test_order;
    using namespace tester::assertions;

    static auto root_done = std::atomic<int>{0};
    static auto a_start = std::chrono::steady_clock::time_point{};
    static auto a_end = std::chrono::steady_clock::time_point{};
    static auto b_start = std::chrono::steady_clock::time_point{};
    static auto b_end = std::chrono::steady_clock::time_point{};
    static auto times_ready = std::atomic<int>{0};

    test_case("test_case [.parallel-probe] independent a",
              test_order{.priority = 0, .depends_on = {}, .id = "parallel_indep_a"}) = []
    {
        a_start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        a_end = std::chrono::steady_clock::now();
        times_ready.fetch_add(1);
        require_true(true);
    };

    test_case("test_case [.parallel-probe] independent b",
              test_order{.priority = 0, .depends_on = {}, .id = "parallel_indep_b"}) = []
    {
        b_start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        b_end = std::chrono::steady_clock::now();
        times_ready.fetch_add(1);
        require_true(true);
    };

    test_case("test_case [.parallel-probe] dependency root",
              test_order{.priority = 0, .depends_on = {}, .id = "parallel_order_root"}) = []
    {
        root_done.store(1);
        require_eq(root_done.load(), 1);
    };

    test_case("test_case [.parallel-probe] dependency child",
              test_order{.priority = 0, .depends_on = {"parallel_order_root"}, .id = "parallel_order_child"}) = []
    {
        require_eq(root_done.load(), 1);
    };

    // Runs after the independents (same level had no edge to this case). Asserts overlap
    // once both sleeps have stored their windows — registration order puts this after a/b
    // only if we depend on both; otherwise it could race. Pin it with depends_on.
    test_case("test_case [.parallel-probe] independents overlapped",
              test_order{.priority = 0,
                         .depends_on = {"parallel_indep_a", "parallel_indep_b"},
                         .id = "parallel_overlap_check"}) = []
    {
        require_eq(times_ready.load(), 2);
        // Intervals [a_start,a_end] and [b_start,b_end] must overlap if they ran together.
        const auto overlapped = a_start < b_end and b_start < a_end;
        require_true(overlapped);
    };

    return 0;
}

const auto _parallel_probe = register_parallel_probe();

// Soft-assert from a child thread under --jobs>1 used to fail the suite with an empty
// failed_test_ids list (orphan counters on the run context, green worker result).
auto register_parallel_thread_assert_probe()
{
    if(std::getenv("TESTER_PARALLEL_THREAD_ASSERT_PROBE") == nullptr)
        return 0;

    using tester::basic::test_case;
    using tester::basic::test_order;
    using namespace tester::assertions;

    // Two independents so the parallel scheduler actually takes the multi-worker path.
    test_case("test_case [.parallel-thread-assert-probe] sibling",
              test_order{.priority = 0, .depends_on = {}, .id = "parallel_thread_sibling"}) = []
    {
        require_true(true);
    };

    test_case("test_case [.parallel-thread-assert-probe] child soft-fail",
              test_order{.priority = 0, .depends_on = {}, .id = "parallel_thread_soft_fail"}) = []
    {
        auto worker = std::jthread{[]
        {
            check_eq(1, 2);
        }};
    };

    return 0;
}

const auto _parallel_thread_assert_probe = register_parallel_thread_assert_probe();

// Hammers soft asserts from many threads under --jobs=1 so they share one context;
// lost size_t increments would make the totals disagree with the loop counts.
auto register_concurrent_assert_probe()
{
    if(std::getenv("TESTER_CONCURRENT_ASSERT_PROBE") == nullptr)
        return 0;

    using tester::basic::test_case;
    using namespace tester::assertions;

    test_case("test_case [.concurrent-assert-probe] many child threads") = []
    {
        constexpr auto threads = 8uz;
        constexpr auto per_thread = 200uz;
        const auto before_total = tester::data::statistics().total_assertions.load(
            std::memory_order_relaxed);
        const auto before_ok = tester::data::statistics().successful_assertions.load(
            std::memory_order_relaxed);

        {
            auto workers = std::vector<std::jthread>{};
            workers.reserve(threads);
            for(std::size_t t = 0; t < threads; ++t)
            {
                workers.emplace_back([]
                {
                    for(std::size_t i = 0; i < per_thread; ++i)
                        check_eq(i, i);
                });
            }
        }

        const auto after_total = tester::data::statistics().total_assertions.load(
            std::memory_order_relaxed);
        const auto after_ok = tester::data::statistics().successful_assertions.load(
            std::memory_order_relaxed);
        require_eq(after_total, before_total + threads * per_thread);
        require_eq(after_ok, before_ok + threads * per_thread);
    };

    return 0;
}

const auto _concurrent_assert_probe = register_concurrent_assert_probe();

} // namespace

auto register_tests()
{
    using tester::basic::test_case;
    using tester::basic::test_order;
    using tester::basic::section;
    // Named one at a time: both APIs export a test_order, so importing either wholesale would
    // make the one used above ambiguous.
    using tester::behavior_driven_development::given;
    using tester::behavior_driven_development::scenario;
    using tester::behavior_driven_development::then;
    using tester::behavior_driven_development::when;
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

        // A dependency cycle must be reported as a failure, not a crash.
        require_false(result.signaled);
        require_eq(result.exit_code, 1);
    };

    test_case("test_case [self][order] a duplicate test id fails the run") = []
    {
        const auto result = run_test_runner(
            {"--tags=[.duplicate-id-probe]"},
            "TESTER_DUPLICATE_ID_PROBE=1 ");

        require_false(result.signaled);
        require_eq(result.exit_code, 1);
        // The message has to name the id: which two cases collided is the whole diagnosis.
        require_true(result.stderr_text.contains("Duplicate test id \"self_duplicate_id\""));
    };

    test_case("test_case [self][order] a depends_on id nothing registered fails the run") = []
    {
        const auto result = run_test_runner(
            {"--tags=[.unknown-dependency-probe]"},
            "TESTER_UNKNOWN_DEPENDENCY_PROBE=1 ");

        require_false(result.signaled);
        require_eq(result.exit_code, 1);
        require_true(result.stderr_text.contains("Unknown test id \"self_no_such_id\""));
    };

    test_case("test_case [self] summary records passing run") = []
    {
        require_true(true);
        require_eq(0, 0);
    };

    test_case("test_case [self] runner owns its tag filter string") = []
    {
        // Prove the filter is copied: destroy the source before any matching.
        auto filter = std::make_unique<std::string>("[self]");
        auto tr = tester::runner{*filter};
        filter.reset();

        require_true(tr.included("test_case [self] kept by owned filter"));
        require_false(tr.included("test_case [other] rejected by owned filter"));
    };

    test_case("test_case [self] observer_instance reconfigures on each call") = []
    {
        using output::jsonl::jsonl_mode;

        auto& jsonl = output::jsonl::observer_instance(std::cout, std::clog, jsonl_mode::summary);
        require_true(jsonl.output_mode() == jsonl_mode::summary);

        auto& again = output::jsonl::observer_instance(std::cout, std::clog, jsonl_mode::trace);
        require_eq(std::addressof(jsonl), std::addressof(again));
        require_true(again.output_mode() == jsonl_mode::trace);

        auto& console = output::console::observer_instance(std::clog, std::clog, false);
        require_false(console.result_line);
        auto& console_again = output::console::observer_instance(std::clog, std::clog, true);
        require_eq(std::addressof(console), std::addressof(console_again));
        require_true(console_again.result_line);

        // Leave the process singletons in a harmless default shape for later in-process use.
        output::jsonl::observer_instance(std::cout, std::clog, jsonl_mode::failures);
        output::console::observer_instance(std::clog, std::clog, false);
    };

    test_case("test_case [self] output observers receive assertion events") = []
    {
        auto recorder = recording_observer{};
        recorder.filter_id = std::string{tester::data::current_test_id()};
        output::observe(recorder);
        check_eq(1, 1);
        output::unobserve(recorder);
        require_eq(recorder.assertions.load(), 1);
    };

    // A step runs where it is written, so the enclosing frame is still alive: capturing a local
    // by reference is correct, each step observes what the step before it did, and all of them
    // have finished by the time the enclosing body reaches its last line.
    scenario("scenario [self] steps run in the frame that declared them") = []
    {
        auto reached = std::vector<std::string>{};
        auto value = 41;

        given("locals held by reference") = [&]
        {
            reached.push_back("given");
            ++value;

            when("a step mutates one") = [&]
            {
                reached.push_back("when");

                then("the step after it sees the new value") = [&]
                {
                    reached.push_back("then");
                    require_eq(value, 42);
                };
            };
        };

        require_eq(reached, std::vector<std::string>{"given", "when", "then"});
    };

    // Kind comes from the wrapper, not from demangled names or the description text. A step
    // whose description mentions "test_case" / "scenario" must still run inline.
    test_case("test_case [self] registration kind ignores description text") = []
    {
        auto ran = false;
        given("mentions test_case and scenario only in the description") = [&]
        {
            ran = true;
            require_true(true);
        };
        section("also a step even if the name says test_case") = [&]
        {
            require_true(ran);
        };
        require_true(ran);
    };

    // Nested suite_case wrappers schedule for the run loop; they are not steps. If this
    // were a step, the body would run at assignment and nested_ran would already be true.
    // The flag is static so a deferred suite_case body does not touch a dead stack local.
    test_case("test_case [self] nested test_case stays a scheduled case") = []
    {
        static auto nested_ran = false;
        nested_ran = false;
        test_case("test_case [self] nested suite_case body") = []
        {
            nested_ran = true;
            require_true(true);
        };
        require_false(nested_ran);
    };

    test_case("test_case [self] a failing step fails itself and not the case it is written in") = []
    {
        const auto result = run_test_runner(
            {"--jsonl=trace", "--tags=[.failing-step-probe]"},
            "TESTER_FAILING_STEP_PROBE=1 ");

        require_eq(result.exit_code, 1);

        // Four cases: the scenario, its given, and the two steps inside that.
        const auto summary = tester_selftest::find_event(result.stdout_text, "summary");
        require_eq(tester_selftest::field(summary, "tests_total"), std::string{"4"});
        require_eq(tester_selftest::field(summary, "tests_ok"), std::string{"3"});

        // The step owns its failure: the scenario and the given it sits in still pass, and the
        // sibling written after it ran rather than being skipped with it.
        const auto failed = tester_selftest::field(summary, "failed_test_ids");
        require_true(failed.contains("then -> this one fails on purpose"));
        require_false(failed.contains("scenario ->"));
        require_false(failed.contains("given ->"));
        require_true(result.stdout_text.contains("and_then -> this one still runs"));
    };

    test_case("test_case [self] execution contexts isolate per thread") = []
    {
        // Parallel workers each activate their own context on TLS. Two threads with
        // distinct contexts must not see each other's id or assertion counters.
        require_true(tester::data::active_execution() != nullptr);

        auto ctx_a = tester::data::execution_context{};
        auto ctx_b = tester::data::execution_context{};
        ctx_a.current_test_id = "worker-a";
        ctx_b.current_test_id = "worker-b";
        ctx_a.statistics.total_assertions.fetch_add(1, std::memory_order_relaxed);

        auto ready = std::atomic<int>{0};
        auto id_a = std::string{};
        auto id_b = std::string{};
        auto assertions_a = std::size_t{};
        auto assertions_b = std::size_t{};

        {
            auto worker_a = std::jthread{[&]
            {
                const auto scope = tester::data::execution_scope{ctx_a};
                ready.fetch_add(1);
                while(ready.load() < 2)
                    std::this_thread::yield();
                id_a = std::string{tester::data::current_test_id()};
                assertions_a = tester::data::statistics().total_assertions.load(
                    std::memory_order_relaxed);
            }};
            auto worker_b = std::jthread{[&]
            {
                const auto scope = tester::data::execution_scope{ctx_b};
                ready.fetch_add(1);
                while(ready.load() < 2)
                    std::this_thread::yield();
                id_b = std::string{tester::data::current_test_id()};
                assertions_b = tester::data::statistics().total_assertions.load(
                    std::memory_order_relaxed);
            }};
        }

        require_eq(id_a, std::string{"worker-a"});
        require_eq(id_b, std::string{"worker-b"});
        require_eq(assertions_a, 1uz);
        require_eq(assertions_b, 0uz);

        // Nested scopes restore the runner's context for this test body.
        require_true(tester::data::active_execution() != nullptr);
        require_true(tester::data::current_test_id().contains("execution contexts isolate"));
    };

    test_case("test_case [self] soft asserts from a spawned thread use run statistics") = []
    {
        // A test may report from a thread it started. Those threads do not inherit TLS, so
        // statistics() must fall back to the run context — otherwise the soft path throws
        // logic_error and std::jthread calls std::terminate.
        const auto before = tester::data::statistics().total_assertions.load(
            std::memory_order_relaxed);
        auto saw_logic_error = std::atomic<bool>{false};
        auto completed = std::atomic<bool>{false};
        auto child_id = std::string{"unset"};

        {
            auto worker = std::jthread{[&]
            {
                try
                {
                    child_id = std::string{tester::data::current_test_id()};
                    check_eq(2, 2);
                    completed.store(true);
                }
                catch(const std::logic_error&)
                {
                    saw_logic_error.store(true);
                }
            }};
        }

        // Snapshot before this case's own require_* calls bump the counters.
        const auto after_worker = tester::data::statistics().total_assertions.load(
            std::memory_order_relaxed);
        require_false(saw_logic_error.load());
        require_true(completed.load());
        // Empty id on the child matches pre-#52 thread_local current-test-id behaviour.
        require_eq(child_id, std::string{});
        require_true(tester::data::current_test_id().contains("soft asserts from a spawned"));
        // Under jobs==1 the child updates the same context the parent reads. Parallel
        // workers leave the run-wide fallback on the runner context (TLS is not
        // inherited), so the child's check_* may not bump this worker's counters —
        // the no-throw / completed checks above are the parallel-safe contract.
        if(tester::data::worker_count() == 1)
            require_eq(after_worker, before + 1);
    };

    test_case("test_case [self] concurrent child-thread soft asserts keep accurate counts") = []
    {
        // Spawn under --jobs=1 so every child shares one execution_context. Under --jobs>1
        // the same hammer would land on the run-wide fallback instead of this worker.
        const auto result = run_test_runner(
            {"--jsonl=failures", "--jobs=1", "--tags=[.concurrent-assert-probe]"},
            "TESTER_CONCURRENT_ASSERT_PROBE=1 ");

        require_false(result.signaled);
        require_eq(result.exit_code, 0);
        const auto summary = tester_selftest::find_event(result.stdout_text, "summary");
        require_eq(tester_selftest::field(summary, "passed"), std::string{"true"});
        // 1600 check_eq from the child threads, plus the two require_eq totals checks.
        require_eq(tester_selftest::field(summary, "assertions_ok"), std::string{"1602"});
        require_eq(tester_selftest::field(summary, "assertions_total"), std::string{"1602"});
    };

    test_case("test_case [self][parallel] --jobs runs independents together and keeps depends_on") = []
    {
        const auto result = run_test_runner(
            {"--jsonl=failures", "--jobs=4", "--tags=[.parallel-probe]"},
            "TESTER_PARALLEL_PROBE=1 ");

        require_false(result.signaled);
        require_eq(result.exit_code, 0);
        const auto summary = tester_selftest::find_event(result.stdout_text, "summary");
        require_eq(tester_selftest::field(summary, "passed"), std::string{"true"});
        // Five top-level cases in the probe (two independents, overlap check, root, child).
        require_eq(tester_selftest::field(summary, "tests_ok"), std::string{"5"});
    };

    test_case("test_case [self][parallel] worker_count maps jobs=0 to hardware concurrency") = []
    {
        const auto previous = tester::data::config().jobs;
        tester::set_jobs(0);
        require_true(tester::data::worker_count() >= 1uz);
        tester::set_jobs(1);
        require_eq(tester::data::worker_count(), 1uz);
        tester::set_jobs(previous);
    };

    test_case("test_case [self][parallel] child-thread soft fail is attributed under --jobs") = []
    {
        const auto result = run_test_runner(
            {"--jsonl=failures", "--jobs=2", "--tags=[.parallel-thread-assert-probe]"},
            "TESTER_PARALLEL_THREAD_ASSERT_PROBE=1 ");

        require_false(result.signaled);
        require_eq(result.exit_code, 1);
        const auto summary = tester_selftest::find_event(result.stdout_text, "summary");
        require_eq(tester_selftest::field(summary, "passed"), std::string{"false"});
        // Must not be an opaque assertion-total mismatch with an empty failure list.
        const auto failed = tester_selftest::field(summary, "failed_test_ids");
        require_true(failed.contains("<unattributed thread assertion>"));
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::runner