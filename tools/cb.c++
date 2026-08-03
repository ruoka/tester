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
#include <expected>
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
#include <functional>
#include <iterator>
#include <ranges>
#include <span>
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
using output::notify;
using output::observer;

// ============================================================================
// Project naming conventions — edit these for other layouts
// ============================================================================

using suffix_list = std::vector<std::string_view>;
using string_list = std::vector<std::string>;
using string_span = std::span<const std::string>;

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

// Language extensions replaced by the driver object extension (`.o` / `.obj`) when naming
// object files (any prefix such as `.test` / `.impl` is kept). Edit alongside
// `supported_suffixes` for other conventions.
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
constexpr auto depfile_suffix = ".d"sv;
constexpr auto compile_log_suffix = ".log"sv;
constexpr auto link_log_suffix = ".link.log"sv;

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

// One syscall for "missing or unreadable" versus exists() + last_write_time().
std::optional<fs::file_time_type> file_time(const fs::path& path)
{
    auto error = std::error_code{};
    const auto stamp = fs::last_write_time(path, error);
    if(error)
        return std::nullopt;
    return stamp;
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
    try
    {
        auto file = std::ofstream{tmp};
        if(not file)
            throw std::runtime_error{"Cannot open "s + std::string{what} + " temporary file: " + tmp};

        write_contents(file);

        file.close();
        if(not file)
            throw std::runtime_error{"Failed to write "s + std::string{what} + " temporary file: " + tmp};

        auto error = std::error_code{};
        fs::rename(tmp, path, error);
        if(error)
            throw std::runtime_error{"Failed to replace "s + std::string{what} + ": " + error.message()};
    }
    catch(...)
    {
        auto ignored = std::error_code{};
        fs::remove(tmp, ignored);
        throw;
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

    static std::string serialize(string_span flags)
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

namespace source {
class scanner;

class translation_unit {
public:
    static bool is_supported(std::string_view filename);
    std::string_view kind_name() const;

    friend class scanner;

    // File identity. Non-const so vectors can move these through collect/order; the private
    // constructor and friend scanner still own how a unit is born.
    std::string filename;
    std::string path;
    std::string suffix;
    std::string base_name;
    std::string full_path;
    // How reports name this unit: the source relative to the project root. Computed once
    // because the inventory and every rebuild sentence ask for the same string.
    std::string display_path;
    std::string unit;

    // Module information
    std::string module;
    string_list imports;

    // File properties
    unit_kind kind = unit_kind::non_module;
    bool has_main = false;
    bool is_test = false;
    bool is_modular = false;

    // Metadata (artifact paths live in scanned_project::artifacts)
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
using unit_index = std::flat_map<std::string_view, std::reference_wrapper<const translation_unit>, std::less<>>;

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

bool translation_unit::is_supported(std::string_view filename) {
    return supported_suffix(filename).has_value();
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
      // collect() walks a weakly_canonical root, so the absolute entry path needs only
      // lexical cleanup — not another symlink-resolving weakly_canonical per source.
      full_path(fs::path{full_path}.lexically_normal().string()),
      display_path(make_display_path(this->path, this->filename)),
      unit(make_unit(module_value, kind_value, this->filename)),
      module(std::move(module_value)),
      imports(std::move(imports_value)),
      kind(kind_value),
      has_main(has_main_flag),
      is_test(determine_is_test(this->path, this->filename, this->suffix)),
      is_modular(kind_value == unit_kind::interface_unit or kind_value == unit_kind::partition_unit),
      last_modified([&] {
          if(auto stamp = detail::file_time(full_path))
              return *stamp;
          throw std::runtime_error{"cannot read modification time"};
      }()) {}

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
    using unit_map = std::flat_map<std::string, std::reference_wrapper<translation_unit>, std::less<>>;
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
    // another literal, which requires lexical state; this pass deliberately remains lexical-state
    // free. The cost is a body ending a line with `)\`, read as closing early;
    // deciding it the other way cost more, freezing every later splice after a fake opener.
    static std::string splice_physical_lines(std::string_view text)
    {
        const auto is_horizontal_whitespace = [](char character)
        {
            return character == ' ' or character == '\t'
                or character == '\v' or character == '\f';
        };

        auto result = std::string{};
        result.reserve(text.size());
        auto cursor = std::size_t{};
        while(cursor < text.size())
        {
            const auto backslash = text.find('\\', cursor);
            if(backslash == std::string_view::npos)
            {
                result.append(text.substr(cursor));
                break;
            }

            result.append(text.substr(cursor, backslash - cursor));
            auto newline = backslash + 1;
            while(newline < text.size() and is_horizontal_whitespace(text[newline]))
                ++newline;
            if(newline < text.size() and text[newline] == '\r')
                ++newline;

            if(newline < text.size() and text[newline] == '\n')
                cursor = newline + 1;
            else
            {
                result.push_back('\\');
                cursor = backslash + 1;
            }
        }
        return result;
    }

    // std::regex over a view of the cleaned text: views::split yields contiguous subranges, so a
    // pointer pair matches in place instead of materialising a std::string per line.
    static bool search(std::string_view text, const std::regex& pattern)
    {
        return std::regex_search(text.data(), text.data() + text.size(), pattern);
    }

    static bool search(std::string_view text, std::cmatch& match, const std::regex& pattern)
    {
        return std::regex_search(text.data(), text.data() + text.size(), match, pattern);
    }

    // One left-to-right pass over the whole preamble, because a block comment and a raw string
    // still span lines after splicing and cannot be recognised a line at a time. Scanning in one
    // direction also gets the interleaving right for free: whichever construct opens first wins,
    // so `"/*"` inside a string starts no comment, and a quote inside a comment stays inert.
    //
    // A raw string's body has no escapes, so only `)delimiter"` closes it. `//` comments and quoted
    // literals end at the newline, a continued one having already been joined. Escapes still matter
    // in the quoted forms (`"\""` closes nothing).
    //
    // Written as a scanner rather than one alternating std::regex: the regex form is O(n) in
    // theory but ~170x slower here in practice (127 ms vs 0.7 ms over this project's sources),
    // which made it the single largest CPU cost of a build, cached or not. A construct is only
    // consumed once its closer is found, so an unterminated quote or `/*` is left as ordinary
    // text — the same answer the alternation gave by failing to match.
    static std::string strip_comments_and_literals(std::string_view text)
    {
        // Comments collapse to a single space, since a comment separates tokens
        // (`import/*x*/foo`); a literal keeps its delimiters but loses contents that may spell
        // `import foo;`, which the unanchored matchers would otherwise find.
        auto result = std::string{};
        result.reserve(text.size());

        const auto is_delimiter_char = [](char character)
        {
            return character != '(' and character != ')' and character != '\\' and character != '"'
                and character != ' ' and character != '\t' and character != '\r' and character != '\n';
        };

        auto cursor = std::size_t{};
        while(cursor < text.size())
        {
            const auto character = text[cursor];
            const auto next = cursor + 1 < text.size() ? text[cursor + 1] : '\0';

            if(character == '/' and next == '/')
            {
                const auto newline = text.find('\n', cursor);
                cursor = newline == std::string_view::npos ? text.size() : newline;
                result.push_back(' ');
                continue;
            }
            if(character == '/' and next == '*')
            {
                const auto close = text.find("*/"sv, cursor + 2);
                if(close != std::string_view::npos)
                {
                    cursor = close + 2;
                    result.push_back(' ');
                    continue;
                }
            }
            else if(character == 'R' and next == '"')
            {
                const auto opener = cursor + 2;
                auto delimiter_end = opener;
                while(delimiter_end < text.size() and delimiter_end - opener < 16
                      and is_delimiter_char(text[delimiter_end]))
                    ++delimiter_end;
                if(delimiter_end < text.size() and text[delimiter_end] == '(')
                {
                    auto closer = ")"s;
                    closer.append(text.substr(opener, delimiter_end - opener));
                    closer.push_back('"');
                    if(const auto close = text.find(closer, delimiter_end + 1);
                       close != std::string_view::npos)
                    {
                        cursor = close + closer.size();
                        result.append(" \"\""sv);
                        continue;
                    }
                }
            }
            else if(character == '"' or character == '\'')
            {
                if(const auto close = quoted_end(text, cursor); close != std::string_view::npos)
                {
                    cursor = close;
                    result.push_back(' ');
                    result.push_back(character);
                    result.push_back(character);
                    continue;
                }
            }

            result.push_back(character);
            ++cursor;
        }
        return result;
    }

    // One past the closing quote, or npos when the literal does not close on its line.
    static std::size_t quoted_end(std::string_view text, std::size_t opener)
    {
        const auto quote = text[opener];
        for(auto cursor = opener + 1; cursor < text.size(); ++cursor)
        {
            if(text[cursor] == '\\')
            {
                ++cursor;
                continue;
            }
            if(text[cursor] == quote)
                return cursor + 1;
            if(text[cursor] == '\n')
                break;
        }
        return std::string_view::npos;
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
        bool is_inactive(std::string_view line)
        {
            // Directives are a tiny minority of lines; the cheap check keeps the regex off
            // the hot path of a scan that runs on every build, cached or not.
            if(line.contains('#'))
            {
                auto m = std::cmatch{};
                if(std::regex_match(line.data(), line.data() + line.size(), m, directive_regex))
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
            // Canonicalize once so each source's absolute path is root + relative, without a
            // weakly_canonical walk per entry.
            const auto root = fs::path{detail::canonical_path(source_root_)};
            auto error = std::error_code{};
            if(not fs::is_directory(root, error) or error)
                return units;

            const auto& root_native = root.native();
            const auto root_prefix = root_native.size() + 1; // skip the separating '/'

            auto entries = fs::recursive_directory_iterator{root};
            const auto end = fs::recursive_directory_iterator{};
            for(; entries != end; ++entries)
            {
                const auto& entry = *entries;
                const auto& native = entry.path().native();
                // Iterator yields paths under root; a bare root entry has no relative suffix.
                const auto rel_path = native.size() > root_native.size()
                    ? std::string_view{native}.substr(std::min(root_prefix, native.size()))
                    : std::string_view{};

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
                const auto slash = rel_path.rfind('/');
                const auto filename = slash == std::string_view::npos
                    ? rel_path
                    : rel_path.substr(slash + 1);
                if(not entry.is_regular_file()
                   or is_excluded_source_path(rel_path)
                   or not translation_unit::is_supported(filename))
                    continue;

                try
                {
                    auto tu = parse(rel_path, entry.path());
                    if(tu.is_test and not include_tests_)
                        continue;
                    units.push_back(std::move(tu));
                }
                catch(const std::exception& error)
                {
                    notify(&observer::warning, "Skipping {}: {}", entry.path().string(), error.what());
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
                const translation_unit& prior = unit_to_tu.at(tu.unit);
                throw std::runtime_error{
                    "Duplicate translation unit key '" + tu.unit + "' from "
                    + prior.display_path + " and " + tu.display_path
                    + " (object/module names must stay unique)"};
            }
            unit_to_tu.emplace(tu.unit, tu);
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

                translation_unit& tu = unit_to_tu.at(unit);
                tu.dependency_level = level;
                // unit_map keys are independent string copies; each unit is dequeued once.
                sorted.push_back(std::move(tu));

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

    static translation_unit parse(std::string_view relative_path, const fs::path& file_path)
    {
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
            const auto code_line = std::string_view{part};
            if(conditionals.is_inactive(code_line))
                continue;
            const auto trimmed = trim(code_line);
            if(trimmed.empty())
                continue;

            // A line that does not spell the keyword cannot match the pattern that requires it,
            // and a substring scan rejects the overwhelming majority for a fraction of what
            // entering std::regex costs. Measured over this project: 61 ms down to 0.2 ms.
            if(not has_main and code_line.contains("main"sv)
               and search(code_line, translation_unit::main_regex))
                has_main = true;
            if(seen_real_code)
                continue;

            // Every preamble pattern requires one of these two keywords, so a line carrying
            // neither cannot match any of them.
            if(code_line.contains("module"sv) or code_line.contains("import"sv))
            {
                auto match = std::cmatch{};
                if(search(code_line, match, translation_unit::fragment_regex))
                {
                    if(kind == unit_kind::non_module)
                        kind = unit_kind::global_fragment;
                }
                else if(search(code_line, match, translation_unit::export_module_regex)
                        and match.size() > 1)
                {
                    module_name = match[1].str();
                    kind = module_name.contains(':')
                        ? unit_kind::partition_unit
                        : unit_kind::interface_unit;
                }
                else if(search(code_line, match, translation_unit::module_regex)
                        and match.size() > 1)
                {
                    const auto module = match[1].str();
                    if(kind == unit_kind::non_module or kind == unit_kind::global_fragment)
                    {
                        module_name = module;
                        kind = unit_kind::implementation_unit;
                    }
                }
                else if(search(code_line, match, translation_unit::import_regex)
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
            }

            // End the preamble only after recording any module/import on this line, and never
            // inside a global module fragment: it may contain declarations before export module.
            if(kind != unit_kind::global_fragment)
            {
                if(trimmed.contains('{')
                   or search(trimmed, translation_unit::keyword_regex)
                   or search(trimmed, translation_unit::using_namespace_regex))
                    seen_real_code = true;
            }
        }

        if((kind == unit_kind::interface_unit or kind == unit_kind::partition_unit)
           and module_name.empty())
            throw std::runtime_error{"module interface/partition missing module name"};
        if(kind == unit_kind::implementation_unit and module_name.empty())
            throw std::runtime_error{"implementation unit missing module name"};

        return translation_unit{
            fs::path{relative_path},
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

namespace process {

class runner;

// Owned by process — used by runner argv joining and by toolchain probe argv.
inline std::string shell_quote(std::string_view arg)
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

} // namespace process

namespace build_tree {

// Object / BMI / executable paths for one scanned unit. Owned by build_system, not the TU.
struct unit_artifacts
{
    std::string object{};
    std::string bmi{};
    std::string executable{};
};

using artifact_index = std::flat_map<std::string_view, unit_artifacts, std::less<>>;

} // namespace build_tree

namespace toolchain {

// How a modular interface becomes a BMI and an object. two_phase runs `--precompile` and then
// compiles the BMI; one_phase asks for both artefacts from one `-c -fmodule-output=` read.
// Two-phase can publish the BMI while its object still compiles; one-phase saves a second parse.
enum class module_compilation : unsigned char { two_phase, one_phase };

enum class linkage : unsigned char { dynamic, static_ };

using module_link_flags = std::flat_map<std::string, std::string, std::less<>>;

// What the driver needs from a provider interface — filled by build_system from TU + artifacts.
// imports is a span (not a reference member) so module_interface_map values stay assignable.
struct module_provider
{
    std::string_view module{};
    string_span imports{};
    std::string_view bmi_path{};
};

using module_interface_map = std::flat_map<std::string, module_provider, std::less<>>;

enum class compile_output : unsigned char { bmi, object };

// Per clang invocation: which artefact the log names, and whether BMI / readiness flips.
struct compile_step
{
    compile_output output = compile_output::object;
    bool writes_bmi = false;
    bool dependencies_ready = false;
};

// Filled by the concrete driver (`clang_driver::artifacts()`); build_tree only places them.
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

class clang_driver
{
public:
    // Clang/libc++ BMI and object naming for this host — selected with the driver, not build_tree.
    static constexpr artifact_conventions artifacts()
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

    // Non-owning snapshot of post-construction identity and flag lists.
    struct state
    {
        const std::string& toolchain_root;
        const std::string& compiler;
        const std::string& compiler_signature;
        const std::string& std_module_source;
        const std::string& std_module_profile;
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

    // Factory: emit probe argv, let runner execute, return an immutable driver.
    static clang_driver make(
        settings values,
        std::string_view std_bmi_path,
        std::string_view module_cache_dir,
        std::string_view probe_capture_path,
        const process::runner& runner);

    static string_list compiler_candidates(std::string_view llvm_prefix)
    {
        auto candidates = string_list{};
        for(const auto* env_name : {"LLVM_CXX", "CXX"})
        {
            if(auto value = std::getenv(env_name); value and *value)
                candidates.emplace_back(value);
        }
        candidates.push_back(std::string{llvm_prefix} + "/bin/clang++");
        return candidates;
    }

    // Empty ⇒ caller checks fs::exists; otherwise a shell `command -v` probe.
    static string_list lookup_argv(std::string_view candidate)
    {
        if(candidate.contains('/'))
            return {};
        return {"/bin/sh", "-c", "command -v " + process::shell_quote(candidate)};
    }

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

    state state() const
    {
        return {
            llvm_prefix_,
            compiler_,
            compiler_signature_,
            std_module_source_,
            std_module_profile_,
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

    // One or two clang invocations; argv is moved into `step` (no plan vector).
    void compile(const source::translation_unit& tu,
                 const build_tree::unit_artifacts& artifacts,
                 const module_interface_map& interfaces,
                 bool object_only,
                 std::invocable<string_list, compile_step> auto&& step) const
    {
        const auto imports = module_file_flags(tu, interfaces);
        if(not tu.is_modular)
        {
            step(compile_source_object_argv(tu, artifacts, imports),
                 {.dependencies_ready = true});
            return;
        }
        if(module_phases_ == module_compilation::one_phase)
        {
            step(compile_module_object_argv(tu, artifacts, imports),
                 {.writes_bmi = true, .dependencies_ready = true});
            return;
        }
        if(object_only)
        {
            step(compile_bmi_object_argv(artifacts.bmi, artifacts.object, imports),
                 {.dependencies_ready = true});
            return;
        }
        step(compile_bmi_argv(tu, artifacts, imports),
             {.output = compile_output::bmi, .writes_bmi = true, .dependencies_ready = true});
        step(compile_bmi_object_argv(artifacts.bmi, artifacts.object, imports), {});
    }

    void compile_standard_module(std::string_view std_bmi_path,
                                 std::string_view std_object_path,
                                 bool object_only,
                                 std::invocable<string_list, compile_step> auto&& step) const
    {
        if(module_phases_ == module_compilation::one_phase)
        {
            step(compile_std_module_object_argv(std_bmi_path, std_object_path),
                 {.writes_bmi = true});
            return;
        }
        if(object_only)
        {
            step(compile_bmi_object_argv(std_bmi_path, std_object_path), {});
            return;
        }
        step(compile_std_bmi_argv(std_bmi_path),
             {.output = compile_output::bmi, .writes_bmi = true});
        step(compile_bmi_object_argv(std_bmi_path, std_object_path), {});
    }

    string_list compile_database_argv(const source::translation_unit& tu,
                                      const build_tree::unit_artifacts& artifacts,
                                      const module_interface_map& interfaces) const
    {
        const auto imports = module_file_flags(tu, interfaces);
        if(not tu.is_modular)
            return compile_source_object_argv(tu, artifacts, imports);
        return module_phases_ == module_compilation::two_phase
            ? compile_bmi_argv(tu, artifacts, imports)
            : compile_module_object_argv(tu, artifacts, imports);
    }

    string_list link_argv(std::string_view executable_path,
                          string_span input_paths,
                          string_span import_flags) const
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
    inline static constexpr auto prebuilt_module_path_flag_prefix = "-fprebuilt-module-path="sv;

    // Prefers `make`: std source and llvm prefix are already resolved there.
    clang_driver(settings values,
                 std::string_view std_bmi_path,
                 std::string_view module_cache_dir,
                 std::string compiler,
                 std::string llvm_prefix)
        : release_{values.release},
          linkage_{values.link},
          module_phases_{values.module_phases},
          std_module_source_{std::move(values.std_module_source)},
          llvm_prefix_{std::move(llvm_prefix)},
          compiler_{std::move(compiler)},
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
        const auto canonical_std_module = fs::weakly_canonical(std_module_source_).string();
        std_module_profile_ = canonical_std_module + '@' + binary_signature(canonical_std_module);
        compiler_signature_ = binary_signature(compiler_);
        initialize_flags(std_bmi_path, module_cache_dir);
    }

    static std::string binary_signature(const std::string& path)
    {
        auto error = std::error_code{};
        const auto stamp = fs::last_write_time(path, error);
        if(error)
            return {};
        const auto size = fs::file_size(path, error);
        if(error)
            return {};
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count();
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

    static std::string resolve_std_module_source(std::string source)
    {
        if(source.empty())
        {
            if(auto env = std::getenv("LLVM_PATH"); env and *env)
                source = env;
            else
                throw std::runtime_error{
                    "std.cppm path not provided. Pass it as the first argument or set LLVM_PATH."};
        }
        if(not fs::exists(source))
            throw std::runtime_error{"std.cppm not found at: " + source};
        return source;
    }

    static std::string llvm_prefix_for(std::string_view std_module_source)
    {
        auto prefix_path = fs::path{std_module_source};
        for(auto level = 0; level < 4 and prefix_path.has_parent_path(); ++level)
            prefix_path = prefix_path.parent_path();
        return prefix_path.string();
    }

    void initialize_flags(std::string_view std_bmi_path,
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
            module_file_flag(std_module_name, std_bmi_path),
            std::string{prebuilt_module_path_flag_prefix} + std::string{module_cache_dir},
        };
    }

    static string_list with_precompile(string_list argv, std::string_view bmi_path)
    {
        argv.push_back("--precompile");
        argv.push_back("-o");
        argv.emplace_back(bmi_path);
        return argv;
    }

    static string_list with_c_output(string_list argv, std::string_view object_path)
    {
        argv.push_back("-c");
        argv.push_back("-o");
        argv.emplace_back(object_path);
        return argv;
    }

    string_list compile_bmi_argv(const source::translation_unit& tu,
                                 const build_tree::unit_artifacts& artifacts,
                                 string_span imports) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags_);
        argv.append_range(imports);
        argv.append_range(depfile_argv(artifacts.object));
        argv.push_back(tu.full_path);
        return with_precompile(std::move(argv), artifacts.bmi);
    }

    // Shared by project units and std: BMI → object (omit imports for std).
    string_list compile_bmi_object_argv(std::string_view bmi_path,
                                        std::string_view object_path,
                                        string_span imports = {}) const
    {
        auto argv = string_list{compiler_};
        argv.append_range(compile_flags_);
        argv.append_range(module_flags_);
        argv.append_range(imports);
        argv.emplace_back(bmi_path);
        return with_c_output(std::move(argv), object_path);
    }

    string_list compile_module_object_argv(const source::translation_unit& tu,
                                           const build_tree::unit_artifacts& artifacts,
                                           string_span imports) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags_);
        argv.append_range(imports);
        argv.append_range(depfile_argv(artifacts.object));
        argv.push_back("-fmodule-output=" + artifacts.bmi);
        argv.push_back(tu.full_path);
        return with_c_output(std::move(argv), artifacts.object);
    }

    string_list compile_source_object_argv(const source::translation_unit& tu,
                                           const build_tree::unit_artifacts& artifacts,
                                           string_span imports) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags_);
        argv.append_range(imports);
        argv.append_range(depfile_argv(artifacts.object));
        argv.push_back(tu.full_path);
        return with_c_output(std::move(argv), artifacts.object);
    }

    string_list compile_std_bmi_argv(std::string_view std_bmi_path) const
    {
        return with_precompile(std_module_source_argv(), std_bmi_path);
    }

    string_list compile_std_module_object_argv(std::string_view std_bmi_path,
                                               std::string_view std_object_path) const
    {
        auto argv = std_module_source_argv();
        argv.push_back("-fmodule-output=" + std::string{std_bmi_path});
        return with_c_output(std::move(argv), std_object_path);
    }

    string_list base_compile_argv() const
    {
        auto argv = string_list{compiler_};
        argv.append_range(compile_flags_);
        argv.append_range(cpp_flags_);
        return argv;
    }

    // Transitive imports (+ implementation unit's primary interface) → -fmodule-file=.
    // Walks names already owned by the TUs; no string copies of module/BMI paths.
    string_list module_file_flags(const source::translation_unit& tu,
                                  const module_interface_map& interfaces) const
    {
        auto pending = std::vector<std::string_view>{};
        auto seen = std::flat_set<std::string_view, std::less<>>{};
        auto enqueue = [&](std::string_view module_name)
        {
            if(module_name.empty() or module_name == std_module_name)
                return;
            if(not seen.insert(module_name).second)
                return;
            pending.push_back(module_name);
        };

        for(const auto& imp : tu.imports)
            enqueue(imp);
        if(tu.kind == unit_kind::implementation_unit)
            enqueue(tu.module);

        auto flags = string_list{};
        for(std::size_t i = 0; i < pending.size(); ++i)
        {
            const auto name = pending[i];
            if(not interfaces.contains(name))
                continue;
            const auto& dep = interfaces.at(name);
            flags.push_back(module_file_flag(dep.module, dep.bmi_path));
            for(const auto& imp : dep.imports)
                enqueue(imp);
        }
        return flags;
    }

    static string_list depfile_argv(std::string_view object_path)
    {
        return {"-MMD", "-MF", std::string{object_path} + std::string{depfile_suffix}};
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
    std::string std_module_profile_{};
    string_list compile_flags_{};
    string_list link_flags_{};
    string_list cpp_flags_{};
    string_list module_flags_{};
    string_list extra_compile_flags_{};
    string_list extra_link_flags_{};
};

} // namespace toolchain

namespace build_tree {

class paths
{
public:
    paths(std::string_view configuration, toolchain::artifact_conventions naming)
        : root{std::string{build_root_prefix} + std::string{toolchain::host_os()} + '-'
               + std::string{configuration}},
          bmi{root / "bmi"},
          obj{root / "obj"},
          bin{root / "bin"},
          cache{root / "cache"},
          naming_{naming}
    {}

    std::string std_bmi() const
    {
        return (bmi / suffixed(std_module_name, naming_.bmi_extension)).string();
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

    std::string bmi_file(const source::translation_unit& tu) const
    {
        if(tu.module.empty())
            throw std::logic_error{
                "bmi_file called on translation unit without module: " + tu.filename};
        return (bmi / suffixed(module_safe_name(tu.module), naming_.bmi_extension)).string();
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
    fs::path bmi;
    fs::path obj;
    fs::path bin;
    fs::path cache;

private:
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

} // namespace build_tree

namespace output {

// Observers format four of a unit's fields, so they receive those four and not the unit: the
// rest is build state, and cb-observer.h++ stays independent of the scanner. The bmi path is the
// one derived field, and deriving it here is why compile_start and compile_end cannot disagree.
compile_unit compile_unit_of(const source::translation_unit& tu,
                             const build_tree::unit_artifacts& artifacts)
{
    return {.source = tu.full_path,
            .object = artifacts.object,
            .bmi = tu.is_modular ? std::string_view{artifacts.bmi} : std::string_view{},
            .module = tu.module,
            .display_path = tu.display_path};
}

source_unit source_unit_of(const source::translation_unit& tu)
{
    return {.unit = tu.unit,
            .path = tu.display_path,
            .module = tu.module,
            .kind = tu.kind_name(),
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
            profile_change_ = profile{*header.value}.diff(profile{current_profile_});
            return;
        }

        auto line = ""s;
        while(std::getline(file, line))
        {
            auto entry_path = ""s;
            auto ticks = 0ll;
            // Keep the entry without a filesystem probe: a missing source still produces a
            // rebuild reason when analyzed, which is the correct outcome.
            if(parse_entry(line, entry_path, ticks))
                entries_[entry_path] = fs::file_time_type{std::chrono::nanoseconds{ticks}};
        }
    }

    void save() const
    {
        file_.replace("object cache", [&](std::ostream& file) {
            file << profile_header_prefix << current_profile_ << "\n";
            // Entries were recorded from units CB itself built; no need to re-stat them.
            for(const auto& [entry_path, timestamp] : entries_)
            {
                const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.time_since_epoch()).count();
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

// Wire format for a link cache entry: objects|flags=…|link=…|modules=…|imports=…|format=…
// Writer and reader share this type so field order cannot silently degrade telemetry.
class link_signature
{
public:
    std::string objects{};
    // Body after "|flags=" and before "|imports=": compile|link=…|modules=…
    std::string flags{};
    std::string imports{};
    std::string format{};

    inline static constexpr auto format_v2 = "cb-link-v2"sv;

    std::string serialize() const
    {
        return objects + "|flags=" + flags + "|imports=" + imports + "|format=" + format;
    }

    static std::optional<link_signature> parse(std::string_view text)
    {
        constexpr auto flags_mark = "|flags="sv;
        constexpr auto imports_mark = "|imports="sv;
        constexpr auto format_mark = "|format="sv;
        const auto flags_at = text.find(flags_mark);
        if(flags_at == std::string_view::npos)
            return std::nullopt;
        const auto imports_at = text.find(imports_mark, flags_at + flags_mark.size());
        if(imports_at == std::string_view::npos)
            return std::nullopt;
        const auto format_at = text.find(format_mark, imports_at + imports_mark.size());
        if(format_at == std::string_view::npos)
            return std::nullopt;

        auto parsed = link_signature{};
        parsed.objects = std::string{text.substr(0, flags_at)};
        parsed.flags = std::string{text.substr(
            flags_at + flags_mark.size(),
            imports_at - (flags_at + flags_mark.size()))};
        parsed.imports = std::string{text.substr(
            imports_at + imports_mark.size(),
            format_at - (imports_at + imports_mark.size()))};
        parsed.format = std::string{text.substr(format_at + format_mark.size())};
        return parsed;
    }

    // nullopt when identical; otherwise which part of the stamp moved.
    std::optional<output::rebuild_kind> compare(const link_signature& other) const
    {
        if(objects != other.objects)
            return output::rebuild_kind::object_changed;
        if(flags != other.flags or imports != other.imports or format != other.format)
            return output::rebuild_kind::link_flags_changed;
        return std::nullopt;
    }
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
        flag_tail_ = cb::flags::codec::serialize(flags.compile_flags);
        flag_tail_ += "|link=";
        flag_tail_ += cb::flags::codec::serialize(flags.link_flags);
        flag_tail_ += "|modules=";
        flag_tail_ += cb::flags::codec::serialize(flags.module_flags);
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

    // Loaded snapshot only — call before parallel remember(), or on a single-threaded path.
    std::optional<std::string> remembered(std::string_view executable) const
    {
        if(not entries_.contains(executable))
            return std::nullopt;
        return entries_.at(executable);
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
        if(input_path.empty())
            return input_path + ":missing";
        const auto stamp = detail::file_time(input_path);
        if(not stamp)
            return input_path + ":missing";
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(stamp->time_since_epoch()).count();
        return input_path + ":" + std::to_string(timestamp);
    }

    std::string dependency_signatures_joined(string_span paths) const
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
    std::string signature_for(string_span input_paths, string_span import_flags) const
    {
        return link_signature{
            .objects = dependency_signatures_joined(input_paths),
            .flags = flag_tail_,
            .imports = cb::flags::codec::serialize(import_flags),
            .format = std::string{link_signature::format_v2},
        }.serialize();
    }

private:
    inline static constexpr auto filename = "executable-cache.txt"sv;

    storage_file file_;
    std::string flag_tail_{};
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
        fs::path bmi;
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
                          std::string bmi_path,
                          std::string object_path)
        : profile_file_{std::move(cache_dir), profile_filename},
          bmi_path_{std::move(bmi_path)},
          object_path_{std::move(object_path)}
    {}

    const std::string& profile_path() const { return profile_file_.path(); }
    const std::string& bmi_path() const { return bmi_path_; }
    const std::string& object_path() const { return object_path_; }

    // Whether the std BMI was precompiled by the profile a project unit is compiled with.
    // The BMI's own presence is a separate question, asked by analyzer::rebuild_reason_for.
    bool profile_matches(std::string_view object_profile) const
    {
        const auto stored = profile_file_.read_first_line();
        return not stored.empty() and stored == object_profile;
    }

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
            .bmi = directory / fs::path{bmi_path_}.filename(),
            .object = directory / fs::path{object_path_}.filename(),
            .profile = storage_file{(directory / shared_profile_filename).string()}};
    }

    bool materialize(const shared_slot& slot,
                     std::string_view shared_profile) const
    {
        if(slot.profile.read_first_line() != shared_profile)
            return false;
        auto error = std::error_code{};
        if(not fs::is_regular_file(slot.bmi, error) or error)
            return false;
        error.clear();
        if(not fs::is_regular_file(slot.object, error) or error)
            return false;

        fs::create_directories(fs::path{bmi_path_}.parent_path());
        fs::create_directories(fs::path{object_path_}.parent_path());
        const auto nonce = shared_nonce();
        const auto temporary_bmi = fs::path{bmi_path_ + ".shared-" + nonce};
        const auto temporary_object = fs::path{object_path_ + ".shared-" + nonce};
        if(not link_or_copy_file(slot.bmi, temporary_bmi)
           or not link_or_copy_file(slot.object, temporary_object))
        {
            fs::remove(temporary_bmi, error);
            fs::remove(temporary_object, error);
            return false;
        }

        fs::remove(bmi_path_, error);
        error.clear();
        fs::rename(temporary_bmi, bmi_path_, error);
        if(error)
        {
            fs::remove(temporary_bmi, error);
            fs::remove(temporary_object, error);
            return false;
        }
        fs::remove(object_path_, error);
        error.clear();
        fs::rename(temporary_object, object_path_, error);
        if(error)
        {
            fs::remove(bmi_path_, error);
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
        if(reason.kind != output::rebuild_kind::own_bmi_missing)
            return {};
        const auto root = shared_root();
        if(not root)
            return {};
        const auto shared_profile = std::forward<decltype(shared_profile_of)>(shared_profile_of)();
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
        const auto shared_profile = std::forward<decltype(shared_profile_of)>(shared_profile_of)();
        try
        {
            const auto slot = shared_slot_for(*root, shared_profile);
            if(slot.profile.read_first_line() == shared_profile
               and fs::is_regular_file(slot.bmi)
               and fs::is_regular_file(slot.object))
                return {.ok = true};

            fs::create_directories(slot.directory.parent_path());
            auto staging = slot.directory;
            staging += ".tmp-" + shared_nonce();
            auto error = std::error_code{};
            if(not fs::create_directories(staging, error) or error)
                return {};

            const auto staged_bmi = staging / slot.bmi.filename();
            const auto staged_object = staging / slot.object.filename();
            const auto staged_profile = staging / shared_profile_filename;
            if(not link_or_copy_file(bmi_path_, staged_bmi)
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
        auto value = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
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
    std::string bmi_path_;
    std::string object_path_;
};

class compiler_stamp_store
{
public:
    explicit compiler_stamp_store(std::string cache_dir)
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

// Std BMI/object freshness — no project artifact index or memo state. Safe to call while a
// scan rebuilds scanned_project / analyzer on another thread.
std::optional<output::rebuild_info> rebuild_reason_for_standard_module(
    const standard_module_store& std_modules,
    std::string_view source_path,
    std::invocable auto&& object_profile_of)
{
    const auto module_of = [&](output::rebuild_kind kind) {
        return output::rebuild_info{
            .kind = kind,
            .module = std::string{std_module_name},
            .bmi_path = std_modules.bmi_path()};
    };

    const auto bmi_time = detail::file_time(std_modules.bmi_path());
    if(not bmi_time)
        return module_of(output::rebuild_kind::own_bmi_missing);
    if(not std_modules.profile_matches(std::forward<decltype(object_profile_of)>(object_profile_of)()))
        return module_of(output::rebuild_kind::profile_change);
    if(*bmi_time < fs::last_write_time(source_path))
        return module_of(output::rebuild_kind::own_bmi_stale);
    const auto object_time = detail::file_time(std_modules.object_path());
    if(not object_time)
        return module_of(output::rebuild_kind::object_missing);
    if(*object_time < *bmi_time)
        return module_of(output::rebuild_kind::object_stale);
    return std::nullopt;
}

// Project-TU freshness (memoized) and link stamp compare. Needs the artifact index.
class analyzer
{
public:
    analyzer(std::string source_root,
             std::string bmi_root,
             const build_tree::artifact_index& unit_artifacts)
        : source_root_{detail::canonical_path(source_root)},
          bmi_root_{detail::canonical_path(bmi_root)},
          unit_artifacts_{unit_artifacts}
    {}

    const build_tree::unit_artifacts& artifacts_of(const source::translation_unit& tu) const
    {
        return unit_artifacts_.at(tu.unit);
    }

    // Reads the loaded link snapshot only — call before parallel remember(), or single-threaded.
    std::optional<output::rebuild_info> rebuild_reason_for(
        const link_store& links,
        std::string_view executable_path,
        const std::string& signature) const
    {
        if(not fs::exists(executable_path))
            return output::rebuild_info{.kind = output::rebuild_kind::missing_executable};

        const auto previous = links.remembered(executable_path);
        if(not previous)
            return output::rebuild_info{.kind = output::rebuild_kind::not_in_cache};
        if(*previous == signature)
            return std::nullopt;

        const auto previous_sig = link_signature::parse(*previous);
        const auto current_sig = link_signature::parse(signature);
        if(not previous_sig or not current_sig)
            return output::rebuild_info{.kind = output::rebuild_kind::signature_changed};
        if(auto kind = previous_sig->compare(*current_sig))
            return output::rebuild_info{.kind = *kind};
        return std::nullopt;
    }

    // Consumers ask about the same provider once per path that reaches it, so an unmemoized
    // walk costs the number of paths through the import DAG rather than the number of units
    // — 2291 decisions for 36 units here, and worse as a graph deepens. A decision only
    // changes when that unit's own artefacts change, which is what artifacts_changed reports.
    std::optional<output::rebuild_info> rebuild_reason_for(
        const source::translation_unit& tu,
        const object_store& loaded,
        const source::unit_index& units) const
    {
        {
            auto lock = std::lock_guard<std::mutex>{decisions_mutex_};
            if(decisions_.contains(tu.unit))
                return decisions_.at(tu.unit);
        }
        auto reason = decide_rebuild(tu, loaded, units);
        {
            auto lock = std::lock_guard<std::mutex>{decisions_mutex_};
            decisions_.insert_or_assign(tu.unit, reason);
        }
        return reason;
    }

    // A compiled unit's object and BMI have moved, so importers must decide against the new
    // state — the same answer an unmemoized walk would reach by re-reading the filesystem.
    void artifacts_changed(const source::translation_unit& tu) const
    {
        auto lock = std::lock_guard<std::mutex>{decisions_mutex_};
        decisions_.erase(tu.unit);
    }

private:
    std::optional<output::rebuild_info> decide_rebuild(
        const source::translation_unit& tu,
        const object_store& loaded,
        const source::unit_index& units) const
    {
        const auto& art = artifacts_of(tu);
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
        const auto object_stamp = detail::file_time(art.object);
        const auto object_absent = not object_stamp.has_value();
        auto object_timestamp = object_stamp.value_or(fs::file_time_type{});
        if(object_absent)
        {
            if(not tu.is_modular)
                return output::rebuild_info{
                    .kind = output::rebuild_kind::object_missing,
                    .trigger_path = tu.full_path};
        }
        else if(not tu.is_modular)
        {
            if(object_timestamp < loaded.entries().at(tu.full_path))
                return output::rebuild_info{
                    .kind = output::rebuild_kind::object_stale,
                    .trigger_path = tu.full_path};
            if(auto header_reason = stale_header(tu, art, object_timestamp))
                return header_reason;
        }

        // Implementation units consume their primary interface even without an explicit import.
        if(tu.kind == unit_kind::implementation_unit and units.contains(tu.module))
        {
            const source::translation_unit& interface = units.at(tu.module);
            const auto& interface_art = artifacts_of(interface);
            const auto interface_bmi = detail::file_time(interface_art.bmi);
            if(not interface_bmi)
                return bmi_rebuild(output::rebuild_kind::dependency_bmi_stale, interface, interface_art);
            if(*interface_bmi > object_timestamp)
                return bmi_rebuild(output::rebuild_kind::bmi_stale, interface, interface_art);
            if(auto interface_reason = rebuild_reason_for(interface, loaded, units))
                return attributed_to(*interface_reason, interface);
        }

        auto freshness_timestamp = object_timestamp;
        auto object_stale_vs_bmi = false;
        if(tu.is_modular)
        {
            const auto bmi_stamp = detail::file_time(art.bmi);
            if(not bmi_stamp)
                return bmi_rebuild(output::rebuild_kind::own_bmi_missing, tu, art);
            const auto bmi_timestamp = *bmi_stamp;
            if(bmi_timestamp < tu.last_modified)
                return bmi_rebuild(output::rebuild_kind::own_bmi_stale, tu, art);
            if(object_absent)
                freshness_timestamp = bmi_timestamp;
            else if(object_timestamp < bmi_timestamp)
            {
                freshness_timestamp = bmi_timestamp;
                object_stale_vs_bmi = true;
            }

            if(auto header_reason = stale_header(tu, art, freshness_timestamp))
                return header_reason;
        }

        auto visited = std::flat_set<std::string>{};
        if(auto stale = transitive_bmi_newer_than_object(
               tu, freshness_timestamp, units, visited))
            return stale;

        for(const auto& dependency_key : tu.imports)
        {
            if(not units.contains(dependency_key))
                continue;

            const source::translation_unit& dependency = units.at(dependency_key);
            if(dependency.is_modular)
            {
                const auto& dependency_art = artifacts_of(dependency);
                const auto dependency_bmi = detail::file_time(dependency_art.bmi);
                if(not dependency_bmi or *dependency_bmi < dependency.last_modified)
                    return bmi_rebuild(output::rebuild_kind::dependency_bmi_stale, dependency, dependency_art);
            }
            if(auto dependency_reason = rebuild_reason_for(dependency, loaded, units))
                return attributed_to(*dependency_reason, dependency);
        }

        if(object_absent)
            return output::rebuild_info{
                .kind = output::rebuild_kind::object_missing,
                .trigger_path = tu.full_path};
        if(object_stale_vs_bmi)
            return bmi_rebuild(output::rebuild_kind::object_stale, tu, art);
        if(object_timestamp < loaded.entries().at(tu.full_path))
            return output::rebuild_info{
                .kind = output::rebuild_kind::object_stale,
                .trigger_path = tu.full_path};
        return std::nullopt;
    }

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

    static output::rebuild_info bmi_rebuild(output::rebuild_kind kind,
                                            const source::translation_unit& tu,
                                            const build_tree::unit_artifacts& artifacts)
    {
        return {.kind = kind,
                .module = tu.module,
                .bmi_path = artifacts.bmi,
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

    std::optional<output::rebuild_info> transitive_bmi_newer_than_object(
        const source::translation_unit& tu,
        fs::file_time_type object_timestamp,
        const source::unit_index& units,
        std::flat_set<std::string>& visited) const
    {
        for(const auto& dependency_key : tu.imports)
        {
            if(not units.contains(dependency_key))
                continue;

            const source::translation_unit& dependency = units.at(dependency_key);
            if(dependency.is_modular)
            {
                const auto& dependency_art = artifacts_of(dependency);
                if(const auto dependency_bmi = detail::file_time(dependency_art.bmi);
                   dependency_bmi and *dependency_bmi > object_timestamp)
                    return bmi_rebuild(output::rebuild_kind::bmi_stale, dependency, dependency_art);
            }

            if(visited.contains(dependency.unit))
                continue;
            visited.insert(dependency.unit);
            if(auto stale = transitive_bmi_newer_than_object(
                   dependency, object_timestamp, units, visited))
                return stale;
        }
        return std::nullopt;
    }

    std::optional<output::rebuild_info> stale_header(
        const source::translation_unit& tu,
        const build_tree::unit_artifacts& artifacts,
        fs::file_time_type freshness_timestamp) const
    {
        const auto depfile = build_tree::paths::depfile(artifacts.object);
        const auto prerequisites = parse_depfile(depfile);
        if(not prerequisites)
            return output::rebuild_info{
                .kind = output::rebuild_kind::depfile_unusable,
                .trigger_path = depfile};

        for(const auto& prerequisite : *prerequisites)
        {
            const auto resolved = resolved_prerequisite(prerequisite);
            // Explicitly mapped BMIs are module-graph inputs, not textual headers.
            if(resolved == tu.full_path
               or not detail::path_at_or_under_root(resolved, source_root_)
               or detail::path_at_or_under_root(resolved, bmi_root_))
                continue;

            const auto timestamp = detail::file_time(resolved);
            if(not timestamp)
                return output::rebuild_info{
                    .kind = output::rebuild_kind::header_missing,
                    .trigger_path = resolved};
            if(*timestamp > freshness_timestamp)
                return output::rebuild_info{
                    .kind = output::rebuild_kind::header_stale,
                    .trigger_path = resolved};
        }
        return std::nullopt;
    }

    // Project headers are shared, so the same prerequisite arrives from many depfiles.
    // weakly_canonical stats every path component (~13.6 us here against 0.4 us for a
    // lexical normalization), which is worth paying once per distinct path rather than
    // per occurrence. Resolution stays canonical: these are compared against a canonical
    // source root, and a lexical spelling would miss a symlinked header.
    std::string resolved_prerequisite(const std::string& prerequisite) const
    {
        {
            auto lock = std::lock_guard<std::mutex>{resolved_mutex_};
            if(resolved_.contains(prerequisite))
                return resolved_.at(prerequisite);
        }
        auto resolved = detail::canonical_path(prerequisite);
        {
            auto lock = std::lock_guard<std::mutex>{resolved_mutex_};
            resolved_.insert_or_assign(prerequisite, resolved);
        }
        return resolved;
    }

    std::string source_root_;
    std::string bmi_root_;
    const build_tree::artifact_index& unit_artifacts_;
    // Decisions and resolved paths are shared by the compile workers; the analyzer itself
    // stays logically const, so both caches lock rather than serialize the callers.
    mutable std::mutex decisions_mutex_{};
    mutable std::flat_map<std::string_view, std::optional<output::rebuild_info>, std::less<>> decisions_{};
    mutable std::mutex resolved_mutex_{};
    mutable std::flat_map<std::string, std::string, std::less<>> resolved_{};
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

inline std::size_t worker_count(std::ptrdiff_t job_limit, std::size_t job_count)
{
    return std::min(job_count, static_cast<std::size_t>(std::max<std::ptrdiff_t>(1, job_limit)));
}

// First exception wins; peers stop claiming once failed() is set. Rethrow after joins.
class failure_latch
{
public:
    bool failed() const { return failed_.load(std::memory_order_relaxed); }

    void capture()
    {
        failed_.store(true, std::memory_order_relaxed);
        auto lock = std::lock_guard<std::mutex>{mutex_};
        if(not failure_)
            failure_ = std::current_exception();
    }

    void rethrow() const
    {
        if(failure_)
            std::rethrow_exception(failure_);
    }

private:
    std::atomic_bool failed_{false};
    std::exception_ptr failure_{};
    mutable std::mutex mutex_{};
};

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

        // Jobs outlive workers; reference_wrappers avoid copying rebuild decisions.
        auto items = std::vector<std::reference_wrapper<const job_type>>{};
        for(const auto& job : jobs)
            items.push_back(job);
        if(items.empty())
            return;

        const auto workers = worker_count(limit, items.size());
        auto next = std::atomic_size_t{0};
        auto failures = failure_latch{};

        run_workers(workers, [&]()
        {
            while(not failures.failed())
            {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if(index >= items.size())
                    return;
                // A peer may have failed while this worker claimed the next index.
                if(failures.failed())
                    return;
                try
                {
                    work(items[index].get());
                }
                catch(...)
                {
                    failures.capture();
                }
            }
        });
        failures.rethrow();
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
        notify(&observer::build_end, ok, interval{started, std::chrono::steady_clock::now()});
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
        notify(&observer::test_end, result, interval{started, std::chrono::steady_clock::now()});
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
        string_span argv,
        std::string_view capture_path = {}) const
    {
        if(argv.empty())
            throw std::logic_error{"invoke_shell: empty argv"};

        auto cmd_str = join_argv(argv);
        auto shell_line = cmd_str;
        if(not capture_path.empty())
            shell_line += " > " + shell_quote(capture_path) + " 2>&1";

        notify(&observer::command, cmd_str);
        notify(&observer::command_start, cmd_str, argv);

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

        notify(&observer::command_end, cmd_str, argv, result, output::interval{started, finished});
        return result;
    }

    // Every toolchain command is a step of a reported build phase: its output is attached to
    // the reporting scope before a failure is thrown. Takes argv by value so callers can move.
    void run_step(
        output::step_scope auto& scope,
        string_list argv,
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
    static std::string join_argv(string_span argv)
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

        auto text = std::string{
            std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
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
        string_span argv,
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

namespace toolchain {

clang_driver clang_driver::make(
    settings values,
    std::string_view std_bmi_path,
    std::string_view module_cache_dir,
    std::string_view probe_capture_path,
    const process::runner& runner)
{
    values.std_module_source = resolve_std_module_source(std::move(values.std_module_source));
    const auto prefix = llvm_prefix_for(values.std_module_source);

    if(not probe_capture_path.empty())
        fs::create_directories(fs::path{probe_capture_path}.parent_path());

    std::string compiler;
    for(const auto& candidate : compiler_candidates(prefix))
    {
        const auto argv = lookup_argv(candidate);
        const auto ok = argv.empty()
            ? fs::exists(candidate)
            : runner.invoke_shell(argv, probe_capture_path).ok();
        if(ok)
        {
            compiler = candidate;
            break;
        }
    }
    if(compiler.empty())
    {
        throw std::runtime_error{
            "clang++ not found. Expected: " + prefix + "/bin/clang++"
            + " (set LLVM_CXX to override)."};
    }

    return clang_driver{
        std::move(values),
        std_bmi_path,
        module_cache_dir,
        std::move(compiler),
        prefix};
}

} // namespace toolchain

// One scan's units plus every view derived from them. scan_and_order replaces the bundle as a
// unit — never mutate `units` without rebuilding artifacts / by_unit / interfaces.
struct scanned_project
{
    source::translation_unit_list units{};
    build_tree::artifact_index artifacts{};
    source::unit_index by_unit{};
    toolchain::module_interface_map interfaces{};

    const build_tree::unit_artifacts& artifacts_of(const source::translation_unit& tu) const
    {
        return artifacts.at(tu.unit);
    }
};

class build_system {
public:
    enum class build_config { debug, release };

    struct settings
    {
        build_config config{build_config::debug};
        std::string source_dir{"."};
        bool include_examples{};
        bool include_tests_for_build{};

        std::string std_module_source{};
        string_list include_paths{};
        toolchain::module_link_flags module_link_flags{};
        toolchain::linkage linkage{toolchain::linkage::dynamic};
        // Changing schemes rebuilds every modular unit.
        toolchain::module_compilation module_phases{toolchain::module_compilation::two_phase};
        string_list extra_compile_flags{};
        string_list extra_link_flags{};

        // Zero requests the hardware-concurrency default.
        int max_jobs{};
    };

private:

    std::string source_dir;
    toolchain::module_link_flags module_ldflags;
    bool toolchain_profile_probed = false;
    std::string compiler_version{};
    scanned_project project_{};
    // Recreated after every scan; holds a reference into project_.artifacts.
    std::optional<cache::analyzer> analyzer_{};
    const build_config config;
    const build_tree::paths artifact_paths;
    bool include_tests = false;
    bool include_examples = false;
    const bool include_tests_for_build;
    int max_jobs = 0;
    process::runner process_runner;
    toolchain::clang_driver driver;
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

    void ensure_toolchain_profile()
    {
        if(toolchain_profile_probed)
            return;
        toolchain_profile_probed = true;

        fs::create_directories(artifact_paths.cache);

        const auto stamp = cache::compiler_stamp_store{artifact_paths.cache.string()};
        if(process_runner.invoke_shell(driver.version_argv(), stamp.path()).ok())
        {
            compiler_version = stamp.read();
            // Surface the probe's first line in console output.
            if(not compiler_version.empty())
                notify(&observer::info, compiler_version);
        }
    }

    void report_toolchain_configuration() const
    {
        const auto state = driver.state();
        const auto static_note = driver.static_linking() ? " (static C++ stdlib)"s : ""s;
        notify(&observer::info, "Building {} configuration{}",
               config == build_config::release ? "RELEASE" : "DEBUG", static_note);
        for(const auto& warning : driver.warnings())
            notify(&observer::warning, warning);
        if(not state.extra_link.empty())
            notify(&observer::info, "Added extra linker flags: {}", cb::flags::codec::serialize(state.extra_link));
        if(not state.extra_compile.empty())
            notify(&observer::info, "Added extra compile flags: {}", cb::flags::codec::serialize(state.extra_compile));
    }

    // Utilities

    cache::analyzer& freshness()
    {
        if(not analyzer_)
            throw std::logic_error{"analyzer requested before scan"};
        return *analyzer_;
    }

    string_list test_runner_argv(const std::string& runner, string_span args) const
    {
        auto argv = string_list{};
        argv.push_back(runner);
        argv.append_range(args);
        return argv;
    }

    auto test_units() const
    {
        return project_.units
            | std::views::filter([](const source::translation_unit& tu) { return tu.is_test and not tu.has_main; });
    }

    string_list test_object_paths() const
    {
        return test_units()
            | std::views::transform([&](const source::translation_unit& tu) { return project_.artifacts_of(tu).object; })
            | std::ranges::to<string_list>();
    }

    string_list linkable_object_paths() const
    {
        return project_.units
            | std::views::filter([](const source::translation_unit& tu) { return not tu.has_main and not tu.is_test; })
            | std::views::transform([&](const source::translation_unit& tu) { return project_.artifacts_of(tu).object; })
            | std::ranges::to<string_list>();
    }

    string_list executable_link_inputs(const source::translation_unit& main,
                                       string_span shared_objects) const
    {
        auto inputs = string_list{project_.artifacts_of(main).object};
        inputs.append_range(shared_objects);
        inputs.push_back(artifact_paths.std_object());
        return inputs;
    }

    string_list test_runner_link_inputs(const source::translation_unit& runner) const
    {
        auto inputs = string_list{project_.artifacts_of(runner).object};
        inputs.append_range(linkable_object_paths());
        inputs.append_range(test_object_paths());
        inputs.push_back(artifact_paths.std_object());
        return inputs;
    }

    string_list collect_module_ldflags(string_span imp) const
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

    std::string cache_profile(bool include_project_includes) {
        ensure_toolchain_profile();
        const auto state = driver.state();
        return cache::profile{cache::profile::ingredients{
            .config = config_name(),
            .static_link = driver.static_linking(),
            .module_phases = driver.module_phases_name(),
            .toolchain_root = state.toolchain_root,
            .compiler = state.compiler,
            .compiler_signature = state.compiler_signature,
            .compiler_version = compiler_version,
            .std_module = state.std_module_profile,
            .compile_flags = state.compile,
            .cpp_flags = include_project_includes ? &state.cpp : nullptr,
        }}.text();
    }

    std::string object_cache_profile() { return cache_profile(true); }
    std::string shared_std_cache_profile() { return cache_profile(false); }

    cache::object_store make_object_store() const
    {
        return cache::object_store{artifact_paths.cache.string()};
    }

    cache::link_store make_link_store() const
    {
        const auto state = driver.state();
        return cache::link_store{
            artifact_paths.cache.string(),
            {.compile_flags = state.compile,
             .link_flags = state.link,
             .module_flags = state.modules}};
    }

    cache::standard_module_store make_standard_module_store() const
    {
        return cache::standard_module_store{
            artifact_paths.cache.string(),
            artifact_paths.std_bmi(),
            artifact_paths.std_object()};
    }

    // Dependency analysis

    void fill_project_artifacts()
    {
        project_.artifacts.clear();
        auto object_owners = std::flat_map<std::string, std::string, std::less<>>{};
        auto bmi_owners = std::flat_map<std::string, std::string, std::less<>>{};
        auto executable_owners = std::flat_map<std::string, std::string, std::less<>>{};

        // Reserve the libc++ std module artifacts so a project TU named `std`
        // (std.c++ / export module std;) cannot silently overwrite them.
        object_owners.emplace(artifact_paths.std_object(), "reserved std module object");
        bmi_owners.emplace(artifact_paths.std_bmi(), "reserved std module BMI");

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

        for(const auto& tu : project_.units)
        {
            auto artifacts = build_tree::unit_artifacts{};
            const auto& source_label = tu.display_path;
            artifacts.object = artifact_paths.object(tu);
            claim(object_owners, artifacts.object, source_label, "object");

            if(tu.is_modular)
            {
                if(tu.module.empty())
                    throw std::logic_error{"modular unit missing module name: " + tu.filename};
                artifacts.bmi = artifact_paths.bmi_file(tu);
                claim(bmi_owners, artifacts.bmi, source_label, "BMI");
            }
            if(tu.has_main)
            {
                artifacts.executable = artifact_paths.executable(tu);
                claim(executable_owners, artifacts.executable, source_label, "executable");
            }
            if(tu.kind == unit_kind::implementation_unit and tu.module.empty())
                throw std::logic_error{"implementation unit missing module name: " + tu.filename};

            project_.artifacts.emplace(std::string_view{tu.unit}, std::move(artifacts));
        }
    }

    void fill_project_indexes()
    {
        project_.by_unit.clear();
        for(const auto& tu : project_.units)
            project_.by_unit.emplace(std::string_view{tu.unit}, tu);

        project_.interfaces = project_.units
            | std::views::filter([](const source::translation_unit& tu) { return tu.is_modular; })
            | std::views::transform([&](const source::translation_unit& tu)
            {
                return std::pair{tu.module, toolchain::module_provider{
                    .module = tu.module,
                    .imports = tu.imports,
                    .bmi_path = project_.artifacts_of(tu).bmi,
                }};
            })
            | std::ranges::to<toolchain::module_interface_map>();
    }

    void scan_and_order()
    {
        // Drop derived views before replacing units so no alias outlives its storage.
        analyzer_.reset();
        project_.by_unit.clear();
        project_.interfaces.clear();
        project_.artifacts.clear();
        project_.units = source::scanner{source_dir, include_tests, include_examples}.scan();
        fill_project_artifacts();
        fill_project_indexes();
        analyzer_.emplace(source_dir, artifact_paths.bmi.string(), project_.artifacts);
    }

    // Standard library module

    // Observers see what they see for a project modular unit: one source, one BMI, one object.
    // The strings are the caller's, because compile_unit holds views.
    output::compile_unit std_compile_unit(const std::string& bmi,
                                          const std::string& object,
                                          const std::string& display) const
    {
        return {.source = driver.state().std_module_source,
                .object = object,
                .bmi = bmi,
                .module = std_module_name,
                .display_path = display};
    }

    void notify_std_cache_warning(const cache::standard_module_store::transfer_result& result) const
    {
        if(not result.warning.empty())
            notify(&observer::warning, result.warning);
    }

    void build_std_module()
    {
        const auto state = driver.state();
        const auto bmi = artifact_paths.std_bmi();
        const auto object = artifact_paths.std_object();
        const auto display = fs::path{state.std_module_source}.filename().string();
        const auto unit = std_compile_unit(bmi, object, display);
        auto std_store = make_standard_module_store();
        const auto reason = cache::rebuild_reason_for_standard_module(
            std_store, state.std_module_source, [&]() { return object_cache_profile(); });

        if(not reason)
        {
            const auto hit = output::compile_scope{unit};
            return;
        }
        // The store only attempts hydration for a missing local BMI. In particular, cache
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
        const auto object_only = reason->kind == output::rebuild_kind::object_missing or reason->kind == output::rebuild_kind::object_stale;
        driver.compile_standard_module(
            bmi, object, object_only,
            [&](string_list argv, toolchain::compile_step step)
            {
                process_runner.run_step(
                    compile,
                    std::move(argv),
                    build_tree::paths::compile_log(step.output == toolchain::compile_output::bmi ? unit.bmi : unit.object));
                if(step.writes_bmi)
                    std_store.save_profile(object_cache_profile());
            });
        notify_std_cache_warning(
            std_store.publish([&]() { return shared_std_cache_profile(); }));
        compile.succeeded();
    }

    // Compilation

    // Edge-driven readiness: a unit is claimable once every provider has published.
    // Two-phase modular units publish after --precompile; everything else publishes when done.
    class compile_schedule
    {
    public:
        explicit compile_schedule(const source::translation_unit_list& units)
            : unit_count_{units.size()},
              dependents_(unit_count_),
              dependencies_remaining_(unit_count_),
              published_(unit_count_)
        {
            auto unit_to_index = std::flat_map<std::string, std::size_t, std::less<>>{};
            for(auto index = std::size_t{0}; index < unit_count_; ++index)
                unit_to_index[units[index].unit] = index;

            for(auto index = std::size_t{0}; index < unit_count_; ++index)
            {
                source::scanner::for_each_provider(
                    units[index], unit_to_index, [&](const std::string& provider)
                {
                    dependents_[unit_to_index.at(provider)].push_back(index);
                    ++dependencies_remaining_[index];
                });
            }

            for(auto index = std::size_t{0}; index < unit_count_; ++index)
                if(dependencies_remaining_[index] == 0)
                    ready_.push(index);
            if(ready_.empty())
                throw std::runtime_error{"No dependency-free translation unit"};
        }

        std::optional<std::size_t> claim(const execution::failure_latch& failures)
        {
            auto lock = std::unique_lock<std::mutex>{mutex_};
            changed_.wait(lock, [&] {
                return failures.failed() or not ready_.empty() or completed_ == unit_count_;
            });
            if(failures.failed() or completed_ == unit_count_)
                return std::nullopt;
            const auto index = ready_.front();
            ready_.pop();
            return index;
        }

        void publish(std::size_t provider, const execution::failure_latch& failures)
        {
            {
                auto lock = std::lock_guard<std::mutex>{mutex_};
                if(published_[provider] != 0)
                    return;
                published_[provider] = 1;
                if(failures.failed())
                    return;
                for(const auto dependent : dependents_[provider])
                {
                    if(--dependencies_remaining_[dependent] == 0)
                        ready_.push(dependent);
                }
            }
            changed_.notify_all();
        }

        void complete_one()
        {
            {
                auto lock = std::lock_guard<std::mutex>{mutex_};
                ++completed_;
            }
            changed_.notify_all();
        }

        void wake() { changed_.notify_all(); }

        std::size_t unit_count() const { return unit_count_; }

    private:
        std::size_t unit_count_ = 0;
        std::vector<std::vector<std::size_t>> dependents_{};
        std::vector<std::size_t> dependencies_remaining_{};
        std::queue<std::size_t> ready_{};
        std::vector<unsigned char> published_{};
        std::size_t completed_ = 0;
        std::mutex mutex_{};
        std::condition_variable changed_{};
    };

    void compile_one(const source::translation_unit& tu,
                     const output::rebuild_info& rebuild,
                     std::invocable auto&& on_dependency_ready) {
        const auto& artifacts = project_.artifacts_of(tu);
        const auto unit = output::compile_unit_of(tu, artifacts);
        auto compile = output::compile_scope{unit, rebuild};
        // Two-phase: object_missing / object_stale reuse the BMI that is already there —
        // same split build_std_module uses. Re-precompiling would bump the BMI mtime and
        // force every importer through bmi_stale for no interface change.
        // One-phase: the BMI is a sibling of the object (reduced on Clang 22+), not an
        // input that can be compiled to an object — always re-read the source, as std does.
        const auto object_only = rebuild.kind == output::rebuild_kind::object_missing or rebuild.kind == output::rebuild_kind::object_stale;
        driver.compile(
            tu, artifacts, project_.interfaces, object_only,
            [&](string_list argv, toolchain::compile_step step)
            {
                process_runner.run_step(
                    compile,
                    std::move(argv),
                    build_tree::paths::compile_log(step.output == toolchain::compile_output::bmi ? unit.bmi : unit.object));
                if(step.dependencies_ready)
                    on_dependency_ready();
            });
        compile.succeeded();
    }

    void compile_units() {
        if (project_.units.empty()) return;
        auto objects = make_object_store();
        objects.load(object_cache_profile());
        if(objects.missing_profile_header())
            notify(&observer::info, "Object cache missing profile header; ignoring");
        if(objects.profile_change())
            notify(&observer::profile_changed, output::rebuild_kind::profile_change,
                   *objects.profile_change());

        auto schedule = compile_schedule{project_.units};
        auto rebuilt = std::vector<unsigned char>(schedule.unit_count());
        auto failures = execution::failure_latch{};
        auto& decisions = freshness();

        execution::run_workers(
            execution::worker_count(job_limit(), schedule.unit_count()),
            [&]()
            {
                while(auto index = schedule.claim(failures))
                {
                    try
                    {
                        const auto& tu = project_.units[*index];
                        const auto reason = decisions.rebuild_reason_for(tu, objects, project_.by_unit);
                        if(reason)
                        {
                            compile_one(tu, *reason, [&]() { schedule.publish(*index, failures); });
                            decisions.artifacts_changed(tu);
                            rebuilt[*index] = 1;
                        }
                        else
                        {
                            {
                                const auto hit = output::compile_scope{output::compile_unit_of(tu, project_.artifacts_of(tu))};
                            }
                            schedule.publish(*index, failures);
                        }
                    }
                    catch(...)
                    {
                        failures.capture();
                        schedule.wake();
                        return;
                    }
                    schedule.complete_one();
                }
            });
        failures.rethrow();

        for(auto index = std::size_t{0}; index < schedule.unit_count(); ++index)
            if(rebuilt[index] != 0)
            {
                const auto& tu = project_.units[index];
                objects.record(tu.full_path, tu.last_modified);
            }
        objects.save();
    }

    // Linking

    void perform_link(const source::translation_unit& main,
                      string_span input_paths,
                      string_span import_flags,
                      const output::rebuild_info& rebuild,
                      std::string signature)
    {
        const auto& executable = project_.artifacts_of(main).executable;
        auto link = output::link_scope{executable, rebuild, std::move(signature)};
        process_runner.run_step(
            link,
            driver.link_argv(executable, input_paths, import_flags),
            build_tree::paths::link_log(executable));
        link.succeeded();
    }

    void link_executables() {
        auto shared_objects = linkable_object_paths();
        auto links = make_link_store();
        links.load();

        // Snapshot relink decisions before workers mutate the store. Interleaving
        // analyzer reads of the loaded snapshot with parallel remember() writes is a data race.
        auto& oracle = freshness();
        struct link_decision {
            const source::translation_unit& tu;
            string_list input_paths{};
            string_list import_flags{};
            std::string signature{};
            std::optional<output::rebuild_info> reason{};
        };
        auto decisions = project_.units
            // Exact base name only — substring matches like contest_runner / aaa_test_runner
            // are ordinary mains and must not be excluded from normal linking.
            | std::views::filter([&](const source::translation_unit& tu) {
                  return tu.has_main and tu.base_name != test_runner_name; })
            | std::views::transform([&](const source::translation_unit& tu) {
                  auto input_paths = executable_link_inputs(tu, shared_objects);
                  auto import_flags = collect_module_ldflags(tu.imports);
                  auto signature = links.signature_for(input_paths, import_flags);
                  auto reason = oracle.rebuild_reason_for(
                      links, project_.artifacts_of(tu).executable, signature);
                  return link_decision{
                      .tu = tu,
                      .input_paths = std::move(input_paths),
                      .import_flags = std::move(import_flags),
                      .signature = std::move(signature),
                      .reason = std::move(reason)}; })
            | std::ranges::to<std::vector>();

        for(const auto& decision : decisions)
        {
            if(decision.reason)
                continue;
            const auto hit = output::link_scope{
                project_.artifacts_of(decision.tu).executable, decision.signature};
        }

        execution::worker_pool{job_limit()}.run(
            decisions | std::views::filter([](const link_decision& decision) {
                return decision.reason.has_value();
            }),
            [&](const link_decision& decision) {
                perform_link(
                    decision.tu,
                    decision.input_paths,
                    decision.import_flags,
                    *decision.reason,
                    decision.signature);
                links.remember(project_.artifacts_of(decision.tu).executable, decision.signature);
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
        if(std::ranges::count_if(project_.units, is_runner) > 1)
            throw std::runtime_error{
                "multiple test_runner mains found — keep a single source named test_runner"};

        const auto found = std::ranges::find_if(project_.units, is_runner);
        if(found == project_.units.end())
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
        const auto signature = links.signature_for(input_paths, import_flags);
        const auto reason = freshness().rebuild_reason_for(
            links, project_.artifacts_of(runner).executable, signature);
        if(not reason)
        {
            const auto hit = output::link_scope{project_.artifacts_of(runner).executable, signature};
            return;
        }

        perform_link(runner, input_paths, import_flags, *reason, signature);
        notify(&observer::success, "test_runner linked with test objects");
        links.remember(project_.artifacts_of(runner).executable, signature);
        links.save();
    }

    // Build orchestration

    void build_steps()
    {
        fs::create_directories(artifact_paths.bmi);
        fs::create_directories(artifact_paths.obj);
        fs::create_directories(artifact_paths.bin);
        fs::create_directories(artifact_paths.cache);

        // Source discovery and std compilation share no mutable build state: the scan writes
        // project_.units while std owns only its artefacts/profile. Hide the scan
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
        if(project_.units.empty())
            throw std::runtime_error{"No sources found"};

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
              toolchain::clang_driver::artifacts()},
          include_tests{values.config == build_config::debug},
          include_examples{values.include_examples},
          include_tests_for_build{values.include_tests_for_build},
          max_jobs{values.max_jobs},
          driver{toolchain::clang_driver::make(
              toolchain::clang_driver::settings{
                  .release = values.config == build_config::release,
                  .link = values.linkage,
                  .module_phases = values.module_phases,
                  .std_module_source = std::move(values.std_module_source),
                  .include_paths = std::move(values.include_paths),
                  .extra_compile_flags = std::move(values.extra_compile_flags),
                  .extra_link_flags = std::move(values.extra_link_flags)},
              artifact_paths.std_bmi(),
              artifact_paths.bmi.string(),
              (artifact_paths.cache / "command-probe.txt").string(),
              process_runner)}
    {
        report_toolchain_configuration();
    }

    void clean() const {
        const auto& dir = artifact_paths.root;
        if (fs::exists(dir)) {
            fs::remove_all(dir);
            notify(&observer::success, "Removed {}", dir.string());
        } else {
            notify(&observer::info, "Nothing to clean for {}", dir.string());
        }
    }

    // Drop only test TU artefacts and the test_runner binary — leave library/app objects so the
    // next build recompiles tests without a full cold rebuild. Scans with include_tests on so
    // release configs still see *.test.c++ even when a normal release build would not.
    void clean_tests() {
        include_tests = true;
        if(not fs::exists(artifact_paths.obj) and not fs::exists(artifact_paths.bin))
        {
            notify(&observer::info, "Nothing to clean for test objects under {}", artifact_paths.root.string());
            return;
        }

        scan_and_order();

        // Object-cache keys are source paths (full_path), not object paths.
        auto dropped_sources = std::flat_set<std::string, std::less<>>{};
        auto removed = 0;

        const auto try_remove = [&](const std::string& path) {
            if(detail::remove_if_exists(path))
                ++removed;
        };

        for(const auto& tu : project_.units)
        {
            const auto is_runner = tu.has_main and tu.base_name == test_runner_name;
            if(not tu.is_test and not is_runner)
                continue;

            dropped_sources.insert(tu.full_path);
            const auto& art = project_.artifacts_of(tu);
            if(not art.object.empty())
            {
                try_remove(art.object);
                try_remove(build_tree::paths::depfile(art.object));
                try_remove(build_tree::paths::compile_log(art.object));
            }
            if(tu.is_modular and not art.bmi.empty())
            {
                try_remove(art.bmi);
                try_remove(build_tree::paths::compile_log(art.bmi));
            }
            if(is_runner and not art.executable.empty())
            {
                try_remove(art.executable);
                try_remove(build_tree::paths::link_log(art.executable));
            }
        }

        auto objects = make_object_store();
        if(not dropped_sources.empty() and objects.exists())
        {
            objects.load(object_cache_profile());
            if(objects.missing_profile_header())
                notify(&observer::info, "Object cache missing profile header; ignoring");
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
            notify(&observer::info, "No test objects to remove under {}", artifact_paths.root.string());
        else
            notify(&observer::success, "Removed {} test artifact(s) under {}", removed, artifact_paths.root.string());
    }

    void cache_status()
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
        const auto stamp = cache::compiler_stamp_store{artifact_paths.cache.string()};

        notify(&observer::cache_status, output::cache_inventory{
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
        // cache is. The std module profile is the one that makes CB rebuild the std BMI.
        const auto removed = output::cache_removals{
            .object_cache = make_object_store().invalidate(),
            .executable_cache = make_link_store().invalidate(),
            .compiler_stamp = cache::compiler_stamp_store{artifact_paths.cache.string()}.invalidate(),
            .std_module_profile = make_standard_module_store().invalidate()};

        notify(&observer::cache_invalidate_end, removed);
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

        notify(&observer::success, "Build completed: {}", artifact_paths.root.string());
    }

    // Returns false when the test runner reports failures (normal outcome, not exceptional).
    bool run_tests(string_span args = {}) {
        notify(&observer::info, "=== Running tests ===");

        include_tests = true;
        {
            // No check that the runner is there afterwards: link_test_runner either produced
            // it or threw, and the missing-source case is the sentence it throws.
            auto build = output::build_scope{config_name(), true, include_examples};
            build_steps();
            link_test_runner();
            build.succeeded();
        }
        notify(&observer::success, "Build completed: {}", artifact_paths.root.string());

        // From the unit link_test_runner just linked, so what runs is what was linked.
        const auto& runner = project_.artifacts_of(test_runner_unit()).executable;

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
    // second two-phase step (BMI → object) is omitted: it has no distinct source path for clangd.
    string_list compile_argv_for_database(const source::translation_unit& tu) const
    {
        return driver.compile_database_argv(tu, project_.artifacts_of(tu), project_.interfaces);
    }

    // Requires a completed scan_and_order — interfaces live on scanned_project.
    void write_compile_commands() const
    {
        const auto path = detail::join_dir(source_dir, compile_commands_filename);
        detail::write_atomic_file(path, "compile commands database", [&](std::ostream& file) {
            file << "[\n";
            auto first = true;
            for(const auto& tu : project_.units)
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
        notify(&observer::info, "Wrote {} ({} entries)", path, project_.units.size());
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
        notify(&observer::info, "Wrote {} ({} units)", path, inventory.units.size());
    }

    void list_sources() {
        scan_and_order();
        write_compile_commands();
        auto inventory = output::source_inventory{
            .config = config_name(),
            .include_tests = include_tests,
            .include_examples = include_examples,
            .source_dir = source_dir,
        };
        inventory.units.reserve(project_.units.size());
        for(const auto& tu : project_.units)
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
        notify(&observer::source_list, inventory);
    }
};

namespace cli {

class options
{
public:
    std::string std_module_source{};
    build_system::build_config config = build_system::build_config::debug;
    toolchain::module_compilation module_phases = toolchain::module_compilation::two_phase;
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
            .include_examples = include_examples,
            // Preserve action semantics: --build-tests changes an explicit build, while list,
            // cache operations and the implicit default build keep their established selection.
            .include_tests_for_build = do_build and build_tests,
            .std_module_source = std_module_source,
            .include_paths = include_paths,
            .linkage = linkage,
            .module_phases = module_phases,
            .extra_compile_flags = extra_compile_flags,
            .extra_link_flags = extra_link_flags,
            .max_jobs = max_jobs};
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

        const auto has_jsonl_mode = std::ranges::any_of(test_runner_args, [](const auto& arg) {
            return arg == "--jsonl" or arg.starts_with("--jsonl=");
        });
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
            const auto already = std::ranges::any_of(test_runner_args, [](const auto& arg) {
                return arg.starts_with("--jobs=");
            });
            if(not already)
                args.emplace_back("--jobs=" + std::to_string(max_jobs));
        }

        args.append_range(test_runner_args);
        return args;
    }
};

// Semantic CLI failure — process exit codes are chosen in main, not here.
enum class parse_error_kind : unsigned char
{
    usage,             // missing values / usage text (main → 1)
    invalid_argument,  // unrecognized or malformed args (main → 2)
};

struct parse_error
{
    parse_error_kind kind = parse_error_kind::invalid_argument;
    std::string message;
    options parsed{};
};

struct parse_success
{
    enum class disposition : unsigned char { run, help } disposition = disposition::run;
    options parsed{};
};

using parse_result = std::expected<parse_success, parse_error>;

class parser
{
public:
    parser(int argc, char* argv[])
        : command_line_{argv, static_cast<std::size_t>(argc)}
    {}

    parse_result parse() const
    {
        auto parsed = options{};
        auto arguments = command_line_.size() > 1
            ? command_line_.subspan(1)
            : std::span<char*>{};

        if(not arguments.empty())
        {
            const auto candidate = fs::path{arguments.front()};
            if(fs::exists(candidate))
            {
                parsed.std_module_source = candidate.string();
                arguments = arguments.subspan(1);
            }
            else if(candidate.extension() == ".cppm")
                return std::unexpected{parse_error{
                    .kind = parse_error_kind::invalid_argument,
                    .message = "std.cppm not found: "s + candidate.string()}};
        }

        const auto take = [&] {
            const auto value = std::string_view{arguments.front()};
            arguments = arguments.subspan(1);
            return value;
        };

        while(not arguments.empty())
        {
            const auto argument = take();
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
                    return std::unexpected{parse_error{
                        .kind = parse_error_kind::usage,
                        .message = "Unknown JSONL mode: "s + std::string{mode},
                        .parsed = std::move(parsed)}};
                parsed.output_name = "jsonl";
                continue;
            }

            if(argument == "test")
            {
                parsed.do_run_tests = true;
                if(not arguments.empty())
                {
                    const auto next = std::string_view{arguments.front()};
                    if(not is_test_runner_token(next)
                       and not is_cb_token(next)
                       and not next.starts_with("-"))
                        parsed.test_filter = std::string{take()};
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
                if(arguments.empty())
                    return std::unexpected{parse_error{
                        .kind = parse_error_kind::usage,
                        .message = "Usage: cache status|invalidate",
                        .parsed = std::move(parsed)}};
                const auto verb = take();
                if(verb == "status")
                    parsed.do_cache_status = true;
                else if(verb == "invalidate")
                    parsed.do_cache_invalidate = true;
                else
                    return std::unexpected{parse_error{
                        .kind = parse_error_kind::usage,
                        .message = "Usage: cache status|invalidate",
                        .parsed = std::move(parsed)}};
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
                const auto [_, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                if(error != std::errc{} or value < 1)
                    return std::unexpected{parse_error{
                        .kind = parse_error_kind::invalid_argument,
                        .message = "--jobs expects a positive integer, got: "s + std::string{text},
                        .parsed = std::move(parsed)}};
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
                    return std::unexpected{parse_error{
                        .kind = parse_error_kind::invalid_argument,
                        .message = "--modules expects one-phase or two-phase, got: "s + std::string{text},
                        .parsed = std::move(parsed)}};
            }
            else if(parsed.do_run_tests and is_test_runner_token(argument))
                parsed.test_runner_args.emplace_back(argument);
            else if(argument == "-I" or argument == "--include")
            {
                if(arguments.empty())
                    return std::unexpected{parse_error{
                        .kind = parse_error_kind::usage,
                        .message = "Missing path after -I/--include",
                        .parsed = std::move(parsed)}};
                parsed.include_paths.emplace_back(take());
            }
            else if(argument == "--link-flags")
            {
                if(arguments.empty())
                    return std::unexpected{parse_error{
                        .kind = parse_error_kind::usage,
                        .message = "Missing flags after --link-flags",
                        .parsed = std::move(parsed)}};
                parsed.extra_link_flags = cb::flags::codec::parse(take());
            }
            else if(argument == "--compile-flags" or argument == "--extra-compile-flags")
            {
                if(arguments.empty())
                    return std::unexpected{parse_error{
                        .kind = parse_error_kind::usage,
                        .message = "Missing flags after --compile-flags",
                        .parsed = std::move(parsed)}};
                parsed.extra_compile_flags = cb::flags::codec::parse(take());
            }
            else if(argument.starts_with("--compile-flags=")
                    or argument.starts_with("--extra-compile-flags="))
            {
                const auto equals = argument.find('=');
                parsed.extra_compile_flags = cb::flags::codec::parse(argument.substr(equals + 1));
            }
            else if(argument == "help" or argument == "-h" or argument == "--help")
                return parse_success{
                    .disposition = parse_success::disposition::help,
                    .parsed = std::move(parsed)};
            else
            {
                auto message = "Unknown argument: "s + std::string{argument};
                if(argument.starts_with("--tag") and not argument.starts_with("--tags="))
                    message += " (did you mean --tags=<filter>?)";
                message += "\nRun with --help for usage.";
                return std::unexpected{parse_error{
                    .kind = parse_error_kind::invalid_argument,
                    .message = std::move(message),
                    .parsed = std::move(parsed)}};
            }
        }

        return parse_success{.parsed = std::move(parsed)};
    }

    void write_help(std::ostream& out) const
    {
        const auto program = command_line_.empty() ? "cb" : command_line_.front();
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
            << "                   two-phase: --precompile to BMI, then BMI to object\n"
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

    std::span<char*> command_line_{};
};

} // namespace cli

} // namespace cb

using namespace std::string_literals;

int main(int argc, char* argv[])
{
    using cb::output::notify;
    using cb::output::observer;

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
        using cb::output::notify;
        using cb::output::observer;

        // Default console until parse decides --jsonl; needed so parse/usage errors notify.
        cb::output::select_observer("console");

        const auto cli_parser = cb::cli::parser{argc, argv};
        const auto result = cli_parser.parse();
        const auto& opts = result ? result->parsed : result.error().parsed;
        if(opts.jsonl_mode)
            jsonl_observer.set_mode(*opts.jsonl_mode);
        if(not cb::output::select_observer(opts.output_name))
        {
            notify(&observer::error, "Unknown output observer: {}", opts.output_name);
            return 1;
        }

        if(result and result->disposition == cb::cli::parse_success::disposition::help)
        {
            cli_parser.write_help(std::cout);
            return 0;
        }
        if(not result)
        {
            notify(&observer::error, result.error().message);
            using cb::cli::parse_error_kind;
            return result.error().kind == parse_error_kind::invalid_argument ? 2 : 1;
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
            notify(&observer::error, "--tests requires clean");
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
        notify(&observer::error, "Fatal error: {}", e.what());
        return 1;
    }
    catch (...)
    {
        notify(&observer::error, "Fatal error: unknown exception");
        return 1;
    }
}
