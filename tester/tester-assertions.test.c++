// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

// Contract tests for the assertion matchers themselves.
//
// A matcher's failure path cannot be observed in-process: the failure would fail
// this run. Each family therefore has probe cases under a hidden tag that fail on
// purpose, and a [self] verifier that spawns test_runner on that tag and asserts on
// the emitted JSONL. One spawn per family keeps the process count down.

#include "details/selftest_spawn.h++"

import std;
import tester;

namespace tester::selftest::assertions {

using tester_selftest::event_field;
using tester_selftest::field;
using tester_selftest::find_event;
using tester_selftest::run_test_runner;

namespace {

auto probe(std::string_view tag)
{
    return run_test_runner({"--jsonl=failures", std::string{"--tags="} + std::string{tag}});
}

// nth assertion_failed event of a probe run, in emission order.
auto failure(std::string_view text, std::size_t index)
{
    return find_event(text, "assertion_failed", index);
}

// Records what the matchers do to an operand. An assertion only reads its operands, so
// comparing two of these must not copy or move either one.
struct counted
{
    inline static int copies = 0;
    inline static int moves = 0;

    int value;

    explicit counted(int v) : value{v} {}
    counted(const counted& other) : value{other.value} { ++copies; }
    counted(counted&& other) noexcept : value{other.value} { ++moves; }

    bool operator==(const counted& other) const { return value == other.value; }
    auto operator<=>(const counted& other) const { return value <=> other.value; }
};

// An unscoped enumeration carrying a negative value, for the conversion that made
// check_eq(legacy_flag::minus_one, 4294967295u) report equal.
enum legacy_flag { minus_one = -1, two = 2 };

// An enumeration with a comparison of its own, and one that disagrees with its values on
// purpose: if the matchers promoted both operands they would answer from the values and
// never call this.
enum sloppy_enum { a = 1, b = 2 };
int sloppy_comparisons = 0;
bool operator==(sloppy_enum, sloppy_enum) { ++sloppy_comparisons; return true; }

} // namespace

auto register_tests()
{
    using tester::basic::test_case;
    using namespace tester::assertions;

    // ========================================================================
    // Harness helpers — the extraction code the rest of this file depends on
    // ========================================================================

    test_case("test_case [self][harness] jsonl field extraction") = []
    {
        const auto line = std::string{
            R"({"type":"assertion_failed","matcher":"check_eq","actual":1,"expected":2,)"
            R"("ok":false,"tags":["a","b"],"nested":{"k":1},"message":"say \"hi\"","line":42})"};

        require_eq(field(line, "matcher"), std::string{"\"check_eq\""});
        require_eq(field(line, "actual"), std::string{"1"});
        require_eq(field(line, "expected"), std::string{"2"});
        require_eq(field(line, "ok"), std::string{"false"});
        require_eq(field(line, "line"), std::string{"42"});
        require_eq(field(line, "tags"), std::string{"[\"a\",\"b\"]"});
        require_eq(field(line, "nested"), std::string{"{\"k\":1}"});
        // An escaped quote must not terminate the value early.
        require_eq(field(line, "message"), std::string{R"("say \"hi\"")"});
        require_eq(field(line, "absent"), std::string{});
    };

    test_case("test_case [self][harness] wait status decoding") = []
    {
        using tester_selftest::decode_wait_status;

        require_eq(decode_wait_status(0).exit_code, 0);
        // A runner exiting 1 produces wait status 256.
        require_eq(decode_wait_status(256).exit_code, 1);
        require_false(decode_wait_status(256).signaled);
        // SIGSEGV without a core dump.
        require_true(decode_wait_status(11).signaled);
        require_eq(decode_wait_status(11).signal, 11);
    };

    // ========================================================================
    // Relational matchers
    // ========================================================================

    test_case("test_case [.probe-relational] relational failures") = []
    {
        check_eq(1, 2);
        check_neq(3, 3);
        check_lt(5, 5);
        check_lteq(6, 5);
        check_gt(7, 7);
        check_gteq(7, 8);
        check_eq(std::string{"actual"}, std::string{"expected"});
    };

    test_case("test_case [self] relational matchers report matcher and operands") = []
    {
        const auto result = probe("[.probe-relational]");
        require_neq(result.exit_code, 0);

        const auto expected_matchers = std::vector<std::string>{
            "\"check_eq\"", "\"check_neq\"", "\"check_lt\"",
            "\"check_lteq\"", "\"check_gt\"", "\"check_gteq\""};

        for(auto index = std::size_t{0}; index < expected_matchers.size(); ++index)
        {
            const auto line = failure(result.stdout_text, index);
            require_false(line.empty());
            require_eq(field(line, "matcher"), expected_matchers.at(index));
        }

        const auto first = failure(result.stdout_text, 0);
        require_eq(field(first, "actual"), std::string{"1"});
        require_eq(field(first, "expected"), std::string{"2"});

        // String operands stay verbatim and stay quoted.
        const auto strings = failure(result.stdout_text, 6);
        require_eq(field(strings, "matcher"), std::string{"\"check_eq\""});
        require_eq(field(strings, "actual"), std::string{"\"actual\""});
        require_eq(field(strings, "expected"), std::string{"\"expected\""});
    };

    test_case("test_case [self] relational matchers pass on satisfied comparisons") = []
    {
        require_eq(1, 1);
        require_neq(1, 2);
        require_lt(1, 2);
        require_lteq(2, 2);
        require_gt(2, 1);
        require_gteq(2, 2);
        require_eq(std::string{"same"}, std::string{"same"});
        // Mixed arithmetic types resolve through std::common_type.
        require_eq(2, 2L);
        require_lt(1, 2.5);
    };

    test_case("test_case [self] matchers neither copy nor move their operands") = []
    {
        const auto a = counted{1};
        const auto b = counted{2};

        counted::copies = counted::moves = 0;
        check_eq(a, a);
        check_neq(a, b);
        check_lt(a, b);
        check_lteq(a, b);
        check_gt(b, a);
        check_gteq(b, a);
        require_eq(a, a);
        const auto copies = counted::copies;
        const auto moves = counted::moves;

        // Taking operands by value cost one copy per matcher layer, seven assertions
        // deep: two in the wrapper, two in the hub, two in the comparator, per pair.
        require_eq(copies, 0);
        require_eq(moves, 0);

        // Move-only operands are comparable only because nothing is copied. By value
        // rejected them outright, at compile time.
        const auto left = std::make_unique<int>(1);
        const auto right = std::unique_ptr<int>{};
        require_neq(left, right);
        require_eq(left, left);
    };

    test_case("test_case [.probe-array-operands] failures with array operands") = []
    {
        check_eq("hello", "world");
        const char* pointer = "hello";
        check_eq(pointer, "world");
    };

    test_case("test_case [self] array operands are reported by content, not by type") = []
    {
        // A forwarding reference preserves char[N] where a by-value parameter decayed it
        // to const char*, so operands are decayed at the hub. Without that, a literal is
        // reported as its array type and a non-const array does not compile at all,
        // because the comparator is instantiated on the decayed pointer type.
        const auto result = probe("[.probe-array-operands]");
        require_neq(result.exit_code, 0);
        for(auto index = std::size_t{0}; index < 2; ++index)
        {
            const auto line = failure(result.stdout_text, index);
            require_eq(field(line, "actual"), std::string{"\"hello\""});
            require_eq(field(line, "expected"), std::string{"\"world\""});
        }

        // Arrays compare as pointers, exactly as they did when taken by value.
        char text[6] = "hello";
        int numbers[3] = {1, 2, 3};
        require_eq(text, text);
        require_eq(numbers, numbers);
    };

    test_case("test_case [.probe-opaque] failure on an operand the framework cannot print") = []
    {
        check_eq(counted{1}, counted{2});
    };

    test_case("test_case [self] unprintable operands are named, not mangled") = []
    {
        const auto result = probe("[.probe-opaque]");
        require_neq(result.exit_code, 0);

        const auto reported = field(failure(result.stdout_text, 0), "actual");
        require_contains(reported, std::string{"counted"});
        require_false(reported.contains("_GLOBAL__N_"));  // the mangled spelling
    };

    test_case("test_case [.probe-signedness] mixed-signedness failures") = []
    {
        check_eq(-1, 4294967295u);   // equal only after -1 converts to unsigned
        check_neq(1, 1u);            // equal as values, so neq must fail
        check_gt(-1, 1u);            // -1 is not greater than 1
        check_gteq(-1, 0u);          // nor greater or equal
        check_lt(1u, -1);            // 1 is not less than -1

        // The same conversion, through operand types that cannot be handed to std::cmp_*
        // as written and were therefore left out of it.
        check_eq(char{-1}, 4294967295u);
        check_gt(char{-1}, 0u);
        check_eq(legacy_flag::minus_one, 4294967295u);
        check_eq('a', 98u);          // a printable character, reported by value as 97
    };

    test_case("test_case [self] mixed-signedness comparisons compare values") = []
    {
        // std::common_type_t<int,unsigned> is unsigned, so comparing through it
        // converts a negative operand: -1 became 4294967295. These matchers route
        // mixed signedness through std::cmp_*, which compares mathematical values.
        require_lt(-1, 1u);
        require_lteq(-1, 0u);
        require_neq(-1, 4294967295u);
        require_gt(1u, -1);

        // Equal values still compare equal across signedness, including the common
        // container-size idiom where an unsigned size meets a signed literal.
        require_eq(1, 1u);
        require_eq(std::vector<int>{1, 2, 3}.size(), 3);
        require_eq(-1, -1);

        // Same-signedness comparisons are untouched, including the unsigned extreme.
        require_eq(4294967295u, 4294967295u);
        require_lt(0u, 4294967295u);

        // bool, the character types and enumerations are not signed or unsigned integer
        // types, so std::cmp_* rejects them as written. Promoting first is what lets them
        // take the same path: excluding them instead left the wraparound in place for
        // exactly these operands.
        require_eq('a', 'a');
        require_true(true);
        require_eq(static_cast<unsigned char>(1), 1);
        require_neq(char{-1}, 4294967295u);
        require_lt(char{-1}, 0u);
        require_eq('a', 97u);
        require_eq(true, 1u);
        require_neq(true, 2u);
        require_neq(legacy_flag::minus_one, 4294967295u);
        require_eq(legacy_flag::two, 2u);

        // A wide character operand is comparable and reportable at all only because it is
        // promoted: C++20 deleted the narrow stream inserters these types used to reach.
        // 0x161 is also the value an unsigned char cast would truncate to 'a', which is why
        // the printable test asks about the code point and not about a byte.
        require_eq(char16_t{0x161}, char16_t{0x161});
        require_neq(char16_t{0x161}, 97u);
        require_eq(L'a', 97u);

        // An enumeration measured against its own type keeps the comparison the type
        // provides, which promoting both sides would bypass. This one counts every call.
        require_eq(sloppy_enum::a, sloppy_enum::b);
        require_gt(sloppy_comparisons, 0);

        const auto result = probe("[.probe-signedness]");
        require_neq(result.exit_code, 0);
        const auto matchers = std::vector<std::string>{
            "\"check_eq\"", "\"check_neq\"", "\"check_gt\"",
            "\"check_gteq\"", "\"check_lt\"",
            "\"check_eq\"", "\"check_gt\"", "\"check_eq\"", "\"check_eq\""};
        for(auto index = std::size_t{0}; index < matchers.size(); ++index)
            require_eq(field(failure(result.stdout_text, index), "matcher"), matchers[index]);

        // Operands are reported as written, not as converted.
        require_eq(field(failure(result.stdout_text, 0), "actual"), std::string{"-1"});
        require_eq(field(failure(result.stdout_text, 0), "expected"), std::string{"4294967295"});

        // A char operand is reported by value, in a field the schema calls a number.
        // Streamed as itself it put a bare `a` there for 'a', and for char{-1} a stray
        // 0xff byte that is not even valid UTF-8, leaving a line no parser could read —
        // and stdout is the whole contract with agents. The last three probe failures are
        // the two char ones and the enumeration.
        for(auto index = std::size_t{5}; index < 8; ++index)
            require_eq(field(failure(result.stdout_text, index), "actual"), std::string{"-1"});
        require_eq(field(failure(result.stdout_text, 8), "actual"), std::string{"97"});
        require_false(std::ranges::any_of(result.stdout_text,
            [](char byte){ return static_cast<unsigned char>(byte) >= 0x80; }));
    };

    // ========================================================================
    // Floating point
    // ========================================================================

    test_case("test_case [.probe-float] float failures") = []
    {
        check_near(1.0, 1.5, 0.1);
        check_eq(1.0, 2.0);
        check_eq(0.5, 0.5 + 5e-9);        // absolute tolerance exceeded below 1.0
        check_eq(1e6, 1e6 + 1e-2);        // relative tolerance exceeded above 1.0
        check_eq(std::nan(""), 1.0);      // NaN never equals a number
        check_eq(1.0f + 1e-6f, 1.0f);     // ~8 ulps of float, past the default tolerance
    };

    test_case("test_case [self] check_eq on floating point is approximate") = []
    {
        // Absolute tolerance at or below 1.0, relative above it, default 1e-9.
        require_eq(0.1 + 0.2, 0.3);
        require_eq(0.5, 0.5 + 5e-10);
        require_eq(1e6, 1e6 + 1e-4);
        require_near(1.0, 1.05, 0.1);
        require_near(1.0, 1.0000000001);

        // Infinities compare by identity; NaN equals only NaN.
        const auto infinity = std::numeric_limits<double>::infinity();
        require_eq(infinity, infinity);
        require_eq(std::nan(""), std::nan(""));

        const auto result = probe("[.probe-float]");
        require_neq(result.exit_code, 0);
        require_eq(field(failure(result.stdout_text, 0), "matcher"), std::string{"\"check_near\""});
        require_eq(field(failure(result.stdout_text, 1), "matcher"), std::string{"\"check_eq\""});
        // Boundary failures are reported, not silently tolerated.
        require_eq(field(failure(result.stdout_text, 2), "matcher"), std::string{"\"check_eq\""});
        require_eq(field(failure(result.stdout_text, 3), "matcher"), std::string{"\"check_eq\""});
        require_eq(field(failure(result.stdout_text, 4), "matcher"), std::string{"\"check_eq\""});
    };

    test_case("test_case [self] the default epsilon scales with the type") = []
    {
        // The default is floored at four times the type's machine epsilon, so float
        // gets a usable ~4.77e-7 tolerance instead of an unreachable 1e-9. One ulp of
        // float rounding error compares equal.
        require_eq(1.0f + 1e-7f, 1.0f);

        // The floor does not loosen the wider types: their machine epsilon is well
        // below 1e-9, so double keeps the historical default.
        require_eq(0.5, 0.5 + 5e-10);
        require_eq(1e6, 1e6 + 1e-4);

        // Tolerance is bounded, not permissive: the probe's ~8 ulp float difference
        // is still reported as a failure.
        const auto result = probe("[.probe-float]");
        const auto line = failure(result.stdout_text, 5);
        require_false(line.empty());
        require_eq(field(line, "matcher"), std::string{"\"check_eq\""});
    };

    test_case("test_case [.probe-float-neq] approximate inequality failures") = []
    {
        check_neq(0.5, 0.5 + 5e-10);    // inside the tolerance, so not unequal
        check_neq(0.1 + 0.2, 0.3);      // equal within tolerance, however it is spelled
        check_neq(1.0f + 1e-7f, 1.0f);  // one ulp of float
    };

    test_case("test_case [self] check_neq negates check_eq on floating point") = []
    {
        // While the matchers picked their own comparators, check_neq compared floating
        // point exactly and check_eq compared within an epsilon, so a pair inside the
        // tolerance satisfied both at once. Inequality now means "not equal within the
        // tolerance", which is the only reading that cannot contradict check_eq.
        require_neq(0.5, 0.6);
        require_neq(0.5, 0.5 + 5e-9);   // outside the tolerance, so genuinely unequal

        const auto result = probe("[.probe-float-neq]");
        require_neq(result.exit_code, 0);
        for(auto index = std::size_t{0}; index < 3; ++index)
            require_eq(field(failure(result.stdout_text, index), "matcher"), std::string{"\"check_neq\""});
    };

    // ========================================================================
    // Boolean
    // ========================================================================

    test_case("test_case [.probe-boolean] boolean failures") = []
    {
        check_true(false);
        check_false(true);
    };

    test_case("test_case [self] boolean matchers") = []
    {
        require_true(true);
        require_false(false);

        const auto result = probe("[.probe-boolean]");
        require_neq(result.exit_code, 0);

        const auto first = failure(result.stdout_text, 0);
        require_eq(field(first, "matcher"), std::string{"\"check_true\""});
        require_eq(field(first, "actual"), std::string{"false"});
        require_eq(field(first, "expected"), std::string{"true"});

        const auto second = failure(result.stdout_text, 1);
        require_eq(field(second, "matcher"), std::string{"\"check_false\""});
        require_eq(field(second, "actual"), std::string{"true"});
        require_eq(field(second, "expected"), std::string{"false"});
    };

    // ========================================================================
    // Exceptions
    // ========================================================================

    test_case("test_case [.probe-throws] exception failures") = []
    {
        check_nothrow([] { throw std::runtime_error{"boom"}; });
        check_throws([] {});
        check_throws_as<std::runtime_error>([] { throw std::logic_error{"wrong type"}; });
        check_throws_as<std::runtime_error>([] {});
    };

    test_case("test_case [self] exception matchers on the happy path") = []
    {
        require_nothrow([] {});
        require_throws([] { throw std::runtime_error{"expected"}; });
        require_throws_as<std::out_of_range>([] { throw std::out_of_range{"expected"}; });
        // A derived exception satisfies a base-class expectation.
        require_throws_as<std::logic_error>([] { throw std::out_of_range{"derived"}; });
    };

    test_case("test_case [self] the deprecated throws_as instance form still works") = []
    {
        // Kept so consumers pinning an older tester keep compiling; the exception
        // argument is discarded and only its type is used.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        require_throws_as([] { throw std::out_of_range{"x"}; }, std::out_of_range{"unused"});
        check_throws_as([] { throw std::runtime_error{"x"}; }, std::runtime_error{"unused"});
#pragma clang diagnostic pop
    };

    test_case("test_case [self] exception matchers name the matcher and reason") = []
    {
        const auto result = probe("[.probe-throws]");
        require_neq(result.exit_code, 0);

        const auto nothrow = failure(result.stdout_text, 0);
        require_eq(field(nothrow, "matcher"), std::string{"\"check_nothrow\""});
        require_eq(field(nothrow, "message"), std::string{"\"expected no exception\""});

        const auto throws = failure(result.stdout_text, 1);
        require_eq(field(throws, "matcher"), std::string{"\"check_throws\""});
        require_eq(field(throws, "actual"), std::string{"\"none\""});
        require_eq(field(throws, "message"), std::string{"\"expected exception\""});

        const auto wrong_type = failure(result.stdout_text, 2);
        require_eq(field(wrong_type, "matcher"), std::string{"\"check_throws_as\""});
        require_eq(field(wrong_type, "message"), std::string{"\"wrong exception type\""});

        const auto none_thrown = failure(result.stdout_text, 3);
        require_eq(field(none_thrown, "matcher"), std::string{"\"check_throws_as\""});
        require_eq(field(none_thrown, "message"),
                   std::string{"\"expected exception of specified type\""});
    };

    test_case("test_case [self] exception type names are demangled") = []
    {
        const auto result = probe("[.probe-throws]");

        // typeid().name() is mangled (St13runtime_error); reports must be readable.
        const auto nothrow = failure(result.stdout_text, 0);
        require_eq(field(nothrow, "actual"), std::string{"\"std::runtime_error\""});

        const auto wrong_type = failure(result.stdout_text, 2);
        require_eq(field(wrong_type, "actual"), std::string{"\"std::logic_error\""});
        require_eq(field(wrong_type, "expected"), std::string{"\"std::runtime_error\""});
    };

    test_case("test_case [self] uncaught exceptions surface as exception events") = []
    {
        const auto result = probe("[.probe-uncaught]");
        require_neq(result.exit_code, 0);

        const auto line = find_event(result.stdout_text, "exception");
        require_false(line.empty());
        require_eq(field(line, "exception_type"), std::string{"\"std::runtime_error\""});
        require_eq(field(line, "message"), std::string{"\"escaped the test body\""});
    };

    test_case("test_case [.probe-uncaught] uncaught exception") = []
    {
        throw std::runtime_error{"escaped the test body"};
    };

    // ========================================================================
    // Containers
    // ========================================================================

    test_case("test_case [.probe-container] container failures") = []
    {
        check_container_eq(std::vector<int>{1, 9, 3}, std::vector<int>{1, 2, 3});
        check_container_eq(std::vector<int>{1, 2, 3, 4}, std::vector<int>{1, 2, 3});
        check_container_eq(std::vector<int>{1, 2}, std::vector<int>{1, 2, 3});
        check_contains(std::vector<int>{1, 2, 3}, 9);
    };

    test_case("test_case [self] container matchers on equal sequences") = []
    {
        require_container_eq(std::vector<int>{1, 2, 3}, std::vector<int>{1, 2, 3});
        require_container_eq(std::vector<int>{}, std::vector<int>{});
        require_contains(std::vector<int>{1, 2, 3}, 2);
        // Different container types with equal contents compare equal.
        require_container_eq(std::vector<int>{1, 2}, std::list<int>{1, 2});
    };

    test_case("test_case [.probe-container-signedness] container failures across signedness") = []
    {
        check_contains(std::vector<std::size_t>{std::string::npos}, -1);
        check_container_eq(std::vector<int>{-1}, std::vector<unsigned>{4294967295u});
    };

    test_case("test_case [.probe-composite-signedness] composite failures across signedness") = []
    {
        check_eq(std::pair{std::string::npos, 0}, std::pair{-1L, 0});
        check_eq(std::tuple{std::string::npos, 'a'}, std::tuple{-1L, 'a'});
        check_container_eq(std::vector{std::pair{-1, 0}}, std::vector{std::pair{4294967295u, 0}});
        check_contains(std::vector{std::pair{1, 2}}, std::pair{3, 4});
    };

    test_case("test_case [self] pair and tuple members follow the scalar comparison rule") = []
    {
        // Two heterogeneous pairs have a common type of their own, so comparing through it
        // converted every member the way scalars were converted before std::cmp_* arrived:
        // pair{npos, 0} met pair{-1L, 0}, and the ordering matchers had the mirror of it,
        // reporting -1 as not less than 4294967295u. Members are walked one at a time now,
        // by the rule each member's own types ask for.
        require_neq(std::pair{std::string::npos, 0}, std::pair{-1L, 0});
        require_neq(std::tuple{std::string::npos, 'a'}, std::tuple{-1L, 'a'});
        require_lt(std::pair{-1, 0}, std::pair{4294967295u, 0});

        // Ordering is lexicographic on those same rules: a member that orders neither way
        // is a tie and the next one decides.
        require_gt(std::pair{1, 4294967295u}, std::pair{1, -1});
        require_lteq(std::pair{1, 2}, std::pair{1L, 2L});
        require_gteq(std::pair{2, 0}, std::pair{1u, 0});

        // What the walk inherits by going through the scalar rule: a composite member is
        // walked again, and the floating-point epsilon reaches a member.
        require_eq(std::pair{std::pair{1, 2}, 3}, std::pair{std::pair{1L, 2L}, 3});
        require_eq(std::pair{0.1 + 0.2, 1}, std::pair{0.3, 1});

        // Same types, plain values: what the comparison did correctly all along.
        require_eq(std::pair{1, 2}, std::pair{1, 2});
        require_neq(std::tuple{1, 'a'}, std::tuple{2, 'a'});

        // Containers of composites could not be compared at all before: the matchers
        // formatted elements with `<<`, which a pair does not have, so the call did not
        // compile. They go through the shared operand formatter now, which is also what
        // puts the members in the report.
        require_container_eq(std::vector{std::pair{1, 2}}, std::vector{std::pair{1L, 2L}});
        require_contains(std::vector{std::pair{1, 2}, std::pair{3, 4}}, std::pair{3L, 4L});

        const auto result = probe("[.probe-composite-signedness]");
        require_neq(result.exit_code, 0);

        // A composite operand reads as its members, in both channels. Reported as a type
        // name — `std::pair<unsigned long, int>` — a failure said nothing about which
        // member differed, or what either side held.
        const auto pair_failure = failure(result.stdout_text, 0);
        require_eq(field(pair_failure, "matcher"), std::string{"\"check_eq\""});
        require_eq(field(pair_failure, "actual"), std::string{"\"(18446744073709551615, 0)\""});
        require_eq(field(pair_failure, "expected"), std::string{"\"(-1, 0)\""});

        // Members are formatted by their own rules, so the character rule reaches into a
        // tuple: both the glyph and the value, as it is for a character on its own.
        require_contains(field(failure(result.stdout_text, 1), "actual"), std::string{"'a' (97)"});

        const auto container_failure = failure(result.stdout_text, 2);
        require_eq(field(container_failure, "matcher"), std::string{"\"check_container_eq\""});
        require_contains(field(container_failure, "expected"), std::string{"mismatch at index 0"});
        require_contains(field(container_failure, "expected"), std::string{"actual=(-1, 0)"});
        require_contains(field(container_failure, "expected"), std::string{"expected=(4294967295, 0)"});

        const auto contains_failure = failure(result.stdout_text, 3);
        require_eq(field(contains_failure, "matcher"), std::string{"\"check_contains\""});
        require_eq(field(contains_failure, "actual"), std::string{"\"[(1, 2)]\""});
        require_eq(field(contains_failure, "expected"), std::string{"\"contains: (3, 4)\""});
    };

    test_case("test_case [.probe-optional-signedness] optional and expected failures across signedness") = []
    {
        check_eq(std::optional<int>{-1}, 4294967295u);
        check_eq(std::optional<unsigned>{4294967295u}, -1);
        check_eq(std::optional<std::size_t>{std::string::npos}, -1L);
        check_eq(std::expected<int, std::string>{-1}, 4294967295u);
        // Heterogeneous optionals have no common_type, so check_eq cannot name one —
        // check_contains goes through values_equal, which unwraps without it.
        check_contains(std::vector<std::optional<unsigned>>{4294967295u},
                       std::optional<int>{-1});
        check_eq(std::optional<int>{}, 0); // nullopt is not 0
    };

    test_case("test_case [self] optional and expected follow the scalar comparison rule") = []
    {
        // optional and expected are not tuple-like, so the composite walk never reached
        // them. Their common_type with a bare integer is the wrapper itself, and building
        // that wrapper from the integer reintroduced the wraparound: optional{-1} met
        // 4294967295u and reported equal. Unwrap to the contained value now.
        require_neq(std::optional<int>{-1}, 4294967295u);
        require_neq(std::optional<unsigned>{4294967295u}, -1);
        require_neq(std::optional<std::size_t>{std::string::npos}, -1L);
        require_neq(std::expected<int, std::string>{-1}, 4294967295u);
        require_lt(std::optional<int>{-1}, std::optional<int>{0});
        require_gt(std::optional<int>{1}, std::optional<int>{-1});

        // Equal values still match across the wrapper, including nullopt with nullopt and
        // an engaged optional with a bare equal value.
        require_eq(std::optional<int>{1}, 1u);
        require_eq(std::optional<int>{}, std::optional<int>{});
        require_eq(std::expected<int, std::string>{2}, 2u);
        require_neq(std::optional<int>{}, 0);

        // Containers of optionals go through values_equal's unwrap. Heterogeneous
        // optional elements have no common_type — without the unwrap, operator== still
        // converts and the probe's check_contains used to pass.
        require_contains(std::vector{std::optional<int>{1}}, 1u);
        require_container_eq(std::vector{std::optional<int>{1}},
                             std::vector{std::optional<int>{1}});

        const auto result = probe("[.probe-optional-signedness]");
        require_neq(result.exit_code, 0);

        require_eq(field(failure(result.stdout_text, 0), "matcher"), std::string{"\"check_eq\""});
        // The contained value, not the type name — naming optional alone hid the wrap.
        require_eq(field(failure(result.stdout_text, 0), "actual"), std::string{"\"-1\""});
        require_eq(field(failure(result.stdout_text, 0), "expected"), std::string{"4294967295"});

        require_eq(field(failure(result.stdout_text, 3), "matcher"), std::string{"\"check_eq\""});
        require_eq(field(failure(result.stdout_text, 3), "actual"), std::string{"\"-1\""});

        require_eq(field(failure(result.stdout_text, 4), "matcher"), std::string{"\"check_contains\""});
        require_eq(field(failure(result.stdout_text, 5), "matcher"), std::string{"\"check_eq\""});
        require_eq(field(failure(result.stdout_text, 5), "actual"), std::string{"\"nullopt\""});
    };

    test_case("test_case [.probe-wrapper-signedness] reference_wrapper duration unexpected atomic failures") = []
    {
        int n = -1;
        check_eq(std::ref(n), 4294967295u);
        check_gt(std::ref(n), 0u);
        check_eq(std::chrono::duration<int>{-1},
                 std::chrono::duration<unsigned>{4294967295u});
        check_contains(std::vector{std::unexpected<int>{-1}},
                       std::unexpected<unsigned>{4294967295u});
        auto atomics = std::array<std::atomic<int>, 1>{};
        atomics[0].store(-1);
        check_contains(atomics, 4294967295u);
    };

    test_case("test_case [self] wrappers follow the scalar comparison rule") = []
    {
        // reference_wrapper's common_type with an integer is the integer, so equal_to
        // converted through the referred type and wrapped: ref(-1) met 4294967295u.
        // Durations of differently signed reps build a common_type whose rep is unsigned.
        // unexpected and atomic have no common_type with a hetero partner, so container
        // matchers fell through to operator==, which still converts.
        int n = -1;
        require_neq(std::ref(n), 4294967295u);
        require_lt(std::ref(n), 0u);
        require_eq(std::ref(n), -1);
        require_neq(std::chrono::duration<int>{-1},
                    std::chrono::duration<unsigned>{4294967295u});
        require_eq(std::chrono::duration<int>{1},
                   std::chrono::duration<unsigned>{1u});
        // Cross-period floating reps must keep chrono's floating common_type. Widening
        // to intmax_t is ill-formed (float→int truncation is refused) and broke
        // check_eq(1.5ms, 1500us). Integer cross-period still widens first.
        require_eq(std::chrono::duration<double, std::milli>{1.5},
                   std::chrono::duration<double, std::micro>{1500.0});
        require_neq(std::chrono::duration<double, std::milli>{1.7},
                    std::chrono::duration<double, std::micro>{1000.0});
        require_eq(std::chrono::duration<int, std::milli>{1},
                   std::chrono::duration<unsigned, std::micro>{1000u});
        require_lt(std::chrono::duration<int, std::milli>{-1},
                   std::chrono::duration<unsigned, std::micro>{0u});
        require_contains(std::vector{std::unexpected<int>{1}},
                         std::unexpected<unsigned>{1u});
        auto atomics = std::array<std::atomic<int>, 1>{};
        atomics[0].store(1);
        require_contains(atomics, 1u);

        const auto result = probe("[.probe-wrapper-signedness]");
        require_neq(result.exit_code, 0);

        require_eq(field(failure(result.stdout_text, 0), "matcher"), std::string{"\"check_eq\""});
        require_eq(field(failure(result.stdout_text, 0), "actual"), std::string{"\"-1\""});
        require_eq(field(failure(result.stdout_text, 0), "expected"), std::string{"4294967295"});

        require_eq(field(failure(result.stdout_text, 1), "matcher"), std::string{"\"check_gt\""});
        require_eq(field(failure(result.stdout_text, 2), "matcher"), std::string{"\"check_eq\""});
        require_eq(field(failure(result.stdout_text, 2), "actual"), std::string{"\"-1\""});
        require_eq(field(failure(result.stdout_text, 3), "matcher"), std::string{"\"check_contains\""});
        require_eq(field(failure(result.stdout_text, 4), "matcher"), std::string{"\"check_contains\""});
        // Five probe failures: ref eq, ref gt, duration eq, unexpected contains, atomic contains.
        require_eq(field(failure(result.stdout_text, 3), "expected"),
                   std::string{"\"contains: unexpected(4294967295)\""});
    };

    test_case("test_case [.probe-expected-void] expected<void> value vs unexpected") = []
    {
        using E = std::expected<void, std::string>;
        check_eq(E{}, E{std::unexpect, "x"});
        check_eq(E{std::unexpect, "a"}, E{std::unexpect, "b"});
    };

    test_case("test_case [self] expected<void> compares without forming a void expression") = []
    {
        // expected<void, E> has no contained value: *e is a void expression. The unwrap
        // path from optional/expected signedness used values_equal(*a, *b) on every
        // engaged pair, which made require_eq(E{}, E{}) ill-formed after that fix.
        using E = std::expected<void, std::string>;
        require_eq(E{}, E{});
        require_neq(E{}, E{std::unexpect, "x"});
        require_eq(E{std::unexpect, "x"}, E{std::unexpect, "x"});
        require_neq(E{std::unexpect, "a"}, E{std::unexpect, "b"});
        require_lt(E{std::unexpect, "a"}, E{});
        require_lt(E{std::unexpect, "a"}, E{std::unexpect, "b"});

        const auto result = probe("[.probe-expected-void]");
        require_neq(result.exit_code, 0);
        require_eq(field(failure(result.stdout_text, 0), "matcher"), std::string{"\"check_eq\""});
        require_eq(field(failure(result.stdout_text, 0), "actual"), std::string{"\"void\""});
        require_eq(field(failure(result.stdout_text, 0), "expected"),
                   std::string{"\"unexpected(x)\""});
        require_eq(field(failure(result.stdout_text, 1), "matcher"), std::string{"\"check_eq\""});
    };

    test_case("test_case [.probe-unwrap-partners] nullopt and unexpected partner failures") = []
    {
        // These must fail: disengaged optional equals nullopt, and unexpected expected
        // equals the matching unexpected. The unwrap used to treat both partners as bare
        // operands and report not-equal, so check_neq falsely passed.
        check_neq(std::optional<int>{}, std::nullopt);
        check_neq(std::nullopt, std::optional<int>{});
        check_lt(std::optional<int>{}, std::nullopt);
        check_neq(std::expected<int, int>{std::unexpect, 5}, std::unexpected<int>{5});
        check_neq(std::unexpected<int>{5}, std::expected<int, int>{std::unexpect, 5});
        check_neq(std::expected<void, std::string>{std::unexpect, "x"},
                  std::unexpected<std::string>{"x"});
        check_eq(std::expected<int, int>{std::unexpect, -1},
                 std::unexpected<unsigned>{4294967295u});
    };

    test_case("test_case [self] optional nullopt and expected unexpected partners compare") = []
    {
        // After the optional/expected unwrap, common_type partners were mishandled:
        // nullopt and unexpected converted as bare operands, so optional{} == nullopt and
        // expected{unexpect, 5} == unexpected{5} returned false. require_neq then passed.
        require_eq(std::optional<int>{}, std::nullopt);
        require_eq(std::nullopt, std::optional<int>{});
        require_neq(std::optional<int>{1}, std::nullopt);
        require_lt(std::nullopt, std::optional<int>{1});
        require_false(std::optional<int>{} < std::nullopt);

        require_eq(std::expected<int, int>{std::unexpect, 5}, std::unexpected<int>{5});
        require_eq(std::unexpected<int>{5}, std::expected<int, int>{std::unexpect, 5});
        require_neq(std::expected<int, int>{std::unexpect, 5}, std::unexpected<int>{6});
        require_neq(std::expected<int, int>{5}, std::unexpected<int>{5});
        require_lt(std::unexpected<int>{5}, std::expected<int, int>{5});
        require_eq(std::expected<void, std::string>{std::unexpect, "x"},
                   std::unexpected<std::string>{"x"});
        require_neq(std::expected<int, int>{std::unexpect, -1},
                    std::unexpected<unsigned>{4294967295u});

        const auto result = probe("[.probe-unwrap-partners]");
        require_neq(result.exit_code, 0);
        require_eq(field(failure(result.stdout_text, 0), "matcher"), std::string{"\"check_neq\""});
        require_eq(field(failure(result.stdout_text, 0), "actual"), std::string{"\"nullopt\""});
        require_eq(field(failure(result.stdout_text, 0), "expected"), std::string{"\"nullopt\""});
        require_eq(field(failure(result.stdout_text, 3), "matcher"), std::string{"\"check_neq\""});
        require_eq(field(failure(result.stdout_text, 3), "actual"),
                   std::string{"\"unexpected(5)\""});
        require_eq(field(failure(result.stdout_text, 3), "expected"),
                   std::string{"\"unexpected(5)\""});
        require_eq(field(failure(result.stdout_text, 5), "matcher"), std::string{"\"check_neq\""});
        require_eq(field(failure(result.stdout_text, 5), "actual"),
                   std::string{"\"unexpected(x)\""});
        require_eq(field(failure(result.stdout_text, 6), "matcher"), std::string{"\"check_eq\""});
    };

    test_case("test_case [.probe-time-point-signedness] time_point failures across signedness") = []
    {
        using clock = std::chrono::system_clock;
        using tpi = std::chrono::time_point<clock, std::chrono::duration<int>>;
        using tpu = std::chrono::time_point<clock, std::chrono::duration<unsigned>>;
        // These must fail: mixed-signedness epochs wrap through common_type / operator==.
        check_eq(tpi{std::chrono::duration<int>{-1}},
                 tpu{std::chrono::duration<unsigned>{4294967295u}});
        // Ordering wrap: 0 < max after conversion, so check_lt used to pass.
        check_lt(tpu{std::chrono::duration<unsigned>{0}},
                 tpi{std::chrono::duration<int>{-1}});
        check_contains(std::vector{tpu{std::chrono::duration<unsigned>{4294967295u}}},
                       tpi{std::chrono::duration<int>{-1}});
    };

    test_case("test_case [self] time_point epochs follow the scalar comparison rule") = []
    {
        // time_point is not duration-like, but two points whose duration reps differ in
        // signedness still share a common_type: equal_to / operator== then compare epochs
        // through an unsigned common duration and wrap -1 into max. Unwrap to epoch counts.
        using clock = std::chrono::system_clock;
        using tpi = std::chrono::time_point<clock, std::chrono::duration<int>>;
        using tpu = std::chrono::time_point<clock, std::chrono::duration<unsigned>>;
        require_neq(tpi{std::chrono::duration<int>{-1}},
                    tpu{std::chrono::duration<unsigned>{4294967295u}});
        require_lt(tpi{std::chrono::duration<int>{-1}},
                   tpu{std::chrono::duration<unsigned>{0}});
        require_eq(tpi{std::chrono::duration<int>{5}},
                   tpu{std::chrono::duration<unsigned>{5}});
        require_lt(tpi{std::chrono::duration<int>{1}},
                   tpi{std::chrono::duration<int>{2}});
        // Floating epoch periods: same intmax widen trap as durations.
        using tpm = std::chrono::time_point<clock, std::chrono::duration<double, std::milli>>;
        using tpmu = std::chrono::time_point<clock, std::chrono::duration<double, std::micro>>;
        require_eq(tpm{std::chrono::duration<double, std::milli>{1.5}},
                   tpmu{std::chrono::duration<double, std::micro>{1500.0}});
        require_neq(tpm{std::chrono::duration<double, std::milli>{1.7}},
                    tpmu{std::chrono::duration<double, std::micro>{1000.0}});

        const auto result = probe("[.probe-time-point-signedness]");
        require_neq(result.exit_code, 0);
        require_eq(field(failure(result.stdout_text, 0), "matcher"), std::string{"\"check_eq\""});
        require_eq(field(failure(result.stdout_text, 0), "actual"), std::string{"\"-1\""});
        require_eq(field(failure(result.stdout_text, 0), "expected"), std::string{"\"4294967295\""});
        require_eq(field(failure(result.stdout_text, 1), "matcher"), std::string{"\"check_lt\""});
        require_eq(field(failure(result.stdout_text, 2), "matcher"), std::string{"\"check_contains\""});
    };

    test_case("test_case [self] container elements follow the scalar comparison rule") = []
    {
        // The container matchers compared elements with `==`, so they kept converting
        // where check_eq had stopped: a size_t of npos matched -1, and {-1} matched
        // {4294967295u}. Elements now go through the same comparator as the scalar
        // matchers, which is also why the epsilon reaches floating-point elements.
        // std::ranges::contains still answers yes here, which is exactly what the matcher
        // used to call: the -1 converts and meets npos.
        const auto sizes = std::vector<std::size_t>{std::string::npos};
        require_true(std::ranges::contains(sizes, -1));

        require_container_eq(std::vector<int>{1, 2}, std::vector<unsigned>{1u, 2u});
        require_contains(std::vector<std::size_t>{3}, 3);
        require_container_eq(std::vector<double>{0.1 + 0.2}, std::vector<double>{0.3});
        require_container_eq(std::vector<std::string>{"a"}, std::vector<std::string>{"a"});
        require_contains(std::vector<std::string>{"a", "b"}, std::string{"b"});

        const auto result = probe("[.probe-container-signedness]");
        require_neq(result.exit_code, 0);
        require_eq(field(failure(result.stdout_text, 0), "matcher"), std::string{"\"check_contains\""});
        require_eq(field(failure(result.stdout_text, 1), "matcher"), std::string{"\"check_container_eq\""});
    };

    test_case("test_case [self] container diffs locate the mismatch") = []
    {
        const auto result = probe("[.probe-container]");
        require_neq(result.exit_code, 0);

        const auto mismatch = failure(result.stdout_text, 0);
        require_eq(field(mismatch, "matcher"), std::string{"\"check_container_eq\""});
        require_eq(field(mismatch, "actual"), std::string{"\"[1, 9, 3]\""});
        require_contains(field(mismatch, "expected"), std::string{"mismatch at index 1"});
        require_contains(field(mismatch, "expected"), std::string{"actual=9"});
        require_contains(field(mismatch, "expected"), std::string{"expected=2"});

        const auto actual_longer = failure(result.stdout_text, 1);
        require_contains(field(actual_longer, "expected"), std::string{"actual has 1 extra element"});
        require_contains(field(actual_longer, "expected"), std::string{"index 3"});

        const auto expected_longer = failure(result.stdout_text, 2);
        require_contains(field(expected_longer, "expected"),
                         std::string{"expected has 1 extra element"});

        const auto element = failure(result.stdout_text, 3);
        require_eq(field(element, "matcher"), std::string{"\"check_contains\""});
        require_eq(field(element, "actual"), std::string{"\"[1, 2, 3]\""});
        require_eq(field(element, "expected"), std::string{"\"contains: 9\""});
    };

    // ========================================================================
    // Strings
    // ========================================================================

    test_case("test_case [.probe-string] string failures") = []
    {
        check_contains(std::string{"abcdef"}, std::string{"xyz"});
        check_contains(std::string{"abcdef"}, 'z');
        check_starts_with(std::string{"abcdef"}, std::string{"xyz"});
        check_ends_with(std::string{"abcdef"}, std::string{"xyz"});
    };

    test_case("test_case [self] string matchers on satisfied inputs") = []
    {
        require_contains(std::string{"abcdef"}, std::string{"cde"});
        require_contains(std::string{"abcdef"}, 'c');
        require_starts_with(std::string{"abcdef"}, std::string{"abc"});
        require_ends_with(std::string{"abcdef"}, std::string{"def"});
        require_has_substr(std::string{"abcdef"}, std::string{"bcd"});
        // string_view and literal operands are accepted alongside std::string.
        require_contains(std::string_view{"abcdef"}, "cde");
        require_starts_with("abcdef", "abc");
    };

    test_case("test_case [self] string matchers describe the expectation") = []
    {
        const auto result = probe("[.probe-string]");
        require_neq(result.exit_code, 0);

        const auto substring = failure(result.stdout_text, 0);
        require_eq(field(substring, "matcher"), std::string{"\"check_contains\""});
        require_eq(field(substring, "actual"), std::string{"\"abcdef\""});
        require_eq(field(substring, "expected"), std::string{"\"contains: xyz\""});

        // The char overload built its needle with std::string{1, ch}, which selects
        // the initializer_list constructor and prefixes a \x01 control character.
        const auto character = failure(result.stdout_text, 1);
        require_eq(field(character, "matcher"), std::string{"\"check_contains\""});
        require_eq(field(character, "expected"), std::string{"\"contains: 'z'\""});

        require_eq(field(failure(result.stdout_text, 2), "expected"),
                   std::string{"\"starts with: xyz\""});
        require_eq(field(failure(result.stdout_text, 3), "expected"),
                   std::string{"\"ends with: xyz\""});
    };

    test_case("test_case [self] string operands are never demangled") = []
    {
        const auto result = probe("[.probe-mangled-string]");
        require_neq(result.exit_code, 0);

        // Test data that happens to look like a mangled symbol must survive intact.
        const auto line = failure(result.stdout_text, 0);
        require_eq(field(line, "actual"), std::string{"\"_ZNSt6vectorIiEC1Ev\""});

        // The console channel formats the same operand independently and used to run
        // it through abi::__cxa_demangle, rendering it as std::vector<int>::vector().
        const auto console = run_test_runner({"--tags=[.probe-mangled-string]"});
        require_contains(console.stderr_text, std::string{"_ZNSt6vectorIiEC1Ev"});
        require_false(console.stderr_text.contains("std::vector<int>::vector()"));
    };

    test_case("test_case [.probe-mangled-string] mangled-looking string data") = []
    {
        check_eq(std::string{"_ZNSt6vectorIiEC1Ev"}, std::string{"other"});
    };

    // ========================================================================
    // Statistics and messaging
    // ========================================================================

    test_case("test_case [.probe-check-only] non-fatal failure only") = []
    {
        check_eq(1, 2);
    };

    test_case("test_case [self] a check-only failure fails its test in the summary") = []
    {
        const auto result = probe("[.probe-check-only]");
        require_eq(result.exit_code, 1);

        // A non-fatal failure does not throw, so the test body returns normally.
        // tests_ok must still exclude it, or agents comparing tests_ok to
        // tests_total conclude the run passed while failed_test_ids is non-empty.
        const auto summary = find_event(result.stdout_text, "summary");
        require_false(summary.empty());
        require_eq(field(summary, "tests_total"), std::string{"1"});
        require_eq(field(summary, "tests_ok"), std::string{"0"});
        require_eq(field(summary, "assertions_total"), std::string{"1"});
        require_eq(field(summary, "assertions_ok"), std::string{"0"});
        require_eq(field(summary, "passed"), std::string{"false"});
        require_contains(field(summary, "failed_test_ids"), std::string{"probe-check-only"});

        require_eq(event_field(result.stdout_text, "test", "success"), std::string{"false"});
    };

    test_case("test_case [self] a fatal failure stops the rest of the test body") = []
    {
        const auto result = probe("[.probe-require-stops]");
        require_eq(result.exit_code, 1);

        // require_eq throws, so the assertion after it must never run.
        const auto summary = find_event(result.stdout_text, "summary");
        require_eq(field(summary, "assertions_total"), std::string{"1"});
        require_eq(field(summary, "tests_ok"), std::string{"0"});
    };

    test_case("test_case [.probe-require-stops] fatal stops execution") = []
    {
        require_eq(1, 2);
        check_eq(3, 3); // unreachable
    };

    test_case("test_case [self] succeed and failed adjust assertion counts") = []
    {
        const auto result = probe("[.probe-messages]");
        require_eq(result.exit_code, 1);

        const auto summary = find_event(result.stdout_text, "summary");
        // succeed() counts as a passing assertion, failed() as a failing one, and
        // warning() as neither.
        require_eq(field(summary, "assertions_total"), std::string{"2"});
        require_eq(field(summary, "assertions_ok"), std::string{"1"});

        const auto message = find_event(result.stdout_text, "message");
        require_false(message.empty());
        require_eq(field(message, "ok"), std::string{"false"});
        require_eq(field(message, "message"), std::string{"\"explicit failure\""});
    };

    test_case("test_case [.probe-messages] messaging helpers") = []
    {
        succeed("explicit success");
        warning("just a warning");
        failed("explicit failure");
    };

    return 0;
}

const auto _ = register_tests();

} // namespace tester::selftest::assertions
