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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <random>
#include <type_traits>
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

// Source extensions CB will scan and compile. Detection selects the longest matching suffix,
// so prefixed forms (`.test.`, `.impl.`) do not depend on this list's order.
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

constexpr auto build_root_prefix = "build-"sv;
constexpr auto compile_commands_filename = "compile_commands.json"sv;
constexpr auto graph_filename = "graph.json"sv;
constexpr auto std_module_name = "std"sv;
constexpr auto test_runner_name = "test_runner"sv;

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

// True when `dir` is a complete component anywhere in path, including the path's leaf.
// path_under_dir intentionally excludes a bare/leaf directory because its original callers
// pass file paths; the scanner also sees directories and needs to prune at the directory itself.
bool path_at_or_under_dir(std::string_view path, std::string_view dir)
{
    if(path == dir or path_under_dir(path, dir))
        return true;
    return path.size() > dir.size()
        and path.ends_with(dir)
        and path[path.size() - dir.size() - 1] == '/';
}

// Component-aware containment for normalized absolute roots. A raw starts_with(root) would
// mistake siblings such as /work/app-copy for children of /work/app.
bool path_at_or_under_root(std::string_view path, std::string_view root)
{
    if(path == root)
        return true;
    if(root == "/"sv)
        return path.starts_with(root);
    return path.starts_with(root)
        and path.size() > root.size()
        and path[root.size()] == '/';
}

// Existing paths become canonical so cache keys and dependency comparisons resolve aliases.
// Missing tails resolve against their existing prefix; filesystem failures retain a normalized
// absolute spelling so the analyzer can still report the requested path.
std::string canonical_path(fs::path path)
{
    if(path.is_relative())
        path = fs::absolute(path);
    auto error = std::error_code{};
    const auto canonical = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal().string() : canonical.string();
}

// A cache (or other stamped index) is replaced, never edited in place: write a sibling
// temporary, then rename it over the target, so a build interrupted mid-write leaves the
// previous file rather than half of the next. Callers differ only in what they write and
// what the failure message calls the file.
void write_atomic_file(const std::string& path,
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

// Whether there was a file to remove, which is what cache_invalidate reports per cache.
bool remove_if_exists(const std::string& path)
{
    return fs::remove(path);
}

} // namespace detail

namespace flags {

// Converts between CB's flag-token representation and the whitespace-normalized text used at
// CLI and cache-profile boundaries. This is intentionally not POSIX shell parsing.
class codec
{
public:
    static string_list parse(std::string_view text)
    {
        const auto normalized = collapse_whitespace(text);
        return normalized
            | std::views::split(' ')
            | std::views::transform([](auto&& part) { return std::string_view{part}; })
            | std::views::filter([](std::string_view token) { return not token.empty(); })
            | std::views::transform([](std::string_view token) { return std::string{token}; })
            | std::ranges::to<string_list>();
    }

    static std::string serialize(const string_list& flags)
    {
        return flags | std::views::join_with(" "sv) | std::ranges::to<std::string>();
    }

private:
    // Collapse any isspace run to a single space; trim leading/trailing whitespace.
    static std::string collapse_whitespace(std::string_view text)
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
};

} // namespace flags

namespace toolchain {

// Views refer to driver-selected, program-lifetime literals; BMI suffixes are compiler-specific.
struct artifact_conventions
{
    std::string_view object_extension;
    std::string_view bmi_extension;
    std::string_view executable_extension;
};

constexpr std::string_view host_os()
{
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

constexpr artifact_conventions clang_artifacts()
{
#if defined(_WIN32)
    return {.object_extension = ".obj",
            .bmi_extension = ".pcm",
            .executable_extension = ".exe"};
#else
    return {.object_extension = ".o",
            .bmi_extension = ".pcm",
            .executable_extension = ""};
#endif
}

} // namespace toolchain

namespace source {
class scanner;

class translation_unit {
public:
    static bool is_supported(const fs::path& file_path);
    std::string_view kind_name() const;

    friend class scanner;

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

    static std::optional<std::string_view> supported_suffix(std::string_view filename);
    static std::string normalize_relative_dir(const fs::path& dir);
    static std::string make_display_path(std::string_view dir, std::string_view filename);
    static std::string make_unit(std::string_view module_value, unit_kind kind, std::string_view filename_value);
    static bool is_tester_framework_path(std::string_view path);
    static bool path_has_test_segment(std::string_view path);
    static bool determine_is_test(std::string_view rel_dir, std::string_view name, std::string_view suffix_value);

    // Module names may contain '.' (e.g. demo.core); partitions use ':' (demo.core:part).
    inline static const std::regex module_regex{R"(\s*(?:export\s+)?module\s+([\w.:-]+)\s*;)"};
    inline static const std::regex export_module_regex{R"(\s*export\s+module\s+([\w.:-]+)\s*;)"};
    inline static const std::regex fragment_regex{R"(\s*module\s*;)"};  // Global module fragment: just "module;"
    inline static const std::regex import_regex{R"(\s*(?:export\s+)?(?:import|module)\s+([\w.:-]+)\s*;)"};
    inline static const std::regex main_regex{R"(\s*int\s+main\s*\()"};
    inline static const std::regex keyword_regex{R"(\b(class|struct|namespace|constexpr|inline|static)\b)"};
    inline static const std::regex using_namespace_regex{R"(\busing\s+namespace\b)"};
};

using translation_unit_list = std::vector<translation_unit>;
using unit_index =
    std::flat_map<std::string, const translation_unit*, std::less<>>;

std::optional<std::string_view> translation_unit::supported_suffix(std::string_view filename)
{
    auto matches = supported_suffixes
        | std::views::filter([&](std::string_view suffix) {
              return filename.ends_with(suffix);
          });
    const auto longest = std::ranges::max_element(
        matches,
        {},
        [](std::string_view suffix) { return suffix.size(); });
    if(longest == matches.end())
        return std::nullopt;
    return *longest;
}

bool translation_unit::is_supported(const fs::path& file_path) {
    return supported_suffix(file_path.filename().string()).has_value();
}

std::string_view translation_unit::kind_name() const
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

std::string translation_unit::normalize_relative_dir(const fs::path& dir)
{
    if(dir.empty())
        return "";
    auto str = dir.string();
    return str == "." ? "" : str;
}

std::string translation_unit::make_display_path(std::string_view dir, std::string_view filename)
{
    return dir.empty() ? std::string{filename} : std::string{dir} + "/" + std::string{filename};
}

std::string translation_unit::make_unit(std::string_view module_value, unit_kind kind, std::string_view filename_value)
{
    switch(kind)
    {
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

bool translation_unit::is_tester_framework_path(std::string_view path)
{
    // Nested or top-level tester library trees (not *.test.c++ sources).
    // Uses path_under_dir (not path_at_or_under_dir): a bare leaf `tester` is not a framework path.
    return detail::path_under_dir(path, tester_dir_name);
}

bool translation_unit::path_has_test_segment(std::string_view path)
{
    // Match path components named exactly "test" or "tests" (not "tester" / "test_exception_bug").
    auto rest = path;
    while(not rest.empty())
    {
        const auto slash = rest.find('/');
        const auto segment = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        if(segment == test_dir_name or segment == tests_dir_name)
            return true;
        if(slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    return false;
}

bool translation_unit::determine_is_test(std::string_view rel_dir, std::string_view name, std::string_view suffix_value)
{
    const auto combined = rel_dir.empty() ? std::string{name} : std::string{rel_dir} + "/" + std::string{name};
    if(is_tester_framework_path(combined))
        return false;
    if(suffix_value == test_cxx_suffix or suffix_value == test_cxxm_suffix)
        return true;
    return path_has_test_segment(combined);
}

translation_unit::translation_unit(const fs::path& relative,
                                          const fs::path& full_path,
                                          std::string module_value,
                                          string_list imports_value,
                                          unit_kind kind_value,
                                          bool has_main_flag)
    : filename(relative.filename().string()),
      path(normalize_relative_dir(relative.parent_path())),
      suffix([&] {
          const auto matched = supported_suffix(this->filename);
          if(not matched)
              throw std::runtime_error{"unsupported source suffix"};
          return std::string{*matched};
      }()),
      base_name(this->filename.substr(0, this->filename.size() - this->suffix.size())),
      full_path(detail::canonical_path(full_path)),
      display_path(make_display_path(this->path, this->filename)),
      unit(make_unit(module_value, kind_value, this->filename)),
      module(std::move(module_value)),
      imports(std::move(imports_value)),
      kind(kind_value),
      has_main(has_main_flag),
      is_test(determine_is_test(this->path, this->filename, this->suffix)),
      is_modular(kind_value == unit_kind::interface_unit or kind_value == unit_kind::partition_unit),
      last_modified(fs::last_write_time(full_path)) {}

class scanner
{
public:
    scanner(std::string source_root, bool include_tests, bool include_examples)
        : source_root_{std::move(source_root)},
          include_tests_{include_tests},
          include_examples_{include_examples}
    {}

    translation_unit_list scan() const
    {
        return order(collect());
    }

    // Unit keys this consumer waits on: explicit imports plus an implementation unit's primary
    // interface. Unknown imports remain a compiler diagnostic rather than a scanner failure.
    template<typename KnownUnits, typename Visit>
    requires std::invocable<Visit&, const std::string&>
    static void for_each_provider(const translation_unit& tu,
                                  const KnownUnits& known,
                                  Visit&& visit)
    {
        auto seen = std::flat_set<std::string, std::less<>>{};
        const auto consider = [&](const std::string& key)
        {
            if(not known.contains(key) or not seen.insert(key).second)
                return;
            visit(key);
        };

        for(const auto& imported : tu.imports)
            consider(imported);
        if(tu.kind == unit_kind::implementation_unit)
            consider(tu.module);
    }

private:
    using dependency_graph = std::flat_map<std::string, string_list, std::less<>>;
    using indegree_map = std::flat_map<std::string, int, std::less<>>;
    using unit_map = std::flat_map<std::string, translation_unit*, std::less<>>;
    using ready_queue = std::queue<std::string>;

    // True for `dir` or `dir/...`.
    static bool is_dir_or_under(std::string_view path, std::string_view dir)
    {
        return path == dir
            or (path.starts_with(dir) and path.size() > dir.size() and path[dir.size()] == '/');
    }

    // Parent repos vendor packages under deps/<name>/. Nested checkouts such as
    // deps/net/deps/tester belong to the child package and must not join the parent
    // build — otherwise same-basename smoke fixtures collide across packages.
    static bool is_nested_dependency_path(std::string_view rel_path)
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
    static bool is_build_output_path(std::string_view rel_path)
    {
        const auto first = rel_path.substr(0, rel_path.find('/'));
        return first.starts_with(build_root_prefix);
    }

    // First-level deps/tester/tests/... (and tester/tests/...) smoke fixtures must not join the
    // consumer scan: determine_is_test calls every tester/ path a library source, so fixture mains
    // such as hello.c++ would compile into parent builds and collide on the duplicate-key check.
    static bool is_tester_package_tests_path(std::string_view rel_path)
    {
        return translation_unit::is_tester_framework_path(rel_path)
            and translation_unit::path_has_test_segment(rel_path);
    }

    // deps/<pkg>/test and deps/<pkg>/tests are the vendored package's own suites (benchmarks,
    // package-local tests), not the consumer's, so they stay out of the parent build even now that
    // project test/ trees are in the scan (#14). Co-located deps/<pkg>/*.test.c++ still joins.
    static bool is_dependency_package_tests_path(std::string_view rel_path)
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

    bool is_excluded_source_path(std::string_view rel_path) const
    {
        return is_nested_dependency_path(rel_path)
            or is_dependency_package_tests_path(rel_path)
            or is_tester_package_tests_path(rel_path)
            or is_build_output_path(rel_path)
            or detail::path_at_or_under_dir(rel_path, tools_dir_name)
            or detail::path_at_or_under_dir(rel_path, git_dir_name)
            or (not include_examples_
                and detail::path_at_or_under_dir(rel_path, examples_dir_name));
    }

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

    static std::string splice_physical_lines(const std::string& text)
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

    static std::string strip_comments_and_literals(const std::string& text)
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

    translation_unit_list collect() const
    {
        auto units = translation_unit_list{};
        try
        {
            const auto root = fs::path{source_root_};
            if(not fs::exists(root) or not fs::is_directory(root))
                return units;

            auto entries = fs::recursive_directory_iterator{root};
            const auto end = fs::recursive_directory_iterator{};
            for(; entries != end; ++entries)
            {
                const auto& entry = *entries;
                const auto rel_path = entry.path().lexically_relative(root).string();

                // Prune excluded trees at their directory entry instead of descending through
                // every file and discarding it. Project test/ trees are deliberately absent:
                // translation_unit marks those after parsing and include_tests decides whether
                // they join.
                if(entry.is_directory())
                {
                    if(is_excluded_source_path(rel_path))
                        entries.disable_recursion_pending();
                    continue;
                }
                if(not entry.is_regular_file()
                   or is_excluded_source_path(rel_path)
                   or not translation_unit::is_supported(entry.path()))
                    continue;

                try
                {
                    auto tu = parse(root, entry.path());
                    if(tu.is_test and not include_tests_)
                        continue;
                    units.push_back(std::move(tu));
                }
                catch(const std::exception& error)
                {
                    output::notify(&output::observer::warning,
                                   "Skipping "s + entry.path().string() + ": " + error.what());
                }
            }
        }
        catch(const std::exception& error)
        {
            throw std::runtime_error{"Failed to scan project: "s + error.what()};
        }
        catch(...)
        {
            throw std::runtime_error{"Failed to scan project: unknown error"};
        }
        return units;
    }

    static translation_unit_list order(translation_unit_list units)
    {
        if(units.empty())
            return units;

        auto dependencies = dependency_graph{};
        auto indegrees = indegree_map{};
        auto unit_to_tu = unit_map{};

        for(auto& tu : units)
        {
            if(unit_to_tu.contains(tu.unit))
            {
                const auto& prior = *unit_to_tu.at(tu.unit);
                throw std::runtime_error{
                    "Duplicate translation unit key '" + tu.unit + "' from "
                    + prior.display_path + " and " + tu.display_path
                    + " (object/module names must stay unique)"};
            }
            unit_to_tu[tu.unit] = &tu;
            indegrees[tu.unit] = 0;
        }

        for(const auto& tu : units)
        {
            for_each_provider(tu, unit_to_tu, [&](const std::string& provider)
            {
                dependencies[provider].push_back(tu.unit);
                ++indegrees[tu.unit];
            });
        }

        auto ready = ready_queue{};
        for(const auto& [unit, degree] : indegrees)
            if(degree == 0)
                ready.push(unit);

        auto sorted = translation_unit_list{};
        auto level = 0;
        while(not ready.empty())
        {
            const auto batch_size = ready.size();
            for(auto index = std::size_t{0}; index < batch_size; ++index)
            {
                const auto unit = ready.front();
                ready.pop();

                auto* tu = unit_to_tu.at(unit);
                tu->dependency_level = level;
                sorted.push_back(*tu);

                for(const auto& dependent_unit : dependencies[unit])
                    if(--indegrees[dependent_unit] == 0)
                        ready.push(dependent_unit);
            }
            ++level;
        }

        auto cyclic_units = string_list{};
        for(const auto& [unit, degree] : indegrees)
            if(degree > 0)
                cyclic_units.push_back(unit);

        if(not cyclic_units.empty())
        {
            auto message = "Cyclic dependency detected between units:"s;
            for(const auto& unit : cyclic_units)
                message += " " + unit;
            throw std::runtime_error{message};
        }
        return sorted;
    }

    static translation_unit parse(const fs::path& project_root, const fs::path& file_path)
    {
        auto relative_path = file_path.lexically_relative(project_root);
        if(relative_path.empty() or relative_path == ".")
            relative_path = file_path.filename();

        auto file = std::ifstream{file_path};
        if(not file)
            throw std::runtime_error{"cannot open file"};

        auto line = std::string{};
        auto module_name = std::string{};
        auto imports = string_list{};
        auto kind = unit_kind::non_module;
        auto has_main = false;
        auto lines_scanned = 0;
        constexpr auto max_lines = 1000;

        const auto trim = [](std::string_view text)
        {
            const auto start = text.find_first_not_of(" \t\r");
            if(start == std::string::npos)
                return ""sv;
            const auto end = text.find_last_not_of(" \t\r");
            return text.substr(start, end - start + 1);
        };

        auto raw = std::string{};
        while(lines_scanned++ < max_lines and std::getline(file, line))
        {
            raw += line;
            raw += '\n';
        }
        const auto cleaned = strip_comments_and_literals(splice_physical_lines(raw));
        auto conditionals = conditional_filter{};
        auto seen_real_code = false;

        for(const auto part : std::views::split(cleaned, '\n'))
        {
            const auto code_line = std::string{std::string_view{part}};
            if(conditionals.is_inactive(code_line))
                continue;
            const auto trimmed = trim(code_line);
            if(trimmed.empty())
                continue;

            if(std::regex_search(code_line, translation_unit::main_regex))
                has_main = true;
            if(seen_real_code)
                continue;

            auto match = std::smatch{};
            if(std::regex_search(code_line, match, translation_unit::fragment_regex))
            {
                if(kind == unit_kind::non_module)
                    kind = unit_kind::global_fragment;
            }
            else if(std::regex_search(code_line, match, translation_unit::export_module_regex)
                    and match.size() > 1)
            {
                module_name = match[1].str();
                kind = module_name.contains(':')
                    ? unit_kind::partition_unit
                    : unit_kind::interface_unit;
            }
            else if(std::regex_search(code_line, match, translation_unit::module_regex)
                    and match.size() > 1)
            {
                const auto module = match[1].str();
                if(kind == unit_kind::non_module or kind == unit_kind::global_fragment)
                {
                    module_name = module;
                    kind = unit_kind::implementation_unit;
                }
            }
            else if(std::regex_search(code_line, match, translation_unit::import_regex)
                    and match.size() > 1)
            {
                auto imported = match[1].str();
                if(not imported.empty() and imported[0] == ':' and not module_name.empty())
                {
                    const auto colon = module_name.find(':');
                    const auto base = colon != std::string::npos
                        ? module_name.substr(0, colon)
                        : module_name;
                    imported = base + imported;
                }
                if(not imported.empty() and imported != std_module_name)
                    imports.push_back(std::move(imported));
            }

            // End the preamble only after recording any module/import on this line, and never
            // inside a global module fragment: it may contain declarations before export module.
            if(kind != unit_kind::global_fragment)
            {
                const auto code = std::string{trimmed};
                if(trimmed.contains('{')
                   or std::regex_search(code, translation_unit::keyword_regex)
                   or std::regex_search(code, translation_unit::using_namespace_regex))
                    seen_real_code = true;
            }
        }

        if((kind == unit_kind::interface_unit or kind == unit_kind::partition_unit)
           and module_name.empty())
            throw std::runtime_error{"module interface/partition missing module name"};
        if(kind == unit_kind::implementation_unit and module_name.empty())
            throw std::runtime_error{"implementation unit missing module name"};

        return translation_unit{
            relative_path,
            file_path,
            std::move(module_name),
            std::move(imports),
            kind,
            has_main};
    }

    std::string source_root_;
    bool include_tests_ = false;
    bool include_examples_ = false;
};

} // namespace source

namespace layout {

class paths
{
public:
    paths(std::string_view configuration, toolchain::artifact_conventions naming)
        : root{std::string{build_root_prefix} + std::string{toolchain::host_os()} + '-'
               + std::string{configuration}},
          pcm{root / "pcm"},
          obj{root / "obj"},
          bin{root / "bin"},
          cache{root / "cache"},
          naming_{naming}
    {}

    std::string std_pcm() const
    {
        return (pcm / suffixed(std_module_name, naming_.bmi_extension)).string();
    }

    std::string std_object() const
    {
        return (obj / suffixed(std_module_name, naming_.object_extension)).string();
    }

    std::string object(const source::translation_unit& tu) const
    {
        const auto stem = tu.is_modular ? module_safe_name(tu.module) : tu.base_name;
        return (obj / (stem + object_suffix(tu))).string();
    }

    std::string pcm_file(const source::translation_unit& tu) const
    {
        if(tu.module.empty())
            throw std::logic_error{
                "pcm_file called on translation unit without module: " + tu.filename};
        return (pcm / suffixed(module_safe_name(tu.module), naming_.bmi_extension)).string();
    }

    std::string executable(const source::translation_unit& tu) const
    {
        if(not tu.has_main)
            throw std::logic_error{
                "executable called on non-main translation unit: " + tu.filename};
        return executable(tu.base_name);
    }

    std::string executable(std::string_view stem) const
    {
        return (bin / suffixed(stem, naming_.executable_extension)).string();
    }

    static std::string depfile(std::string_view object)
    {
        return suffixed(object, depfile_suffix);
    }

    static std::string compile_log(std::string_view artifact)
    {
        return suffixed(artifact, compile_log_suffix);
    }

    static std::string link_log(std::string_view executable)
    {
        return suffixed(executable, link_log_suffix);
    }

    fs::path root;
    fs::path pcm;
    fs::path obj;
    fs::path bin;
    fs::path cache;

private:
    inline static constexpr auto depfile_suffix = ".d"sv;
    inline static constexpr auto compile_log_suffix = ".log"sv;
    inline static constexpr auto link_log_suffix = ".link.log"sv;

    static std::string suffixed(std::string_view stem, std::string_view suffix)
    {
        auto result = std::string{stem};
        result += suffix;
        return result;
    }

    static std::string module_safe_name(std::string_view module_name)
    {
        auto safe = std::string{module_name};
        std::ranges::replace(safe, ':', '-');
        std::ranges::replace(safe, '.', '-');
        return safe;
    }

    std::string object_suffix(const source::translation_unit& tu) const
    {
        const auto ending = std::ranges::find_if(
            object_stem_suffixes,
            [&](std::string_view suffix) { return tu.suffix.ends_with(suffix); });
        if(ending == object_stem_suffixes.end())
            throw std::logic_error{"Unsupported suffix for object file: " + tu.suffix};
        return suffixed(tu.suffix.substr(0, tu.suffix.size() - ending->size()), naming_.object_extension);
    }

    toolchain::artifact_conventions naming_;
};

} // namespace layout

namespace output {

// Observers format four of a unit's fields, so they receive those four and not the unit: the
// rest is build state, and cb-observer.h++ stays independent of the scanner. The pcm path is the
// one derived field, and deriving it here is why compile_start and compile_end cannot disagree.
compile_unit compile_unit_of(const source::translation_unit& tu)
{
    return {.source = tu.full_path,
            .object = tu.object_path,
            .pcm = tu.is_modular ? std::string_view{tu.pcm_path} : std::string_view{},
            .module = tu.module,
            .display_path = tu.display_path};
}

source_unit source_unit_of(const source::translation_unit& tu)
{
    return {.unit = tu.unit,
            .path = tu.display_path,
            .module = tu.module,
            .kind = std::string{tu.kind_name()},
            .imports = tu.imports,
            .level = tu.dependency_level,
            .has_main = tu.has_main,
            .is_test = tu.is_test,
            .is_modular = tu.is_modular};
}

} // namespace output

namespace cache {

class analyzer;

class storage_file
{
public:
    storage_file(std::string cache_dir, std::string_view filename)
        : path_{detail::join_dir(cache_dir, filename)}
    {}

    explicit storage_file(std::string path)
        : path_{std::move(path)}
    {}

    const std::string& path() const { return path_; }
    bool exists() const { return fs::exists(path_); }
    bool invalidate() const { return detail::remove_if_exists(path_); }

    std::string read_first_line() const
    {
        auto file = std::ifstream{path_};
        if(not file)
            return {};

        auto line = ""s;
        if(not std::getline(file, line))
            return {};

        if(not line.empty() and line.back() == '\r')
            line.pop_back();
        return line;
    }

    void replace(std::string_view what,
                 const std::invocable<std::ostream&> auto& writer) const
    {
        fs::create_directories(fs::path{path_}.parent_path());
        detail::write_atomic_file(path_, what, writer);
    }

private:
    std::string path_;
};

// Tab-delimited key=value fields; values cannot contain '\t', '\n', '\r', or '%'.
class profile
{
public:
    static constexpr std::string_view format_id = "cb-object-cache-v4";

    struct ingredients
    {
        std::string_view config;
        bool static_link = false;
        std::string_view module_phases;
        std::string_view toolchain_root;
        std::string_view compiler;
        std::string_view compiler_signature;
        std::string_view compiler_version;
        std::string_view std_module;
        const string_list& compile_flags;
        const string_list* cpp_flags = nullptr; // null → shared-std profile (omit cpp)
    };

    explicit profile(ingredients facts)
    {
        text_.reserve(768);
        append("format", format_id);
        append("config", facts.config);
        append("static_link", facts.static_link ? "1" : "0");
        append("module_phases", facts.module_phases);
        append("toolchain_root", facts.toolchain_root);
        append("compiler", facts.compiler);
        append("compiler_signature", facts.compiler_signature);
        if(not facts.compiler_version.empty())
            append("compiler_version", facts.compiler_version);
        append("std_module", facts.std_module);
        append("compile", cb::flags::codec::serialize(facts.compile_flags));
        if(facts.cpp_flags != nullptr)
            append("cpp", cb::flags::codec::serialize(*facts.cpp_flags));
    }

    explicit profile(std::string text)
        : text_{std::move(text)}
    {}

    const std::string& text() const & { return text_; }
    std::string text() && { return std::move(text_); }

    // Stable across CB processes, with the full profile stored beside the artefacts to verify
    // the key before reuse. The length makes accidental collisions across differently sized
    // profiles still less likely; a collision never bypasses the profile comparison.
    std::string key() const
    {
        constexpr auto offset = std::uint64_t{14695981039346656037ULL};
        constexpr auto prime = std::uint64_t{1099511628211ULL};
        const auto hash = std::ranges::fold_left(text_, offset, [=](std::uint64_t value, char byte)
        {
            return (value ^ static_cast<unsigned char>(byte)) * prime;
        });
        auto digits = std::array<char, 16>{};
        const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), hash, 16);
        return std::to_string(text_.size()) + '-' + std::string{digits.data(), converted.ptr};
    }

    output::object_cache_profile_diff diff(const profile& newer) const
    {
        const auto old_fields = fields();
        const auto new_fields = newer.fields();
        auto result = output::object_cache_profile_diff{};

        // Named, because `return {}` has nothing to deduce from.
        const auto field_value = [](const field_map& map, std::string_view key) -> std::string {
            if(map.contains(key))
                return map.at(key);
            return {};
        };

        const auto diff_scalar = [&](std::string_view key, std::optional<output::profile_scalar_change>& out) {
            const auto old_value = field_value(old_fields, key);
            const auto new_value = field_value(new_fields, key);
            if(old_value != new_value)
                out = output::profile_scalar_change{old_value, new_value};
        };

        output::for_each_profile_scalar(result, diff_scalar);

        const auto diff_tokens = [&](std::string_view key, std::optional<output::profile_token_change>& out) {
            auto change = token_change(field_value(old_fields, key), field_value(new_fields, key));
            if(change.changed())
                out = std::move(change);
        };

        output::for_each_profile_tokens(result, diff_tokens);
        return result;
    }

    bool operator==(const profile&) const = default;

private:
    using field_map = std::flat_map<std::string, std::string, std::less<>>;

    void append(std::string_view key, std::string_view value)
    {
        if(not text_.empty())
            text_ += '\t';
        text_ += key;
        text_ += '=';
        text_ += value;
    }

    static std::pair<std::string, std::string> parse_field(std::string_view segment)
    {
        const auto eq = segment.find('=');
        return {
            std::string{segment.substr(0, eq)},
            std::string{segment.substr(eq + 1)}};
    }

    field_map fields() const
    {
        return text_
            | std::views::split('\t')
            | std::views::transform([](auto&& part) { return parse_field(std::string_view{part}); })
            | std::ranges::to<field_map>();
    }

    static output::profile_token_change token_change(std::string_view old_text, std::string_view new_text)
    {
        auto old_tokens = cb::flags::codec::parse(old_text);
        auto new_tokens = cb::flags::codec::parse(new_text);
        std::ranges::sort(old_tokens);
        std::ranges::sort(new_tokens);

        auto change = output::profile_token_change{};
        std::ranges::set_difference(new_tokens, old_tokens, std::back_inserter(change.added));
        std::ranges::set_difference(old_tokens, new_tokens, std::back_inserter(change.removed));
        return change;
    }

    std::string text_;
};

// Analyzers read entries_ without locking; call record() only after workers join.
class object_store
{
private:
    using map = std::flat_map<std::string, fs::file_time_type, std::less<>>;

    struct profile_header
    {
        bool line_present = false;
        std::optional<std::string> value{};
    };

public:
    struct disk_status
    {
        bool exists = false;
        bool profile_match = false;
        int entries = 0;
        int stale_entries = 0;
    };

    explicit object_store(std::string cache_dir)
        : file_{std::move(cache_dir), filename}
    {}

    const std::string& path() const { return file_.path(); }
    bool exists() const { return file_.exists(); }
    bool missing_profile_header() const { return missing_profile_header_; }
    const std::optional<output::object_cache_profile_diff>& profile_change() const
    {
        return profile_change_;
    }

    void load(std::string current_profile)
    {
        current_profile_ = std::move(current_profile);
        entries_.clear();
        profile_change_.reset();
        missing_profile_header_ = false;

        auto file = std::ifstream{file_.path()};
        if(not file)
            return;

        const auto header = read_profile_header(file);
        if(not header.line_present)
            return;
        if(not header.value)
        {
            missing_profile_header_ = true;
            return;
        }
        if(*header.value != current_profile_)
        {
            profile_change_ =
                profile{*header.value}.diff(profile{current_profile_});
            return;
        }

        auto line = ""s;
        while(std::getline(file, line))
        {
            auto entry_path = ""s;
            auto ticks = 0ll;
            if(parse_entry(line, entry_path, ticks) and fs::exists(entry_path))
                entries_[entry_path] = fs::file_time_type{std::chrono::nanoseconds{ticks}};
        }
    }

    void save() const
    {
        file_.replace("object cache", [&](std::ostream& file) {
            file << profile_header_prefix << current_profile_ << "\n";
            for(const auto& [entry_path, timestamp] : entries_)
            {
                if(not fs::exists(entry_path))
                    continue;
                const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    timestamp.time_since_epoch()).count();
                file << entry_path << "\t" << ticks << "\n";
            }
        });
    }

    void record(std::string source_path, fs::file_time_type mtime)
    {
        entries_[std::move(source_path)] = mtime;
    }

    bool erase(std::string_view source_path)
    {
        return entries_.erase(std::string{source_path}) != 0;
    }

    bool invalidate() const { return file_.invalidate(); }

    disk_status status(std::string_view current_profile) const
    {
        auto result = disk_status{.exists = file_.exists()};
        if(not result.exists)
            return result;

        auto file = std::ifstream{file_.path()};
        const auto header = read_profile_header(file);
        if(header.value)
        {
            result.profile_match = *header.value == current_profile;
            count_entries(file, result.entries, result.stale_entries);
        }
        return result;
    }

private:
    friend class analyzer;

    inline static constexpr auto filename = "object-cache.txt"sv;
    inline static constexpr auto profile_header_prefix = "profile\t"sv;

    const map& entries() const { return entries_; }

    static profile_header read_profile_header(std::istream& file)
    {
        auto line = ""s;
        if(not std::getline(file, line))
            return {};
        if(not line.starts_with(profile_header_prefix))
            return {.line_present = true};
        return {.line_present = true,
                .value = line.substr(profile_header_prefix.size())};
    }

    static bool parse_entry(const std::string& line, std::string& path, long long& ticks)
    {
        if(line.empty() or line.starts_with(profile_header_prefix))
            return false;
        const auto tab = line.find('\t');
        if(tab == std::string::npos)
            return false;
        path = line.substr(0, tab);
        try
        {
            ticks = std::stoll(line.substr(tab + 1));
        }
        catch(...)
        {
            return false;
        }
        return not path.empty();
    }

    static void count_entries(std::istream& file, int& entries, int& stale_entries)
    {
        auto line = ""s;
        while(std::getline(file, line))
        {
            auto entry_path = ""s;
            auto ticks = 0ll;
            if(not parse_entry(line, entry_path, ticks))
                continue;
            ++entries;
            if(not fs::exists(entry_path))
                ++stale_entries;
        }
    }

    storage_file file_;
    std::string current_profile_;
    map entries_{};
    std::optional<output::object_cache_profile_diff> profile_change_{};
    bool missing_profile_header_ = false;
};

// Executable signatures and their common compile/link/module flag tail.
class link_store
{
private:
    using map = std::flat_map<std::string, std::string, std::less<>>;

public:
    struct flag_ingredients
    {
        const string_list& compile_flags;
        const string_list& link_flags;
        const string_list& module_flags;
    };

    struct disk_status
    {
        bool exists = false;
        int entries = 0;
    };

    link_store(std::string cache_dir, flag_ingredients flags)
        : file_{std::move(cache_dir), filename}
    {
        signature_flag_tail_ = "|flags=";
        signature_flag_tail_ += cb::flags::codec::serialize(flags.compile_flags);
        signature_flag_tail_ += "|link=";
        signature_flag_tail_ += cb::flags::codec::serialize(flags.link_flags);
        signature_flag_tail_ += "|modules=";
        signature_flag_tail_ += cb::flags::codec::serialize(flags.module_flags);
    }

    const std::string& path() const { return file_.path(); }
    bool exists() const { return file_.exists(); }

    void load()
    {
        entries_.clear();
        auto file = std::ifstream{file_.path()};
        if(not file)
            return;
        auto entry_path = ""s;
        auto signature = ""s;
        while(std::getline(file, entry_path, '\t') and std::getline(file, signature))
            entries_[entry_path] = signature;
    }

    void save() const
    {
        if(entries_.empty())
        {
            file_.invalidate();
            return;
        }
        file_.replace("executable cache", [&](std::ostream& file) {
            for(const auto& [entry_path, signature] : entries_)
                file << entry_path << "\t" << signature << "\n";
        });
    }

    // Parallel link workers: lock, then record the signature that made the link succeed.
    void remember(std::string executable, std::string signature)
    {
        auto lock = std::lock_guard<std::mutex>{mutex_};
        entries_[std::move(executable)] = std::move(signature);
    }

    bool erase(std::string_view executable)
    {
        return entries_.erase(std::string{executable}) != 0;
    }

    bool invalidate() const { return file_.invalidate(); }

    disk_status status() const
    {
        auto result = disk_status{.exists = file_.exists()};
        if(not result.exists)
            return result;

        auto file = std::ifstream{file_.path()};
        auto line = ""s;
        while(std::getline(file, line))
        {
            const auto tab = line.find('\t');
            if(tab != std::string::npos and not line.substr(0, tab).empty())
                ++result.entries;
        }
        return result;
    }

private:
    static std::string dependency_signature(const std::string& input_path)
    {
        if(input_path.empty() or not fs::exists(input_path))
            return input_path + ":missing";
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            fs::last_write_time(input_path).time_since_epoch()).count();
        return input_path + ":" + std::to_string(timestamp);
    }

    std::string dependency_signatures_joined(const string_list& paths) const
    {
        return paths
            | std::views::transform([&](const std::string& input_path) {
                  return dependency_signature(input_path);
              })
            | std::views::join_with("|"sv)
            | std::ranges::to<std::string>();
    }

public:
    // What identifies a link: every input's timestamp, plus the flag sets that would change
    // the result even when no input moved. Callers differ only in the input list.
    std::string link_signature(const string_list& input_paths,
                               const string_list& import_flags) const
    {
        auto signature = dependency_signatures_joined(input_paths);
        signature += signature_flag_tail_;
        signature += "|imports=";
        signature += cb::flags::codec::serialize(import_flags);
        signature += "|format=";
        signature += signature_format;
        return signature;
    }

    // Reads entries_ only — call only before parallel remember(), or on a single-threaded path.
    std::optional<output::rebuild_info> needs_relinking(std::string_view executable_path,
                                                        const std::string& signature) const
    {
        if(not fs::exists(executable_path))
            return output::rebuild_info{.kind = output::rebuild_kind::missing_executable};

        if(not entries_.contains(executable_path))
            return output::rebuild_info{.kind = output::rebuild_kind::not_in_cache};

        const auto& previous = entries_.at(executable_path);
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

private:
    inline static constexpr auto filename = "executable-cache.txt"sv;
    // v2 records the import flags actually passed to every link, including test-unit imports.
    inline static constexpr auto signature_format = "cb-link-v2"sv;

    storage_file file_;
    std::string signature_flag_tail_{};
    map entries_{};
    std::mutex mutex_{};
};

// Local std-module profile and machine-wide artifact sharing.
class standard_module_store
{
private:
    struct shared_slot
    {
        fs::path directory;
        fs::path pcm;
        fs::path object;
        storage_file profile;
    };

public:
    struct disk_status
    {
        bool exists = false;
        bool profile_match = false;
    };

    // ok reports a completed transfer; warning is set only for caught I/O exceptions.
    struct transfer_result
    {
        bool ok = false;
        std::string warning{};
    };

    standard_module_store(std::string cache_dir,
                          std::string pcm_path,
                          std::string object_path)
        : profile_file_{std::move(cache_dir), profile_filename},
          pcm_path_{std::move(pcm_path)},
          object_path_{std::move(object_path)}
    {}

    const std::string& profile_path() const { return profile_file_.path(); }

private:
    // Whether std.pcm was precompiled by the profile a project unit is compiled with. The
    // pcm's own presence is a separate question, asked by rebuild_reason_for.
    bool profile_matches(std::string_view object_profile) const
    {
        const auto stored = profile_file_.read_first_line();
        return not stored.empty() and stored == object_profile;
    }

public:
    void save_profile(std::string_view object_profile) const
    {
        profile_file_.replace("std module profile", [&](std::ostream& file) {
            file << object_profile << '\n';
        });
    }

    bool invalidate() const { return profile_file_.invalidate(); }

    disk_status status(std::string_view object_profile) const
    {
        return {.exists = profile_file_.exists(),
                .profile_match = profile_matches(object_profile)};
    }

    std::optional<output::rebuild_info> rebuild_reason_for(
        std::string_view source_path,
        std::invocable auto&& object_profile_of) const
    {
        const auto module_of = [&](output::rebuild_kind kind) {
            return output::rebuild_info{
                .kind = kind,
                .module = std::string{std_module_name},
                .pcm_path = pcm_path_};
        };

        if(not fs::exists(pcm_path_))
            return module_of(output::rebuild_kind::own_pcm_missing);
        if(not profile_matches(
               std::forward<decltype(object_profile_of)>(object_profile_of)()))
            return module_of(output::rebuild_kind::profile_change);
        if(fs::last_write_time(pcm_path_) < fs::last_write_time(source_path))
            return module_of(output::rebuild_kind::own_pcm_stale);
        if(not fs::exists(object_path_))
            return module_of(output::rebuild_kind::object_missing);
        if(fs::last_write_time(object_path_) < fs::last_write_time(pcm_path_))
            return module_of(output::rebuild_kind::object_stale);
        return std::nullopt;
    }

private:
    // An explicitly empty CB_STD_CACHE_DIR disables sharing, which keeps benchmarks and
    // isolated cache-contract tests able to request a truly cold std build.
    static std::optional<fs::path> shared_root()
    {
        if(const auto* configured = std::getenv("CB_STD_CACHE_DIR"))
        {
            if(*configured == '\0')
                return std::nullopt;
            return fs::path{configured};
        }
        if(const auto* xdg = std::getenv("XDG_CACHE_HOME"); xdg and *xdg)
            return fs::path{xdg} / "cb" / "std-module";
        if(const auto* home = std::getenv("HOME"); home and *home)
            return fs::path{home} / ".cache" / "cb" / "std-module";
        return std::nullopt;
    }

    shared_slot shared_slot_for(const fs::path& root,
                                std::string_view shared_profile) const
    {
        const auto directory = root / profile{std::string{shared_profile}}.key();
        return shared_slot{
            .directory = directory,
            .pcm = directory / fs::path{pcm_path_}.filename(),
            .object = directory / fs::path{object_path_}.filename(),
            .profile = storage_file{(directory / shared_profile_filename).string()}};
    }

    bool materialize(const shared_slot& slot,
                     std::string_view shared_profile) const
    {
        if(slot.profile.read_first_line() != shared_profile)
            return false;
        auto error = std::error_code{};
        if(not fs::is_regular_file(slot.pcm, error) or error)
            return false;
        error.clear();
        if(not fs::is_regular_file(slot.object, error) or error)
            return false;

        fs::create_directories(fs::path{pcm_path_}.parent_path());
        fs::create_directories(fs::path{object_path_}.parent_path());
        const auto nonce = shared_nonce();
        const auto temporary_pcm = fs::path{pcm_path_ + ".shared-" + nonce};
        const auto temporary_object = fs::path{object_path_ + ".shared-" + nonce};
        if(not link_or_copy_file(slot.pcm, temporary_pcm)
           or not link_or_copy_file(slot.object, temporary_object))
        {
            fs::remove(temporary_pcm, error);
            fs::remove(temporary_object, error);
            return false;
        }

        fs::remove(pcm_path_, error);
        error.clear();
        fs::rename(temporary_pcm, pcm_path_, error);
        if(error)
        {
            fs::remove(temporary_pcm, error);
            fs::remove(temporary_object, error);
            return false;
        }
        fs::remove(object_path_, error);
        error.clear();
        fs::rename(temporary_object, object_path_, error);
        if(error)
        {
            fs::remove(pcm_path_, error);
            fs::remove(temporary_object, error);
            return false;
        }
        return true;
    }

public:
    transfer_result hydrate_for(const output::rebuild_info& reason,
                                std::invocable auto&& shared_profile_of,
                                std::invocable auto&& object_profile_of) const
    {
        if(reason.kind != output::rebuild_kind::own_pcm_missing)
            return {};
        const auto root = shared_root();
        if(not root)
            return {};
        const auto shared_profile =
            std::forward<decltype(shared_profile_of)>(shared_profile_of)();
        try
        {
            const auto slot = shared_slot_for(*root, shared_profile);
            if(not materialize(slot, shared_profile))
                return {};
            save_profile(std::forward<decltype(object_profile_of)>(object_profile_of)());
            return {.ok = true};
        }
        catch(const std::exception& error)
        {
            return {.warning = "Shared std module cache read failed: "s + error.what()};
        }
    }

    transfer_result publish(std::invocable auto&& shared_profile_of) const
    {
        const auto root = shared_root();
        if(not root)
            return {};
        const auto shared_profile =
            std::forward<decltype(shared_profile_of)>(shared_profile_of)();
        try
        {
            const auto slot = shared_slot_for(*root, shared_profile);
            if(slot.profile.read_first_line() == shared_profile
               and fs::is_regular_file(slot.pcm)
               and fs::is_regular_file(slot.object))
                return {.ok = true};

            fs::create_directories(slot.directory.parent_path());
            auto staging = slot.directory;
            staging += ".tmp-" + shared_nonce();
            auto error = std::error_code{};
            if(not fs::create_directories(staging, error) or error)
                return {};

            const auto staged_pcm = staging / slot.pcm.filename();
            const auto staged_object = staging / slot.object.filename();
            const auto staged_profile = staging / shared_profile_filename;
            if(not link_or_copy_file(pcm_path_, staged_pcm)
               or not link_or_copy_file(object_path_, staged_object))
            {
                fs::remove_all(staging, error);
                return {};
            }
            {
                auto profile_file = std::ofstream{staged_profile, std::ios::trunc};
                if(not profile_file)
                {
                    fs::remove_all(staging, error);
                    return {};
                }
                profile_file << shared_profile << '\n';
                if(not profile_file)
                {
                    profile_file.close();
                    fs::remove_all(staging, error);
                    return {};
                }
            }

            // Publish the complete directory at once. If another CB won the same key, retain
            // its equivalent slot and discard this staging tree.
            fs::rename(staging, slot.directory, error);
            if(error)
            {
                fs::remove_all(staging, error);
                return {};
            }
            return {.ok = true};
        }
        catch(const std::exception& error)
        {
            return {.warning = "Shared std module cache write failed: "s + error.what()};
        }
    }

private:
    inline static constexpr auto profile_filename = "std-module-profile.txt"sv;
    inline static constexpr auto shared_profile_filename = "profile.txt"sv;

    static bool link_or_copy_file(const fs::path& source, const fs::path& destination)
    {
        auto error = std::error_code{};
        fs::create_hard_link(source, destination, error);
        if(not error)
            return true;
        error.clear();
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
        return not error;
    }

    static std::string shared_nonce()
    {
        auto value = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        try
        {
            auto entropy = std::random_device{};
            value ^= static_cast<std::uint64_t>(entropy()) << 32;
            value ^= static_cast<std::uint64_t>(entropy());
        }
        catch(...)
        {
            // The monotonic-clock value still makes a collision between practical publishers
            // vanishingly unlikely; create_directories below refuses an existing staging path.
        }
        return std::to_string(value);
    }

    storage_file profile_file_;
    std::string pcm_path_;
    std::string object_path_;
};

class compiler_stamp
{
public:
    explicit compiler_stamp(std::string cache_dir)
        : file_{std::move(cache_dir), filename}
    {}

    const std::string& path() const { return file_.path(); }
    bool exists() const { return file_.exists(); }
    std::string read() const { return file_.read_first_line(); }
    bool invalidate() const { return file_.invalidate(); }

private:
    inline static constexpr auto filename = "compiler-version.txt"sv;
    storage_file file_;
};

// Returns the first reason an object/BMI set cannot be reused.
class analyzer
{
public:
    analyzer(std::string source_root, std::string pcm_root)
        : source_root_{detail::canonical_path(source_root)},
          pcm_root_{detail::canonical_path(pcm_root)}
    {}

    std::optional<output::rebuild_info> rebuild_reason_for(
        const source::translation_unit& tu,
        const object_store& loaded,
        const source::unit_index& units) const
    {
        if(not loaded.entries().contains(tu.full_path))
        {
            if(loaded.profile_change())
                return output::rebuild_info{
                    .kind = output::rebuild_kind::profile_change,
                    .trigger_path = tu.full_path};
            return output::rebuild_info{
                .kind = output::rebuild_kind::not_in_cache,
                .trigger_path = tu.full_path};
        }
        if(loaded.entries().at(tu.full_path) < tu.last_modified)
            return output::rebuild_info{
                .kind = output::rebuild_kind::source_stale,
                .trigger_path = tu.full_path};

        // A modular object-only repair reuses its BMI, so validate all BMI inputs before
        // returning object_missing/object_stale. Non-modular units can decide immediately.
        const auto object_absent = not fs::exists(tu.object_path);
        auto object_timestamp = fs::file_time_type{};
        if(object_absent)
        {
            if(not tu.is_modular)
                return output::rebuild_info{
                    .kind = output::rebuild_kind::object_missing,
                    .trigger_path = tu.full_path};
        }
        else
        {
            object_timestamp = fs::last_write_time(tu.object_path);
            if(not tu.is_modular)
            {
                if(object_timestamp < loaded.entries().at(tu.full_path))
                    return output::rebuild_info{
                        .kind = output::rebuild_kind::object_stale,
                        .trigger_path = tu.full_path};
                if(auto header_reason = stale_header(tu, object_timestamp))
                    return header_reason;
            }
        }

        // Implementation units consume their primary interface even without an explicit import.
        if(tu.kind == unit_kind::implementation_unit and units.contains(tu.module))
        {
            const auto& interface = *units.at(tu.module);
            if(not fs::exists(interface.pcm_path))
                return pcm_rebuild(output::rebuild_kind::dependency_pcm_stale, interface);
            if(fs::last_write_time(interface.pcm_path) > object_timestamp)
                return pcm_rebuild(output::rebuild_kind::pcm_stale, interface);
            if(auto interface_reason = rebuild_reason_for(interface, loaded, units))
                return attributed_to(*interface_reason, interface);
        }

        auto freshness_timestamp = object_timestamp;
        auto object_stale_vs_pcm = false;
        if(tu.is_modular)
        {
            if(not fs::exists(tu.pcm_path))
                return pcm_rebuild(output::rebuild_kind::own_pcm_missing, tu);
            const auto pcm_timestamp = fs::last_write_time(tu.pcm_path);
            if(pcm_timestamp < tu.last_modified)
                return pcm_rebuild(output::rebuild_kind::own_pcm_stale, tu);
            if(object_absent)
                freshness_timestamp = pcm_timestamp;
            else if(object_timestamp < pcm_timestamp)
            {
                freshness_timestamp = pcm_timestamp;
                object_stale_vs_pcm = true;
            }

            if(auto header_reason = stale_header(tu, freshness_timestamp))
                return header_reason;
        }

        auto visited = std::flat_set<std::string>{};
        if(auto stale = transitive_pcm_newer_than_object(
               tu, freshness_timestamp, units, visited))
            return stale;

        for(const auto& dependency_key : tu.imports)
        {
            if(not units.contains(dependency_key))
                continue;

            const auto& dependency = *units.at(dependency_key);
            if(dependency.is_modular
               and (not fs::exists(dependency.pcm_path)
                    or fs::last_write_time(dependency.pcm_path) < dependency.last_modified))
                return pcm_rebuild(output::rebuild_kind::dependency_pcm_stale, dependency);
            if(auto dependency_reason = rebuild_reason_for(dependency, loaded, units))
                return attributed_to(*dependency_reason, dependency);
        }

        if(object_absent)
            return output::rebuild_info{
                .kind = output::rebuild_kind::object_missing,
                .trigger_path = tu.full_path};
        if(object_stale_vs_pcm)
            return pcm_rebuild(output::rebuild_kind::object_stale, tu);
        if(object_timestamp < loaded.entries().at(tu.full_path))
            return output::rebuild_info{
                .kind = output::rebuild_kind::object_stale,
                .trigger_path = tu.full_path};
        return std::nullopt;
    }

private:
    // Make-style depfile (clang -MMD -MF): `target: prereq prereq \<newline> prereq`, spaces in a
    // path backslash-escaped. Returns the prerequisites only. `nullopt` when the file cannot be
    // trusted — unreadable, or lacking the `target:` every depfile has — which is not the same
    // answer as an empty list: conflating them turns an unreadable depfile into a cache hit.
    static std::optional<string_list> parse_depfile(const std::string& path)
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

    static output::rebuild_info pcm_rebuild(output::rebuild_kind kind,
                                            const source::translation_unit& tu)
    {
        return {.kind = kind,
                .module = tu.module,
                .pcm_path = tu.pcm_path,
                .trigger_path = tu.full_path};
    }

    static output::rebuild_info attributed_to(output::rebuild_info reason,
                                              const source::translation_unit& tu)
    {
        if(reason.trigger_path.empty())
            reason.trigger_path = tu.full_path;
        if(reason.module.empty())
            reason.module = tu.module;
        return reason;
    }

    std::optional<output::rebuild_info> transitive_pcm_newer_than_object(
        const source::translation_unit& tu,
        fs::file_time_type object_timestamp,
        const source::unit_index& units,
        std::flat_set<std::string>& visited) const
    {
        for(const auto& dependency_key : tu.imports)
        {
            if(not units.contains(dependency_key))
                continue;

            const auto& dependency = *units.at(dependency_key);
            if(dependency.is_modular and fs::exists(dependency.pcm_path)
               and fs::last_write_time(dependency.pcm_path) > object_timestamp)
                return pcm_rebuild(output::rebuild_kind::pcm_stale, dependency);

            if(visited.contains(dependency.unit))
                continue;
            visited.insert(dependency.unit);
            if(auto stale = transitive_pcm_newer_than_object(
                   dependency, object_timestamp, units, visited))
                return stale;
        }
        return std::nullopt;
    }

    std::optional<output::rebuild_info> stale_header(
        const source::translation_unit& tu,
        fs::file_time_type freshness_timestamp) const
    {
        const auto depfile = layout::paths::depfile(tu.object_path);
        const auto prerequisites = parse_depfile(depfile);
        if(not prerequisites)
            return output::rebuild_info{
                .kind = output::rebuild_kind::depfile_unusable,
                .trigger_path = depfile};

        for(const auto& prerequisite : *prerequisites)
        {
            const auto resolved = detail::canonical_path(prerequisite);
            // Explicitly mapped BMIs are module-graph inputs, not textual headers.
            if(resolved == tu.full_path
               or not detail::path_at_or_under_root(resolved, source_root_)
               or detail::path_at_or_under_root(resolved, pcm_root_))
                continue;

            auto error = std::error_code{};
            const auto timestamp = fs::last_write_time(resolved, error);
            if(error)
                return output::rebuild_info{
                    .kind = output::rebuild_kind::header_missing,
                    .trigger_path = resolved};
            if(timestamp > freshness_timestamp)
                return output::rebuild_info{
                    .kind = output::rebuild_kind::header_stale,
                    .trigger_path = resolved};
        }
        return std::nullopt;
    }

    std::string source_root_;
    std::string pcm_root_;
};

} // namespace cache

namespace execution {

// Worker bodies own exception handling; this helper always joins every thread.
template<std::copy_constructible Work>
requires std::invocable<Work>
void run_workers(std::size_t worker_count, Work work)
{
    auto workers = std::vector<std::jthread>{};
    workers.reserve(worker_count);
    for(auto worker = std::size_t{0}; worker < worker_count; ++worker)
        workers.emplace_back(work);
    for(auto& worker : workers)
        worker.join();
}

// Bounded independent jobs; the first failure stops claims and is rethrown after all joins.
class worker_pool
{
public:
    explicit worker_pool(std::ptrdiff_t job_limit) : limit{job_limit} {}

    template <std::ranges::input_range Jobs, typename Work>
    void run(Jobs&& jobs, Work work) const
    {
        using job_reference = std::ranges::range_reference_t<Jobs>;
        static_assert(std::is_lvalue_reference_v<job_reference>);
        using job_type = std::remove_cvref_t<job_reference>;

        // Jobs outlive workers; pointers avoid copying rebuild decisions.
        auto items = std::vector<const job_type*>{};
        for(const auto& job : jobs)
            items.push_back(std::addressof(job));
        if(items.empty())
            return;

        const auto worker_count = std::min(
            items.size(),
            static_cast<std::size_t>(std::max<std::ptrdiff_t>(1, limit)));
        auto next = std::atomic_size_t{0};
        auto failed = std::atomic_bool{false};
        auto failure = std::exception_ptr{};
        auto failure_mutex = std::mutex{};

        run_workers(worker_count, [&]()
        {
            while(not failed.load(std::memory_order_relaxed))
            {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if(index >= items.size())
                    return;
                // A peer may have failed while this worker claimed the next index.
                if(failed.load(std::memory_order_relaxed))
                    return;
                try
                {
                    work(*items[index]);
                }
                catch(...)
                {
                    failed.store(true, std::memory_order_relaxed);
                    auto lock = std::lock_guard<std::mutex>{failure_mutex};
                    if(not failure)
                        failure = std::current_exception();
                }
            }
        });
        if(failure)
            std::rethrow_exception(failure);
    }

private:
    std::ptrdiff_t limit;
};

} // namespace execution

namespace output {

// Keep only the diagnostic head in JSONL; the full capture remains on disk.
inline constexpr auto diagnostics_head_limit = std::size_t{8192};

// A failing step replaces prior warnings so its error gets the full diagnostic budget.
class diagnostic_buffer
{
public:
    void append(diagnostics more)
    {
        if(more.empty())
            return;
        if(value_.empty())
        {
            value_ = std::move(more);
            return;
        }

        if(not more.head.empty())
        {
            if(not value_.head.empty())
                value_.head.push_back('\n');
            value_.head += more.head;
            if(value_.head.size() > diagnostics_head_limit)
            {
                value_.head.resize(diagnostics_head_limit);
                value_.truncated = true;
            }
        }
        value_.bytes += more.bytes;
        value_.truncated = value_.truncated or more.truncated;
        if(value_.path.empty())
            value_.path = std::move(more.path);
    }

    void replace(diagnostics value) { value_ = std::move(value); }
    const diagnostics& value() const { return value_; }

private:
    diagnostics value_{};
};

class step_state
{
public:
    explicit step_state(const rebuild_info& reason)
        : rebuild{reason}
    {}

    void mark_cache_hit()
    {
        hit = true;
        ok = true;
    }

    void succeeded() { ok = true; }
    void attach(diagnostics said) { diag.append(std::move(said)); }
    void failed(diagnostics said)
    {
        ok = false;
        diag.replace(std::move(said));
    }

    step_result result() const
    {
        const auto finished = hit ? started : std::chrono::steady_clock::now();
        return {.ok = ok,
                .cache_hit = hit,
                .timing = {started, finished},
                .rebuild = rebuild,
                .diag = diag.value()};
    }

private:
    rebuild_info rebuild;
    diagnostic_buffer diag{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool ok = false;
    bool hit = false;
};

// Pairs one build_start with one build_end.
class build_scope
{
public:
    build_scope(std::string_view config, bool include_tests, bool include_examples)
    {
        notify(&observer::build_start, config, include_tests, include_examples);
    }

    build_scope(const build_scope&) = delete;
    build_scope& operator=(const build_scope&) = delete;

    ~build_scope() { report(false); }

    void succeeded() { report(true); }

private:
    void report(bool ok)
    {
        if(std::exchange(reported, true))
            return;
        notify(&observer::build_end, ok,
               interval{started, std::chrono::steady_clock::now()});
    }

    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool reported = false;
};

// Pairs compile events and accumulates diagnostics across multi-step module compilation.
class compile_scope
{
public:
    compile_scope(const compile_unit& compiled, const rebuild_info& reason)
        : unit{compiled}, state{reason}
    {
        notify(&observer::compile_start, unit, reason);
    }

    // Cache hits have no rebuild reason or command duration.
    explicit compile_scope(const compile_unit& compiled)
        : compile_scope{compiled, rebuild_info{}}
    {
        state.mark_cache_hit();
    }

    compile_scope(const compile_scope&) = delete;
    compile_scope& operator=(const compile_scope&) = delete;

    ~compile_scope()
    {
        notify(&observer::compile_end, unit, state.result());
    }

    void succeeded() { state.succeeded(); }
    void attach(diagnostics said) { state.attach(std::move(said)); }
    void failed(diagnostics said) { state.failed(std::move(said)); }

private:
    compile_unit unit;
    step_state state;
};

// Emits exactly one link_end; the event contract has no link_start.
class link_scope
{
public:
    link_scope(std::string_view executable, const rebuild_info& reason, std::string sig)
        : executable_path{executable}, signature{std::move(sig)}, state{reason}
    {}

    // Cache hits have no rebuild reason or command duration.
    link_scope(std::string_view executable, std::string sig)
        : link_scope{executable, rebuild_info{}, std::move(sig)}
    {
        state.mark_cache_hit();
    }

    link_scope(const link_scope&) = delete;
    link_scope& operator=(const link_scope&) = delete;

    ~link_scope()
    {
        auto result = state.result();
        result.signature = signature;
        notify(&observer::link_end, executable_path, result);
    }

    void succeeded() { state.succeeded(); }
    void attach(diagnostics said) { state.attach(std::move(said)); }
    void failed(diagnostics said) { state.failed(std::move(said)); }

private:
    std::string executable_path;
    std::string signature{};
    step_state state;
};

template <typename Scope>
concept step_scope = requires(Scope& scope, diagnostics said) {
    scope.attach(std::move(said));
    scope.failed(std::move(said));
};

// Pairs one test_start with one test_end.
class test_scope
{
public:
    explicit test_scope(std::string_view runner)
    {
        notify(&observer::test_start, runner);
    }

    test_scope(const test_scope&) = delete;
    test_scope& operator=(const test_scope&) = delete;

    // An unreported run retains the default failed result.
    ~test_scope()
    {
        notify(&observer::test_end, result,
               interval{started, std::chrono::steady_clock::now()});
        if(reported and not result.ok())
            notify(&observer::error, test_failure_message(result));
    }

    // Test failure is a normal process outcome, not an exception.
    void finished(process_result outcome)
    {
        result = std::move(outcome);
        reported = true;
    }

private:
    process_result result{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool reported = false;
};

} // namespace output

namespace process {

// Thread-safe; CB's sole subprocess boundary.
class runner
{
public:
    output::process_result invoke_shell(
        const string_list& argv,
        std::string_view capture_path = {}) const
    {
        if(argv.empty())
            throw std::logic_error{"invoke_shell: empty argv"};

        auto cmd_str = join_argv(argv);
        auto shell_line = cmd_str;
        if(not capture_path.empty())
            shell_line += " > " + shell_quote(capture_path) + " 2>&1";

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

        auto result = output::process_result{.status = decode_wait_status(raw)};
        if(not capture_path.empty())
        {
            auto diag = read_diagnostics(capture_path);
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

    bool command_available(std::string_view candidate,
                           std::string_view capture_path) const
    {
        // `command` is a shell builtin, so invoke an explicit shell with the candidate quoted.
        return invoke_shell(
            string_list{"/bin/sh", "-c", "command -v " + shell_quote(candidate)},
            capture_path).ok();
    }

    // Every toolchain command is a step of a reported build phase: its output is attached to
    // the reporting scope before a failure is thrown.
    void run_step(
        output::step_scope auto& scope,
        const string_list& argv,
        std::string_view capture) const
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

private:
    static std::string shell_quote(std::string_view arg)
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

    static std::string join_argv(const string_list& argv)
    {
        return argv
            | std::views::transform([](const std::string& arg) { return shell_quote(arg); })
            | std::views::join_with(" "sv)
            | std::ranges::to<std::string>();
    }

    // waitpid yields a wait status; decode once so command_end / test_end report the child's
    // own exit_code (or signal) instead of the packed wait value (256 for exit 1).
    static output::process_status decode_wait_status(int status)
    {
        if(status < 0) // spawn or waitpid itself failed
            return {.exit_code = status, .wait_status = status};
        if(WIFSIGNALED(status))
            return {.exit_code = -1,
                    .wait_status = status,
                    .signaled = true,
                    .signal = WTERMSIG(status)};
        if(WIFEXITED(status))
            return {.exit_code = WEXITSTATUS(status), .wait_status = status};
        return {.exit_code = -1, .wait_status = status};
    }

    static output::diagnostics read_diagnostics(std::string_view path)
    {
        auto file = std::ifstream{std::string{path}, std::ios::binary};
        if(not file)
            return {};

        auto text =
            std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        auto diag = output::diagnostics{.path = std::string{path}, .bytes = text.size()};
        if(text.size() > output::diagnostics_head_limit)
        {
            text.resize(output::diagnostics_head_limit);
            diag.truncated = true;
        }
        diag.head = std::move(text);
        return diag;
    }

    static std::string command_failure_message(
        const string_list& argv,
        const output::process_result& result)
    {
        auto message = "Command failed: " + join_argv(argv);
        if(result.status.signaled)
            message += " (killed by signal " + std::to_string(result.status.signal) + ')';
        else
            message += " (exit " + std::to_string(result.status.exit_code) + ')';
        if(not result.diag.head.empty())
            message += '\n' + result.diag.head;
        return message;
    }
};

} // namespace process

class build_system;

namespace toolchain {

// How a modular interface becomes a .pcm and a .o. two_phase runs `--precompile` and then
// compiles the BMI; one_phase asks for both artefacts from one `-c -fmodule-output=` read.
// Two-phase can publish the BMI while its object still compiles; one-phase saves a second parse.
enum class module_compilation : unsigned char { two_phase, one_phase };

enum class linkage : unsigned char { dynamic, static_ };

using module_link_flags =
    std::flat_map<std::string, std::string, std::less<>>;

struct module_file
{
    std::string name;
    std::string bmi_path;
};

using module_file_list = std::vector<module_file>;

struct compile_request
{
    std::string source;
    std::string object;
    std::string bmi;
    std::string depfile;
    module_file_list modules{};
    bool modular = false;
};

enum class compile_output : unsigned char { bmi, object };

struct compile_step
{
    string_list argv;
    compile_output output = compile_output::object;
    bool writes_bmi = false;
    bool dependencies_ready = false;
};

using compile_plan = std::vector<compile_step>;

class clang_driver
{
public:
    struct identity_view
    {
        const std::string& toolchain_root;
        const std::string& compiler;
        const std::string& compiler_signature;
        const std::string& compiler_version;
        const std::string& std_module_source;
        const std::string& std_module_profile;
    };

    struct flag_view
    {
        const string_list& compile;
        const string_list& link;
        const string_list& cpp;
        const string_list& modules;
        const string_list& extra_compile;
        const string_list& extra_link;
    };

    struct settings
    {
        bool release = false;
        linkage link = linkage::dynamic;
        module_compilation module_phases = module_compilation::two_phase;
        std::string std_module_source{};
        string_list include_paths{};
        string_list extra_compile_flags{};
        string_list extra_link_flags{};
    };

private:
    friend class ::cb::build_system;

    template <typename CommandAvailable>
    requires std::invocable<CommandAvailable&, const std::string&>
    clang_driver(settings values,
                 std::string_view std_pcm_path,
                 std::string_view module_cache_dir,
                 CommandAvailable command_available)
        : release_{values.release},
          linkage_{values.link},
          module_phases_{values.module_phases},
          std_module_source_{std::move(values.std_module_source)},
          extra_compile_flags_{std::move(values.extra_compile_flags)},
          extra_link_flags_{std::move(values.extra_link_flags)}
    {
        cpp_flags_ = values.include_paths
            | std::views::transform([](const auto& path)
              {
                  return std::array{"-I"s, path};
              })
            | std::views::join
            | std::ranges::to<string_list>();
        discover(command_available);
        initialize_flags(std_pcm_path, module_cache_dir);
    }

public:
    std::string_view module_phases_name() const
    {
        switch(module_phases_)
        {
            case module_compilation::two_phase: return "two-phase";
            case module_compilation::one_phase: return "one-phase";
        }
        std::unreachable();
    }
    bool static_linking() const { return linkage_ == linkage::static_; }

    identity_view identity() const
    {
        return {
            llvm_prefix_,
            compiler_,
            compiler_signature_,
            compiler_version_,
            std_module_source_,
            std_module_profile_,
        };
    }

    flag_view flags() const
    {
        return {
            compile_flags_,
            link_flags_,
            cpp_flags_,
            module_flags_,
            extra_compile_flags_,
            extra_link_flags_,
        };
    }

    string_list warnings() const
    {
        if(static_linking() and host_os() == "darwin")
            return {"Static linking on macOS is limited – libc++ remains dynamically linked"};
        return {};
    }

    string_list version_argv() const { return {compiler_, "--version"}; }
    void set_compiler_version(std::string value) const
    {
        compiler_version_ = std::move(value);
    }

    compile_plan compile(const compile_request& request, bool object_only) const
    {
        if(not request.modular)
        {
            return {{.argv = compile_source_object_argv(request),
                     .dependencies_ready = true}};
        }
        if(module_phases_ == module_compilation::one_phase)
        {
            return {{.argv = compile_module_object_argv(request),
                     .writes_bmi = true,
                     .dependencies_ready = true}};
        }
        if(object_only)
        {
            return {{.argv = compile_pcm_object_argv(request),
                     .dependencies_ready = true}};
        }
        return {
            {.argv = compile_pcm_argv(request),
             .output = compile_output::bmi,
             .writes_bmi = true,
             .dependencies_ready = true},
            {.argv = compile_pcm_object_argv(request)},
        };
    }

    compile_plan compile_standard_module(std::string_view std_pcm_path,
                                         std::string_view std_object_path,
                                         bool object_only) const
    {
        if(module_phases_ == module_compilation::one_phase)
        {
            return {{
                .argv = build_std_module_object_argv(std_pcm_path, std_object_path),
                .writes_bmi = true,
            }};
        }
        if(object_only)
            return {{.argv = build_std_o_argv(std_pcm_path, std_object_path)}};
        return {
            {.argv = build_std_pcm_argv(std_pcm_path),
             .output = compile_output::bmi,
             .writes_bmi = true},
            {.argv = build_std_o_argv(std_pcm_path, std_object_path)},
        };
    }

    string_list compile_database_argv(const compile_request& request) const
    {
        if(not request.modular)
            return compile_source_object_argv(request);
        return module_phases_ == module_compilation::two_phase
            ? compile_pcm_argv(request)
            : compile_module_object_argv(request);
    }

    string_list link_argv(std::string_view executable_path,
                          const string_list& input_paths,
                          const string_list& import_flags) const
    {
        auto argv = string_list{compiler_};
        argv.append_range(compile_flags_);
        argv.append_range(import_flags);
        argv.append_range(module_flags_);
        argv.append_range(input_paths);
        argv.append_range(link_flags_);
        argv.push_back("-o");
        argv.emplace_back(executable_path);
        return argv;
    }

private:
    inline static constexpr auto module_file_flag_prefix = "-fmodule-file="sv;
    inline static constexpr auto prebuilt_module_path_flag_prefix =
        "-fprebuilt-module-path="sv;

    static std::string binary_signature(const std::string& path)
    {
        if(not fs::exists(path))
            return {};

        const auto size = fs::file_size(path);
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
            fs::last_write_time(path).time_since_epoch()).count();
        return std::to_string(size) + ':' + std::to_string(ticks);
    }

    static std::string module_file_flag(std::string_view module_name,
                                        std::string_view bmi_path)
    {
        auto flag = std::string{module_file_flag_prefix};
        flag.append(module_name);
        flag.push_back('=');
        flag.append(bmi_path);
        return flag;
    }

    static std::string_view linux_arch()
    {
#if defined(__x86_64__) or defined(__amd64__)
        return "x86_64";
#elif defined(__aarch64__) or defined(__arm64__)
        return "aarch64";
#else
        throw std::runtime_error{
            "Unsupported architecture. Only x86_64 and aarch64 are supported."};
#endif
    }

    void discover(std::invocable<const std::string&> auto& command_available)
    {
        if(std_module_source_.empty())
        {
            if(auto env = std::getenv("LLVM_PATH"); env and *env)
                std_module_source_ = env;
            else
                throw std::runtime_error{
                    "std.cppm path not provided. Pass it as the first argument or set LLVM_PATH."};
        }

        const auto std_module_path = fs::path{std_module_source_};
        if(not fs::exists(std_module_path))
            throw std::runtime_error{"std.cppm not found at: " + std_module_source_};

        auto prefix_path = std_module_path;
        for(auto level = 0; level < 4 and prefix_path.has_parent_path(); ++level)
            prefix_path = prefix_path.parent_path();
        llvm_prefix_ = prefix_path.string();

        const auto try_env_compiler = [&]()
        {
            for(const auto* env_name : {"LLVM_CXX", "CXX"})
            {
                if(auto value = std::getenv(env_name); value and command_available(value))
                {
                    compiler_ = value;
                    return true;
                }
            }
            return false;
        };
        if(not try_env_compiler())
        {
            compiler_ = llvm_prefix_ + "/bin/clang++";
            if(not command_available(compiler_))
            {
                throw std::runtime_error{
                    "clang++ not found. Expected: " + compiler_
                    + " (set LLVM_CXX to override)."};
            }
        }

        const auto canonical_std_module = fs::weakly_canonical(std_module_path).string();
        std_module_profile_ =
            canonical_std_module + '@' + binary_signature(canonical_std_module);
        compiler_signature_ = binary_signature(compiler_);
    }

    void initialize_flags(std::string_view std_pcm_path,
                          std::string_view module_cache_dir)
    {
        const auto os = host_os();
        const auto is_darwin = os == "darwin";
        const auto is_linux = os == "linux";

        compile_flags_ = {
            "-B" + llvm_prefix_ + "/bin",
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
            compile_flags_.push_back("-I" + llvm_prefix_ + "/include/c++/v1");
        else
        {
            compile_flags_.append_range(string_list{
                "-nostdinc++",
                "-isystem",
                llvm_prefix_ + "/include/c++/v1",
                "-fno-implicit-modules",
                "-fno-implicit-module-maps",
            });
        }

        compile_flags_.append_range(
            release_
                ? string_list{"-O3", "-DNDEBUG"}
                : string_list{"-O0", "-g3"});

        if(static_linking())
        {
            if(is_darwin)
            {
                link_flags_ = {
                    "-pthread",
                    "-lc++",
                    "-L" + llvm_prefix_ + "/lib",
                    "-Wl,-dead_strip",
                };
            }
            else
            {
                const auto arch = linux_arch();
                link_flags_ = {
                    "-Wl,-Bstatic",
                    "-lc++",
                    "-lc++abi",
                    "-lc++experimental",
                    "-Wl,-Bdynamic",
                    "-pthread",
                    "-ldl",
                    "-L/usr/lib/" + std::string{arch} + "-linux-gnu",
                    "-L" + llvm_prefix_ + "/lib",
                    "-O3",
                };
                if(not release_)
                    link_flags_.push_back("-g3");
            }
        }
        else if(is_darwin)
        {
            link_flags_ = {
                "-pthread",
                "-L" + llvm_prefix_ + "/lib",
                "-Wl,-rpath," + llvm_prefix_ + "/lib",
                "-lc++abi",
                "-lunwind",
                "-Wl,-dead_strip",
            };
            if(fs::exists("/usr/lib/system/introspection/libunwind.reexported_symbols"))
            {
                link_flags_.push_back(
                    "-Wl,-unexported_symbols_list,/usr/lib/system/introspection/libunwind.reexported_symbols");
            }
        }
        else
        {
            const auto arch = linux_arch();
            link_flags_ = {
                "-pthread",
                "-lc++",
                "-lc++abi",
                "-lc++experimental",
                "-L/usr/lib/" + std::string{arch} + "-linux-gnu",
                "-L" + llvm_prefix_ + "/lib",
                "-Wl,-rpath," + llvm_prefix_ + "/lib",
                "-O3",
            };
            if(not release_)
                link_flags_.push_back("-g3");
        }

        link_flags_.append_range(extra_link_flags_);
        compile_flags_.append_range(extra_compile_flags_);
        module_flags_ = {
            "-fno-implicit-modules",
            "-fno-implicit-module-maps",
            module_file_flag(std_module_name, std_pcm_path),
            std::string{prebuilt_module_path_flag_prefix} + std::string{module_cache_dir},
        };
    }

    string_list compile_pcm_argv(const compile_request& request) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags_);
        argv.append_range(module_file_flags(request.modules));
        argv.append_range(depfile_argv(request.depfile));
        argv.push_back(request.source);
        argv.push_back("--precompile");
        argv.push_back("-o");
        argv.push_back(request.bmi);
        return argv;
    }

    string_list compile_pcm_object_argv(const compile_request& request) const
    {
        auto argv = string_list{compiler_};
        argv.append_range(compile_flags_);
        argv.append_range(module_flags_);
        argv.append_range(module_file_flags(request.modules));
        argv.push_back(request.bmi);
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(request.object);
        return argv;
    }

    string_list compile_module_object_argv(const compile_request& request) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags_);
        argv.append_range(module_file_flags(request.modules));
        argv.append_range(depfile_argv(request.depfile));
        argv.push_back("-fmodule-output=" + request.bmi);
        argv.push_back(request.source);
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(request.object);
        return argv;
    }

    string_list compile_source_object_argv(const compile_request& request) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags_);
        argv.append_range(module_file_flags(request.modules));
        argv.append_range(depfile_argv(request.depfile));
        argv.push_back(request.source);
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(request.object);
        return argv;
    }

    string_list build_std_pcm_argv(std::string_view std_pcm_path) const
    {
        auto argv = std_module_source_argv();
        argv.push_back("--precompile");
        argv.push_back("-o");
        argv.emplace_back(std_pcm_path);
        return argv;
    }

    string_list build_std_module_object_argv(std::string_view std_pcm_path,
                                             std::string_view std_object_path) const
    {
        auto argv = std_module_source_argv();
        argv.push_back("-fmodule-output=" + std::string{std_pcm_path});
        argv.push_back("-c");
        argv.push_back("-o");
        argv.emplace_back(std_object_path);
        return argv;
    }

    string_list build_std_o_argv(std::string_view std_pcm_path,
                                 std::string_view std_object_path) const
    {
        auto argv = string_list{compiler_};
        argv.append_range(compile_flags_);
        argv.append_range(module_flags_);
        argv.emplace_back(std_pcm_path);
        argv.push_back("-c");
        argv.push_back("-o");
        argv.emplace_back(std_object_path);
        return argv;
    }

    string_list base_compile_argv() const
    {
        auto argv = string_list{compiler_};
        argv.append_range(compile_flags_);
        argv.append_range(cpp_flags_);
        return argv;
    }

    string_list module_file_flags(const module_file_list& modules) const
    {
        return modules
            | std::views::transform([](const module_file& module)
              {
                  return clang_driver::module_file_flag(module.name, module.bmi_path);
              })
            | std::ranges::to<string_list>();
    }

    static string_list depfile_argv(std::string_view path)
    {
        return {"-MMD", "-MF", std::string{path}};
    }

    string_list std_module_source_argv() const
    {
        auto argv = string_list{compiler_};
        argv.append_range(compile_flags_);
        argv.append_range(string_list{
            "-nostdinc++",
            "-isystem",
            llvm_prefix_ + "/include/c++/v1",
            "-Wno-unused-command-line-argument",
            "-fno-implicit-modules",
            "-fno-implicit-module-maps",
            "-Wno-reserved-module-identifier",
            std_module_source_,
        });
        return argv;
    }

    bool release_ = false;
    linkage linkage_ = linkage::dynamic;
    module_compilation module_phases_ = module_compilation::two_phase;
    std::string std_module_source_{};
    std::string llvm_prefix_{};
    std::string compiler_{};
    std::string compiler_signature_{};
    mutable std::string compiler_version_{};
    std::string std_module_profile_{};
    string_list compile_flags_{};
    string_list link_flags_{};
    string_list cpp_flags_{};
    string_list module_flags_{};
    string_list extra_compile_flags_{};
    string_list extra_link_flags_{};
};

} // namespace toolchain

class build_system {
public:
    enum class build_config { debug, release };

    struct settings
    {
        build_config config = build_config::debug;
        std::string source_dir = ".";
        std::string std_module_source{};
        string_list include_paths{};
        toolchain::module_link_flags module_link_flags{};
        toolchain::linkage linkage = toolchain::linkage::dynamic;
        bool include_examples = false;
        bool include_tests_for_build = false;
        // Part of the object-cache profile; changing schemes rebuilds every modular unit.
        toolchain::module_compilation module_phases = toolchain::module_compilation::two_phase;
        // Zero requests the hardware-concurrency default.
        int max_jobs = 0;
        string_list extra_compile_flags{};
        string_list extra_link_flags{};
    };

private:

    std::string source_dir;
    toolchain::module_link_flags module_ldflags;
    // Indexed after scanning for per-TU transitive BMI requests.
    std::flat_map<std::string, const source::translation_unit*, std::less<>> module_interfaces;
    mutable bool toolchain_profile_probed = false;
    source::translation_unit_list units_in_topological_order;
    const build_config config;
    const layout::paths artifact_paths;
    bool include_tests = false;
    bool include_examples = false;
    const bool include_tests_for_build;
    int max_jobs = 0;
    process::runner process_runner;
    mutable toolchain::clang_driver compiler;
    std::string_view config_name() const
    {
        switch(config)
        {
            case build_config::debug: return "debug";
            case build_config::release: return "release";
        }
        std::unreachable();
    }

    // Initialization

    bool command_available(const std::string& candidate)
    {
        if(candidate.contains('/'))
            return fs::exists(candidate);
        fs::create_directories(artifact_paths.cache);
        return process_runner.command_available(
            candidate, (artifact_paths.cache / "command-probe.txt").string());
    }

    void ensure_toolchain_profile() const
    {
        if(toolchain_profile_probed)
            return;
        toolchain_profile_probed = true;

        fs::create_directories(artifact_paths.cache);

        const auto stamp = cache::compiler_stamp{artifact_paths.cache.string()};
        if(process_runner.invoke_shell(compiler.version_argv(), stamp.path()).ok())
        {
            compiler.set_compiler_version(stamp.read());
            const auto identity = compiler.identity();
            // Surface the probe's first line in console output.
            if(not identity.compiler_version.empty())
                output::notify(&output::observer::info, identity.compiler_version);
        }
    }

    void report_toolchain_configuration() const
    {
        const auto flags = compiler.flags();
        const auto static_note =
            compiler.static_linking() ? " (static C++ stdlib)"s : ""s;
        output::notify(
            &output::observer::info,
            "Building "s + (config == build_config::release ? "RELEASE" : "DEBUG")
                + " configuration" + static_note);
        for(const auto& warning : compiler.warnings())
            output::notify(
                &output::observer::warning,
                warning);
        if(not flags.extra_link.empty())
        {
            output::notify(
                &output::observer::info,
                "Added extra linker flags: "s
                    + cb::flags::codec::serialize(flags.extra_link));
        }
        if(not flags.extra_compile.empty())
        {
            output::notify(
                &output::observer::info,
                "Added extra compile flags: "s
                    + cb::flags::codec::serialize(flags.extra_compile));
        }
    }

    void index_module_interfaces()
    {
        module_interfaces =
            units_in_topological_order
            | std::views::filter([](const source::translation_unit& tu) { return tu.is_modular; })
            | std::views::transform([](const source::translation_unit& tu)
            {
                return std::pair{tu.module, &tu};
            })
            | std::ranges::to<std::flat_map<std::string, const source::translation_unit*, std::less<>>>();
    }

    // Includes transitive imports and an implementation unit's primary interface.
    toolchain::module_file_list module_files_for(const source::translation_unit& tu) const
    {
        auto pending = string_list{};
        auto seen = std::flat_set<std::string, std::less<>>{};

        auto enqueue = [&](std::string_view module_name)
        {
            if(module_name.empty() or module_name == std_module_name)
                return;
            if(not seen.insert(std::string{module_name}).second)
                return;
            pending.emplace_back(module_name);
        };

        for(const auto& imp : tu.imports)
            enqueue(imp);
        if(tu.kind == unit_kind::implementation_unit)
            enqueue(tu.module);

        auto modules = toolchain::module_file_list{};
        for(std::size_t i = 0; i < pending.size(); ++i)
        {
            const auto& name = pending[i];
            if(not module_interfaces.contains(name))
                continue;
            const auto& dep = *module_interfaces.at(name);
            modules.push_back({dep.module, dep.pcm_path});
            for(const auto& imp : dep.imports)
                enqueue(imp);
        }
        return modules;
    }

    void validate_translation_unit(const source::translation_unit& tu) const {
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

    // Utilities

    toolchain::compile_request compile_request_for(
        const source::translation_unit& tu) const
    {
        return {
            .source = tu.full_path,
            .object = tu.object_path,
            .bmi = tu.pcm_path,
            .depfile = layout::paths::depfile(tu.object_path),
            .modules = module_files_for(tu),
            .modular = tu.is_modular,
        };
    }

    string_list test_runner_argv(const std::string& runner, const std::vector<std::string>& args) const
    {
        auto argv = string_list{};
        argv.push_back(runner);
        argv.append_range(args);
        return argv;
    }

    auto test_units() const
    {
        return units_in_topological_order
            | std::views::filter([](const source::translation_unit& tu) { return tu.is_test and not tu.has_main; });
    }

    string_list test_object_paths() const
    {
        return test_units()
            | std::views::transform([](const source::translation_unit& tu) { return tu.object_path; })
            | std::ranges::to<string_list>();
    }

    string_list linkable_object_paths() const
    {
        return units_in_topological_order
            | std::views::filter([](const source::translation_unit& tu) { return not tu.has_main and not tu.is_test; })
            | std::views::transform([](const source::translation_unit& tu) { return tu.object_path; })
            | std::ranges::to<string_list>();
    }

    string_list executable_link_inputs(const source::translation_unit& main,
                                       const string_list& shared_objects) const
    {
        auto inputs = string_list{main.object_path};
        inputs.append_range(shared_objects);
        inputs.push_back(artifact_paths.std_object());
        return inputs;
    }

    string_list test_runner_link_inputs(const source::translation_unit& runner) const
    {
        auto inputs = string_list{runner.object_path};
        inputs.append_range(linkable_object_paths());
        inputs.append_range(test_object_paths());
        inputs.push_back(artifact_paths.std_object());
        return inputs;
    }

    string_list collect_module_ldflags(const string_list& imp) const
    {
        return std::ranges::fold_left(
            imp | std::views::filter([&](const std::string& m) { return module_ldflags.contains(m); }),
            string_list{},
            [&](string_list flags, const std::string& m) {
                flags.append_range(cb::flags::codec::parse(module_ldflags.at(m)));
                return flags;
            });
    }

    string_list collect_test_module_ldflags() const
    {
        return std::ranges::fold_left(
            test_units(),
            string_list{},
            [&](string_list flags, const source::translation_unit& tu) {
                flags.append_range(collect_module_ldflags(tu.imports));
                return flags;
            });
    }

    string_list test_runner_link_flags(const source::translation_unit& runner) const
    {
        auto flags = collect_test_module_ldflags();
        flags.append_range(collect_module_ldflags(runner.imports));
        return flags;
    }

    // Cache

    std::string cache_profile(bool include_project_includes) const {
        ensure_toolchain_profile();
        const auto identity = compiler.identity();
        const auto flags = compiler.flags();
        return cache::profile{cache::profile::ingredients{
            .config = config_name(),
            .static_link = compiler.static_linking(),
            .module_phases = compiler.module_phases_name(),
            .toolchain_root = identity.toolchain_root,
            .compiler = identity.compiler,
            .compiler_signature = identity.compiler_signature,
            .compiler_version = identity.compiler_version,
            .std_module = identity.std_module_profile,
            .compile_flags = flags.compile,
            .cpp_flags = include_project_includes ? &flags.cpp : nullptr,
        }}.text();
    }

    std::string object_cache_profile() const { return cache_profile(true); }
    std::string shared_std_cache_profile() const { return cache_profile(false); }

    cache::object_store make_object_store() const
    {
        return cache::object_store{artifact_paths.cache.string()};
    }

    cache::link_store make_link_store() const
    {
        const auto flags = compiler.flags();
        return cache::link_store{
            artifact_paths.cache.string(),
            {.compile_flags = flags.compile,
             .link_flags = flags.link,
             .module_flags = flags.modules}};
    }

    cache::standard_module_store make_standard_module_store() const
    {
        return cache::standard_module_store{
            artifact_paths.cache.string(),
            artifact_paths.std_pcm(),
            artifact_paths.std_object()};
    }

    // Dependency analysis

    void attach_artifact_paths(source::translation_unit_list& units)
    {
        auto object_owners = std::flat_map<std::string, std::string, std::less<>>{};
        auto pcm_owners = std::flat_map<std::string, std::string, std::less<>>{};
        auto executable_owners = std::flat_map<std::string, std::string, std::less<>>{};

        // Reserve the libc++ std module artifacts so a project TU named `std`
        // (std.c++ / export module std;) cannot silently overwrite them.
        object_owners.emplace(artifact_paths.std_object(), "reserved std module object");
        pcm_owners.emplace(artifact_paths.std_pcm(), "reserved std module PCM");

        const auto claim = [](auto& owners,
                              const std::string& path,
                              const std::string& owner,
                              std::string_view kind)
        {
            const auto [prior, inserted] = owners.try_emplace(path, owner);
            if(not inserted)
                throw std::runtime_error{
                    "Duplicate "s + std::string{kind} + " path '" + path + "' from "
                    + prior->second + " and " + owner
                    + " (object/module names must stay unique)"};
        };

        for(auto& tu : units)
        {
            // Assign artifact paths once the full configuration is known.
            const auto& source_label = tu.display_path;
            tu.object_path = artifact_paths.object(tu);
            claim(object_owners, tu.object_path, source_label, "object");

            if (tu.is_modular) {
                tu.pcm_path = artifact_paths.pcm_file(tu);
                claim(pcm_owners, tu.pcm_path, source_label, "PCM");
            }
            if (tu.has_main) {
                tu.executable_path = artifact_paths.executable(tu);
                claim(executable_owners, tu.executable_path, source_label, "executable");
            }
            validate_translation_unit(tu);
        }
    }

    void scan_and_order()
    {
        auto units = source::scanner{
            source_dir, include_tests, include_examples}.scan();
        attach_artifact_paths(units);
        units_in_topological_order = std::move(units);
    }

    // Standard library module

    // Observers see what they see for a project modular unit: one source, one pcm, one object.
    // The strings are the caller's, because compile_unit holds views.
    output::compile_unit std_compile_unit(const std::string& pcm,
                                          const std::string& object,
                                          const std::string& display) const
    {
        return {.source = compiler.identity().std_module_source,
                .object = object,
                .pcm = pcm,
                .module = std_module_name,
                .display_path = display};
    }

    void notify_std_cache_warning(const cache::standard_module_store::transfer_result& result) const
    {
        if(not result.warning.empty())
            output::notify(&output::observer::warning, result.warning);
    }

    void build_std_module()
    {
        const auto identity = compiler.identity();
        const auto pcm = artifact_paths.std_pcm();
        const auto object = artifact_paths.std_object();
        const auto display = fs::path{identity.std_module_source}.filename().string();
        const auto unit = std_compile_unit(pcm, object, display);
        auto std_store = make_standard_module_store();
        const auto reason = std_store.rebuild_reason_for(
            identity.std_module_source, [&]() { return object_cache_profile(); });

        if(not reason)
        {
            const auto hit = output::compile_scope{unit};
            return;
        }
        // The store only attempts hydration for a missing local PCM. In particular, cache
        // invalidate removes the local profile and must continue to force a rebuild.
        const auto hydrated = std_store.hydrate_for(
            *reason,
            [&]() { return shared_std_cache_profile(); },
            [&]() { return object_cache_profile(); });
        notify_std_cache_warning(hydrated);
        if(hydrated.ok)
        {
            const auto hit = output::compile_scope{unit};
            return;
        }

        auto compile = output::compile_scope{unit, *reason};
        const auto object_only =
            reason->kind == output::rebuild_kind::object_missing
            or reason->kind == output::rebuild_kind::object_stale;
        for(const auto& step :
            compiler.compile_standard_module(pcm, object, object_only))
        {
            process_runner.run_step(
                compile,
                step.argv,
                layout::paths::compile_log(
                    step.output == toolchain::compile_output::bmi ? unit.pcm : unit.object));
            if(step.writes_bmi)
                std_store.save_profile(object_cache_profile());
        }
        notify_std_cache_warning(
            std_store.publish([&]() { return shared_std_cache_profile(); }));
        compile.succeeded();
    }

    // Compilation

    void compile_unit(const source::translation_unit& tu,
                      const output::rebuild_info& rebuild,
                      std::invocable auto&& on_dependency_ready) {
        const auto unit = output::compile_unit_of(tu);
        auto compile = output::compile_scope{unit, rebuild};
        // Two-phase: object_missing / object_stale reuse the pcm that is already there —
        // same split build_std_module uses. Re-precompiling would bump the pcm mtime and
        // force every importer through pcm_stale for no interface change.
        // One-phase: the BMI is a sibling of the object (reduced on Clang 22+), not an
        // input that can be compiled to .o — always re-read the source, as std does.
        const auto object_only =
            rebuild.kind == output::rebuild_kind::object_missing
            or rebuild.kind == output::rebuild_kind::object_stale;
        for(const auto& step : compiler.compile(compile_request_for(tu), object_only))
        {
            process_runner.run_step(
                compile,
                step.argv,
                layout::paths::compile_log(
                    step.output == toolchain::compile_output::bmi ? unit.pcm : unit.object));
            if(step.dependencies_ready)
                on_dependency_ready();
        }
        compile.succeeded();
    }

    void compile_units() {
        if (units_in_topological_order.empty()) return;
        auto objects = make_object_store();
        objects.load(object_cache_profile());
        if(objects.missing_profile_header())
            output::notify(&output::observer::info,
                           "Object cache missing profile header; ignoring"s);
        if(objects.profile_change())
            output::notify(&output::observer::profile_changed,
                           output::rebuild_kind::profile_change,
                           *objects.profile_change());

        auto u2tu = source::unit_index{};
        for (auto& tu : units_in_topological_order) {
            auto k = tu.unit;
            u2tu[k] = &tu;
        }
        const auto cache_analyzer =
            cache::analyzer{source_dir, artifact_paths.pcm.string()};

        const auto unit_count = units_in_topological_order.size();
        auto unit_to_index = std::flat_map<std::string, std::size_t, std::less<>>{};
        for(auto index = std::size_t{0}; index < unit_count; ++index)
            unit_to_index[units_in_topological_order[index].unit] = index;

        // The same provider rule drives inventory levels and compile readiness.
        auto dependents = std::vector<std::vector<std::size_t>>(unit_count);
        auto dependencies_remaining = std::vector<std::size_t>(unit_count);
        for(auto index = std::size_t{0}; index < unit_count; ++index)
        {
            const auto& tu = units_in_topological_order[index];
            source::scanner::for_each_provider(
                tu, unit_to_index, [&](const std::string& provider)
            {
                dependents[unit_to_index.at(provider)].push_back(index);
                ++dependencies_remaining[index];
            });
        }

        auto ready = std::queue<std::size_t>{};
        for(auto index = std::size_t{0}; index < unit_count; ++index)
            if(dependencies_remaining[index] == 0)
                ready.push(index);
        if(ready.empty())
            throw std::runtime_error{"No dependency-free translation unit"};

        auto scheduler_mutex = std::mutex{};
        auto scheduler_changed = std::condition_variable{};
        auto failure = std::exception_ptr{};
        auto completed = std::size_t{0};
        auto published = std::vector<unsigned char>(unit_count);
        auto rebuilt = std::vector<unsigned char>(unit_count);

        // Publish once. For two-phase modular units this runs after --precompile; for one-phase,
        // object-only repairs, non-modular units and hits it runs when the whole unit is ready.
        const auto publish_dependency = [&](std::size_t provider)
        {
            {
                auto lock = std::lock_guard<std::mutex>{scheduler_mutex};
                if(published[provider] != 0)
                    return;
                published[provider] = 1;
                if(failure)
                    return;
                for(const auto dependent : dependents[provider])
                {
                    if(--dependencies_remaining[dependent] == 0)
                        ready.push(dependent);
                }
            }
            scheduler_changed.notify_all();
        };

        const auto worker_count = std::min(
            unit_count,
            static_cast<std::size_t>(std::max<std::ptrdiff_t>(1, job_limit())));
        execution::run_workers(worker_count, [&]()
        {
            while(true)
            {
                auto index = std::size_t{};
                {
                    auto lock = std::unique_lock<std::mutex>{scheduler_mutex};
                    scheduler_changed.wait(lock, [&]()
                    {
                        return failure or not ready.empty() or completed == unit_count;
                    });
                    if(failure or completed == unit_count)
                        return;
                    index = ready.front();
                    ready.pop();
                }

                try
                {
                    const auto& tu = units_in_topological_order[index];
                    const auto reason =
                        cache_analyzer.rebuild_reason_for(tu, objects, u2tu);
                    if(reason)
                    {
                        compile_unit(tu, *reason, [&]() { publish_dependency(index); });
                        rebuilt[index] = 1;
                    }
                    else
                    {
                        {
                            const auto hit = output::compile_scope{output::compile_unit_of(tu)};
                        }
                        publish_dependency(index);
                    }
                }
                catch(...)
                {
                    {
                        auto lock = std::lock_guard<std::mutex>{scheduler_mutex};
                        if(not failure)
                            failure = std::current_exception();
                    }
                    scheduler_changed.notify_all();
                    return;
                }

                {
                    auto lock = std::lock_guard<std::mutex>{scheduler_mutex};
                    ++completed;
                }
                scheduler_changed.notify_all();
            }
        });
        if(failure)
            std::rethrow_exception(failure);

        for(auto index = std::size_t{0}; index < unit_count; ++index)
            if(rebuilt[index] != 0)
            {
                const auto& tu = units_in_topological_order[index];
                objects.record(tu.full_path, tu.last_modified);
            }
        objects.save();
    }

    // Linking

    void perform_link(const source::translation_unit& main,
                      const string_list& input_paths,
                      const string_list& import_flags,
                      const output::rebuild_info& rebuild,
                      std::string signature)
    {
        auto link = output::link_scope{main.executable_path, rebuild, std::move(signature)};
        process_runner.run_step(
            link,
            compiler.link_argv(main.executable_path, input_paths, import_flags),
            layout::paths::link_log(main.executable_path));
        link.succeeded();
    }

    void link_executables() {
        auto shared_objects = linkable_object_paths();
        auto links = make_link_store();
        links.load();

        // Snapshot relink decisions before workers mutate the store. Interleaving
        // needs_relinking (unlocked reads) with parallel remember() writes is a data race.
        struct link_decision {
            const source::translation_unit* tu = nullptr;
            string_list input_paths{};
            string_list import_flags{};
            std::string signature{};
            std::optional<output::rebuild_info> reason{};
        };
        auto decisions = units_in_topological_order
            // Exact base name only — substring matches like contest_runner / aaa_test_runner
            // are ordinary mains and must not be excluded from normal linking.
            | std::views::filter([&](const source::translation_unit& tu) {
                  return tu.has_main and tu.base_name != test_runner_name; })
            | std::views::transform([&](const source::translation_unit& tu) {
                  auto input_paths = executable_link_inputs(tu, shared_objects);
                  auto import_flags = collect_module_ldflags(tu.imports);
                  auto signature = links.link_signature(input_paths, import_flags);
                  auto reason = links.needs_relinking(tu.executable_path, signature);
                  return link_decision{
                      .tu = &tu,
                      .input_paths = std::move(input_paths),
                      .import_flags = std::move(import_flags),
                      .signature = std::move(signature),
                      .reason = std::move(reason)}; })
            | std::ranges::to<std::vector>();

        for(const auto& decision : decisions)
        {
            if(decision.reason)
                continue;
            const auto hit =
                output::link_scope{decision.tu->executable_path, decision.signature};
        }

        execution::worker_pool{job_limit()}.run(
            decisions | std::views::filter([](const link_decision& decision) {
                return decision.reason.has_value();
            }),
            [&](const link_decision& decision) {
                const auto& tu = *decision.tu;
                perform_link(
                    tu,
                    decision.input_paths,
                    decision.import_flags,
                    *decision.reason,
                    decision.signature);
                links.remember(tu.executable_path, decision.signature);
            });
        links.save();
    }

    // Test support

    // Require an exact base name: substring selection (aaa_test_runner, contest_runner) can link
    // a different bin/<name> while run_tests always executes bin/test_runner, leaving a stale
    // runner and silent CI passes. Never absent either — linking the test objects without a main
    // dies at the linker, so a project with no runner source is told so here.
    const source::translation_unit& test_runner_unit() const
    {
        const auto is_runner = [](const source::translation_unit& tu) {
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

    void link_test_runner() {
        const auto& runner = test_runner_unit();

        auto links = make_link_store();
        links.load();
        const auto input_paths = test_runner_link_inputs(runner);
        const auto import_flags = test_runner_link_flags(runner);
        const auto signature = links.link_signature(input_paths, import_flags);
        const auto reason = links.needs_relinking(runner.executable_path, signature);
        if(not reason)
        {
            const auto hit = output::link_scope{runner.executable_path, signature};
            return;
        }

        perform_link(runner, input_paths, import_flags, *reason, signature);
        output::notify(&output::observer::success, "test_runner linked with test objects");
        links.remember(runner.executable_path, signature);
        links.save();
    }

    // Build orchestration

    void build_steps()
    {
        fs::create_directories(artifact_paths.pcm);
        fs::create_directories(artifact_paths.obj);
        fs::create_directories(artifact_paths.bin);
        fs::create_directories(artifact_paths.cache);

        // Source discovery and std compilation share no mutable build state: the scan writes
        // units_in_topological_order while std owns only its artefacts/profile. Hide the scan
        // under the cold build's root module work, then join before module flags consume units.
        auto scan_failure = std::exception_ptr{};
        auto scan = std::jthread{[&]()
        {
            try
            {
                scan_and_order();
            }
            catch(...)
            {
                scan_failure = std::current_exception();
            }
        }};

        auto std_failure = std::exception_ptr{};
        try
        {
            build_std_module();
        }
        catch(...)
        {
            std_failure = std::current_exception();
        }
        scan.join();

        // Prefer the std failure, matching the old phase order when both sides fail.
        if(std_failure)
            std::rethrow_exception(std_failure);
        if(scan_failure)
            std::rethrow_exception(scan_failure);
        if(units_in_topological_order.empty())
            throw std::runtime_error{"No sources found"};

        index_module_interfaces();
        compile_units();
        link_executables();
    }

public:

    explicit build_system(settings values)
        : source_dir{detail::canonical_path(values.source_dir)},
          module_ldflags{std::move(values.module_link_flags)},
          config{values.config},
          artifact_paths{
              values.config == build_config::release ? "release"sv : "debug"sv,
              toolchain::clang_artifacts()},
          include_tests{values.config == build_config::debug},
          include_examples{values.include_examples},
          include_tests_for_build{values.include_tests_for_build},
          max_jobs{values.max_jobs},
          compiler{toolchain::clang_driver::settings{
              .release = values.config == build_config::release,
              .link = values.linkage,
              .module_phases = values.module_phases,
              .std_module_source = std::move(values.std_module_source),
              .include_paths = std::move(values.include_paths),
              .extra_compile_flags = std::move(values.extra_compile_flags),
              .extra_link_flags = std::move(values.extra_link_flags)},
              artifact_paths.std_pcm(),
              artifact_paths.pcm.string(),
              [this](const std::string& candidate)
              {
                  return command_available(candidate);
              }}
    {
        report_toolchain_configuration();
    }

    void clean() const {
        const auto& dir = artifact_paths.root;
        if (fs::exists(dir)) {
            fs::remove_all(dir);
            output::notify(&output::observer::success, "Removed "s + dir.string());
        } else {
            output::notify(
                &output::observer::info, "Nothing to clean for "s + dir.string());
        }
    }

    // Drop only test TU artefacts and the test_runner binary — leave library/app objects so the
    // next build recompiles tests without a full cold rebuild. Scans with include_tests on so
    // release configs still see *.test.c++ even when a normal release build would not.
    void clean_tests() {
        include_tests = true;
        if(not fs::exists(artifact_paths.obj) and not fs::exists(artifact_paths.bin))
        {
            output::notify(&output::observer::info,
                           "Nothing to clean for test objects under "s
                               + artifact_paths.root.string());
            return;
        }

        scan_and_order();

        // Object-cache keys are source paths (full_path), not .o paths.
        auto dropped_sources = std::flat_set<std::string, std::less<>>{};
        auto removed = 0;

        const auto try_remove = [&](const std::string& path) {
            if(detail::remove_if_exists(path))
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
                try_remove(layout::paths::depfile(tu.object_path));
                try_remove(layout::paths::compile_log(tu.object_path));
            }
            if(tu.is_modular and not tu.pcm_path.empty())
            {
                try_remove(tu.pcm_path);
                try_remove(layout::paths::compile_log(tu.pcm_path));
            }
            if(is_runner and not tu.executable_path.empty())
            {
                try_remove(tu.executable_path);
                try_remove(layout::paths::link_log(tu.executable_path));
            }
        }

        auto objects = make_object_store();
        if(not dropped_sources.empty() and objects.exists())
        {
            objects.load(object_cache_profile());
            if(objects.missing_profile_header())
                output::notify(&output::observer::info,
                               "Object cache missing profile header; ignoring"s);
            if(not objects.profile_change())
            {
                auto erased = false;
                for(const auto& path : dropped_sources)
                {
                    if(objects.erase(path))
                        erased = true;
                }
                if(erased)
                    objects.save();
            }
        }

        auto links = make_link_store();
        if(links.exists())
        {
            links.load();
            const auto runner_exe = artifact_paths.executable(test_runner_name);
            if(links.erase(runner_exe))
                links.save();
        }

        if(removed == 0)
            output::notify(&output::observer::info,
                           "No test objects to remove under "s
                               + artifact_paths.root.string());
        else
            output::notify(&output::observer::success,
                           "Removed " + std::to_string(removed) + " test artifact(s) under "s
                               + artifact_paths.root.string());
    }

    void cache_status() const
    {
        ensure_toolchain_profile();
        fs::create_directories(artifact_paths.cache);

        const auto current_profile = object_cache_profile();
        const auto objects = make_object_store();
        const auto object_status = objects.status(current_profile);

        // Paths outlive the notify: cache_inventory holds views, so the strings they point at
        // are named here rather than built inside the aggregate.
        const auto links = make_link_store();
        const auto link_status = links.status();
        const auto std_modules = make_standard_module_store();
        const auto std_status = std_modules.status(current_profile);
        const auto stamp = cache::compiler_stamp{artifact_paths.cache.string()};

        output::notify(
            &output::observer::cache_status,
            output::cache_inventory{
                .object_cache_path = objects.path(),
                .object_cache_exists = object_status.exists,
                .profile_match = object_status.profile_match,
                .object_entries = object_status.entries,
                .object_stale_entries = object_status.stale_entries,
                .executable_cache_path = links.path(),
                .executable_cache_exists = link_status.exists,
                .executable_entries = link_status.entries,
                .std_module_profile_path = std_modules.profile_path(),
                .std_module_profile_exists = std_status.exists,
                .std_module_profile_match = std_status.profile_match,
                .compiler_stamp_path = stamp.path(),
                .compiler_stamp_exists = stamp.exists(),
                .current_profile = current_profile});
    }

    void cache_invalidate() const
    {
        fs::create_directories(artifact_paths.cache);

        // Every file cache_status reports, so the two commands cannot disagree about what the
        // cache is. The std module profile is the one that makes CB rebuild std.pcm.
        const auto removed = output::cache_removals{
            .object_cache = make_object_store().invalidate(),
            .executable_cache = make_link_store().invalidate(),
            .compiler_stamp =
                cache::compiler_stamp{artifact_paths.cache.string()}.invalidate(),
            .std_module_profile = make_standard_module_store().invalidate()};

        output::notify(&output::observer::cache_invalidate_end, removed);
    }

    std::ptrdiff_t job_limit() const {
        if(max_jobs > 0)
            return max_jobs;
        const auto detected = std::thread::hardware_concurrency();
        return detected > 0 ? static_cast<std::ptrdiff_t>(detected) : 1;
    }

    void build() {
        if(include_tests_for_build)
            include_tests = true;
        auto build = output::build_scope{config_name(), include_tests, include_examples};
        build_steps();
        build.succeeded();

        output::notify(
            &output::observer::success,
            "Build completed: "s + artifact_paths.root.string());
    }

    // Returns false when the test runner reports failures (normal outcome, not exceptional).
    bool run_tests(const std::vector<std::string>& args = {}) {
        output::notify(&output::observer::info, "=== Running tests ===");

        include_tests = true;
        {
            // No check that the runner is there afterwards: link_test_runner either produced
            // it or threw, and the missing-source case is the sentence it throws.
            auto build = output::build_scope{config_name(), true, include_examples};
            build_steps();
            link_test_runner();
            build.succeeded();
        }
        output::notify(
            &output::observer::success,
            "Build completed: "s + artifact_paths.root.string());

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
            auto test = output::test_scope{runner};
            // Not captured: the runner's stdout is the JSONL stream being forwarded.
            result = process_runner.invoke_shell(test_runner_argv(runner, args));
            test.finished(result);
        }
        return result.ok();
    }

    // The compilation-database entry for a source is the argv that reads that source —
    // --precompile for two-phase modular interfaces/partitions, -c for everything else. The
    // second two-phase step (pcm → .o) is omitted: it has no distinct source path for clangd.
    string_list compile_argv_for_database(const source::translation_unit& tu) const
    {
        return compiler.compile_database_argv(compile_request_for(tu));
    }

    // Interface indexing must precede per-TU argv generation.
    void write_compile_commands() const
    {
        const auto path = detail::join_dir(source_dir, compile_commands_filename);
        detail::write_atomic_file(path, "compile commands database", [&](std::ostream& file) {
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
        detail::write_atomic_file(path, "module dependency graph", [&](std::ostream& file) {
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
        index_module_interfaces();
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
            inventory.units.push_back(output::source_unit_of(tu));
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

namespace cli {

enum class parse_status : unsigned char { ok, help, error };

struct parse_error
{
    int exit_code = 2;
    std::string message;
};

class options
{
public:
    std::string std_module_source{};
    build_system::build_config config = build_system::build_config::debug;
    toolchain::module_compilation module_phases =
        toolchain::module_compilation::two_phase;
    bool do_clean = false;
    bool do_list = false;
    bool do_build = false;
    bool do_run_tests = false;
    bool do_cache_status = false;
    bool do_cache_invalidate = false;
    bool clean_tests_only = false;
    toolchain::linkage linkage = toolchain::linkage::dynamic;
    bool include_examples = false;
    bool build_tests = false;
    bool jobs_explicit = false;
    int max_jobs = 0;
    std::string test_filter{};
    std::vector<std::string> test_runner_args{};
    std::vector<std::string> include_paths{};
    string_list extra_compile_flags{};
    string_list extra_link_flags{};
    std::string output_name = "console";
    std::optional<output::jsonl::jsonl_mode> jsonl_mode{};

    build_system::settings build_settings() const
    {
        return {
            .config = config,
            .std_module_source = std_module_source,
            .include_paths = include_paths,
            .linkage = linkage,
            .include_examples = include_examples,
            // Preserve action semantics: --build-tests changes an explicit build, while list,
            // cache operations and the implicit default build keep their established selection.
            .include_tests_for_build = do_build and build_tests,
            .module_phases = module_phases,
            .max_jobs = max_jobs,
            .extra_compile_flags = extra_compile_flags,
            .extra_link_flags = extra_link_flags};
    }

    bool has_action() const
    {
        return do_clean or do_list or do_run_tests or do_build
            or do_cache_status or do_cache_invalidate;
    }

    std::vector<std::string> runner_arguments() const
    {
        auto args = std::vector<std::string>{};
        if(not test_filter.empty())
            args.push_back(test_filter);

        const auto has_jsonl_mode = std::ranges::any_of(
            test_runner_args,
            [](const auto& arg) { return arg == "--jsonl" or arg.starts_with("--jsonl="); });
        if(output_name == "jsonl" and not has_jsonl_mode)
        {
            const auto mode = jsonl_mode.value_or(output::jsonl::jsonl_mode::failures);
            args.emplace_back(mode == output::jsonl::jsonl_mode::summary
                ? "--jsonl=summary"
                : mode == output::jsonl::jsonl_mode::trace
                    ? "--jsonl=trace"
                    : "--jsonl=failures");
        }

        if(jobs_explicit)
        {
            const auto already = std::ranges::any_of(
                test_runner_args,
                [](const auto& arg) { return arg.starts_with("--jobs="); });
            if(not already)
                args.emplace_back("--jobs=" + std::to_string(max_jobs));
        }

        args.append_range(test_runner_args);
        return args;
    }
};

struct parse_result
{
    parse_status status = parse_status::ok;
    options parsed{};
    std::optional<parse_error> error{};
};

class parser
{
public:
    parser(int argc, char* argv[]) : argc_{argc}, argv_{argv} {}

    parse_result parse() const
    {
        auto parsed = options{};
        auto arg_index = 1;
        if(argc_ > 1)
        {
            const auto candidate = fs::path{argv_[1]};
            if(fs::exists(candidate))
            {
                parsed.std_module_source = candidate.string();
                ++arg_index;
            }
            else if(candidate.extension() == ".cppm")
                return failure(2, "std.cppm not found: "s + candidate.string());
        }

        for(auto index = arg_index; index < argc_; ++index)
        {
            const auto argument = std::string_view{argv_[index]};
            if(argument == "--jsonl" or argument.starts_with("--jsonl="))
            {
                const auto mode = argument == "--jsonl"
                    ? "failures"sv
                    : argument.substr(std::string_view{"--jsonl="}.size());
                if(mode == "summary")
                    parsed.jsonl_mode = output::jsonl::jsonl_mode::summary;
                else if(mode == "failures")
                    parsed.jsonl_mode = output::jsonl::jsonl_mode::failures;
                else if(mode == "trace")
                    parsed.jsonl_mode = output::jsonl::jsonl_mode::trace;
                else
                    return failure(
                        1, "Unknown JSONL mode: "s + std::string{mode}, std::move(parsed));
                parsed.output_name = "jsonl";
                continue;
            }

            if(argument == "test")
            {
                parsed.do_run_tests = true;
                if(index + 1 < argc_)
                {
                    const auto next = std::string_view{argv_[index + 1]};
                    if(not is_test_runner_token(next)
                       and not is_cb_token(next)
                       and not next.starts_with("-"))
                        parsed.test_filter = argv_[++index];
                }
            }
            else if(argument == "release")
                parsed.config = build_system::build_config::release;
            else if(argument == "debug")
                parsed.config = build_system::build_config::debug;
            else if(argument == "ci")
            {
                parsed.do_clean = true;
                parsed.do_run_tests = true;
            }
            else if(argument == "clean")
                parsed.do_clean = true;
            else if(argument == "build")
                parsed.do_build = true;
            else if(argument == "list")
                parsed.do_list = true;
            else if(argument == "cache")
            {
                if(index + 1 >= argc_)
                    return failure(1, "Usage: cache status|invalidate", std::move(parsed));
                const auto verb = std::string_view{argv_[++index]};
                if(verb == "status")
                    parsed.do_cache_status = true;
                else if(verb == "invalidate")
                    parsed.do_cache_invalidate = true;
                else
                    return failure(1, "Usage: cache status|invalidate", std::move(parsed));
            }
            else if(argument == "static")
                parsed.linkage = toolchain::linkage::static_;
            else if(argument == "--include-examples")
                parsed.include_examples = true;
            else if(argument == "--build-tests")
                parsed.build_tests = true;
            else if(argument == "--tests")
                parsed.clean_tests_only = true;
            else if(argument.starts_with("--jobs="))
            {
                const auto text = argument.substr(std::string_view{"--jobs="}.size());
                auto value = 0;
                const auto [_, error] =
                    std::from_chars(text.data(), text.data() + text.size(), value);
                if(error != std::errc{} or value < 1)
                    return failure(
                        2,
                        "--jobs expects a positive integer, got: "s + std::string{text},
                        std::move(parsed));
                parsed.max_jobs = value;
                parsed.jobs_explicit = true;
            }
            else if(argument.starts_with("--modules="))
            {
                const auto text = argument.substr(std::string_view{"--modules="}.size());
                if(text == "two-phase")
                    parsed.module_phases = toolchain::module_compilation::two_phase;
                else if(text == "one-phase")
                    parsed.module_phases = toolchain::module_compilation::one_phase;
                else
                    return failure(
                        2,
                        "--modules expects one-phase or two-phase, got: "s
                            + std::string{text},
                        std::move(parsed));
            }
            else if(parsed.do_run_tests and is_test_runner_token(argument))
                parsed.test_runner_args.emplace_back(argv_[index]);
            else if(argument == "-I" or argument == "--include")
            {
                if(index + 1 >= argc_)
                    return failure(
                        1, "Missing path after -I/--include", std::move(parsed));
                parsed.include_paths.emplace_back(argv_[++index]);
            }
            else if(argument == "--link-flags")
            {
                if(index + 1 >= argc_)
                    return failure(
                        1, "Missing flags after --link-flags", std::move(parsed));
                parsed.extra_link_flags = cb::flags::codec::parse(argv_[++index]);
            }
            else if(argument == "--compile-flags" or argument == "--extra-compile-flags")
            {
                if(index + 1 >= argc_)
                    return failure(
                        1, "Missing flags after --compile-flags", std::move(parsed));
                parsed.extra_compile_flags = cb::flags::codec::parse(argv_[++index]);
            }
            else if(argument.starts_with("--compile-flags=")
                    or argument.starts_with("--extra-compile-flags="))
            {
                const auto equals = argument.find('=');
                parsed.extra_compile_flags =
                    cb::flags::codec::parse(argument.substr(equals + 1));
            }
            else if(argument == "help" or argument == "-h" or argument == "--help")
                return {.status = parse_status::help, .parsed = std::move(parsed)};
            else
            {
                auto message = "Unknown argument: "s + std::string{argument};
                if(argument.starts_with("--tag") and not argument.starts_with("--tags="))
                    message += " (did you mean --tags=<filter>?)";
                message += "\nRun with --help for usage.";
                return failure(2, std::move(message), std::move(parsed));
            }
        }

        return {.status = parse_status::ok, .parsed = std::move(parsed)};
    }

    void write_help(std::ostream& out) const
    {
        const auto program = argc_ > 0 ? argv_[0] : "cb";
        out << "Usage: " << program << " [std.cppm] [options]\n\n"
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
            << "  " << program << " debug build\n"
            << "  " << program << " release build\n"
            << "  " << program << " release build --build-tests\n"
            << "  " << program << " -I include/path debug build\n"
            << "  " << program << " -I path1 -I path2 debug build\n"
            << "  " << program << " clean build\n"
            << "  " << program << " clean --tests\n"
            << "  " << program << " ci\n"
            << "  " << program << " test\n"
            << "  " << program << " test --tags=[module]\n"
            << "  " << program << " test --jsonl=failures --tags=[module]\n"
            << "  " << program << " test --jsonl=failures --junit=report.xml --tags=[module]\n"
            << "  " << program << " debug build --jsonl=summary\n"
            << "  " << program << " test --jsonl=trace --slowest=10\n"
            << "  " << program << " clean\n";
    }

private:
    static parse_result failure(int exit_code,
                                std::string message,
                                options parsed = {})
    {
        return {.status = parse_status::error,
                .parsed = std::move(parsed),
                .error = parse_error{exit_code, std::move(message)}};
    }

    static bool is_cb_token(std::string_view arg)
    {
        return arg == "release" or arg == "debug" or arg == "ci" or arg == "clean"
            or arg == "build" or arg == "list" or arg == "test" or arg == "cache"
            or arg == "status" or arg == "invalidate" or arg == "static"
            or arg == "help" or arg == "-h" or arg == "--help"
            or arg == "--include-examples" or arg == "--build-tests" or arg == "--tests"
            or arg == "-I" or arg == "--include" or arg == "--link-flags"
            or arg == "--compile-flags" or arg == "--extra-compile-flags"
            or arg == "--jsonl" or arg.starts_with("--jsonl=")
            or arg.starts_with("--jobs=") or arg.starts_with("--modules=");
    }

    static bool is_test_runner_token(std::string_view arg)
    {
        return arg == "--list" or arg == "--jsonl" or arg == "--result" or arg == "--help"
            or arg.starts_with("--tags=")
            or arg.starts_with("--slowest=")
            or arg.starts_with("--jobs=")
            or arg.starts_with("--jsonl=")
            or arg.starts_with("--jsonl-output-max-bytes=")
            or arg.starts_with("--junit=")
            or arg.starts_with("--xunit-xml=");
    }

    int argc_ = 0;
    char** argv_ = nullptr;
};

} // namespace cli

} // namespace cb

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
        // Default console until parse decides --jsonl; needed so parse/usage errors notify.
        cb::output::select_observer("console");

        const auto cli_parser = cb::cli::parser{argc, argv};
        const auto result = cli_parser.parse();
        const auto& opts = result.parsed;
        if(opts.jsonl_mode)
            jsonl_observer.set_mode(*opts.jsonl_mode);
        if(not cb::output::select_observer(opts.output_name))
        {
            cb::output::notify(&cb::output::observer::error,
                "Unknown output observer: "s + opts.output_name);
            return 1;
        }

        if(result.status == cb::cli::parse_status::help)
        {
            cli_parser.write_help(std::cout);
            return 0;
        }
        if(result.status == cb::cli::parse_status::error)
        {
            cb::output::notify(&cb::output::observer::error, result.error->message);
            return result.error->exit_code;
        }

        auto build_system = cb::build_system{opts.build_settings()};

        if(opts.do_list)
            build_system.list_sources();
        if(opts.do_cache_status)
        {
            build_system.cache_status();
            return 0;
        }
        if(opts.do_cache_invalidate)
        {
            build_system.cache_invalidate();
            return 0;
        }
        if(opts.clean_tests_only and not opts.do_clean)
        {
            cb::output::notify(&cb::output::observer::error, "--tests requires clean");
            return 2;
        }
        if(opts.do_clean)
        {
            if(opts.clean_tests_only)
                build_system.clean_tests();
            else
                build_system.clean();
        }
        if(opts.do_build)
            build_system.build();
        if(opts.do_run_tests)
        {
            if(not build_system.run_tests(opts.runner_arguments()))
                return 1;
        }
        if(not opts.has_action())
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
