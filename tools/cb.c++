// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

// deps/tester/tools/cb.c++ — C++ Builder & Tester: The C++ Builder
// Part of the C++ Builder & Tester project - the greatest single-file C++ build system in existence
// clang++ -std=c++23 -O3 -pthread -fuse-ld=lld deps/tester/tools/cb.c++ -o tools/cb

#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <regex>
#include <fstream>
#include <queue>
#include <flat_map>
#include <flat_set>
#include <optional>
#include <array>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <ranges>
#include <utility>
#include <stdexcept>
#include <system_error>
#include <cctype>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <semaphore>
#include "cb-jsonl_observer.h++"
#include "cb-console_observer.h++"

namespace fs = std::filesystem;

// Process environment for posix_spawn; declared here so it is not looked up as cb::environ.
extern char** environ;

namespace cb {

using namespace std::string_literals;
using namespace std::string_view_literals;

// ============================================================================
// Project naming conventions — edit these for other layouts
// ============================================================================

using suffix_list = std::vector<std::string_view>;
using string_list = std::vector<std::string>;

constexpr auto test_cxxm_suffix = ".test.c++m"sv;
constexpr auto test_cxx_suffix = ".test.c++"sv;
constexpr auto impl_cxx_suffix = ".impl.c++"sv;
constexpr auto cxxm_suffix = ".c++m"sv;
constexpr auto cppm_suffix = ".cppm"sv;
constexpr auto cxx_suffix = ".c++"sv;
constexpr auto cpp_suffix = ".cpp"sv;

// Source extensions CB will scan and compile. Prefixed forms (`.test.`, `.impl.`)
// must appear before their base extension so `make_base_name` strips the longest match.
const suffix_list supported_suffixes = {
    test_cxxm_suffix,
    test_cxx_suffix,
    impl_cxx_suffix,
    cxxm_suffix,
    cppm_suffix,
    cxx_suffix,
    cpp_suffix
};

// Language extensions replaced by `.o` when naming object files (any prefix such as
// `.test` / `.impl` is kept). Edit alongside `supported_suffixes` for other conventions.
const suffix_list object_stem_suffixes = {
    cxxm_suffix,
    cppm_suffix,
    cxx_suffix,
    cpp_suffix
};

// CB build tree: `build-<os>-{debug|release}/`. Alternatives advertise themselves:
// Make → `build-make-<os>-<config>/`, CMake → `build-cmake-<os>-<config>/`.
constexpr auto build_root_prefix = "build-"sv;
constexpr auto debug_build_suffix = "-debug"sv;
constexpr auto release_build_suffix = "-release"sv;
constexpr auto pcm_dir_name = "pcm"sv;
constexpr auto obj_dir_name = "obj"sv;
constexpr auto bin_dir_name = "bin"sv;
constexpr auto cache_dir_name = "cache"sv;
constexpr auto object_cache_filename = "object-cache.txt"sv;
constexpr auto executable_cache_filename = "executable-cache.txt"sv;
constexpr auto std_module_profile_filename = "std-module-profile.txt"sv;
constexpr auto compiler_version_filename = "compiler-version.txt"sv;
constexpr auto std_pcm_filename = "std.pcm"sv;
constexpr auto std_obj_filename = "std.o"sv;
constexpr auto compile_commands_filename = "compile_commands.json"sv;
constexpr auto graph_filename = "graph.json"sv;
constexpr auto std_module_name = "std"sv;
constexpr auto pcm_extension = ".pcm"sv;
constexpr auto object_extension = ".o"sv;
constexpr auto test_runner_name = "test_runner"sv;

constexpr auto module_file_flag_prefix = "-fmodule-file="sv;
constexpr auto prebuilt_module_path_flag_prefix = "-fprebuilt-module-path="sv;

// Source-scan directory names (exact path components unless noted).
constexpr auto deps_dir_prefix = "deps/"sv;
constexpr auto deps_dir_name = "deps"sv;
constexpr auto tester_dir_name = "tester"sv;
constexpr auto test_dir_name = "test"sv;
constexpr auto tests_dir_name = "tests"sv;
constexpr auto tools_dir_name = "tools"sv;
constexpr auto examples_dir_name = "examples"sv;
constexpr auto git_dir_name = ".git"sv;

// Env vars CB sets when spawning test_runner.
constexpr auto tester_config_env = "TESTER_CONFIG"sv;
constexpr auto tester_parent_run_id_env = "TESTER_PARENT_RUN_ID"sv;

enum class unit_kind : unsigned {
    non_module,          // non-modular source, no module declaration at all  (.c++ .cpp)
    interface_unit,      // module interface, export module name;             (.c++m .cppm)
    partition_unit,      // module partition, export module name:part;        (.c++m .cppm)
    implementation_unit, // module implementation, module name;               (.impl.c++)
    global_fragment      // global module fragment, only contains "module;"   (.c++m .cppm)
};

namespace detail {

std::string join_dir(std::string_view root, std::string_view name)
{
    auto out = std::string{root};
    out.push_back('/');
    out.append(name);
    return out;
}

// True for `dir/...` or `.../dir/...` (not a bare `dir` leaf, not `dirfoo`).
bool path_under_dir(std::string_view path, std::string_view dir)
{
    if(path.starts_with(dir) and path.size() > dir.size() and path[dir.size()] == '/')
        return true;
    auto needle = std::string{};
    needle.reserve(dir.size() + 2);
    needle.push_back('/');
    needle.append(dir);
    needle.push_back('/');
    return path.contains(needle);
}

// True for `dir` or `dir/...`.
bool is_dir_or_under(std::string_view path, std::string_view dir)
{
    return path == dir
        or (path.starts_with(dir) and path.size() > dir.size() and path[dir.size()] == '/');
}

std::string module_file_flag(std::string_view module_name, std::string_view pcm_path)
{
    auto out = std::string{module_file_flag_prefix};
    out.append(module_name);
    out.push_back('=');
    out.append(pcm_path);
    return out;
}

std::string shell_quote(std::string_view arg)
{
    // POSIX: wrap in single quotes; internal ' becomes '\''.
    auto out = std::ranges::fold_left(arg, "'"s, [](std::string acc, char c)
    {
        if(c == '\'')
            acc += "'\\''"sv;
        else
            acc += c;
        return acc;
    });
    out += '\'';
    return out;
}

std::string_view unit_kind_name(unit_kind kind)
{
    switch(kind)
    {
        case unit_kind::non_module: return "non_module";
        case unit_kind::interface_unit: return "interface";
        case unit_kind::partition_unit: return "partition";
        case unit_kind::implementation_unit: return "implementation";
        case unit_kind::global_fragment: return "global_fragment";
    }
    return "unknown";
}

// Collapse any isspace run to a single space; trim leading/trailing whitespace.
std::string collapse_whitespace(std::string_view text)
{
    auto out = std::ranges::fold_left(
        text,
        std::string{},
        [](std::string acc, const char ch) {
            if(std::isspace(static_cast<unsigned char>(ch)) != 0)
            {
                if(not acc.empty() && acc.back() != ' ')
                    acc += ' ';
            }
            else
                acc += ch;
            return acc;
        });
    if(not out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// Parse external flag text only (CLI --compile-flags, object-cache profile fields).
// CB stores flags as string_list; this runs at text boundaries, not in argv builders.
// Symmetric with flags_profile_string (views::join_with on ' '); not POSIX shell parsing.
string_list parse_external_flag_text(std::string_view text)
{
    const auto normalized = collapse_whitespace(text);
    return normalized
        | std::views::split(' ')
        | std::views::transform([](auto&& part) { return std::string_view{part}; })
        | std::views::filter([](std::string_view t) { return not t.empty(); })
        | std::views::transform([](std::string_view t) { return std::string{t}; })
        | std::ranges::to<string_list>();
}

std::string flags_profile_string(const string_list& flags)
{
    return flags | std::views::join_with(" "sv) | std::ranges::to<std::string>();
}

std::string join_argv(const string_list& argv)
{
    return argv
        | std::views::transform([](const std::string& arg) { return shell_quote(arg); })
        | std::views::join_with(" "sv)
        | std::ranges::to<std::string>();
}

using profile_fields = std::flat_map<std::string, std::string, std::less<>>;

// Object-cache profile values must not contain '\t', '\n', '\r', or '%' (tab-delimited format).
void append_profile_field(std::string& profile, std::string_view key, std::string_view value)
{
    if(not profile.empty())
        profile += '\t';
    profile += key;
    profile += '=';
    profile += value;
}

std::pair<std::string, std::string> parse_profile_field(std::string_view segment)
{
    const auto eq = segment.find('=');
    return {
        std::string{segment.substr(0, eq)},
        std::string{segment.substr(eq + 1)}};
}

profile_fields parse_object_cache_profile_fields(std::string_view profile)
{
    return profile
        | std::views::split('\t')
        | std::views::transform([](auto&& part) { return parse_profile_field(std::string_view{part}); })
        | std::ranges::to<profile_fields>();
}

output::profile_token_change diff_profile_tokens(std::string_view old_text, std::string_view new_text)
{
    auto old_tokens = parse_external_flag_text(old_text);
    auto new_tokens = parse_external_flag_text(new_text);
    std::ranges::sort(old_tokens);
    std::ranges::sort(new_tokens);

    auto change = output::profile_token_change{};
    std::ranges::set_difference(new_tokens, old_tokens, std::back_inserter(change.added));
    std::ranges::set_difference(old_tokens, new_tokens, std::back_inserter(change.removed));
    return change;
}

output::object_cache_profile_diff diff_object_cache_profiles(std::string_view old_profile, std::string_view new_profile)
{
    const auto old_fields = parse_object_cache_profile_fields(old_profile);
    const auto new_fields = parse_object_cache_profile_fields(new_profile);
    auto diff = output::object_cache_profile_diff{};

    // Named, because `return {}` has nothing to deduce from.
    const auto field_value = [](const profile_fields& fields, std::string_view key) -> std::string {
        if(fields.contains(key))
            return fields.at(key);
        return {};
    };

    const auto diff_scalar = [&](std::string_view key, std::optional<output::profile_scalar_change>& out) {
        const auto old_value = field_value(old_fields, key);
        const auto new_value = field_value(new_fields, key);
        if(old_value != new_value)
            out = output::profile_scalar_change{old_value, new_value};
    };

    output::for_each_profile_scalar(diff, diff_scalar);

    const auto diff_tokens = [&](std::string_view key, std::optional<output::profile_token_change>& out) {
        auto change = diff_profile_tokens(field_value(old_fields, key), field_value(new_fields, key));
        if(change.changed())
            out = std::move(change);
    };

    output::for_each_profile_tokens(diff, diff_tokens);
    return diff;
}

std::string make_base_name(std::string_view filename)
{
    const auto suffix = std::ranges::find_if(supported_suffixes, [&](std::string_view s) {
        return filename.ends_with(s);
    });
    if(suffix != supported_suffixes.end())
        return std::string{filename.substr(0, filename.size() - suffix->size())};
    return std::string{filename};
}

std::string extract_suffix(std::string_view filename)
{
    const auto suffix = std::ranges::find_if(supported_suffixes, [&](std::string_view s) {
        return filename.ends_with(s);
    });
    if(suffix == supported_suffixes.end())
        throw std::runtime_error{"unsupported source suffix"};
    return std::string{*suffix};
}

std::string normalize_relative_dir(const fs::path& dir) {
    if (dir.empty()) return "";
    auto str = dir.string();
    return str == "." ? "" : str;
}

// The name reports use for a unit: its directory and filename joined, or the bare filename
// for a unit at the project root.
std::string make_display_path(std::string_view dir, std::string_view filename) {
    return dir.empty() ? std::string{filename} : std::string{dir} + "/" + std::string{filename};
}

bool is_tester_framework_path(std::string_view path) {
    // Nested or top-level tester library trees (not *.test.c++ sources).
    return path_under_dir(path, tester_dir_name);
}

// Parent repos vendor packages under deps/<name>/. Nested checkouts such as
// deps/net/deps/tester belong to the child package and must not join the parent
// build — otherwise same-basename smoke fixtures collide across packages.
bool is_nested_dependency_path(std::string_view rel_path)
{
    if(not rel_path.starts_with(deps_dir_prefix))
        return false;

    const auto rest = rel_path.substr(deps_dir_prefix.size());
    const auto slash = rest.find('/');
    if(slash == std::string_view::npos)
        return false;

    const auto after_pkg = rest.substr(slash + 1);
    return is_dir_or_under(after_pkg, deps_dir_name);
}

// Top-level build-* trees are outputs (CB, Make, CMake), not project sources.
// CMake's CompilerId .cpp under build-cmake-*/ would otherwise join the scan and
// collide across configure trees.
bool is_build_output_path(std::string_view rel_path)
{
    const auto first = rel_path.substr(0, rel_path.find('/'));
    return first.starts_with(build_root_prefix);
}

bool path_has_test_segment(std::string_view path) {
    // Match path components named exactly "test" or "tests" (not "tester" / "test_exception_bug").
    auto rest = path;
    while (not rest.empty()) {
        const auto slash = rest.find('/');
        const auto segment = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        if (segment == test_dir_name or segment == tests_dir_name)
            return true;
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    return false;
}

// First-level deps/tester/tests/... (and tester/tests/...) smoke fixtures must not join the
// consumer scan: determine_is_test calls every tester/ path a library source, so fixture mains
// such as hello.c++ would compile into parent builds and collide on the duplicate-key check.
bool is_tester_package_tests_path(std::string_view rel_path)
{
    return is_tester_framework_path(rel_path) and path_has_test_segment(rel_path);
}

// deps/<pkg>/test and deps/<pkg>/tests are the vendored package's own suites (benchmarks,
// package-local tests), not the consumer's, so they stay out of the parent build even now that
// project test/ trees are in the scan (#14). Co-located deps/<pkg>/*.test.c++ still joins.
bool is_dependency_package_tests_path(std::string_view rel_path)
{
    if(not rel_path.starts_with(deps_dir_prefix))
        return false;

    const auto rest = rel_path.substr(deps_dir_prefix.size());
    const auto slash = rest.find('/');
    if(slash == std::string_view::npos)
        return false;

    const auto after_pkg = rest.substr(slash + 1);
    return is_dir_or_under(after_pkg, test_dir_name) or is_dir_or_under(after_pkg, tests_dir_name);
}

bool determine_is_test(std::string_view rel_dir, std::string_view name, std::string_view suffix_value) {
    const auto combined = rel_dir.empty() ? std::string{name} : std::string{rel_dir} + "/" + std::string{name};
    if (is_tester_framework_path(combined))
        return false;
    if (suffix_value == test_cxx_suffix or suffix_value == test_cxxm_suffix)
        return true;
    return path_has_test_segment(combined);
}

std::string make_unit(std::string_view module_value, unit_kind kind, std::string_view filename_value) {
    switch (kind) {
        case unit_kind::interface_unit:
        case unit_kind::partition_unit:
            return std::string{module_value};
        case unit_kind::implementation_unit:
        case unit_kind::non_module:
        case unit_kind::global_fragment:
            return std::string{filename_value};
    }
    return std::string{filename_value};
}

std::string make_full_path(const fs::path& file_path) {
    auto absolute = file_path;
    if (absolute.is_relative()) absolute = fs::absolute(absolute);
    try {
        absolute = fs::canonical(absolute);
    } catch (...) {
        absolute = fs::absolute(absolute);
    }
    return absolute.string();
}

std::string binary_signature(const std::string& path)
{
    if(not fs::exists(path))
        return {};

    const auto size = fs::file_size(path);
    const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
        fs::last_write_time(path).time_since_epoch()).count();
    return std::to_string(size) + ':' + std::to_string(ticks);
}

// Make-style depfile (clang -MMD -MF): `target: prereq prereq \<newline> prereq`, spaces in a
// path backslash-escaped. Returns the prerequisites only. `nullopt` when the file cannot be
// trusted — unreadable, or lacking the `target:` every depfile has — which is not the same
// answer as an empty list: conflating them turns an unreadable depfile into a cache hit.
std::optional<string_list> parse_depfile(const std::string& path)
{
    auto file = std::ifstream{path};
    if(not file)
        return std::nullopt;

    auto text = std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if(const auto colon = text.find(':'); colon != std::string::npos)
        text.erase(0, colon + 1);
    else
        return std::nullopt;

    auto prerequisites = string_list{};
    auto current = std::string{};
    const auto flush = [&]
    {
        if(not current.empty())
            prerequisites.push_back(std::exchange(current, std::string{}));
    };

    for(auto index = std::size_t{}; index < text.size(); ++index)
    {
        const auto ch = text[index];
        if(ch == '\\' and index + 1 < text.size())
        {
            const auto next = text[index + 1];
            // Line continuation: the backslash and the newline both vanish.
            if(next == '\n' or next == '\r')
            {
                flush();
                continue;
            }
            // Escaped literal (most often `\ ` in a path).
            current.push_back(next);
            ++index;
            continue;
        }
        if(std::isspace(static_cast<unsigned char>(ch)) != 0)
            flush();
        else
            current.push_back(ch);
    }
    flush();

    return prerequisites;
}

// waitpid yields a wait status; decode once so command_end / test_end report the
// child's own exit_code (or signal) instead of the packed wait value (256 for exit 1).
inline output::process_status decode_wait_status(int status)
{
    if(status < 0) // spawn or waitpid itself failed
        return {.exit_code = status, .wait_status = status};
    if(WIFSIGNALED(status))
        return {.exit_code = -1, .wait_status = status, .signaled = true, .signal = WTERMSIG(status)};
    if(WIFEXITED(status))
        return {.exit_code = WEXITSTATUS(status), .wait_status = status};
    return {.exit_code = -1, .wait_status = status};
}

// Compiler output can run to megabytes on a template error; only the head is worth
// putting on a JSONL line, and the full capture stays on disk for the human.
inline constexpr auto diagnostics_head_limit = std::size_t{8192};

inline output::diagnostics read_diagnostics(std::string_view path)
{
    auto file = std::ifstream{std::string{path}, std::ios::binary};
    if(not file)
        return {};

    auto text = std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    auto diag = output::diagnostics{.path = std::string{path}, .bytes = text.size()};
    if(text.size() > diagnostics_head_limit)
    {
        text.resize(diagnostics_head_limit);
        diag.truncated = true;
    }
    diag.head = std::move(text);
    return diag;
}

// A modular unit runs two toolchain steps that share one compile_end, so warnings from the
// first must still be visible when the second also printed (or failed). Cap the combined head
// at the same limit a single capture uses.
inline void append_diagnostics(output::diagnostics& into, output::diagnostics&& more)
{
    if(more.empty())
        return;
    if(into.empty())
    {
        into = std::move(more);
        return;
    }

    if(not more.head.empty())
    {
        if(not into.head.empty())
            into.head.push_back('\n');
        into.head += more.head;
        if(into.head.size() > diagnostics_head_limit)
        {
            into.head.resize(diagnostics_head_limit);
            into.truncated = true;
        }
    }
    into.bytes += more.bytes;
    into.truncated = into.truncated or more.truncated;
    if(into.path.empty())
        into.path = std::move(more.path);
}

std::string read_first_line(const std::string& path)
{
    auto file = std::ifstream{path};
    if(not file)
        return {};

    auto line = ""s;
    if(not std::getline(file, line))
        return {};

    if(not line.empty() and line.back() == '\r')
        line.pop_back();
    return line;
}

constexpr std::string_view object_cache_format = "cb-object-cache-v3"sv;

// Comments, string literals and `#if 0` blocks are not declarations, and matching the
// module regexes against them invents edges in the module graph.

// Phase 2 of translation, done first because everything below assumes it: a backslash, any
// horizontal whitespace after it (C++23 trims it, with a diagnostic) and the newline go. The
// compiler splices before it recognises a directive, a comment or a literal, so a scanner
// reading physical lines disagrees with it about what the source says — `#if \` on its own
// line is `#if 0` there and an unrecognised directive here, and `import \` hides a real edge.
//
// Splices inside a raw-string body go too, which [lex.pptoken] would revert before looking for
// the closer. Honouring that means telling an `R"(` opener from the same text in a comment or
// another literal, which is lexical state rather than a pattern, and scanning here is
// regex-based on purpose. The cost is a body ending a line with `)\`, read as closing early;
// deciding it the other way cost more, freezing every later splice after a fake opener.
inline static const std::regex line_splice_regex{R"(\\[ \t\v\f]*\r?\n)"};

inline std::string splice_physical_lines(const std::string& text)
{
    return std::regex_replace(text, line_splice_regex, "");
}

// One alternation over the whole preamble, because a block comment and a raw string still
// span lines after splicing and cannot be recognised a line at a time. Alternation also gets
// the interleaving right for free: whichever construct opens first wins, so `"/*"` inside a
// string starts no comment, and a quote inside a comment stays inert. Comments collapse to a
// space, since a comment separates tokens (`import/*x*/foo`); literals keep their delimiters
// but lose contents that may spell `import foo;`, which the unanchored matchers would find.
//
// A raw string's body has no escapes, so only `)delimiter"` closes it, matched by backreference
// — the one unavoidable lazy `[\s\S]*?`, and it only runs where `R"` appears. `//` comments and
// quoted literals end at the newline, a continued one having already been joined. The quoted
// branches keep `\\.` for the escapes that remain (`"\""` closes nothing) and are unrolled as a
// character-class run plus a group per escape (`[^"\\\n]*(?:\\.[^"\\\n]*)*`) rather than a
// per-character alternation: run and escape group cannot both match a backslash, which is what
// keeps the form linear in a pass that has dominated scan time before.
inline static const std::regex comment_or_literal_regex{
    R"(//[^\n]*)"
    R"(|/\*[^*]*\*+(?:[^/*][^*]*\*+)*/)"
    R"(|R(")([^()\\ \t\r\n"]{0,16})\([\s\S]*?\)\2")"
    R"(|(")[^"\\\n]*(?:\\.[^"\\\n]*)*")"
    R"(|(')[^'\\\n]*(?:\\.[^'\\\n]*)*')"};

inline std::string strip_comments_and_literals(const std::string& text)
{
    // Comments collapse to a single space; a literal keeps its delimiters via whichever
    // quote group matched, so `"import foo;"` cannot register as an edge. Exactly one of
    // the three quote groups participates per match, so the others expand to nothing.
    return std::regex_replace(text, comment_or_literal_regex, " $1$1$3$3$4$4");
}

// `#if 0` is the commented-out idiom, so its body is elided wholesale — and the idiom has more
// spellings than the bare constant: parenthesised (`#if (0)`), short-circuited (`#if 0 &&
// OLD_FEATURE`), and spread over arms (`#elif 0`). Only nesting is beyond a regex, balanced
// delimiters not being regular, so directives are recognised and the depth counted. Genuine
// conditionals are left alone: scanning both branches of an `#ifdef` costs a spurious edge,
// while guessing which one is live risks dropping a real one.
class conditional_filter
{
public:
    // True when a line carries no live code: a preprocessor directive, or any line
    // inside a dead `#if` region or dead `#elif` arm.
    bool is_inactive(const std::string& line)
    {
        // Directives are a tiny minority of lines; the cheap check keeps the regex off
        // the hot path of a scan that runs on every build, cached or not.
        if(line.contains('#'))
        {
            auto m = std::smatch{};
            if(std::regex_match(line, m, directive_regex))
            {
                apply(m[1].str(), m[2].str());
                return true;
            }
        }
        return m_skip_depth > 0;
    }

private:
    // False without evaluating anything: the constant, in any grouping, optionally
    // short-circuiting the rest away. Grouping and whitespace carry no meaning here, so they go
    // first and `(0) && defined(X)` reads as `0&&definedX`. `!0` and `0 || X` stay live.
    static bool is_never_taken(std::string_view condition)
    {
        const auto normalized = condition
            | std::views::filter([](char c) { return c != '(' and c != ')' and not std::isspace(static_cast<unsigned char>(c)); })
            | std::ranges::to<std::string>();
        return normalized == "0" or normalized == "false"
            or normalized.starts_with("0&&") or normalized.starts_with("false&&");
    }

    void apply(std::string_view name, std::string_view condition)
    {
        const auto skipping = m_skip_depth > 0;
        if(name == "if" or name == "ifdef" or name == "ifndef")
        {
            ++m_if_depth;
            if(not skipping and name == "if" and is_never_taken(condition))
                m_skip_depth = m_if_depth;
        }
        else if(name == "endif")
        {
            if(skipping and m_if_depth == m_skip_depth)
                m_skip_depth = 0;
            if(m_if_depth > 0)
                --m_if_depth;
        }
        else if(name == "else" or name == "elif")
        {
            // Each arm is judged on its own: leaving the skip region on every `#elif` would
            // revive the next arm of `#if 0 / #elif 0`, and entering it on a dead arm is what
            // elides `#elif 0` after a live one. `#else` has no condition to rule it out.
            const auto dead_arm = name == "elif" and is_never_taken(condition);
            if(skipping and m_if_depth == m_skip_depth and not dead_arm)
                m_skip_depth = 0;
            else if(not skipping and dead_arm)
                m_skip_depth = m_if_depth;
        }
    }

    // Group 2 is the whole condition, so a trailing operand cannot fail the full-line
    // match: `#if 0 && OLD` used to match nothing, leaving the directive unrecognised,
    // its body live, and the `#endif` depth bookkeeping off by one.
    inline static const std::regex directive_regex{R"(\s*#\s*(\w+)\s*(.*))"};

    int m_if_depth = 0;
    int m_skip_depth = 0;
};

} // namespace detail

class translation_unit {
public:
    static bool match_supported_suffix(std::string_view filename, std::string& out_suffix);
    static bool is_supported(const fs::path& file_path);

    friend translation_unit parse_translation_unit(const fs::path& project_root, const fs::path& file_path);

    // File identity
    const std::string filename;
    const std::string path;
    const std::string suffix;
    const std::string base_name;
    const std::string full_path;
    // How reports name this unit: the source relative to the project root. Computed once
    // because the inventory and every rebuild sentence ask for the same string.
    const std::string display_path;
    const std::string unit;
    
    // Module information
    const std::string module;
    const string_list imports;
    
    // File properties
    const unit_kind kind = unit_kind::non_module;
    const bool has_main = false;
    const bool is_test = false;
    const bool is_modular = false;
    
    // Build artifacts
    std::string object_path{};
    std::string pcm_path{};
    std::string executable_path{};
    
    // Metadata
    fs::file_time_type last_modified{};
    int dependency_level = -1;

private:
    translation_unit(const fs::path& relative,
                     const fs::path& full_path,
                     std::string module,
                     string_list imports,
                     unit_kind kind_value,
                     bool has_main_flag);


    // Module names may contain '.' (e.g. demo.core); partitions use ':' (demo.core:part).
    inline static const std::regex module_regex{R"(\s*(?:export\s+)?module\s+([\w.:-]+)\s*;)"};
    inline static const std::regex export_module_regex{R"(\s*export\s+module\s+([\w.:-]+)\s*;)"};
    inline static const std::regex fragment_regex{R"(\s*module\s*;)"};  // Global module fragment: just "module;"
    inline static const std::regex import_regex{R"(\s*(?:export\s+)?(?:import|module)\s+([\w.:-]+)\s*;)"};
    inline static const std::regex main_regex{R"(\s*int\s+main\s*\()"};
    inline static const std::regex keyword_regex{R"(\b(class|struct|namespace|constexpr|inline|static)\b)"};
    inline static const std::regex using_namespace_regex{R"(\busing\s+namespace\b)"};
};

bool translation_unit::match_supported_suffix(std::string_view filename, std::string& out_suffix)
{
    const auto suffix = std::ranges::find_if(supported_suffixes, [&](std::string_view s) {
        return filename.ends_with(s);
    });
    if(suffix == supported_suffixes.end())
        return false;
    out_suffix.assign(*suffix);
    return true;
}

bool translation_unit::is_supported(const fs::path& file_path) {
    auto name = file_path.filename().string();
    auto suffix = std::string{};
    return match_supported_suffix(name, suffix);
}

translation_unit::translation_unit(const fs::path& relative,
                                          const fs::path& full_path,
                                          std::string module_value,
                                          string_list imports_value,
                                          unit_kind kind_value,
                                          bool has_main_flag)
    : filename(relative.filename().string()),
      path(detail::normalize_relative_dir(relative.parent_path())),
      suffix(detail::extract_suffix(relative.filename().string())),
      base_name(detail::make_base_name(this->filename)),
      full_path(detail::make_full_path(full_path)),
      display_path(detail::make_display_path(this->path, this->filename)),
      unit(detail::make_unit(module_value, kind_value, this->filename)),
      module(std::move(module_value)),
      imports(std::move(imports_value)),
      kind(kind_value),
      has_main(has_main_flag),
      is_test(detail::determine_is_test(this->path, this->filename, this->suffix)),
      is_modular(kind_value == unit_kind::interface_unit or kind_value == unit_kind::partition_unit),
      last_modified(fs::last_write_time(full_path)) {}

translation_unit parse_translation_unit(const fs::path& project_root, const fs::path& file_path) {
    auto relative_path = file_path.lexically_relative(project_root);
    if (relative_path.empty() or relative_path == ".") relative_path = file_path.filename();

    std::ifstream file{file_path};
    if (not file) throw std::runtime_error{"cannot open file"};

    std::string line;
    std::string module_name;
    string_list imports;
    unit_kind kind = unit_kind::non_module;
    bool has_main = false;
    int lines_scanned = 0;
    const int max_lines = 1000;  // generous

    auto trim = [](std::string_view s) {
        auto start = s.find_first_not_of(" \t\r");
        if (start == std::string::npos) return ""sv;
        auto end = s.find_last_not_of(" \t\r");
        return s.substr(start, end - start + 1);
    };

    bool seen_real_code = false;

    // Read the bounded preamble first, then splice and clean it: phase 2 joins continued
    // lines the way the compiler does, and block comments and raw strings span lines even
    // after that, so a per-line view cannot see them.
    auto raw = std::string{};
    while (lines_scanned++ < max_lines and std::getline(file, line)) {
        raw += line;
        raw += '\n';
    }
    const auto cleaned = detail::strip_comments_and_literals(detail::splice_physical_lines(raw));
    auto conditionals = detail::conditional_filter{};

    for (const auto part : std::views::split(cleaned, '\n')) {
        const auto code_line = std::string{std::string_view{part}};
        if (conditionals.is_inactive(code_line)) continue;
        auto trimmed = trim(code_line);
        if (trimmed.empty()) continue;

        // === ALWAYS CHECK FOR main() — ON EVERY LINE ===
        if (std::regex_search(code_line, translation_unit::main_regex)) {
            has_main = true;
        }

        // === Only scan module/import if we haven't seen real code yet ===
        if (seen_real_code) continue;

        std::smatch m;
        if (std::regex_search(code_line, m, translation_unit::fragment_regex)) {
            if (kind == unit_kind::non_module) kind = unit_kind::global_fragment;
        }
        else if (std::regex_search(code_line, m, translation_unit::export_module_regex) and m.size() > 1) {
            module_name = m[1].str();
            kind = module_name.contains(':') ? unit_kind::partition_unit : unit_kind::interface_unit;
        }
        else if (std::regex_search(code_line, m, translation_unit::module_regex) and m.size() > 1) {
            auto mod = m[1].str();
            if (kind == unit_kind::non_module or kind == unit_kind::global_fragment) {
                module_name = mod;
                kind = unit_kind::implementation_unit;
            }
        }
        else if (std::regex_search(code_line, m, translation_unit::import_regex) and m.size() > 1) {
            std::string imp = m[1].str();
            if (not imp.empty() and imp[0] == ':' and not module_name.empty()) {
                auto colon = module_name.find(':');
                auto base = colon != std::string::npos ? module_name.substr(0, colon) : module_name;
                imp = base + imp;
            }
            if (not imp.empty() and imp != std_module_name) imports.push_back(std::move(imp));
        }

        // End the preamble only after recording any module/import on this line, and never
        // inside a global module fragment: it may hold braces, keywords and declarations before
        // the `export module` line. Word boundaries keep "struct" out of "structured_log_stream".
        if (kind != unit_kind::global_fragment) {
            const auto code = trimmed;
            auto code_str = std::string{code};
            if (code.contains('{') or
                std::regex_search(code_str, translation_unit::keyword_regex) or
                std::regex_search(code_str, translation_unit::using_namespace_regex)) {
                seen_real_code = true;
            }
        }
    }

    // Validate prerequisites
    if ((kind == unit_kind::interface_unit or kind == unit_kind::partition_unit) and module_name.empty())
        throw std::runtime_error{"module interface/partition missing module name"};
    if (kind == unit_kind::implementation_unit and module_name.empty())
        throw std::runtime_error{"implementation unit missing module name"};

    return translation_unit{
        relative_path,
        file_path,
        std::move(module_name),
        std::move(imports),
        kind,
        has_main
    };
}

// Observers format four of a unit's fields, so they receive those four and not the unit: the
// rest is build state, and cb-observer.h++ stays independent of the scanner. The pcm path is the
// one derived field, and deriving it here is why compile_start and compile_end cannot disagree.
output::compile_unit compile_unit_of(const translation_unit& tu)
{
    return {.source = tu.full_path,
            .object = tu.object_path,
            .pcm = tu.is_modular ? std::string_view{tu.pcm_path} : std::string_view{},
            .module = tu.module,
            .display_path = tu.display_path};
}

// The inventory projection, the sibling of compile_unit_of: the list command reports what a
// unit is rather than where it compiles to, so it carries its own strings. Three adjacent
// bools are why this is named fields and not an aggregate.
output::source_unit source_unit_of(const translation_unit& tu)
{
    return {.unit = tu.unit,
            .path = tu.display_path,
            .module = tu.module,
            .kind = std::string{detail::unit_kind_name(tu.kind)},
            .imports = tu.imports,
            .level = tu.dependency_level,
            .has_main = tu.has_main,
            .is_test = tu.is_test,
            .is_modular = tu.is_modular};
}

// Every module-artefact reason (own_pcm_missing, own_pcm_stale, pcm_stale,
// dependency_pcm_stale) names the same three things about the unit whose module triggered it:
// this unit for the own_ reasons, an imported one otherwise. Callers have established it is
// modular, that being what made the reason apply.
output::rebuild_info pcm_rebuild(output::rebuild_kind kind, const translation_unit& tu)
{
    return {.kind = kind, .module = tu.module, .pcm_path = tu.pcm_path, .trigger_path = tu.full_path};
}

// A reason travelling up from a dependency keeps its own trigger — the header or source that
// actually changed — and names the dependency only for the fields it left blank.
output::rebuild_info attributed_to(output::rebuild_info reason, const translation_unit& tu)
{
    if(reason.trigger_path.empty())
        reason.trigger_path = tu.full_path;
    if(reason.module.empty())
        reason.module = tu.module;
    return reason;
}

using dependency_graph = std::flat_map<std::string, string_list, std::less<>>;
using indegree_map = std::flat_map<std::string, int, std::less<>>;
using unit_to_tu_map = std::flat_map<std::string, translation_unit*, std::less<>>;
using object_cache_map = std::flat_map<std::string, fs::file_time_type, std::less<>>;

// What reading the object cache found: the entries, and the one thing that invalidates all of
// them at once. A profile mismatch answers two questions together — every unit is about to miss,
// and here is the field that changed — so it travels with the entries rather than being left in
// the build system for the caller to collect. The diff is the miss, not a reason beside a diff
// only some reasons fill: engaged means profile_change, the only blanket miss there is.
struct object_cache_load
{
    object_cache_map entries{};
    std::optional<output::object_cache_profile_diff> profile_change{};
};

using translation_unit_list = std::vector<translation_unit>;
using topo_sort_queue = std::queue<std::string>;
using level_groups_map = std::flat_map<int, std::vector<const translation_unit*>>;
using thread_list = std::vector<std::jthread>;
using module_to_ldflags_map = std::flat_map<std::string, std::string, std::less<>>;
using executable_cache_map = std::flat_map<std::string, std::string, std::less<>>;

// Bounds concurrent toolchain processes. CB spawned one jthread per translation unit
// in a dependency level, so a level with hundreds of units launched hundreds of
// clang++ processes at once — each one a multi-hundred-megabyte peak.
class job_gate
{
public:
    explicit job_gate(std::ptrdiff_t limit) : slots{limit} {}

    class slot
    {
    public:
        explicit slot(job_gate& gate) : gate{gate} { gate.slots.acquire(); }
        ~slot() { gate.slots.release(); }
        slot(const slot&) = delete;
        slot& operator=(const slot&) = delete;

    private:
        job_gate& gate;
    };

private:
    std::counting_semaphore<> slots;
};

// Compiling and linking differ only in the work: one worker per job, at most limit at a time,
// nothing new started once a job has failed, and the first failure rethrown after every worker
// has joined, so a failing build never leaves a toolchain process running. Written twice, the
// copies had to agree about the relaxed loads, the recheck after the slot and which exception
// survives.
template <std::ranges::input_range Jobs, typename Work>
void run_in_parallel(Jobs&& jobs, std::ptrdiff_t limit, Work work)
{
    auto threads = thread_list{};
    auto failed = std::atomic_bool{false};
    auto failure = std::exception_ptr{};
    auto failure_mutex = std::mutex{};
    auto gate = job_gate{limit};

    for(const auto& job : jobs)
    {
        // The job is addressed, not copied: it outlives join(), and a build decision carries a
        // rebuild reason of four strings.
        threads.emplace_back([&work, item = std::addressof(job), &failed, &failure, &failure_mutex, &gate]()
        {
            if(failed.load(std::memory_order_relaxed))
                return;
            const auto slot = job_gate::slot{gate};
            // Recheck: a failure may have landed while waiting for a slot.
            if(failed.load(std::memory_order_relaxed))
                return;
            try
            {
                work(*item);
            }
            catch(...)
            {
                failed.store(true, std::memory_order_relaxed);
                auto lock = std::lock_guard<std::mutex>{failure_mutex};
                if(not failure)
                    failure = std::current_exception();
            }
        });
    }

    for(auto& thread : threads)
        thread.join();
    if(failure)
        std::rethrow_exception(failure);
}

// One build_start, exactly one build_end, whichever way the steps end. Here rather than in the
// callers, which each had to remember a phase flag, an emitted flag and a catch-and-rethrow, and
// rather than in the observers, which would each reimplement the latch and could then disagree.
class build_scope
{
public:
    build_scope(std::string_view config, bool include_tests, bool include_examples)
    {
        output::notify(&output::observer::build_start, config, include_tests, include_examples);
    }

    build_scope(const build_scope&) = delete;
    build_scope& operator=(const build_scope&) = delete;

    // A build that never reports success failed, including when a step threw.
    ~build_scope() { report(false); }

    void succeeded() { report(true); }

private:
    void report(bool ok)
    {
        if(std::exchange(reported, true))
            return;
        output::notify(&output::observer::build_end, ok,
                       output::interval{started, std::chrono::steady_clock::now()});
    }

    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool reported = false;
};

// One compile_start, exactly one compile_end: build_scope's pairing one level down. A modular
// unit compiles in two steps and either can fail, so without the scope each exit path carried its
// own copy of the event and had to agree about the unit, the reason and the clock. attach()
// collects the compiler's own output — errors on failure, warnings on success — so a consumer
// sees the diagnostic and not just ok:false / silence.
class compile_scope
{
public:
    compile_scope(const output::compile_unit& compiled, const output::rebuild_info& reason)
        : unit{compiled}, rebuild{reason}
    {
        output::notify(&output::observer::compile_start, unit, rebuild);
    }

    // A cache hit is the same pair with nothing in between: no reason, no duration, and ok from
    // the start, since there is no step that could fail. Constructing one reports the hit.
    explicit compile_scope(const output::compile_unit& compiled)
        : compile_scope{compiled, output::rebuild_info{}}
    {
        hit = true;
        ok = true;
    }

    compile_scope(const compile_scope&) = delete;
    compile_scope& operator=(const compile_scope&) = delete;

    // A compile that never reports success failed, including when a step threw.
    ~compile_scope()
    {
        const auto finished = hit ? started : std::chrono::steady_clock::now();
        output::notify(&output::observer::compile_end,
                       unit,
                       output::step_result{.ok = ok,
                                           .cache_hit = hit,
                                           .timing = {started, finished},
                                           .rebuild = rebuild,
                                           .diag = diag});
    }

    void succeeded() { ok = true; }
    // Warnings from a successful step accumulate across a modular unit's two toolchain
    // invocations. A failing step replaces them: the error is what triage reads first, and
    // prior warning text must not consume the diagnostics head budget.
    void attach(output::diagnostics said) { detail::append_diagnostics(diag, std::move(said)); }
    void failed(output::diagnostics said) { diag = std::move(said); }

private:
    output::compile_unit unit;
    output::rebuild_info rebuild;
    output::diagnostics diag{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool ok = false;
    bool hit = false;
};

// Linking has no start event, so this is the exit half only: exactly one link_end however the
// link ends. The three copies it replaces needed a linked flag to keep the catch-all from
// emitting a second one — a latch in a bool, which is what a scope is for.
class link_scope
{
public:
    link_scope(std::string_view executable, const output::rebuild_info& reason, std::string sig)
        : executable_path{executable}, rebuild{reason}, signature{std::move(sig)}
    {}

    // An up-to-date executable: nothing ran, so no reason and no duration, and there is no step
    // that could fail. Constructing one reports the hit — with the signature that made it a hit.
    link_scope(std::string_view executable, std::string sig)
        : link_scope{executable, output::rebuild_info{}, std::move(sig)}
    {
        hit = true;
        ok = true;
    }

    link_scope(const link_scope&) = delete;
    link_scope& operator=(const link_scope&) = delete;

    ~link_scope()
    {
        const auto finished = hit ? started : std::chrono::steady_clock::now();
        output::notify(&output::observer::link_end,
                       executable_path,
                       output::step_result{.ok = ok,
                                           .cache_hit = hit,
                                           .timing = {started, finished},
                                           .rebuild = rebuild,
                                           .diag = diag,
                                           .signature = signature});
    }

    void succeeded() { ok = true; }
    void attach(output::diagnostics said) { detail::append_diagnostics(diag, std::move(said)); }
    void failed(output::diagnostics said) { diag = std::move(said); }

private:
    std::string executable_path;
    output::rebuild_info rebuild;
    output::diagnostics diag{};
    std::string signature{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool ok = false;
    bool hit = false;
};

// What run_step needs of a scope: somewhere to hand the child's own output — failures and
// warnings alike — so it reaches stdout on compile_end / link_end rather than vanishing with
// the capture file. compile_scope and link_scope both qualify, which is the whole reason one
// function can run either phase's steps.
template <typename Scope>
concept step_scope = requires(Scope& scope, output::diagnostics said) {
    scope.attach(std::move(said));
    scope.failed(std::move(said));
};

// One test_start, exactly one test_end, with the run's duration in the scope rather than in two
// locals beside it. Nothing between the events throws today — a failing runner comes back as a
// status — so this states the pairing rather than repairing it, and keeps it true if a step that
// can throw is ever added between them.
class test_scope
{
public:
    explicit test_scope(std::string_view runner)
    {
        output::notify(&output::observer::test_start, runner);
    }

    test_scope(const test_scope&) = delete;
    test_scope& operator=(const test_scope&) = delete;

    // A run whose outcome was never reported did not finish: the default process_result says
    // exit -1, what decode_wait_status reports when the shell cannot be started. A run that ran
    // and failed also says so as an error — the stream's cb_error after a failed run — but only
    // when an outcome was reported: on the way out of a throw, main's handler reports it.
    ~test_scope()
    {
        output::notify(&output::observer::test_end, result,
                       output::interval{started, std::chrono::steady_clock::now()});
        if(reported and not result.ok())
            output::notify(&output::observer::error, output::test_failure_message(result));
    }

    // finished(), not the succeeded() / failed() of the other scopes: a test run's outcome is not
    // a flag. test_end reports the exit code, the wait status and the signal, and a runner that
    // fails is a normal outcome the command turns into a return value rather than an exception.
    void finished(output::process_result outcome)
    {
        result = std::move(outcome);
        reported = true;
    }

private:
    output::process_result result{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool reported = false;
};

class build_system {
public:
    enum class build_config { debug, release };

    // How a modular interface becomes a .pcm and a .o. two_phase runs `--precompile` and then
    // compiles the BMI; one_phase asks for both artefacts from one `-c -fmodule-output=` read of
    // the source. Two-phase only pays off when the BMI is published to dependents while the .o
    // still compiles — CB's scheduler waits for both steps, so one_phase saves the second parse
    // and (Clang 22+) gets reduced BMIs by default.
    enum class module_compilation { two_phase, one_phase };

private:

    std::string source_dir;
    string_list compile_flags, link_flags, cpp_flags;
    module_to_ldflags_map module_ldflags;
    string_list module_flags;
    std::string std_module_source;
    std::string llvm_prefix, llvm_cxx;
    std::string std_cppm_profile;
    std::string cxx_sig;
    // Lazily filled by ensure_toolchain_profile() from const cache queries.
    mutable std::string clang_version;
    mutable bool toolchain_profile_probed = false;
    translation_unit_list units_in_topological_order;
    std::mutex cache_mutex;
    std::mutex link_cache_mutex;
    const build_config config;
    const bool static_link;
    bool include_tests = false;
    bool include_examples = false;
    module_compilation module_phases = module_compilation::two_phase;
    int max_jobs = 0;
    string_list extra_compile_flag_tokens;
    string_list extra_link_flag_tokens;
    std::string_view config_name() const
    {
        switch(config)
        {
            case build_config::debug: return "debug";
            case build_config::release: return "release";
        }
        std::unreachable();
    }

    std::string_view module_phases_name() const
    {
        switch(module_phases)
        {
            case module_compilation::two_phase: return "two-phase";
            case module_compilation::one_phase: return "one-phase";
        }
        std::unreachable();
    }
    
    // ============================================================================
    // Initialization and Setup
    // ============================================================================

    void detect_llvm_environment() {
        // std.cppm names the LLVM prefix, which names clang++. Every compile and link needs all
        // three, so a missing one is an error here rather than a failure mid-build.
        
        if (std_module_source.empty()) {
            if (auto env = std::getenv("LLVM_PATH"); env and *env) {
                std_module_source = env;
            } else {
                throw std::runtime_error{
                    "std.cppm path not provided. Pass it as the first argument or set LLVM_PATH."};
            }
        }

        auto std_module_path = fs::path{std_module_source};
        if (not fs::exists(std_module_path)) {
            throw std::runtime_error{"std.cppm not found at: " + std_module_source};
        }

        // Determine LLVM prefix from std.cppm path
        // Navigate up from share/libc++/v1/std.cppm or include/c++/v1/std.cppm to get LLVM root
        auto p = std_module_path;
        for (int i = 0; i < 4 and p.has_parent_path(); ++i) p = p.parent_path();
        llvm_prefix = p.string();
        
        // Find clang++. Paths are a filesystem check; bare names need the shell's PATH
        // lookup. Both go through invoke_shell so nothing reaches the shell unquoted.
        auto command_available = [this](const std::string& candidate) {
            if(candidate.contains('/'))
                return fs::exists(candidate);

            fs::create_directories(cache_dir());
            const auto probe = detail::join_dir(cache_dir(), "command-probe.txt");
            // `command` is a shell builtin, so the argv is sh -c with the name quoted.
            return invoke_shell(
                string_list{"/bin/sh", "-c", "command -v " + detail::shell_quote(candidate)},
                probe).ok();
        };
        auto try_env_compiler = [&]() {
            for(const auto* env_name : {"LLVM_CXX", "CXX"})
            {
                if(auto value = std::getenv(env_name); value and command_available(value))
                {
                    llvm_cxx = value;
                    return true;
                }
            }
            return false;
        };
        if(not try_env_compiler())
        {
            llvm_cxx = llvm_prefix + "/bin/clang++";
            if(not command_available(llvm_cxx))
            {
                throw std::runtime_error{
                    "clang++ not found. Expected: " + llvm_cxx + " (set LLVM_CXX to override)."};
            }
        }

        const auto canonical_std_cppm = fs::weakly_canonical(std_module_path).string();
        std_cppm_profile = canonical_std_cppm + '@' + detail::binary_signature(canonical_std_cppm);
        cxx_sig = detail::binary_signature(llvm_cxx);
    }

    void ensure_toolchain_profile() const
    {
        if(toolchain_profile_probed)
            return;
        toolchain_profile_probed = true;

        fs::create_directories(cache_dir());

        // Same boundary as every compile and link: argv in, stamp file out.
        const auto stamp = compiler_stamp_path();
        if(invoke_shell(string_list{llvm_cxx, "--version"}, stamp).ok())
            clang_version = detail::read_first_line(stamp);
    }

    void initialize_build_flags()
    {
        const auto os = os_name();
        const auto is_darwin = (os == "darwin");
        const auto is_linux = (os == "linux");

        compile_flags = {
            "-B" + llvm_prefix + "/bin",
            "-fuse-ld=lld",
            "-std=c++23",
            "-stdlib=libc++",
            "-pthread",
            "-fPIC",
            "-fexperimental-library",
            "-Wall",
            "-Wextra",
            "-Wno-reserved-module-identifier",
            "-Wno-unused-command-line-argument",
        };

        if(is_linux)
            compile_flags.push_back("-I" + llvm_prefix + "/include/c++/v1");
        else
        {
            compile_flags.append_range(string_list{
                "-nostdinc++",
                "-isystem",
                llvm_prefix + "/include/c++/v1",
                "-fno-implicit-modules",
                "-fno-implicit-module-maps",
            });
        }

        if(config == build_config::release)
        {
            compile_flags.append_range(string_list{"-O3", "-DNDEBUG"});
            output::notify(&output::observer::info, "Building RELEASE configuration"s + (static_link ? " (static C++ stdlib)"s : ""s));
        }
        else
        {
            compile_flags.append_range(string_list{"-O0", "-g3"});
            output::notify(&output::observer::info, "Building DEBUG configuration"s + (static_link ? " (static C++ stdlib)"s : ""s));
        }

        if(static_link)
        {
            if(is_darwin)
            {
                link_flags = {
                    "-pthread",
                    "-lc++",
                    "-L" + llvm_prefix + "/lib",
                    "-Wl,-dead_strip",
                };
                output::notify(&output::observer::warning, "Static linking on macOS is limited – libc++ remains dynamically linked");
            }
            else
            {
                const auto arch = linux_arch();
                link_flags = {
                    "-Wl,-Bstatic",
                    "-lc++",
                    "-lc++abi",
                    "-lc++experimental",
                    "-Wl,-Bdynamic",
                    "-pthread",
                    "-ldl",
                    "-L/usr/lib/" + arch + "-linux-gnu",
                    "-L" + llvm_prefix + "/lib",
                    "-O3",
                };
                if(config == build_config::debug)
                    link_flags.push_back("-g3");
            }
        }
        else if(is_darwin)
        {
            // -stdlib=libc++ already pulls -lc++; name -lc++abi -lunwind so the
            // link uses this LLVM's runtimes, not the SDK unwinder (see
            // docs/clang-modules-macos.md). Same pair as config/compiler.mk and
            // CMakeLists.txt on Apple.
            link_flags = {
                "-pthread",
                "-L" + llvm_prefix + "/lib",
                "-Wl,-rpath," + llvm_prefix + "/lib",
                "-lc++abi",
                "-lunwind",
                "-Wl,-dead_strip",
            };
            if(fs::exists("/usr/lib/system/introspection/libunwind.reexported_symbols"))
            {
                link_flags.push_back(
                    "-Wl,-unexported_symbols_list,/usr/lib/system/introspection/libunwind.reexported_symbols");
            }
        }
        else
        {
            const auto arch = linux_arch();
            link_flags = {
                "-pthread",
                "-lc++",
                "-lc++abi",
                "-lc++experimental",
                "-L/usr/lib/" + arch + "-linux-gnu",
                "-L" + llvm_prefix + "/lib",
                "-Wl,-rpath," + llvm_prefix + "/lib",
                "-O3",
            };
            if(config == build_config::debug)
                link_flags.push_back("-g3");
        }

        if(not extra_link_flag_tokens.empty())
        {
            link_flags.append_range(extra_link_flag_tokens);
            output::notify(&output::observer::info, "Added extra linker flags: "s + detail::flags_profile_string(extra_link_flag_tokens));
        }

        if(not extra_compile_flag_tokens.empty())
        {
            compile_flags.append_range(extra_compile_flag_tokens);
            output::notify(&output::observer::info, "Added extra compile flags: "s + detail::flags_profile_string(extra_compile_flag_tokens));
        }

        module_flags = {
            "-fno-implicit-modules",
            "-fno-implicit-module-maps",
            detail::module_file_flag(std_module_name, std_pcm_path()),
            std::string{prebuilt_module_path_flag_prefix} + module_cache_dir(),
        };
    }

    // The rest of module_flags: one -fmodule-file= per modular unit, which is why it cannot be
    // set above — the units are only known once scan_and_order has run. build_steps calls this
    // between the two.
    void update_module_flags()
    {
        module_flags.append_range(
            units_in_topological_order
            | std::views::filter([](const translation_unit& tu) { return tu.is_modular; })
            | std::views::transform([](const translation_unit& tu)
            {
                return detail::module_file_flag(tu.module, tu.pcm_path);
            })
            | std::views::filter([&](const auto& flag)
            {
                return not std::ranges::contains(module_flags, flag);
            }));
    }

    // ============================================================================
    // Platform and Path Utilities
    // ============================================================================

    std::string os_name() const {
#if defined(__linux__)
        return "linux";
#elif defined(__APPLE__)
        return "darwin";
#elif defined(_WIN32)
        return "windows";
#else
        return "unknown";
#endif
    }

    std::string linux_arch() const {
        // Get Linux architecture for library paths (e.g., aarch64, x86_64)
        // Used for /usr/lib/$(ARCH)-linux-gnu paths
        // Detect at compile time using preprocessor macros
#if defined(__x86_64__) or defined(__amd64__)
        return "x86_64";
#elif defined(__aarch64__) or defined(__arm64__)
        return "aarch64";
#else
        throw std::runtime_error{
            "Unsupported architecture. Only x86_64 and aarch64 are supported."};
#endif
    }

    // COMPUTED ON DEMAND — NEVER CACHED
    std::string build_root() const {
        return std::string{build_root_prefix} + os_name()
            + std::string{config == build_config::release ? release_build_suffix : debug_build_suffix};
    }

    std::string module_cache_dir() const      { return detail::join_dir(build_root(), pcm_dir_name); }
    std::string object_dir() const            { return detail::join_dir(build_root(), obj_dir_name); }
    std::string binary_dir() const            { return detail::join_dir(build_root(), bin_dir_name); }
    std::string cache_dir() const             { return detail::join_dir(build_root(), cache_dir_name); }
    std::string object_cache_path() const     { return detail::join_dir(cache_dir(), object_cache_filename); }
    std::string executable_cache_path() const { return detail::join_dir(cache_dir(), executable_cache_filename); }
    std::string std_module_profile_path() const { return detail::join_dir(cache_dir(), std_module_profile_filename); }
    std::string compiler_stamp_path() const   { return detail::join_dir(cache_dir(), compiler_version_filename); }
    std::string std_pcm_path() const          { return detail::join_dir(module_cache_dir(), std_pcm_filename); }
    std::string std_obj_path() const          { return detail::join_dir(object_dir(), std_obj_filename); }

    // ============================================================================
    // Path Computation Utilities
    // ============================================================================

    std::string normalize_path(std::string_view p) const {
        auto path = fs::path{p};
        if (path.is_relative()) path = fs::absolute(path);
        try { path = fs::canonical(path); } catch(...) {}
        return path.string();
    }

    // Artifact stems follow source naming: `foo:bar` / `foo.bar` → `foo-bar`.
    // Keep '_' literal so `demo:part` and `demo_part` stay distinct.
    std::string module_safe_name(std::string_view module_name) const
    {
        auto safe = std::string{module_name};
        std::ranges::replace(safe, ':', '-');
        std::ranges::replace(safe, '.', '-');
        return safe;
    }

    std::string object_suffix(const translation_unit& tu) const
    {
        const auto ending = std::ranges::find_if(object_stem_suffixes, [&](std::string_view s)
        {
            return tu.suffix.ends_with(s);
        });
        if(ending == object_stem_suffixes.end())
            throw std::logic_error{"Unsupported suffix for object file: " + tu.suffix};
        return std::string{tu.suffix.substr(0, tu.suffix.size() - ending->size())} + std::string{object_extension};
    }

    std::string compute_object_path(const translation_unit& tu) const {
        auto base = tu.is_modular ? module_safe_name(tu.module) : tu.base_name;
        return object_dir() + "/" + base + object_suffix(tu);
    }

    std::string compute_pcm_path(const translation_unit& tu) const {
        if (tu.module.empty())
            throw std::logic_error{"compute_pcm_path called on translation unit without module: " + tu.filename};
        return detail::join_dir(module_cache_dir(), module_safe_name(tu.module) + std::string{pcm_extension});
    }

    std::string compute_executable_path(const translation_unit& tu) const {
        if (not tu.has_main)
            throw std::logic_error{"compute_executable_path called on non-main translation unit: " + tu.filename};
        return binary_dir() + "/" + tu.base_name;
    }

    void validate_translation_unit(const translation_unit& tu) const {
        if (tu.object_path.empty())
            throw std::logic_error{"translation unit missing object path: " + tu.filename};
        
        if (tu.is_modular) {
            if (tu.module.empty())
                throw std::logic_error{"modular unit missing module name: " + tu.filename};
            if (tu.pcm_path.empty())
                throw std::logic_error{"modular unit missing PCM path: " + tu.filename};
        }
        
        if (tu.kind == unit_kind::implementation_unit and tu.module.empty())
            throw std::logic_error{"implementation unit missing module name: " + tu.filename};
        
        if (tu.has_main and tu.executable_path.empty())
            throw std::logic_error{"main unit missing executable path: " + tu.filename};
    }

    // ============================================================================
    // General Utilities
    // ============================================================================

    // Sole shell boundary: argv is non-empty (contract) and join_argv quotes each element. With
    // capture_path the child's stdout and stderr are redirected there and read back whenever the
    // child printed anything — failures always, successes when there are warnings. The test
    // runner must not be captured — its stdout is the JSONL stream the caller forwards.
    output::process_result invoke_shell(const string_list& argv, std::string_view capture_path = {}) const
    {
        if(argv.empty())
            throw std::logic_error{"invoke_shell: empty argv"};

        auto cmd_str = detail::join_argv(argv);
        auto shell_line = cmd_str;
        if(not capture_path.empty())
            shell_line += " > " + detail::shell_quote(std::string{capture_path}) + " 2>&1";

        output::notify(&output::observer::command, cmd_str);
        output::notify(&output::observer::command_start, cmd_str, argv);

        const auto started = std::chrono::steady_clock::now();
        // Apple's libc serializes std::system; posix_spawn does not. Still go through
        // /bin/sh -c so capture_path's `> … 2>&1` redirect keeps working.
        auto raw = -1;
        auto spawn_error = std::string{};
        auto pid = pid_t{};
        char* const sh_argv[] = {
            const_cast<char*>("/bin/sh"),
            const_cast<char*>("-c"),
            const_cast<char*>(shell_line.c_str()),
            nullptr,
        };
        if(const auto spawn_rc = posix_spawn(&pid, "/bin/sh", nullptr, nullptr, sh_argv, environ);
           spawn_rc == 0)
        {
            auto status = 0;
            if(waitpid(pid, &status, 0) >= 0)
                raw = status;
            else
                spawn_error = "waitpid failed: " + std::string{std::strerror(errno)};
        }
        else
            spawn_error = "posix_spawn failed: " + std::string{std::strerror(spawn_rc)};
        const auto finished = std::chrono::steady_clock::now();

        auto result = output::process_result{.status = detail::decode_wait_status(raw)};
        if(not capture_path.empty())
        {
            auto diag = detail::read_diagnostics(capture_path);
            // A silent success leaves an empty capture; keep that off the wire. A failure still
            // reports the path even when the child printed nothing, so the consumer can see that
            // the capture was attempted.
            if(not result.ok() or not diag.head.empty())
                result.diag = std::move(diag);
        }
        // Spawn/wait failures never produce a capture file; surface strerror on the wire.
        if(not spawn_error.empty() and result.diag.head.empty())
            result.diag.head = std::move(spawn_error);

        output::notify(&output::observer::command_end, cmd_str, argv, result,
                       output::interval{started, finished});
        return result;
    }

    // Every toolchain command is a step of a reported build phase: its output is the scope's
    // before it is the caller's, so the compiler's or linker's text travels on compile_end /
    // link_end — including warnings from a successful step — and the exception only has to name
    // a failing command. A modular unit runs two of these, each link one, and the std module's
    // two go through the same path.
    void run_step(step_scope auto& scope, const string_list& argv, std::string_view capture) const
    {
        const auto result = invoke_shell(argv, capture);
        if(not result.ok())
        {
            scope.failed(result.diag);
            throw std::runtime_error{command_failure_message(argv, result)};
        }
        if(not result.diag.empty())
            scope.attach(result.diag);
    }

    static std::string command_failure_message(const string_list& argv, const output::process_result& result)
    {
        auto message = "Command failed: " + detail::join_argv(argv);
        if(result.status.signaled)
            message += " (killed by signal " + std::to_string(result.status.signal) + ')';
        else
            message += " (exit " + std::to_string(result.status.exit_code) + ')';
        if(not result.diag.head.empty())
            message += '\n' + result.diag.head;
        return message;
    }

    string_list base_compile_argv() const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(compile_flags);
        argv.append_range(cpp_flags);
        return argv;
    }

    // Header dependencies live next to the object file. Written by the step that
    // actually reads the source: --precompile for modular units, -c otherwise.
    std::string depfile_path(const translation_unit& tu) const
    {
        return tu.object_path + ".d";
    }

    string_list depfile_argv(const translation_unit& tu) const
    {
        return string_list{"-MMD", "-MF", depfile_path(tu)};
    }

    // Per-target capture files: parallel workers must not share one. A modular unit (and the
    // std module) runs two toolchain steps that share one compile_end; each step still needs
    // its own capture — the shell redirect truncates, so reusing the object log would wipe
    // the precompile warnings that compile_end.diagnostics.path still names. Both name the
    // artefact rather than a free path, so moving captures into a logs/ tree is an edit here
    // and nowhere else. The argument type is compile_unit because the std module compiles
    // without being a scanned unit, and that is what both compile steps report with anyway.
    std::string diagnostics_path_for_pcm(const output::compile_unit& unit) const
    {
        if(unit.pcm.empty())
            throw std::logic_error{"diagnostics_path_for_pcm: non-modular unit"};
        return std::string{unit.pcm} + ".log";
    }

    std::string diagnostics_path_for_object(const output::compile_unit& unit) const
    {
        return std::string{unit.object} + ".log";
    }

    std::string diagnostics_path_for_executable(const translation_unit& tu) const
    {
        return std::string{tu.executable_path} + ".link.log";
    }

    string_list compile_pcm_argv(const translation_unit& tu) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags);
        argv.append_range(depfile_argv(tu));
        argv.push_back(tu.full_path);
        argv.push_back("--precompile");
        argv.push_back("-o");
        argv.push_back(tu.pcm_path);
        return argv;
    }

    string_list compile_pcm_object_argv(const translation_unit& tu) const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(compile_flags);
        argv.append_range(module_flags);
        argv.push_back(tu.pcm_path);
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(tu.object_path);
        return argv;
    }

    // One-phase: one read of the source emits the object and, via -fmodule-output, the BMI its
    // importers consume. The BMI lands at the same pcm_path two-phase writes, so -fmodule-file=
    // flags, staleness checks and clean are unchanged.
    string_list compile_module_object_argv(const translation_unit& tu) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags);
        argv.append_range(depfile_argv(tu));
        argv.push_back("-fmodule-output=" + tu.pcm_path);
        argv.push_back(tu.full_path);
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(tu.object_path);
        return argv;
    }

    string_list compile_source_object_argv(const translation_unit& tu) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags);
        argv.append_range(depfile_argv(tu));
        if (tu.kind == unit_kind::implementation_unit) {
            auto module_pcm = compute_pcm_path(tu);
            argv.push_back(detail::module_file_flag(tu.module, module_pcm));
        }
        argv.push_back(tu.full_path);
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(tu.object_path);
        return argv;
    }

    // Everything up to and including std.cppm: the project's compile flags with libc++'s own
    // include setup. Both module-compilation schemes read the source with exactly these.
    string_list std_module_source_argv() const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(compile_flags);
        argv.append_range(cpp_flags);
        argv.push_back("-nostdinc++");
        argv.push_back("-isystem");
        argv.push_back(llvm_prefix + "/include/c++/v1");
        argv.push_back("-Wno-unused-command-line-argument");
        argv.push_back("-fno-implicit-modules");
        argv.push_back("-fno-implicit-module-maps");
        argv.push_back("-Wno-reserved-module-identifier");
        argv.push_back(std_module_source);
        return argv;
    }

    string_list build_std_pcm_argv() const
    {
        auto argv = std_module_source_argv();
        argv.push_back("--precompile");
        argv.push_back("-o");
        argv.push_back(std_pcm_path());
        return argv;
    }

    // One-phase std: the single most expensive parse in a cold build, done once instead of twice.
    string_list build_std_module_object_argv() const
    {
        auto argv = std_module_source_argv();
        argv.push_back("-fmodule-output=" + std_pcm_path());
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(std_obj_path());
        return argv;
    }

    // The same compile_flags as project TUs and build_std_pcm_argv: a hardcoded subset once
    // dropped --compile-flags (e.g. -fsanitize=address), leaving std.o disagreeing with an
    // ASAN-built std.pcm and with instrumented project objects.
    string_list build_std_o_argv() const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(compile_flags);
        argv.append_range(module_flags);
        argv.push_back(std_pcm_path());
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(std_obj_path());
        return argv;
    }

    string_list link_executable_argv(const translation_unit& tu, const string_list& shared_objects) const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(compile_flags);
        argv.append_range(collect_module_ldflags(tu.imports));
        argv.append_range(module_flags);
        argv.push_back(tu.object_path);
        argv.append_range(shared_objects);
        argv.push_back(std_obj_path());
        argv.append_range(link_flags);
        argv.push_back("-o");
        argv.push_back(tu.executable_path);
        return argv;
    }

    string_list link_test_runner_argv(const translation_unit& runner) const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(compile_flags);
        argv.append_range(collect_module_ldflags(runner.imports));
        argv.append_range(module_flags);
        argv.push_back(runner.object_path);
        argv.append_range(linkable_object_paths());
        argv.append_range(test_object_paths());
        argv.push_back(std_obj_path());
        argv.append_range(link_flags);
        argv.push_back("-o");
        argv.push_back(runner.executable_path);
        return argv;
    }

    string_list test_runner_argv(const std::string& runner, const std::vector<std::string>& args) const
    {
        auto argv = string_list{};
        argv.push_back(runner);
        argv.append_range(args);
        return argv;
    }

    // The units the test runner links on top of the ordinary objects: test translation units
    // without a main of their own. A view, because two callers want the objects and one the imports.
    auto test_units() const
    {
        return units_in_topological_order
            | std::views::filter([](const translation_unit& tu) { return tu.is_test and not tu.has_main; });
    }

    string_list test_object_paths() const
    {
        return test_units()
            | std::views::transform([](const translation_unit& tu) { return tu.object_path; })
            | std::ranges::to<string_list>();
    }

    string_list linkable_object_paths() const
    {
        return units_in_topological_order
            | std::views::filter([](const translation_unit& tu) { return not tu.has_main and not tu.is_test; })
            | std::views::transform([](const translation_unit& tu) { return tu.object_path; })
            | std::ranges::to<string_list>();
    }

    string_list collect_module_ldflags(const string_list& imp) const
    {
        return std::ranges::fold_left(
            imp | std::views::filter([&](const std::string& m) { return module_ldflags.contains(m); }),
            string_list{},
            [&](string_list flags, const std::string& m) {
                flags.append_range(detail::parse_external_flag_text(module_ldflags.at(m)));
                return flags;
            });
    }

    string_list collect_test_module_ldflags() const
    {
        return std::ranges::fold_left(
            test_units(),
            string_list{},
            [&](string_list flags, const translation_unit& tu) {
                flags.append_range(collect_module_ldflags(tu.imports));
                return flags;
            });
    }

    // ============================================================================
    // Cache Management
    // ============================================================================

    std::string object_cache_profile() const {
        ensure_toolchain_profile();

        auto profile = ""s;
        profile.reserve(768);
        detail::append_profile_field(profile, "format", detail::object_cache_format);
        detail::append_profile_field(profile, "config", config_name());
        detail::append_profile_field(profile, "static_link", static_link ? "1" : "0");
        detail::append_profile_field(profile, "module_phases", module_phases_name());
        detail::append_profile_field(profile, "llvm", llvm_prefix);
        detail::append_profile_field(profile, "cxx", llvm_cxx);
        detail::append_profile_field(profile, "cxx_sig", cxx_sig);
        if(not clang_version.empty())
            detail::append_profile_field(profile, "clang_ver", clang_version);
        detail::append_profile_field(profile, "std_cppm", std_cppm_profile);
        detail::append_profile_field(profile, "compile", detail::flags_profile_string(compile_flags));
        detail::append_profile_field(profile, "cpp", detail::flags_profile_string(cpp_flags));
        return profile;
    }

    static bool parse_object_cache_entry(const std::string& line, std::string& path, long long& ticks) {
        if (line.empty() or line.starts_with("profile\t"))
            return false;
        const auto tab = line.find('\t');
        if (tab == std::string::npos)
            return false;
        path = line.substr(0, tab);
        try {
            ticks = std::stoll(line.substr(tab + 1));
        } catch (...) {
            return false;
        }
        return not path.empty();
    }

    object_cache_load load_object_cache() {
        auto loaded = object_cache_load{};
        auto file = std::ifstream{object_cache_path()};
        if (not file)
            return loaded;

        auto header = ""s;
        if (not std::getline(file, header))
            return loaded;

        const auto current_profile = object_cache_profile();
        if (header.starts_with("profile\t")) {
            const auto stored_profile = header.substr(std::string_view{"profile\t"}.size());
            if (stored_profile != current_profile) {
                loaded.profile_change = detail::diff_object_cache_profiles(stored_profile, current_profile);
                return loaded;
            }
        } else {
            output::notify(&output::observer::info, "Object cache missing profile header; ignoring"s);
            return loaded;
        }

        auto line = ""s;
        while (std::getline(file, line)) {
            auto path = ""s;
            auto ticks = 0ll;
            if (parse_object_cache_entry(line, path, ticks) and fs::exists(path))
                loaded.entries[path] = fs::file_time_type{std::chrono::nanoseconds{ticks}};
        }
        return loaded;
    }

    // A cache is replaced, never edited in place: write a sibling temporary, then rename it over
    // the target, so a build interrupted mid-write leaves the previous cache rather than half of
    // the next. The caches differ only in what they write and what the failure message calls them.
    static void write_cache_file(const std::string& path,
                                 std::string_view what,
                                 const std::invocable<std::ostream&> auto& write_contents)
    {
        const auto tmp = path + ".tmp";
        auto file = std::ofstream{tmp};
        if(not file)
            throw std::runtime_error{"Cannot open "s + std::string{what} + " temporary file: " + tmp};

        write_contents(file);

        file.close();
        if(not file)
        {
            auto ignored = std::error_code{};
            fs::remove(tmp, ignored);
            throw std::runtime_error{"Failed to write "s + std::string{what} + " temporary file: " + tmp};
        }

        auto error = std::error_code{};
        fs::rename(tmp, path, error);
        if(error)
        {
            auto ignored = std::error_code{};
            fs::remove(tmp, ignored);
            throw std::runtime_error{"Failed to replace "s + std::string{what} + ": " + error.message()};
        }
    }

    void save_object_cache(const object_cache_map& c) {
        write_cache_file(object_cache_path(), "object cache", [&](std::ostream& file) {
            file << "profile\t" << object_cache_profile() << "\n";
            for (const auto& [path, timestamp] : c) {
                if (not fs::exists(path))
                    continue;
                auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    timestamp.time_since_epoch()).count();
                file << path << "\t" << ticks << "\n";
            }
        });
    }

    static void count_cache_entries(std::istream& file,
                                    int& entries,
                                    int& stale_entries)
    {
        auto line = ""s;
        while(std::getline(file, line))
        {
            auto path = ""s;
            auto ticks = 0ll;
            if(not parse_object_cache_entry(line, path, ticks))
                continue;
            ++entries;
            if(not fs::exists(path))
                ++stale_entries;
        }
    }

    // The reason is the return value: a bool plus an out-parameter that only means something
    // when it is true is what optional says. visited stays a parameter — it is the walk's own
    // state, shared across the recursion rather than an answer travelling back.
    std::optional<output::rebuild_info> transitive_pcm_newer_than_object(const translation_unit& tu,
                                                                        fs::file_time_type object_timestamp,
                                                                        const unit_to_tu_map& u2tu,
                                                                        std::flat_set<std::string>& visited) const
    {
        for (const auto& dependency_key : tu.imports) {
            if (not u2tu.contains(dependency_key))
                continue;

            const auto& dep_tu = *u2tu.at(dependency_key);

            if (dep_tu.is_modular && fs::exists(dep_tu.pcm_path)
                && fs::last_write_time(dep_tu.pcm_path) > object_timestamp)
                return pcm_rebuild(output::rebuild_kind::pcm_stale, dep_tu);

            if (visited.contains(dep_tu.unit))
                continue;
            visited.insert(dep_tu.unit);

            if (auto stale = transitive_pcm_newer_than_object(dep_tu, object_timestamp, u2tu, visited))
                return stale;
        }
        return std::nullopt;
    }

    // Prerequisites are restricted to the project tree: toolchain headers change as a unit and
    // are already covered by the object-cache profile (cxx_sig / clang_ver), so scanning them
    // would only add thousands of stat calls per build. An unreadable depfile is a rebuild of its
    // own — it is the only record of the unit's textual includes, so a cache hit would silently
    // ignore every header edit until the source changed. Every compile writes one (`-MMD` writes
    // it even for a unit that includes nothing), so this fires once and then settles.
    //
    // Resolve each prerequisite first. Clang -MMD writes a relative path when -I is relative
    // (the documented `-I include/path` form), while source_dir is absolute — comparing the
    // relative spelling with starts_with would skip every such header and never fire
    // header_stale.
    std::optional<output::rebuild_info> stale_header(const translation_unit& tu,
                                                     fs::file_time_type object_timestamp) const
    {
        const auto depfile = depfile_path(tu);
        const auto prerequisites = detail::parse_depfile(depfile);
        if(not prerequisites)
            return output::rebuild_info{.kind = output::rebuild_kind::depfile_unusable, .trigger_path = depfile};

        for(const auto& prerequisite : *prerequisites)
        {
            const auto resolved = normalize_path(prerequisite);
            if(resolved == tu.full_path or not resolved.starts_with(source_dir))
                continue;

            auto error = std::error_code{};
            const auto timestamp = fs::last_write_time(resolved, error);
            // Missing or unreadable: the depfile still names this header, so a cache hit
            // would keep shipping an object built against content that is gone. Force a
            // recompile; clang then reports the include failure if the source still needs it.
            if(error)
                return output::rebuild_info{.kind = output::rebuild_kind::header_missing, .trigger_path = resolved};
            if(timestamp > object_timestamp)
                return output::rebuild_info{.kind = output::rebuild_kind::header_stale, .trigger_path = resolved};
        }
        return std::nullopt;
    }

    std::optional<output::rebuild_info> needs_recompile(const translation_unit& tu, const object_cache_load& cache, const unit_to_tu_map& u2tu) const {
        // First-seen path for this config vs edited source after a prior compile.
        if (not cache.entries.contains(tu.full_path)) {
            if (cache.profile_change)
                return output::rebuild_info{.kind = output::rebuild_kind::profile_change, .trigger_path = tu.full_path};
            return output::rebuild_info{.kind = output::rebuild_kind::not_in_cache, .trigger_path = tu.full_path};
        }
        if (cache.entries.at(tu.full_path) < tu.last_modified)
            return output::rebuild_info{.kind = output::rebuild_kind::source_stale, .trigger_path = tu.full_path};

        // Ensure the object file exists and is up-to-date versus the source timestamp we cached.
        if (not fs::exists(tu.object_path))
            return output::rebuild_info{.kind = output::rebuild_kind::object_missing, .trigger_path = tu.full_path};

        auto object_timestamp = fs::last_write_time(tu.object_path);
        if (object_timestamp < cache.entries.at(tu.full_path))
            return output::rebuild_info{.kind = output::rebuild_kind::object_stale, .trigger_path = tu.full_path};

        // Textual #include dependencies are invisible to the module graph, so the
        // compiler's own depfile is the only record of them.
        if (auto header_reason = stale_header(tu, object_timestamp))
            return header_reason;

        // Implementation units consume their interface PCM implicitly through
        // -fmodule-file=<module>=<pcm>, even when they do not import that module.
        if(tu.kind == unit_kind::implementation_unit && u2tu.contains(tu.module))
        {
            const auto& interface = *u2tu.at(tu.module);
            if(not fs::exists(interface.pcm_path))
                return pcm_rebuild(output::rebuild_kind::dependency_pcm_stale, interface);
            if(fs::last_write_time(interface.pcm_path) > object_timestamp)
                return pcm_rebuild(output::rebuild_kind::pcm_stale, interface);
            if(auto interface_reason = needs_recompile(interface, cache, u2tu))
                return attributed_to(*interface_reason, interface);
        }

        // For modular units, also check if .pcm file is stale. The pcm is the object's
        // input under two-phase (and the sibling artefact under one-phase): a successful
        // --precompile followed by a failed/skipped object step leaves a pcm newer than
        // the .o, and without this check the unit cache-hits while importers rebuild
        // against the new interface — linking the old implementation into the binary.
        if (tu.is_modular) {
            if (not fs::exists(tu.pcm_path))
                return pcm_rebuild(output::rebuild_kind::own_pcm_missing, tu);
            auto pcm_timestamp = fs::last_write_time(tu.pcm_path);
            if (pcm_timestamp < tu.last_modified)
                return pcm_rebuild(output::rebuild_kind::own_pcm_stale, tu);
            if (object_timestamp < pcm_timestamp)
                return pcm_rebuild(output::rebuild_kind::object_stale, tu);
        }

        // Rebuild when any transitive import PCM is newer than this object file.
        // Catches partition updates (e.g. tester:assertions) for test TUs that import an umbrella module.
        auto visited = std::flat_set<std::string>{};
        if (auto stale = transitive_pcm_newer_than_object(tu, object_timestamp, u2tu, visited))
            return stale;

        // Rebuild if any imported modules have changed (their .pcm files are stale or they need recompiling)
        for (const auto& dependency_key : tu.imports) {
            if (u2tu.contains(dependency_key)) {
                const auto& dep_tu = *u2tu.at(dependency_key);
                // Check if the imported module's .pcm is stale compared to its source
                if (dep_tu.is_modular) {
                    if (not fs::exists(dep_tu.pcm_path) or 
                        fs::last_write_time(dep_tu.pcm_path) < dep_tu.last_modified) {
                        return pcm_rebuild(output::rebuild_kind::dependency_pcm_stale, dep_tu);
                    }
                }
                // Also recursively check if the imported module needs recompiling
                if (auto dep_reason = needs_recompile(dep_tu, cache, u2tu))
                    return attributed_to(*dep_reason, dep_tu);
            }
        }

        return std::nullopt;
    }

    executable_cache_map load_executable_cache() const {
        auto cache = executable_cache_map{};
        auto file = std::ifstream{executable_cache_path()};
        if (not file) return cache;
        auto path = ""s;
        auto signature = ""s;
        while (std::getline(file, path, '\t') and std::getline(file, signature)) {
            cache[path] = signature;
        }
        return cache;
    }

    void save_executable_cache(const executable_cache_map& cache) const {
        if (cache.empty()) {
            remove_if_exists(executable_cache_path());
            return;
        }
        write_cache_file(executable_cache_path(), "executable cache", [&](std::ostream& file) {
            for (const auto& [path, signature] : cache)
                file << path << "\t" << signature << "\n";
        });
    }

    std::string dependency_signature(const std::string& path) const {
        if (path.empty() or not fs::exists(path))
            return path + ":missing";
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            fs::last_write_time(path).time_since_epoch()).count();
        return path + ":" + std::to_string(timestamp);
    }

    // Whether there was a file to remove, which is what cache_invalidate reports per cache.
    static bool remove_if_exists(const std::string& path)
    {
        if(not fs::exists(path))
            return false;
        fs::remove(path);
        return true;
    }

    std::string dependency_signatures_joined(const string_list& paths) const
    {
        return paths
            | std::views::transform([&](const std::string& path) { return dependency_signature(path); })
            | std::views::join_with("|"sv)
            | std::ranges::to<std::string>();
    }

    // What identifies a link in the executable cache: every input's timestamp, plus the flag sets
    // that would change the result even when no input moved. Both links are cached the same way —
    // they differ only in what they take in, which is the caller's half of the answer.
    std::string link_signature(const string_list& input_paths, const string_list& import_flags) const
    {
        auto signature = dependency_signatures_joined(input_paths);
        signature += "|flags=";
        signature += detail::flags_profile_string(compile_flags);
        signature += "|link=";
        signature += detail::flags_profile_string(link_flags);
        signature += "|modules=";
        signature += detail::flags_profile_string(module_flags);
        signature += "|imports=";
        signature += detail::flags_profile_string(import_flags);
        return signature;
    }

    std::optional<output::rebuild_info> needs_relinking(std::string_view executable_path,
                                                        const std::string& signature,
                                                        const executable_cache_map& link_cache) const
    {
        if(not fs::exists(executable_path))
            return output::rebuild_info{.kind = output::rebuild_kind::missing_executable};

        if(not link_cache.contains(executable_path))
            return output::rebuild_info{.kind = output::rebuild_kind::not_in_cache};

        const auto& previous = link_cache.at(executable_path);
        if(previous == signature)
            return std::nullopt;

        const auto flag_marker = "|flags="sv;
        const auto previous_flags = previous.find(flag_marker);
        const auto current_flags = signature.find(flag_marker);
        if(previous_flags != std::string::npos and current_flags != std::string::npos)
        {
            const auto previous_objects = previous.substr(0, previous_flags);
            const auto current_objects = signature.substr(0, current_flags);
            if(previous_objects != current_objects)
                return output::rebuild_info{.kind = output::rebuild_kind::object_changed};
            if(previous.substr(previous_flags) != signature.substr(current_flags))
                return output::rebuild_info{.kind = output::rebuild_kind::link_flags_changed};
        }

        return output::rebuild_info{.kind = output::rebuild_kind::signature_changed};
    }

    // ============================================================================
    // Dependency Analysis
    // ============================================================================

    void scan_and_order() {
        auto units = translation_unit_list{};
        try {
            auto path = fs::path{source_dir};
            if (not fs::exists(path) or not fs::is_directory(path)) {
                units_in_topological_order = std::move(units); return;
            }

            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (not entry.is_regular_file()) continue;

                auto rel_path = entry.path().lexically_relative(path).string();

                // Skip nested package checkouts, vendored package test trees, build-*
                // output trees, tools/, and .git/. Do not hard-skip project test/ trees
                // here — determine_is_test marks them as is_test, and include_tests
                // (debug / --build-tests) decides whether they join.
                if (detail::is_nested_dependency_path(rel_path) or
                    detail::is_dependency_package_tests_path(rel_path) or
                    detail::is_tester_package_tests_path(rel_path) or
                    detail::is_build_output_path(rel_path) or
                    detail::path_under_dir(rel_path, tools_dir_name) or
                    detail::path_under_dir(rel_path, git_dir_name))
                    continue;

                // Exclude examples by default, include only if flag is set
                if (not include_examples and detail::path_under_dir(rel_path, examples_dir_name))
                    continue;

                if (not translation_unit::is_supported(entry.path()))
                    continue;

                try {
                    auto tu = parse_translation_unit(path, entry.path());
                    if (tu.is_test and not include_tests)
                        continue;
                    units.push_back(std::move(tu));
                } catch (const std::exception& e) {
                    output::notify(&output::observer::warning, "Skipping "s + entry.path().string() + ": " + e.what());
                }
            }
        } catch (const std::exception& e) {
            throw std::runtime_error{"Failed to scan project: "s + e.what()};
        } catch (...) {
            throw std::runtime_error{"Failed to scan project: unknown error"};
        }

        if (units.empty()) { units_in_topological_order = std::move(units); return; }

        auto dependencies = dependency_graph{};
        auto indegrees = indegree_map{};
        auto unit_to_tu = unit_to_tu_map{};

        for (auto& tu : units) {
            if(unit_to_tu.contains(tu.unit))
            {
                const auto& prior = *unit_to_tu.at(tu.unit);
                const auto prior_path = prior.path.empty() ? prior.filename : prior.path + "/" + prior.filename;
                const auto current_path = tu.path.empty() ? tu.filename : tu.path + "/" + tu.filename;
                throw std::runtime_error{
                    "Duplicate translation unit key '" + tu.unit + "' from "
                    + prior_path + " and " + current_path
                    + " (object/module names must stay unique)"};
            }
            unit_to_tu[tu.unit] = &tu;
            indegrees[tu.unit] = 0;
        }

        for (const auto& tu : units) {
            // Module imports create edges from imported module -> importer.
            for (const auto& module : tu.imports) {
                if (unit_to_tu.contains(module)) {
                    dependencies[module].push_back(tu.unit);
                    indegrees[tu.unit]++;
                }
            }

            // Implementation units must build after their interface.
            if (tu.kind == unit_kind::implementation_unit) {
                if (unit_to_tu.contains(tu.module)) {
                    dependencies[tu.module].push_back(tu.unit);
                    indegrees[tu.unit]++;
                }
            }
        }

        auto ready = topo_sort_queue{};
        for (const auto& [unit, degree] : indegrees)
            if (degree == 0) ready.push(unit);

        auto sorted = translation_unit_list{};
        auto level = 0;
        while (not ready.empty()) {
            auto batch_size = ready.size();
            for (size_t i = 0; i < batch_size; ++i) {
                auto unit = ready.front();
                ready.pop();

                auto* tu = unit_to_tu.at(unit);
                tu->dependency_level = level;
                sorted.push_back(*tu);

                for (const auto& dependent_unit : dependencies[unit]) {
                    if (--indegrees[dependent_unit] == 0)
                        ready.push(dependent_unit);
                }
            }
            ++level;
        }

        auto cyclic_units = string_list{};
        for (const auto& [unit, degree] : indegrees)
            if (degree > 0)
                cyclic_units.push_back(unit);

        if (not cyclic_units.empty()) {
            auto message = "Cyclic dependency detected between units:"s;
            for (const auto& unit : cyclic_units)
                message += " " + unit;
            throw std::runtime_error{message};
        }

        auto object_owners = std::flat_map<std::string, std::string, std::less<>>{};
        auto pcm_owners = std::flat_map<std::string, std::string, std::less<>>{};
        auto executable_owners = std::flat_map<std::string, std::string, std::less<>>{};

        // Reserve the libc++ std module artifacts so a project TU named `std`
        // (std.c++ / export module std;) cannot silently overwrite them.
        object_owners.emplace(std_obj_path(), "reserved std module object");
        pcm_owners.emplace(std_pcm_path(), "reserved std module PCM");

        for (auto& tu : sorted) {
            // Attach builder-managed artifact paths once we know the full configuration.
            // Keeping them here keeps the translation unit metadata immutable while giving downstream
            // steps a single place to read object/PCM/binary locations from.
            const auto source_label = tu.path.empty() ? tu.filename : tu.path + "/" + tu.filename;
            tu.object_path = compute_object_path(tu);
            if(object_owners.contains(tu.object_path))
                throw std::runtime_error{
                    "Duplicate object path '" + tu.object_path + "' from "
                    + object_owners.at(tu.object_path) + " and " + source_label
                    + " (object/module names must stay unique)"};
            object_owners.emplace(tu.object_path, source_label);

            if (tu.is_modular) {
                tu.pcm_path = compute_pcm_path(tu);
                if(pcm_owners.contains(tu.pcm_path))
                    throw std::runtime_error{
                        "Duplicate PCM path '" + tu.pcm_path + "' from "
                        + pcm_owners.at(tu.pcm_path) + " and " + source_label
                        + " (object/module names must stay unique)"};
                pcm_owners.emplace(tu.pcm_path, source_label);
            }
            if (tu.has_main) {
                tu.executable_path = compute_executable_path(tu);
                if(executable_owners.contains(tu.executable_path))
                    throw std::runtime_error{
                        "Duplicate executable path '" + tu.executable_path + "' from "
                        + executable_owners.at(tu.executable_path) + " and " + source_label
                        + " (object/module names must stay unique)"};
                executable_owners.emplace(tu.executable_path, source_label);
            }
            validate_translation_unit(tu);
        }

        units_in_topological_order = std::move(sorted);
    }

    // ============================================================================
    // Standard Library Module Building
    // ============================================================================
    // The same two halves as Compilation, for the one modular unit that is not in the scan:
    // needs_std_module_rebuild is its needs_recompile and build_std_module its compile_unit,
    // reporting through compile_scope like every other unit — the two most expensive steps of a
    // cold build used to explain nothing.

    // Whether std.pcm was precompiled by the profile a project unit is compiled with. The pcm's
    // own presence is a separate question, asked by needs_std_module_rebuild before this one.
    bool std_module_profile_matches() const
    {
        const auto stored = detail::read_first_line(std_module_profile_path());
        return not stored.empty() and stored == object_cache_profile();
    }

    void save_std_module_profile() const
    {
        fs::create_directories(cache_dir());
        write_cache_file(std_module_profile_path(), "std module profile", [&](std::ostream& file) {
            file << object_cache_profile() << '\n';
        });
    }

    // In the order the artifacts depend on each other, so the reason names the first thing that
    // has to be rebuilt and the caller can tell whether the pcm step is included. The profile is
    // what mtimes cannot see: a toolchain change leaves a pcm newer than std.cppm and wrong.
    // std.cppm is known to exist — detect_llvm_environment throws otherwise.
    std::optional<output::rebuild_info> needs_std_module_rebuild() const
    {
        const auto pcm = std_pcm_path();
        const auto object = std_obj_path();
        const auto module_of = [&](output::rebuild_kind kind) {
            return output::rebuild_info{
                .kind = kind, .module = std::string{std_module_name}, .pcm_path = pcm};
        };

        if(not fs::exists(pcm))
            return module_of(output::rebuild_kind::own_pcm_missing);
        if(not std_module_profile_matches())
            return module_of(output::rebuild_kind::profile_change);
        if(fs::last_write_time(pcm) < fs::last_write_time(std_module_source))
            return module_of(output::rebuild_kind::own_pcm_stale);
        if(not fs::exists(object))
            return module_of(output::rebuild_kind::object_missing);
        if(fs::last_write_time(object) < fs::last_write_time(pcm))
            return module_of(output::rebuild_kind::object_stale);
        return std::nullopt;
    }

    // Observers see what they see for a project modular unit: one source, one pcm, one object.
    // The strings are the caller's, because compile_unit holds views.
    output::compile_unit std_compile_unit(const std::string& pcm,
                                          const std::string& object,
                                          const std::string& display) const
    {
        return {.source = std_module_source,
                .object = object,
                .pcm = pcm,
                .module = std_module_name,
                .display_path = display};
    }

    void build_std_module()
    {
        const auto pcm = std_pcm_path();
        const auto object = std_obj_path();
        const auto display = fs::path{std_module_source}.filename().string();
        const auto unit = std_compile_unit(pcm, object, display);
        const auto reason = needs_std_module_rebuild();

        if(not reason)
        {
            const auto hit = compile_scope{unit};
            return;
        }

        auto compile = compile_scope{unit, *reason};
        if(module_phases == module_compilation::one_phase)
        {
            // One step emits both artefacts, so there is no object-only shortcut to take: a
            // missing std.o re-reads std.cppm. Rare, and the cold build is a whole parse cheaper.
            run_step(compile, build_std_module_object_argv(), diagnostics_path_for_object(unit));
            save_std_module_profile();
            compile.succeeded();
            return;
        }
        // The pcm is the object's input, so every reason that reaches it rebuilds both; the two
        // object-only reasons reuse the pcm that is already there.
        const auto object_only = reason->kind == output::rebuild_kind::object_missing
                              or reason->kind == output::rebuild_kind::object_stale;
        if(not object_only)
        {
            run_step(compile, build_std_pcm_argv(), diagnostics_path_for_pcm(unit));
            save_std_module_profile();
        }
        run_step(compile, build_std_o_argv(), diagnostics_path_for_object(unit));
        compile.succeeded();
    }

    // ============================================================================
    // Compilation
    // ============================================================================
    // Two halves, mirrored by Linking below: compile_unit does one translation unit, and
    // compile_units is the pass that decides what to build, reports the hits and runs the rest
    // through run_in_parallel. What both phases share sits above, in Cache Management (caches and
    // staleness) and General Utilities (argv builders, unit projections).

    void compile_unit(const translation_unit& tu, const output::rebuild_info& rebuild) {
        const auto unit = compile_unit_of(tu);
        auto compile = compile_scope{unit, rebuild};
        // Two-phase: object_missing / object_stale reuse the pcm that is already there —
        // same split build_std_module uses. Re-precompiling would bump the pcm mtime and
        // force every importer through pcm_stale for no interface change.
        // One-phase: the BMI is a sibling of the object (reduced on Clang 22+), not an
        // input that can be compiled to .o — always re-read the source, as std does.
        const auto object_only = module_phases == module_compilation::two_phase
                              and (rebuild.kind == output::rebuild_kind::object_missing
                                   or rebuild.kind == output::rebuild_kind::object_stale);
        if (tu.is_modular and module_phases == module_compilation::two_phase) {
            if(not object_only)
                run_step(compile, compile_pcm_argv(tu), diagnostics_path_for_pcm(unit));
            run_step(compile, compile_pcm_object_argv(tu), diagnostics_path_for_object(unit));
        } else if (tu.is_modular) {
            run_step(compile, compile_module_object_argv(tu), diagnostics_path_for_object(unit));
        } else {
            run_step(compile, compile_source_object_argv(tu), diagnostics_path_for_object(unit));
        }
        compile.succeeded();
    }

    void compile_units() {
        if (units_in_topological_order.empty()) return;
        auto cache = load_object_cache();
        if(cache.profile_change)
            output::notify(&output::observer::profile_changed,
                           output::rebuild_kind::profile_change,
                           *cache.profile_change);

        auto u2tu = unit_to_tu_map{};
        for (auto& tu : units_in_topological_order) {
            auto k = tu.unit;
            u2tu[k] = &tu;
        }

        auto levels = level_groups_map{};
        for (const auto& tu : units_in_topological_order)
            levels[tu.dependency_level >= 0 ? tu.dependency_level : INT_MAX].push_back(&tu);

        // A reason is a fact about this moment: once a unit at this level rewrites its pcm,
        // needs_recompile answers differently, and for the unit just built it answers nothing. So
        // decision and reason are one answer taken before any worker starts, as link_decision is.
        struct compile_decision {
            const translation_unit* tu = nullptr;
            std::optional<output::rebuild_info> reason{};
        };

        for (const auto& [lvl, group] : levels) {
            auto decisions = group
                | std::views::transform([&](const translation_unit* tu) {
                      return compile_decision{tu, needs_recompile(*tu, cache, u2tu)}; })
                | std::ranges::to<std::vector>();

            for (const auto& decision : decisions) {
                if(decision.reason)
                    continue;
                const auto hit = compile_scope{compile_unit_of(*decision.tu)};
            }

            run_in_parallel(decisions | std::views::filter([](const compile_decision& decision) {
                                            return decision.reason.has_value(); }),
                            job_limit(),
                            [&](const compile_decision& decision) {
                                const auto& tu = *decision.tu;
                                compile_unit(tu, *decision.reason);
                                auto lock = std::lock_guard<std::mutex>{cache_mutex};
                                cache.entries[tu.full_path] = tu.last_modified;
                            });
        }
        save_object_cache(cache.entries);
    }

    // ============================================================================
    // Linking
    // ============================================================================
    // The same two halves as Compilation: link_executable does one executable, link_executables
    // is the pass. Only what nothing else uses lives here — the test-runner link reads the same
    // signature inputs and executable cache from the shared sections above.

    void link_executable(const translation_unit& tu,
                         const string_list& shared_objects,
                         const output::rebuild_info& rebuild,
                         std::string signature) {
        if (not tu.has_main) return;
        auto link = link_scope{tu.executable_path, rebuild, std::move(signature)};
        run_step(link,
                 link_executable_argv(tu, shared_objects),
                 diagnostics_path_for_executable(tu));
        link.succeeded();
    }

    // What this executable takes in: its own object, the shared objects, the std object.
    std::string compute_link_signature(const translation_unit& tu, const string_list& shared_objects) const {
        auto paths = string_list{tu.object_path};
        paths.append_range(shared_objects);
        paths.push_back(std_obj_path());
        return link_signature(paths, collect_module_ldflags(tu.imports));
    }

    void link_executables() {
        auto shared_objects = linkable_object_paths();
        auto link_cache = load_executable_cache();

        // Snapshot relink decisions before workers mutate link_cache. Interleaving
        // needs_relinking (unlocked reads) with parallel operator[] writes is a data race.
        struct link_decision {
            const translation_unit* tu = nullptr;
            std::string signature{};
            std::optional<output::rebuild_info> reason{};
        };
        auto decisions = units_in_topological_order
            // Exact base name only — substring matches like contest_runner / aaa_test_runner
            // are ordinary mains and must not be excluded from normal linking.
            | std::views::filter([&](const translation_unit& tu) {
                  return tu.has_main and tu.base_name != test_runner_name; })
            | std::views::transform([&](const translation_unit& tu) {
                  auto signature = compute_link_signature(tu, shared_objects);
                  auto reason = needs_relinking(tu.executable_path, signature, link_cache);
                  return link_decision{&tu, std::move(signature), std::move(reason)}; })
            | std::ranges::to<std::vector>();

        for(const auto& decision : decisions)
        {
            if(decision.reason)
                continue;
            const auto hit = link_scope{decision.tu->executable_path, decision.signature};
        }

        run_in_parallel(decisions | std::views::filter([](const link_decision& decision) {
                                        return decision.reason.has_value(); }),
                        job_limit(),
                        [&](const link_decision& decision) {
                            const auto& tu = *decision.tu;
                            link_executable(tu, shared_objects, *decision.reason, decision.signature);
                            auto lock = std::lock_guard<std::mutex>{link_cache_mutex};
                            link_cache[tu.executable_path] = decision.signature;
                        });
        save_executable_cache(link_cache);
    }

    // ============================================================================
    // Test Support
    // ============================================================================
    // The same order as Linking: one executable, its signature, then the pass. test_runner_unit
    // is the one extra — Linking filters mains inline, while here a missing or duplicate runner
    // is an error rather than a skip.

    // Require an exact base name: substring selection (aaa_test_runner, contest_runner) can link
    // a different bin/<name> while run_tests always executes bin/test_runner, leaving a stale
    // runner and silent CI passes. Never absent either — linking the test objects without a main
    // dies at the linker, so a project with no runner source is told so here.
    const translation_unit& test_runner_unit() const
    {
        const auto is_runner = [](const translation_unit& tu) {
            return tu.has_main and tu.base_name == test_runner_name;
        };
        if(std::ranges::count_if(units_in_topological_order, is_runner) > 1)
            throw std::runtime_error{
                "multiple test_runner mains found — keep a single source named test_runner"};

        const auto found = std::ranges::find_if(units_in_topological_order, is_runner);
        if(found == units_in_topological_order.end())
            throw std::runtime_error{
                "test_runner not found — make sure .test.c++ files or test_runner.c++ exist"};
        return *found;
    }

    void link_test_runner_executable(const translation_unit& runner,
                                     const output::rebuild_info& rebuild,
                                     std::string signature)
    {
        auto link = link_scope{runner.executable_path, rebuild, std::move(signature)};
        run_step(link,
                 link_test_runner_argv(runner),
                 diagnostics_path_for_executable(runner));
        link.succeeded();
    }

    // What the runner takes in: its object, the test objects, the shared objects, the std object.
    // The flag tail covers every test unit's imports as well as the runner's — a change to any is
    // a relink — while the argv needs only the runner's own, the split link_executable also has.
    std::string compute_test_runner_signature(const translation_unit& runner) const {
        auto paths = string_list{runner.object_path};
        paths.append_range(test_object_paths());
        paths.append_range(linkable_object_paths());
        paths.push_back(std_obj_path());
        auto flags = collect_test_module_ldflags();
        flags.append_range(collect_module_ldflags(runner.imports));
        return link_signature(paths, flags);
    }

    void link_test_runner() {
        // The unit carries the path, so the link, the cache key, the log and what run_tests
        // executes cannot be spelled two ways: test_runner_unit pins the base name, and
        // compute_executable_path is the only place a main's executable is sited.
        const auto& runner = test_runner_unit();

        auto link_cache = load_executable_cache();
        const auto signature = compute_test_runner_signature(runner);
        const auto reason = needs_relinking(runner.executable_path, signature, link_cache);
        if(not reason)
        {
            const auto hit = link_scope{runner.executable_path, signature};
            return;
        }

        link_test_runner_executable(runner, *reason, signature);
        output::notify(&output::observer::success, "test_runner linked with test objects");
        {
            auto lock = std::lock_guard<std::mutex>{link_cache_mutex};
            link_cache[runner.executable_path] = signature;
        }
        save_executable_cache(link_cache);
    }

    // ============================================================================
    // Build Orchestration
    // ============================================================================
    // The phases above in the order a build runs them, the only place that order is written
    // down. build() runs it alone; run_tests() follows it with the test-runner link.

    void build_steps()
    {
        // Ensure build directories exist (they may have been removed by clean())
        fs::create_directories(module_cache_dir());
        fs::create_directories(object_dir());
        fs::create_directories(binary_dir());
        fs::create_directories(cache_dir());

        build_std_module();
        scan_and_order();
        if(units_in_topological_order.empty())
            throw std::runtime_error{"No sources found"};

        update_module_flags();
        compile_units();
        link_executables();
    }

public:

    build_system(
        build_config cfg,
        const string_list& cpf = {},
        const module_to_ldflags_map& mlf = {},
        const std::string& src = ".",
        const std::string& stdcppm = "",
        bool static_linking = false,
        bool include_examples_flag = false,
        const string_list& extra_compile_flags_param = {},
        const string_list& extra_link_flags_param = {}
    ) : config(cfg), static_link(static_linking), source_dir(src), cpp_flags(cpf), module_ldflags(mlf), std_module_source(stdcppm), include_tests(config == build_config::debug), include_examples(include_examples_flag), extra_compile_flag_tokens(extra_compile_flags_param), extra_link_flag_tokens(extra_link_flags_param) {
        source_dir = normalize_path(source_dir);

        // Detect and setup LLVM environment (std.cppm location, LLVM prefix, compiler path)
        detect_llvm_environment();

        // Initialize compile and link flags based on OS, config, and LLVM paths
        initialize_build_flags();
    }

    void clean() const {
        auto dir = build_root();
        if (fs::exists(dir)) {
            fs::remove_all(dir);
            output::notify(&output::observer::success, "Removed "s + dir);
        } else {
            output::notify(&output::observer::info, "Nothing to clean for "s + dir);
        }
    }

    // Drop only test TU artefacts and the test_runner binary — leave library/app objects so the
    // next build recompiles tests without a full cold rebuild. Scans with include_tests on so
    // release configs still see *.test.c++ even when a normal release build would not.
    void clean_tests() {
        include_tests = true;
        if(not fs::exists(object_dir()) and not fs::exists(binary_dir()))
        {
            output::notify(&output::observer::info,
                           "Nothing to clean for test objects under "s + build_root());
            return;
        }

        scan_and_order();

        // Object-cache keys are source paths (full_path), not .o paths.
        auto dropped_sources = std::flat_set<std::string, std::less<>>{};
        auto removed = 0;

        const auto try_remove = [&](const std::string& path) {
            if(remove_if_exists(path))
                ++removed;
        };

        for(const auto& tu : units_in_topological_order)
        {
            const auto is_runner = tu.has_main and tu.base_name == test_runner_name;
            if(not tu.is_test and not is_runner)
                continue;

            dropped_sources.insert(tu.full_path);
            if(not tu.object_path.empty())
            {
                try_remove(tu.object_path);
                try_remove(depfile_path(tu));
                try_remove(tu.object_path + ".log");
            }
            if(tu.is_modular and not tu.pcm_path.empty())
            {
                try_remove(tu.pcm_path);
                try_remove(tu.pcm_path + ".log");
            }
            if(is_runner and not tu.executable_path.empty())
            {
                try_remove(tu.executable_path);
                try_remove(tu.executable_path + ".link.log");
            }
        }

        if(not dropped_sources.empty() and fs::exists(object_cache_path()))
        {
            auto cache = load_object_cache();
            if(not cache.profile_change)
            {
                auto erased = false;
                for(const auto& path : dropped_sources)
                {
                    if(cache.entries.contains(path))
                    {
                        cache.entries.erase(path);
                        erased = true;
                    }
                }
                if(erased)
                    save_object_cache(cache.entries);
            }
        }

        if(fs::exists(executable_cache_path()))
        {
            auto link_cache = load_executable_cache();
            const auto runner_exe = detail::join_dir(binary_dir(), test_runner_name);
            if(link_cache.contains(runner_exe))
            {
                link_cache.erase(runner_exe);
                save_executable_cache(link_cache);
            }
        }

        if(removed == 0)
            output::notify(&output::observer::info,
                           "No test objects to remove under "s + build_root());
        else
            output::notify(&output::observer::success,
                           "Removed " + std::to_string(removed) + " test artifact(s) under "s
                               + build_root());
    }

    void cache_status() const
    {
        ensure_toolchain_profile();
        fs::create_directories(cache_dir());

        const auto current_profile = object_cache_profile();
        const auto cache_path = object_cache_path();
        const auto cache_exists = fs::exists(cache_path);

        auto profile_match = false;
        auto object_entries = 0;
        auto object_stale = 0;

        if(cache_exists)
        {
            auto file = std::ifstream{cache_path};
            auto header = ""s;
            if(std::getline(file, header) && header.starts_with("profile\t"))
            {
                const auto stored_profile = header.substr(std::string_view{"profile\t"}.size());
                profile_match = stored_profile == current_profile;
                count_cache_entries(file, object_entries, object_stale);
            }
        }

        // Paths outlive the notify: cache_inventory holds views, so the strings they point at
        // are named here rather than built inside the aggregate.
        const auto link_cache_path = executable_cache_path();
        const auto std_profile_path = std_module_profile_path();
        const auto stamp_path = compiler_stamp_path();

        auto executable_entries = 0;
        if(fs::exists(link_cache_path))
        {
            auto file = std::ifstream{link_cache_path};
            auto line = ""s;
            while(std::getline(file, line))
            {
                const auto tab = line.find('\t');
                if(tab != std::string::npos && not line.substr(0, tab).empty())
                    ++executable_entries;
            }
        }

        output::notify(
            &output::observer::cache_status,
            output::cache_inventory{
                .object_cache_path = cache_path,
                .object_cache_exists = cache_exists,
                .profile_match = profile_match,
                .object_entries = object_entries,
                .object_stale_entries = object_stale,
                .executable_cache_path = link_cache_path,
                .executable_cache_exists = fs::exists(link_cache_path),
                .executable_entries = executable_entries,
                .std_module_profile_path = std_profile_path,
                .std_module_profile_exists = fs::exists(std_profile_path),
                .std_module_profile_match = std_module_profile_matches(),
                .compiler_stamp_path = stamp_path,
                .compiler_stamp_exists = fs::exists(stamp_path),
                .current_profile = current_profile});
    }

    void cache_invalidate() const
    {
        fs::create_directories(cache_dir());

        // Every file cache_status reports, so the two commands cannot disagree about what the
        // cache is. The std module profile is the one that makes CB rebuild std.pcm.
        const auto removed = output::cache_removals{
            .object_cache = remove_if_exists(object_cache_path()),
            .executable_cache = remove_if_exists(executable_cache_path()),
            .compiler_stamp = remove_if_exists(compiler_stamp_path()),
            .std_module_profile = remove_if_exists(std_module_profile_path())};

        output::notify(&output::observer::cache_invalidate_end, removed);
    }

    void set_include_tests(bool value) {
        include_tests = value;
    }

    // Recorded in the object-cache profile, so flipping it rebuilds every modular unit rather
    // than mixing BMIs the two schemes do not produce identically.
    void set_module_phases(module_compilation value) {
        module_phases = value;
    }

    // 0 requests the hardware default.
    void set_max_jobs(int value) {
        max_jobs = value;
    }

    std::ptrdiff_t job_limit() const {
        if(max_jobs > 0)
            return max_jobs;
        const auto detected = std::thread::hardware_concurrency();
        return detected > 0 ? static_cast<std::ptrdiff_t>(detected) : 1;
    }

    void build() {
        auto build = build_scope{config_name(), include_tests, include_examples};
        build_steps();
        build.succeeded();

        output::notify(&output::observer::success, "Build completed: "s + build_root());
    }

    // Returns false when the test runner reports failures (normal outcome, not exceptional).
    bool run_tests(const std::vector<std::string>& args = {}) {
        output::notify(&output::observer::info, "=== Running tests ===");

        include_tests = true;
        {
            // No check that the runner is there afterwards: link_test_runner either produced
            // it or threw, and the missing-source case is the sentence it throws.
            auto build = build_scope{config_name(), true, include_examples};
            build_steps();
            link_test_runner();
            build.succeeded();
        }
        output::notify(&output::observer::success, "Build completed: "s + build_root());

        // From the unit link_test_runner just linked, so what runs is what was linked.
        const auto& runner = test_runner_unit().executable_path;

        const auto set_env = [](std::string_view key, std::string_view value)
        {
            if(::setenv(std::string{key}.c_str(), std::string{value}.c_str(), /*overwrite=*/1) != 0)
                throw std::system_error{errno, std::generic_category(), "setenv"};
        };
        set_env(tester_config_env, config_name());
        if(const auto parent = output::run_id(); not parent.empty())
            set_env(tester_parent_run_id_env, parent);

        auto result = output::process_result{};
        {
            auto test = test_scope{runner};
            // Not captured: the runner's stdout is the JSONL stream being forwarded.
            result = invoke_shell(test_runner_argv(runner, args));
            test.finished(result);
        }
        return result.ok();
    }

    // The compilation-database entry for a source is the argv that reads that source —
    // --precompile for two-phase modular interfaces/partitions, -c for everything else. The
    // second two-phase step (pcm → .o) is omitted: it has no distinct source path for clangd.
    string_list compile_argv_for_database(const translation_unit& tu) const
    {
        if(tu.is_modular)
            return module_phases == module_compilation::two_phase
                ? compile_pcm_argv(tu)
                : compile_module_object_argv(tu);
        return compile_source_object_argv(tu);
    }

    // clangd discovers how each file is built through this file. list already scanned the
    // active TU set; writing here keeps the database aligned with that inventory without a
    // second configuration language. Module -fmodule-file= flags require update_module_flags.
    void write_compile_commands() const
    {
        const auto path = detail::join_dir(source_dir, compile_commands_filename);
        write_cache_file(path, "compile commands database", [&](std::ostream& file) {
            file << "[\n";
            auto first = true;
            for(const auto& tu : units_in_topological_order)
            {
                if(not first)
                    file << ",\n";
                first = false;
                const auto arguments = compile_argv_for_database(tu);
                file << "  {\n";
                file << "    \"directory\": \"" << output::jsonl::escape(source_dir) << "\",\n";
                file << "    \"file\": \"" << output::jsonl::escape(tu.full_path) << "\",\n";
                file << "    \"arguments\": [" << output::jsonl::join_json_strings(arguments) << "]\n";
                file << "  }";
            }
            file << "\n]\n";
        });
        output::notify(&output::observer::info,
                       "Wrote "s + path + " (" + std::to_string(units_in_topological_order.size())
                           + " entries)");
    }

    // Convenience dump of the same graph `list --jsonl` streams as `unit` events — for tools
    // that want one file rather than a JSONL parse. Same fields as the inventory projection.
    void write_graph_json(const output::source_inventory& inventory) const
    {
        const auto path = detail::join_dir(source_dir, graph_filename);
        write_cache_file(path, "module dependency graph", [&](std::ostream& file) {
            file << "{\n";
            file << "  \"schema\": \"cb-graph\",\n";
            file << "  \"version\": 1,\n";
            file << "  \"config\": \"" << output::jsonl::escape(inventory.config) << "\",\n";
            file << "  \"source_dir\": \"" << output::jsonl::escape(inventory.source_dir) << "\",\n";
            file << "  \"include_tests\": " << (inventory.include_tests ? "true" : "false") << ",\n";
            file << "  \"include_examples\": " << (inventory.include_examples ? "true" : "false") << ",\n";
            file << "  \"units\": [\n";
            auto first = true;
            for(const auto& unit : inventory.units)
            {
                if(not first)
                    file << ",\n";
                first = false;
                file << "    {\n";
                file << "      \"unit\": \"" << output::jsonl::escape(unit.unit) << "\",\n";
                file << "      \"path\": \"" << output::jsonl::escape(unit.path) << "\",\n";
                if(not unit.module.empty())
                    file << "      \"module\": \"" << output::jsonl::escape(unit.module) << "\",\n";
                file << "      \"kind\": \"" << output::jsonl::escape(unit.kind) << "\",\n";
                file << "      \"imports\": [" << output::jsonl::join_json_strings(unit.imports) << "],\n";
                if(unit.level >= 0)
                    file << "      \"level\": " << unit.level << ",\n";
                file << "      \"has_main\": " << (unit.has_main ? "true" : "false") << ",\n";
                file << "      \"is_test\": " << (unit.is_test ? "true" : "false") << ",\n";
                file << "      \"is_modular\": " << (unit.is_modular ? "true" : "false") << "\n";
                file << "    }";
            }
            file << "\n  ],\n";
            file << "  \"units_total\": " << inventory.units.size() << ",\n";
            file << "  \"main_count\": " << inventory.main_count << ",\n";
            file << "  \"test_count\": " << inventory.test_count << ",\n";
            file << "  \"max_level\": " << inventory.max_level << "\n";
            file << "}\n";
        });
        output::notify(&output::observer::info,
                       "Wrote "s + path + " (" + std::to_string(inventory.units.size())
                           + " units)");
    }

    void list_sources() {
        scan_and_order();
        // Same as build: per-module -fmodule-file= flags are only known after the scan.
        update_module_flags();
        write_compile_commands();
        auto inventory = output::source_inventory{
            .config = std::string{config_name()},
            .include_tests = include_tests,
            .include_examples = include_examples,
            .source_dir = source_dir,
        };
        inventory.units.reserve(units_in_topological_order.size());
        for(const auto& tu : units_in_topological_order)
        {
            inventory.units.push_back(source_unit_of(tu));
            if(tu.has_main)
                ++inventory.main_count;
            if(tu.is_test)
                ++inventory.test_count;
            if(tu.dependency_level >= 0)
                inventory.max_level = std::max(inventory.max_level, tu.dependency_level);
        }
        write_graph_json(inventory);
        output::notify(&output::observer::source_list, inventory);
    }
};

} // namespace cb

namespace {

bool is_cb_token(std::string_view arg)
{
    return arg == "release" || arg == "debug" || arg == "ci" || arg == "clean"
        || arg == "build" || arg == "list" || arg == "test" || arg == "cache" || arg == "status" || arg == "invalidate" || arg == "static"
        || arg == "help" || arg == "-h" || arg == "--help"
        || arg == "--include-examples" || arg == "--build-tests" || arg == "--tests"
        || arg == "-I" || arg == "--include" || arg == "--link-flags"
        || arg == "--compile-flags" || arg == "--extra-compile-flags"
        || arg == "--jsonl" || arg.starts_with("--jsonl=")
        || arg.starts_with("--jobs=")
        || arg.starts_with("--modules=");
}

bool is_test_runner_token(std::string_view arg)
{
    return arg == "--list" || arg == "--jsonl" || arg == "--result" || arg == "--help"
        || arg.starts_with("--tags=")
        || arg.starts_with("--slowest=")
        || arg.starts_with("--jobs=")
        || arg.starts_with("--jsonl=")
        || arg.starts_with("--jsonl-output-max-bytes=")
        || arg.starts_with("--junit=")
        || arg.starts_with("--xunit-xml=");
}

} // namespace

using namespace std::string_literals;

int main(int argc, char* argv[])
{
    auto console_observer = cb::output::console::observer{std::cerr};
    auto jsonl_observer = cb::output::jsonl::observer{std::cout};
    cb::output::register_observer("console", console_observer);
    cb::output::register_observer("jsonl", jsonl_observer);

    // Emit JSONL eof (and any other observer teardown) on every exit path,
    // including returns and exceptions — without std::atexit.
    struct output_finish_guard
    {
        ~output_finish_guard() { cb::output::finish(); }
    };
    const auto finish_guard = output_finish_guard{};

    try
    {
        auto output_name = std::string_view{"console"};
        cb::output::select_observer(output_name);
        auto stdcppm = ""s;  // Empty string triggers auto-detection
        auto arg_index = 1;
        if (argc > 1) {
            auto candidate = fs::path{argv[1]};
            if (fs::exists(candidate)) {
                stdcppm = candidate.string();
                ++arg_index;
            } else if (candidate.extension() == ".cppm") {
                // Distinguish a mistyped std.cppm path from an unknown flag, which is
                // what the terminal else below would otherwise report it as.
                cb::output::notify(&cb::output::observer::error,
                    "std.cppm not found: "s + candidate.string());
                return 2;
            }
        }

        auto config = cb::build_system::build_config::debug;  // default to debug
        auto do_clean = false, do_list = false, do_build = false, do_run_tests = false;
        auto do_cache_status = false, do_cache_invalidate = false;
        auto clean_tests_only = false;
        auto test_filter = std::string{};
        auto test_runner_args = std::vector<std::string>{};
        auto static_linking = false;
        auto include_examples = false;
        auto build_tests = false;  // --build-tests flag: build tests but don't run them
        auto include_paths = std::vector<std::string>{};
        auto extra_compile_flags = cb::string_list{};
        auto extra_link_flags = cb::string_list{};
        auto max_jobs = 0; // 0 = derive from hardware_concurrency
        auto module_phases = cb::build_system::module_compilation::two_phase;
        // When the user passes --jobs=N, also forward it to test_runner. Default compile
        // parallelism must not flip the runner off its sequential default (jobs=1).
        auto jobs_explicit = false;

        // Returns nullopt if not a --jsonl form; false if the mode is unknown. Named, because
        // the two answers are different types and deduction takes only the first one it sees.
        const auto apply_jsonl_arg = [&](std::string_view argument) -> std::optional<bool>
        {
            if(argument == "--jsonl")
            {
                jsonl_observer.set_mode(cb::output::jsonl::jsonl_mode::failures);
                output_name = "jsonl";
                return true;
            }
            if(not argument.starts_with("--jsonl="))
                return std::nullopt;

            const auto mode = argument.substr(std::string_view{"--jsonl="}.size());
            if(mode == "summary")
                jsonl_observer.set_mode(cb::output::jsonl::jsonl_mode::summary);
            else if(mode == "failures")
                jsonl_observer.set_mode(cb::output::jsonl::jsonl_mode::failures);
            else if(mode == "trace")
                jsonl_observer.set_mode(cb::output::jsonl::jsonl_mode::trace);
            else
                return false;

            output_name = "jsonl";
            return true;
        };

        for (int i = arg_index; i < argc; ++i) {
            auto argument = std::string_view{argv[i]};
            if(const auto applied = apply_jsonl_arg(argument))
            {
                if(not *applied)
                {
                    cb::output::notify(&cb::output::observer::error,
                        "Unknown JSONL mode: "s + std::string{argument.substr(std::string_view{"--jsonl="}.size())});
                    return 1;
                }
                cb::output::select_observer(output_name);
                continue;
            }
            if (argument == "test") {
                do_run_tests = true;
                // Optional positional filter (substring); do not consume test_runner/CB flags.
                if (i + 1 < argc) {
                    const auto next = std::string_view{argv[i + 1]};
                    if (!is_test_runner_token(next) && !is_cb_token(next) && !next.starts_with("-"))
                        test_filter = argv[++i];
                }
            } else if (argument == "release") {
                config = cb::build_system::build_config::release;
            } else if (argument == "debug") {
                config = cb::build_system::build_config::debug;
            } else if (argument == "ci") {
                do_clean = true;
                do_run_tests = true;
            } else if (argument == "clean") {
                do_clean = true;
            } else if (argument == "build") {
                do_build = true;
            } else if (argument == "list") {
                do_list = true;
            } else if (argument == "cache") {
                if (i + 1 >= argc) {
                    cb::output::notify(&cb::output::observer::error, "Usage: cache status|invalidate");
                    return 1;
                }
                const auto cache_verb = std::string_view{argv[++i]};
                if (cache_verb == "status") {
                    do_cache_status = true;
                } else if (cache_verb == "invalidate") {
                    do_cache_invalidate = true;
                } else {
                    cb::output::notify(&cb::output::observer::error, "Usage: cache status|invalidate");
                    return 1;
                }
            } else if (argument == "static") {
                static_linking = true;
            } else if (argument == "--include-examples") {
                include_examples = true;
            } else if (argument == "--build-tests") {
                build_tests = true;
            } else if (argument == "--tests") {
                clean_tests_only = true;
            } else if (argument.starts_with("--jobs=")) {
                const auto text = argument.substr(std::string_view{"--jobs="}.size());
                auto value = 0;
                const auto [_, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (error != std::errc{} or value < 1) {
                    cb::output::notify(&cb::output::observer::error,
                        "--jobs expects a positive integer, got: "s + std::string{text});
                    return 2;
                }
                max_jobs = value;
                jobs_explicit = true;
            } else if (argument.starts_with("--modules=")) {
                const auto text = argument.substr(std::string_view{"--modules="}.size());
                if (text == "two-phase") {
                    module_phases = cb::build_system::module_compilation::two_phase;
                } else if (text == "one-phase") {
                    module_phases = cb::build_system::module_compilation::one_phase;
                } else {
                    cb::output::notify(&cb::output::observer::error,
                        "--modules expects one-phase or two-phase, got: "s + std::string{text});
                    return 2;
                }
            } else if (do_run_tests && is_test_runner_token(argument)) {
                // Forward recognized test_runner flags (e.g. --tags=, --list, --result).
                // (--jsonl is handled above; CB injects the mode into test_runner later if needed.)
                test_runner_args.emplace_back(argv[i]);
            } else if (argument == "-I" or argument == "--include") {
                if (i+1 < argc) {
                    include_paths.push_back(argv[++i]);
                } else {
                    cb::output::notify(&cb::output::observer::error, "Missing path after -I/--include");
                    return 1;
                }
            } else if (argument == "--link-flags") {
                if (i+1 < argc) {
                    extra_link_flags = cb::detail::parse_external_flag_text(argv[++i]);
                } else {
                    cb::output::notify(&cb::output::observer::error, "Missing flags after --link-flags");
                    return 1;
                }
            } else if (argument == "--compile-flags" or argument == "--extra-compile-flags") {
                if (i+1 < argc) {
                    extra_compile_flags = cb::detail::parse_external_flag_text(argv[++i]);
                } else {
                    cb::output::notify(&cb::output::observer::error, "Missing flags after --compile-flags");
                    return 1;
                }
            } else if (argument.starts_with("--compile-flags=") or argument.starts_with("--extra-compile-flags=")) {
                const auto eq = argument.find('=');
                extra_compile_flags = cb::detail::parse_external_flag_text(argument.substr(eq + 1));
            } else if (argument == "help" or argument == "-h" or argument == "--help") {
                std::cout << "Usage: " << argv[0] << " [std.cppm] [options]\n\n"
                          << "Options:\n"
                          << "  release          Build in release mode (optimized, no tests)\n"
                          << "  debug            Build in debug mode (with debug symbols, includes tests)\n"
                          << "  build            Build the project (default if no action specified)\n"
                          << "  clean            Remove build directories\n"
                          << "  clean --tests    Remove only test objects and test_runner (keep app/lib)\n"
                          << "  ci               Clean and run tests (shortcut for: clean test)\n"
                          << "  list             List all translation units; write compile_commands.json and graph.json\n"
                          << "  cache status     Inspect object-cache profile and entry counts\n"
                          << "  cache invalidate Remove object/link cache indexes (lighter than clean)\n"
                          << "  test [filter]  Build and run tests (optional substring filter)\n"
                          << "                 Forward test_runner flags directly (e.g. --tags=, --list, --result)\n"
                          << "  static           Enable static linking (C++ stdlib static)\n"
                          << "  --include-examples Include examples directory in build (excluded by default)\n"
                          << "  --build-tests    Build tests in release mode (useful for CI to verify compilation)\n"
                          << "  --jsonl[=<summary|failures|trace>]  Machine-readable output (default: failures)\n"
                          << "  --junit=<path>   Also write a JUnit/xUnit XML report (additive with --jsonl)\n"
                          << "  --jobs=N         Cap concurrent compile/link; also forward to test_runner\n"
                          << "                   (compile default: CPU count; test_runner default: 1)\n"
                          << "  --modules=<two-phase|one-phase>  How modular units compile (default: two-phase)\n"
                          << "                   two-phase: --precompile to .pcm, then .pcm to .o\n"
                          << "                   one-phase: one -c -fmodule-output= step for both\n"
                          << "  -I, --include    Add include directory (can be specified multiple times)\n"
                          << "  --link-flags     Add extra linker flags (e.g., --link-flags \"-lcrypto\")\n"
                          << "  --compile-flags  Add extra compiler flags\n"
                          << "  help, -h, --help Show this help message\n\n"
                          << "Examples:\n"
                          << "  " << argv[0] << " debug build\n"
                          << "  " << argv[0] << " release build\n"
                          << "  " << argv[0] << " release build --build-tests\n"
                          << "  " << argv[0] << " -I include/path debug build\n"
                          << "  " << argv[0] << " -I path1 -I path2 debug build\n"
                          << "  " << argv[0] << " clean build\n"
                          << "  " << argv[0] << " clean --tests\n"
                          << "  " << argv[0] << " ci\n"
                          << "  " << argv[0] << " test\n"
                          << "  " << argv[0] << " test --tags=[module]\n"
                          << "  " << argv[0] << " test --jsonl=failures --tags=[module]\n"
                          << "  " << argv[0] << " test --jsonl=failures --junit=report.xml --tags=[module]\n"
                          << "  " << argv[0] << " debug build --jsonl=summary\n"
                          << "  " << argv[0] << " test --jsonl=trace --slowest=10\n"
                          << "  " << argv[0] << " clean\n";
                return 0;
            } else {
                // Terminal else: CB used to fall through here silently, so a typo like
                // --tag= (for --tags=) ran the full suite and reported success. A build
                // tool must not quietly ignore what it was asked to do.
                cb::output::notify(&cb::output::observer::error,
                    "Unknown argument: "s + std::string{argument}
                    + (argument.starts_with("--tag") and not argument.starts_with("--tags=")
                        ? " (did you mean --tags=<filter>?)"
                        : "")
                    + "\nRun with --help for usage.");
                return 2;
            }
        }

        if(not cb::output::select_observer(output_name))
        {
            cb::output::notify(&cb::output::observer::error, "Unknown output observer: "s + std::string{output_name});
            return 1;
        }

        auto include_flags =
            include_paths
            | std::views::transform([](const auto& path)
            {
                return std::array{"-I"s, path};
            })
            | std::views::join
            | std::ranges::to<cb::string_list>();

        auto build_system = cb::build_system{config, include_flags, {}, ".", stdcppm, static_linking, include_examples, extra_compile_flags, extra_link_flags};
        build_system.set_max_jobs(max_jobs);
        build_system.set_module_phases(module_phases);

        if (do_list) build_system.list_sources();
        if (do_cache_status) {
            build_system.cache_status();
            return 0;
        }
        if (do_cache_invalidate) {
            build_system.cache_invalidate();
            return 0;
        }
        if(clean_tests_only and not do_clean)
        {
            cb::output::notify(&cb::output::observer::error, "--tests requires clean");
            return 2;
        }
        if(do_clean)
        {
            if(clean_tests_only)
                build_system.clean_tests();
            else
                build_system.clean();
        }
        if (do_build) {
            if (build_tests) {
                // --build-tests: build tests but don't run them (useful for CI)
                build_system.set_include_tests(true);
            }
            build_system.build();
        }
        if (do_run_tests) {
            // Run tests with filter + optional extra args for test_runner.
            // We pass both as argv-like tokens to avoid shell injection and to preserve spaces.
            auto args = std::vector<std::string>{};
            if (!test_filter.empty())
                args.emplace_back(test_filter);

            const auto has_jsonl_mode = std::ranges::any_of(test_runner_args, [](const auto& arg) {
                return arg == "--jsonl" || arg.starts_with("--jsonl=");
            });
            if(output_name == "jsonl" && not has_jsonl_mode)
            {
                const auto mode = jsonl_observer.mode();
                args.emplace_back(mode == cb::output::jsonl::jsonl_mode::summary
                    ? "--jsonl=summary"
                    : mode == cb::output::jsonl::jsonl_mode::trace
                        ? "--jsonl=trace"
                        : "--jsonl=failures");
            }

            // CB consumes --jobs= for the build; inject the same cap into test_runner only
            // when the user set it, so an unset flag leaves the runner sequential.
            if(jobs_explicit)
            {
                const auto already = std::ranges::any_of(test_runner_args, [](const auto& arg) {
                    return arg.starts_with("--jobs=");
                });
                if(not already)
                    args.emplace_back("--jobs=" + std::to_string(max_jobs));
            }

            args.append_range(test_runner_args);

            // Build include_tests etc inside run_tests(), but pass args as tokens.
            if(not build_system.run_tests(args))
                return 1;
        }
        if (not do_clean and not do_list and not do_run_tests and not do_build
            and not do_cache_status and not do_cache_invalidate)
            build_system.build();

        return 0;
    }
    catch (const std::exception& e)
    {
        cb::output::notify(&cb::output::observer::error, "Fatal error: "s + e.what());
        return 1;
    }
    catch (...)
    {
        cb::output::notify(&cb::output::observer::error, "Fatal error: unknown exception");
        return 1;
    }
}
