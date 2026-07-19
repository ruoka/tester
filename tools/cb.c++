// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
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
#include <cctype>
#include "cb-jsonl_observer.h++"
#include "cb-console_observer.h++"

namespace fs = std::filesystem;

namespace cb {

using namespace std::string_literals;
using namespace std::string_view_literals;

inline auto& jsonl_observer()
{
    static auto value = output::jsonl::observer{std::cout};
    return value;
}

inline auto& console_observer()
{
    static auto value = output::console::observer{std::cerr};
    return value;
}

enum class build_phase { none, build };

inline bool select_output_observer(std::string_view name)
{
    static const auto registered = []
    {
        output::register_observer("console", console_observer());
        output::register_observer("jsonl", jsonl_observer());
        return true;
    }();
    (void)registered;
    return output::select_observer(name);
}

enum class build_config { debug, release };

enum class unit_kind : unsigned {
    non_module,          // non-modular source, no module declaration at all  (.c++ .cpp)
    interface_unit,      // module interface, export module name;             (.c++m .cppm)
    partition_unit,      // module partition, export module name:part;        (.c++m .cppm)
    implementation_unit, // module implementation, module name;               (.impl.c++)
    global_fragment      // global module fragment, only contains "module;"   (.c++m .cppm)
};

using suffix_list = std::vector<std::string>;
using string_list = std::vector<std::string>;

namespace detail {

inline std::string shell_quote(std::string_view arg)
{
    // POSIX: wrap in single quotes; internal ' becomes '\'' (split/join idiom).
    const auto inner = arg
        | std::views::split('\'')
        | std::views::transform([](auto&& part) { return std::string_view{part}; })
        | std::views::join_with("'\\''"sv)
        | std::ranges::to<std::string>();
    return std::string{'\''} + inner + '\'';
}

inline std::string_view config_name(build_config cfg)
{
    switch(cfg)
    {
        case build_config::debug: return "debug";
        case build_config::release: return "release";
    }
    return "debug";
}

inline std::string_view unit_kind_name(unit_kind kind)
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
inline std::string collapse_whitespace(std::string_view text)
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
inline string_list parse_external_flag_text(std::string_view text)
{
    const auto normalized = collapse_whitespace(text);
    return normalized
        | std::views::split(' ')
        | std::views::transform([](auto&& part) { return std::string_view{part}; })
        | std::views::filter([](std::string_view t) { return not t.empty(); })
        | std::views::transform([](std::string_view t) { return std::string{t}; })
        | std::ranges::to<string_list>();
}

inline std::string flags_profile_string(const string_list& flags)
{
    return flags | std::views::join_with(" "sv) | std::ranges::to<std::string>();
}

inline std::string join_argv(const string_list& argv)
{
    return argv
        | std::views::transform([](const std::string& arg) { return shell_quote(arg); })
        | std::views::join_with(" "sv)
        | std::ranges::to<std::string>();
}

using profile_fields = std::flat_map<std::string, std::string, std::less<>>;

// Object-cache profile values must not contain '\t', '\n', '\r', or '%' (tab-delimited format).
inline void append_profile_field(std::string& profile, std::string_view key, std::string_view value)
{
    if(not profile.empty())
        profile += '\t';
    profile += key;
    profile += '=';
    profile += value;
}

inline std::pair<std::string, std::string> parse_profile_field(std::string_view segment)
{
    const auto eq = segment.find('=');
    return {
        std::string{segment.substr(0, eq)},
        std::string{segment.substr(eq + 1)}};
}

inline profile_fields parse_object_cache_profile_fields(std::string_view profile)
{
    return profile
        | std::views::split('\t')
        | std::views::transform([](auto&& part) { return parse_profile_field(std::string_view{part}); })
        | std::ranges::to<profile_fields>();
}

inline output::profile_token_change diff_profile_tokens(std::string_view old_text, std::string_view new_text)
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

inline output::object_cache_profile_diff diff_object_cache_profiles(std::string_view old_profile, std::string_view new_profile)
{
    const auto old_fields = parse_object_cache_profile_fields(old_profile);
    const auto new_fields = parse_object_cache_profile_fields(new_profile);
    auto diff = output::object_cache_profile_diff{};

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

inline const suffix_list supported_suffixes = {
    ".test.c++m",
    ".test.c++",
    ".impl.c++",
    ".c++m",
    ".cppm",
    ".c++",
    ".cpp"
};

inline std::string make_base_name(std::string_view filename)
{
    const auto suffix = std::ranges::find_if(supported_suffixes, [&](std::string_view s) {
        return filename.ends_with(s);
    });
    if(suffix != supported_suffixes.end())
        return std::string{filename.substr(0, filename.size() - suffix->size())};
    return std::string{filename};
}

inline std::string normalize_relative_dir(const fs::path& dir) {
    if (dir.empty()) return "";
    auto str = dir.string();
    return str == "." ? "" : str;
}

inline bool is_tester_framework_path(std::string_view path) {
    // Nested or top-level tester library trees (not *.test.c++ sources).
    return path.contains("/tester/") or path.starts_with("tester/");
}

inline bool path_has_test_segment(std::string_view path) {
    // Match path components named exactly "test" or "tests" (not "tester" / "test_exception_bug").
    auto rest = path;
    while (not rest.empty()) {
        const auto slash = rest.find('/');
        const auto segment = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        if (segment == "test" or segment == "tests")
            return true;
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    return false;
}

inline bool determine_is_test(std::string_view rel_dir, std::string_view name, std::string_view suffix_value) {
    const auto combined = rel_dir.empty() ? std::string{name} : std::string{rel_dir} + "/" + std::string{name};
    if (is_tester_framework_path(combined))
        return false;
    if (suffix_value == ".test.c++" or suffix_value == ".test.c++m")
        return true;
    return path_has_test_segment(combined);
}

inline std::string make_unit(std::string_view module_value, unit_kind kind, std::string_view filename_value) {
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

inline std::string make_full_path(const fs::path& file_path) {
    auto absolute = file_path;
    if (absolute.is_relative()) absolute = fs::absolute(absolute);
    try {
        absolute = fs::canonical(absolute);
    } catch (...) {
        absolute = fs::absolute(absolute);
    }
    return absolute.string();
}

inline std::string binary_signature(const std::string& path)
{
    if(not fs::exists(path))
        return {};

    const auto size = fs::file_size(path);
    const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
        fs::last_write_time(path).time_since_epoch()).count();
    return std::to_string(size) + ':' + std::to_string(ticks);
}

inline std::string read_first_line(const std::string& path)
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

inline constexpr std::string_view object_cache_format = "cb-object-cache-v3"sv;

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


    inline static const std::regex module_regex{R"(\s*(?:export\s+)?module\s+([\w:-]+)\s*;)"};
    inline static const std::regex export_module_regex{R"(\s*export\s+module\s+([\w:-]+)\s*;)"};
    inline static const std::regex fragment_regex{R"(\s*module\s*;)"};  // Global module fragment: just "module;"
    inline static const std::regex import_regex{R"(\s*(?:export\s+)?(?:import|module)\s+([\w:-]+)\s*;)"};
    inline static const std::regex main_regex{R"(\s*int\s+main\s*\()"};
    inline static const std::regex keyword_regex{R"(\b(class|struct|namespace|constexpr|inline|static)\b)"};
    inline static const std::regex using_namespace_regex{R"(\busing\s+namespace\b)"};
};

inline bool translation_unit::match_supported_suffix(std::string_view filename, std::string& out_suffix)
{
    const auto suffix = std::ranges::find_if(detail::supported_suffixes, [&](const std::string& s) {
        return filename.ends_with(s);
    });
    if(suffix == detail::supported_suffixes.end())
        return false;
    out_suffix.assign(suffix->data(), suffix->size());
    return true;
}

namespace detail {

inline std::string extract_suffix(std::string_view filename) {
    auto suffix = std::string{};
    if (not translation_unit::match_supported_suffix(filename, suffix))
        throw std::runtime_error{"unsupported source suffix"};
    return suffix;
}

} // namespace detail

inline bool translation_unit::is_supported(const fs::path& file_path) {
    auto name = file_path.filename().string();
    auto suffix = std::string{};
    return match_supported_suffix(name, suffix);
}

inline translation_unit::translation_unit(const fs::path& relative,
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
      unit(detail::make_unit(module_value, kind_value, this->filename)),
      module(std::move(module_value)),
      imports(std::move(imports_value)),
      kind(kind_value),
      has_main(has_main_flag),
      is_test(detail::determine_is_test(this->path, this->filename, this->suffix)),
      is_modular(kind_value == unit_kind::interface_unit or kind_value == unit_kind::partition_unit),
      last_modified(fs::last_write_time(full_path)) {}

inline translation_unit parse_translation_unit(const fs::path& project_root, const fs::path& file_path) {
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

    auto trim = [](std::string_view s) -> std::string_view {
        auto start = s.find_first_not_of(" \t\r");
        if (start == std::string::npos) return {};
        auto end = s.find_last_not_of(" \t\r");
        return s.substr(start, end - start + 1);
    };

    bool seen_real_code = false;

    while (std::getline(file, line) and ++lines_scanned < max_lines) {
        auto trimmed = trim(line);
        if (trimmed.empty() or trimmed.starts_with("//") or trimmed.starts_with("#")) continue;

        // === ALWAYS CHECK FOR main() — ON EVERY LINE ===
        if (std::regex_search(line, translation_unit::main_regex)) {
            has_main = true;
        }

        // === If we're clearly past the preamble, stop scanning for module stuff ===
        // Use regex word boundaries to avoid false matches (e.g., "struct" in "structured_log_stream")
        if (not seen_real_code) {
            // Convert string_view to string for regex_search
            auto trimmed_str = std::string{trimmed};
            if (trimmed.contains('{') or
                std::regex_search(trimmed_str, translation_unit::keyword_regex) or
                std::regex_search(trimmed_str, translation_unit::using_namespace_regex)) {
                seen_real_code = true;
            }
        }

        // === Only scan module/import if we haven't seen real code yet ===
        if (seen_real_code) continue;

        std::smatch m;
        if (std::regex_search(line, m, translation_unit::fragment_regex)) {
            if (kind == unit_kind::non_module) kind = unit_kind::global_fragment;
        }
        else if (std::regex_search(line, m, translation_unit::export_module_regex) and m.size() > 1) {
            module_name = m[1].str();
            // Allow dots in module names
            kind = module_name.contains(':') ? unit_kind::partition_unit : unit_kind::interface_unit;
        }
        else if (std::regex_search(line, m, translation_unit::module_regex) and m.size() > 1) {
            auto mod = m[1].str();
            if (kind == unit_kind::non_module or kind == unit_kind::global_fragment) {
                module_name = mod;
                kind = unit_kind::implementation_unit;
            }
        }
        else if (std::regex_search(line, m, translation_unit::import_regex) and m.size() > 1) {
            std::string imp = m[1].str();
            if (not imp.empty() and imp[0] == ':' and not module_name.empty()) {
                auto colon = module_name.find(':');
                auto base = colon != std::string::npos ? module_name.substr(0, colon) : module_name;
                imp = base + imp;
            }
            if (not imp.empty() and imp != "std") imports.push_back(std::move(imp));
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

using dependency_graph = std::flat_map<std::string, string_list, std::less<>>;
using indegree_map = std::flat_map<std::string, int, std::less<>>;
using unit_to_tu_map = std::flat_map<std::string, translation_unit*, std::less<>>;
using object_cache_map = std::flat_map<std::string, fs::file_time_type, std::less<>>;
using translation_unit_list = std::vector<translation_unit>;
using topo_sort_queue = std::queue<std::string>;
using level_groups_map = std::flat_map<int, std::vector<const translation_unit*>>;
using thread_list = std::vector<std::jthread>;
using module_to_ldflags_map = std::flat_map<std::string, std::string, std::less<>>;
using executable_cache_map = std::flat_map<std::string, std::string, std::less<>>;

class build_system {
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
    string_list extra_compile_flag_tokens;
    string_list extra_link_flag_tokens;
    std::optional<std::string> object_cache_miss_reason;
    output::object_cache_profile_diff profile_diff;
    
    // JSONL phase tracking state
    build_phase current_phase = build_phase::none;
    std::chrono::steady_clock::time_point phase_started{};
    bool build_end_emitted = false;
    
    void emit_failed_build_end()
    {
        if(current_phase == build_phase::build && !build_end_emitted)
        {
            const auto finished = std::chrono::steady_clock::now();
            output::notify(&output::observer::build_end, false, phase_started, finished);
            build_end_emitted = true;
        }
    }

    // ============================================================================
    // Initialization and Setup
    // ============================================================================

    void detect_llvm_environment() {
        // Detect and setup LLVM environment:
        // 1. Find std.cppm (libc++ standard library module source) either from argument or LLVM_PATH
        // 2. Determine LLVM prefix from std.cppm path
        // 3. Find clang++ compiler binary
        // This ensures we have all paths needed for compilation and linking
        
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
        
        // Find clang++ compiler binary
        // This is the C++ compiler we'll use for all compilation and linking
        auto command_available = [](const std::string& candidate) {
            if (not candidate.contains('/')) {
                auto test_cmd = "command -v " + candidate + " >/dev/null 2>&1";
                return system(test_cmd.c_str()) == 0;
            }
            return fs::exists(candidate);
        };
        auto try_env_compiler = [&, this]() -> bool {
            for (const auto* env_name : {"LLVM_CXX", "CXX"}) {
                if (auto value = std::getenv(env_name); value and command_available(value)) {
                    llvm_cxx = value;
                    return true;
                }
            }
            return false;
        };
        if (not try_env_compiler()) {
            llvm_cxx = llvm_prefix + "/bin/clang++";
            if (!command_available(llvm_cxx)) {
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

        const auto stamp = cache_dir() + "/compiler-version.txt";
        const auto cmd = detail::shell_quote(llvm_cxx) + " --version > " + detail::shell_quote(stamp) + " 2>/dev/null";
        if(std::system(cmd.c_str()) == 0)
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
            link_flags = {
                "-pthread",
                "-L" + llvm_prefix + "/lib",
                "-Wl,-rpath," + llvm_prefix + "/lib",
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
            "-fmodule-file=std=" + std_pcm_path(),
            "-fprebuilt-module-path=" + module_cache_dir(),
        };
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
        return "build-" + os_name() + (config == build_config::release ? "-release" : "-debug");
    }

    std::string module_cache_dir() const      { return build_root() + "/pcm"; }
    std::string object_dir() const            { return build_root() + "/obj"; }
    std::string binary_dir() const            { return build_root() + "/bin"; }
    std::string cache_dir() const             { return build_root() + "/cache"; }
    std::string object_cache_path() const     { return cache_dir() + "/object-cache.txt"; }
    std::string executable_cache_path() const { return cache_dir() + "/executable-cache.txt"; }
    std::string std_pcm_path() const          { return module_cache_dir() + "/std.pcm"; }
    std::string std_obj_path() const          { return object_dir() + "/std.o"; }

    // ============================================================================
    // Path Computation Utilities
    // ============================================================================

    std::string normalize_path(std::string_view p) const {
        auto path = fs::path{p};
        if (path.is_relative()) path = fs::absolute(path);
        try { path = fs::canonical(path); } catch(...) {}
        return path.string();
    }

    std::string module_safe_name(std::string_view module_name) const {
        auto safe = std::string{module_name};
        std::ranges::replace(safe, ':', '_');
        std::ranges::replace(safe, '-', '_');
        std::ranges::replace(safe, '.', '_');
        return safe;
    }

    std::string object_suffix(const translation_unit& tu) const {
        static constexpr std::array<std::string_view, 4> endings{".c++m"sv,".c++"sv,".cpp"sv,".cppm"sv};
        for (const auto ending : endings) {
            if (tu.suffix.ends_with(ending)) {
                auto prefix = tu.suffix.substr(0, tu.suffix.size() - ending.size());
                return std::string{prefix} + ".o";
            }
        }
        throw std::logic_error{"Unsupported suffix for object file: " + tu.suffix};
    }

    std::string compute_object_path(const translation_unit& tu) const {
        auto base = tu.is_modular ? module_safe_name(tu.module) : tu.base_name;
        return object_dir() + "/" + base + object_suffix(tu);
    }

    std::string compute_pcm_path(const translation_unit& tu) const {
        if (tu.module.empty())
            throw std::logic_error{"compute_pcm_path called on translation unit without module: " + tu.filename};
        return module_cache_dir() + "/" + module_safe_name(tu.module) + ".pcm";
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

    // Sole shell boundary: argv is non-empty (contract); join_argv quotes each element with join_with.
    int invoke_shell(const string_list& argv) const
    {
        if(argv.empty())
            throw std::logic_error{"invoke_shell: empty argv"};

        auto cmd_str = detail::join_argv(argv);
        output::notify(&output::observer::command, cmd_str);
        output::notify(&output::observer::command_start, cmd_str, argv);

        const auto started = std::chrono::steady_clock::now();
        auto r = system(cmd_str.c_str());
        const auto finished = std::chrono::steady_clock::now();

        output::notify(&output::observer::command_end, cmd_str, argv, r == 0, r, started, finished);
        return r;
    }

    void execute_system_command(const string_list& argv) const
    {
        if(const auto r = invoke_shell(argv); r)
            throw std::runtime_error{"Command failed: " + detail::join_argv(argv)};
    }

    static std::string_view rebuild_hint(std::string_view kind)
    {
        if(kind == "not_in_cache"sv)
            return "Source path not present in object cache for this config.";
        if(kind == "source_stale"sv)
            return "Source mtime newer than cached compile timestamp.";
        if(kind == "object_missing"sv)
            return "Object file missing on disk.";
        if(kind == "object_stale"sv)
            return "Object file older than cached source timestamp.";
        if(kind == "own_pcm_missing"sv)
            return "Module PCM missing on disk.";
        if(kind == "own_pcm_stale"sv)
            return "Module PCM older than its source.";
        if(kind == "pcm_stale"sv)
            return "Imported PCM newer than this object; recompile follows module graph.";
        if(kind == "dependency_pcm_stale"sv)
            return "Imported module PCM is missing or older than its source.";
        if(kind == "profile_change"sv)
            return "Object-cache toolchain profile changed; see profile_changed event.";
        if(kind == "missing_executable"sv)
            return "Linked executable missing on disk.";
        if(kind == "object_changed"sv)
            return "One or more input objects changed since the last link.";
        if(kind == "link_flags_changed"sv)
            return "Link/compile/module flags changed since the last link.";
        if(kind == "signature_changed"sv)
            return "Link signature changed since the last successful link.";
        return {};
    }

    static std::string tu_label(const translation_unit& tu)
    {
        return tu.path.empty() ? tu.filename : tu.path + "/" + tu.filename;
    }

    static std::string format_rebuild_message(const translation_unit& tu, const output::rebuild_info& info)
    {
        const auto label = tu_label(tu);
        if(info.kind == "profile_change"sv)
            return "Rebuilding " + label + " because compile profile changed";
        if(info.kind == "not_in_cache"sv)
            return "Rebuilding " + label + " because it is not in the object cache";
        if(info.kind == "source_stale"sv)
        {
            if(not info.trigger_path.empty() and info.trigger_path != tu.full_path)
                return "Rebuilding " + label + " because dependency " + info.trigger_path + " is newer than its cached object";
            return "Rebuilding " + label + " because source is newer than the cached object";
        }
        if(info.kind == "pcm_stale"sv)
            return "Rebuilding " + label + " because PCM " + info.module + " is newer than the object (import graph)";
        if(info.kind == "dependency_pcm_stale"sv)
            return "Rebuilding " + label + " because imported module " + info.module + " PCM is missing or stale";
        if(info.kind == "object_missing"sv)
            return "Rebuilding " + label + " because object file is missing";
        if(info.kind == "object_stale"sv)
            return "Rebuilding " + label + " because object file is older than the cached source timestamp";
        if(info.kind == "own_pcm_missing"sv)
            return "Rebuilding " + label + " because its PCM is missing";
        if(info.kind == "own_pcm_stale"sv)
            return "Rebuilding " + label + " because its PCM is older than the source";
        return "Rebuilding " + label + " (" + info.kind + ")";
    }

    static output::rebuild_info make_rebuild(std::string kind,
                                             std::string module = {},
                                             std::string pcm_path = {},
                                             std::string trigger_path = {})
    {
        auto info = output::rebuild_info{};
        info.kind = std::move(kind);
        info.module = std::move(module);
        info.pcm_path = std::move(pcm_path);
        info.trigger_path = std::move(trigger_path);
        info.hint = std::string{rebuild_hint(info.kind)};
        if(info.kind == "profile_change"sv)
            info.see_event = "profile_changed";
        return info;
    }

    static output::rebuild_info finalize_rebuild(output::rebuild_info info, const translation_unit& tu)
    {
        info.object_path = tu.object_path;
        info.message = format_rebuild_message(tu, info);
        return info;
    }

    static std::string format_link_message(std::string_view executable_path, const output::rebuild_info& info)
    {
        if(info.kind == "missing_executable"sv)
            return "Linking " + std::string{executable_path} + " because executable is missing";
        if(info.kind == "not_in_cache"sv)
            return "Linking " + std::string{executable_path} + " because it is not in the link cache";
        if(info.kind == "object_changed"sv)
            return "Linking " + std::string{executable_path} + " because input objects changed";
        if(info.kind == "link_flags_changed"sv)
            return "Linking " + std::string{executable_path} + " because link flags changed";
        return "Linking " + std::string{executable_path} + " (" + info.kind + ")";
    }

    static output::rebuild_info finalize_link_rebuild(output::rebuild_info info, std::string_view executable_path)
    {
        info.message = format_link_message(executable_path, info);
        return info;
    }

    void emit_compile_start(const translation_unit& tu,
                            const output::rebuild_info& rebuild = {}) const
    {
        output::notify(
            &output::observer::compile_start,
            tu.full_path,
            tu.object_path,
            tu.is_modular ? tu.pcm_path : std::string_view{},
            tu.module,
            rebuild);
    }

    void emit_compile_end(const translation_unit& tu,
                          bool ok,
                          bool cache_hit,
                          std::chrono::steady_clock::time_point started,
                          std::chrono::steady_clock::time_point finished,
                          const output::rebuild_info& rebuild = {}) const
    {
        output::notify(
            &output::observer::compile_end,
            tu.full_path,
            tu.object_path,
            tu.is_modular ? tu.pcm_path : std::string_view{},
            tu.module,
            ok,
            cache_hit,
            started,
            finished,
            rebuild);
    }

    void emit_link_end(std::string_view executable_path,
                       bool ok,
                       bool cache_hit,
                       std::chrono::steady_clock::time_point started,
                       std::chrono::steady_clock::time_point finished,
                       const output::rebuild_info& rebuild = {}) const
    {
        output::notify(&output::observer::link_end, executable_path, ok, cache_hit, started, finished, rebuild);
    }

    void emit_profile_changed()
    {
        if(object_cache_miss_reason != "profile_change"sv)
            return;

        output::notify(&output::observer::profile_changed, *object_cache_miss_reason, profile_diff);
    }

    string_list base_compile_argv() const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(compile_flags);
        argv.append_range(cpp_flags);
        return argv;
    }

    string_list precompile_argv(const translation_unit& tu) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags);
        argv.push_back(tu.full_path);
        argv.push_back("--precompile");
        argv.push_back("-o");
        argv.push_back(tu.pcm_path);
        return argv;
    }

    string_list pcm_object_argv(const translation_unit& tu) const
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

    string_list source_object_argv(const translation_unit& tu) const
    {
        auto argv = base_compile_argv();
        argv.append_range(module_flags);
        if (tu.kind == unit_kind::implementation_unit) {
            auto module_pcm = compute_pcm_path(tu);
            argv.push_back("-fmodule-file=" + tu.module + "=" + module_pcm);
        }
        argv.push_back(tu.full_path);
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back(tu.object_path);
        return argv;
    }

    string_list build_std_pcm_argv() const
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
        argv.push_back("--precompile");
        argv.push_back("-o");
        argv.push_back(std_pcm_path());
        return argv;
    }

    string_list build_std_o_argv() const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(string_list{
            "-std=c++23",
            "-pthread",
            "-fPIC",
            "-fexperimental-library",
            "-Wall",
            "-Wextra",
        });
        if(os_name() == "darwin")
            argv.push_back("-fapplication-extension");
        if(config == build_config::release)
            argv.append_range(string_list{"-O3", "-DNDEBUG"});
        else
            argv.append_range(string_list{"-O0", "-g"});
        argv.push_back("-fno-implicit-modules");
        argv.push_back("-fno-implicit-module-maps");
        argv.push_back("-fmodule-file=std=" + std_pcm_path());
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

    string_list link_test_runner_argv(const std::string& output_path,
                                      const std::string& test_runner_obj,
                                      const string_list& link_module_ldflags) const
    {
        auto argv = string_list{};
        argv.push_back(llvm_cxx);
        argv.append_range(compile_flags);
        argv.append_range(link_module_ldflags);
        argv.append_range(module_flags);
        if(not test_runner_obj.empty())
            argv.push_back(test_runner_obj);
        argv.append_range(linkable_object_paths());
        argv.append_range(
            units_in_topological_order
            | std::views::filter([](const translation_unit& tu) { return tu.is_test and not tu.has_main; })
            | std::views::transform([](const translation_unit& tu) { return tu.object_path; })
            | std::ranges::to<string_list>());
        argv.push_back(std_obj_path());
        argv.append_range(link_flags);
        argv.push_back("-o");
        argv.push_back(output_path);
        return argv;
    }

    string_list test_runner_argv(const std::string& runner, const std::vector<std::string>& args) const
    {
        auto argv = string_list{};
        argv.push_back(runner);
        argv.append_range(args);
        return argv;
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

    // ============================================================================
    // Cache Management
    // ============================================================================

    std::string object_cache_profile() const {
        ensure_toolchain_profile();

        auto profile = ""s;
        profile.reserve(768);
        detail::append_profile_field(profile, "format", detail::object_cache_format);
        detail::append_profile_field(profile, "config", detail::config_name(config));
        detail::append_profile_field(profile, "static_link", static_link ? "1" : "0");
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

    object_cache_map load_object_cache() {
        object_cache_miss_reason.reset();
        profile_diff = {};
        auto cache = object_cache_map{};
        auto file = std::ifstream{object_cache_path()};
        if (not file)
            return cache;

        auto header = ""s;
        if (not std::getline(file, header))
            return cache;

        const auto current_profile = object_cache_profile();
        if (header.starts_with("profile\t")) {
            const auto stored_profile = header.substr(std::string_view{"profile\t"}.size());
            if (stored_profile != current_profile) {
                object_cache_miss_reason = "profile_change";
                profile_diff = detail::diff_object_cache_profiles(stored_profile, current_profile);
                return cache;
            }
        } else {
            output::notify(&output::observer::info, "Object cache missing profile header; ignoring"s);
            return cache;
        }

        auto line = ""s;
        while (std::getline(file, line)) {
            auto path = ""s;
            auto ticks = 0ll;
            if (parse_object_cache_entry(line, path, ticks) and fs::exists(path))
                cache[path] = fs::file_time_type{std::chrono::nanoseconds{ticks}};
        }
        return cache;
    }

    void save_object_cache(const object_cache_map& c) {
        auto tmp = object_cache_path() + ".tmp";
        auto file = std::ofstream{tmp};
        if(not file)
            throw std::runtime_error{"Cannot open object cache temporary file: " + tmp};

        file << "profile\t" << object_cache_profile() << "\n";
        for (const auto& [path, timestamp] : c) {
            if (not fs::exists(path))
                continue;
            auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                timestamp.time_since_epoch()).count();
            file << path << "\t" << ticks << "\n";
        }
        file.close();
        if(not file)
        {
            auto ignored = std::error_code{};
            fs::remove(tmp, ignored);
            throw std::runtime_error{"Failed to write object cache temporary file: " + tmp};
        }

        auto error = std::error_code{};
        fs::rename(tmp, object_cache_path(), error);
        if(error)
        {
            auto ignored = std::error_code{};
            fs::remove(tmp, ignored);
            throw std::runtime_error{"Failed to replace object cache: " + error.message()};
        }
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

    bool any_transitive_pcm_newer_than_object(const translation_unit& tu,
                                              fs::file_time_type object_timestamp,
                                              const unit_to_tu_map& u2tu,
                                              std::flat_set<std::string>& visited,
                                              output::rebuild_info& stale) const
    {
        for (const auto& dependency_key : tu.imports) {
            if (not u2tu.contains(dependency_key))
                continue;

            const auto& dep_tu = *u2tu.at(dependency_key);

            if (dep_tu.is_modular && fs::exists(dep_tu.pcm_path)) {
                if (fs::last_write_time(dep_tu.pcm_path) > object_timestamp) {
                    stale = make_rebuild("pcm_stale", dep_tu.module, dep_tu.pcm_path, dep_tu.full_path);
                    return true;
                }
            }

            if (visited.contains(dep_tu.unit))
                continue;
            visited.insert(dep_tu.unit);

            if (any_transitive_pcm_newer_than_object(dep_tu, object_timestamp, u2tu, visited, stale))
                return true;
        }
        return false;
    }

    std::optional<output::rebuild_info> needs_recompile(const translation_unit& tu, object_cache_map& c, const unit_to_tu_map& u2tu) const {
        // First-seen path for this config vs edited source after a prior compile.
        if (not c.contains(tu.full_path)) {
            if (object_cache_miss_reason)
                return make_rebuild(*object_cache_miss_reason, {}, {}, tu.full_path);
            return make_rebuild("not_in_cache", {}, {}, tu.full_path);
        }
        if (c.at(tu.full_path) < tu.last_modified)
            return make_rebuild("source_stale", {}, {}, tu.full_path);

        // Ensure the object file exists and is up-to-date versus the source timestamp we cached.
        if (not fs::exists(tu.object_path))
            return make_rebuild("object_missing", {}, {}, tu.full_path);

        auto object_timestamp = fs::last_write_time(tu.object_path);
        if (object_timestamp < c.at(tu.full_path))
            return make_rebuild("object_stale", {}, {}, tu.full_path);

        // Implementation units consume their interface PCM implicitly through
        // -fmodule-file=<module>=<pcm>, even when they do not import that module.
        if(tu.kind == unit_kind::implementation_unit && u2tu.contains(tu.module))
        {
            const auto& interface = *u2tu.at(tu.module);
            if(not fs::exists(interface.pcm_path))
                return make_rebuild("dependency_pcm_stale", interface.module, interface.pcm_path, interface.full_path);
            if(fs::last_write_time(interface.pcm_path) > object_timestamp)
                return make_rebuild("pcm_stale", interface.module, interface.pcm_path, interface.full_path);
            if(auto interface_reason = needs_recompile(interface, c, u2tu))
            {
                auto info = *interface_reason;
                if(info.trigger_path.empty())
                    info.trigger_path = interface.full_path;
                if(info.module.empty())
                    info.module = interface.module;
                return info;
            }
        }

        // For modular units, also check if .pcm file is stale
        if (tu.is_modular) {
            if (not fs::exists(tu.pcm_path))
                return make_rebuild("own_pcm_missing", tu.module, tu.pcm_path, tu.full_path);
            auto pcm_timestamp = fs::last_write_time(tu.pcm_path);
            if (pcm_timestamp < tu.last_modified)
                return make_rebuild("own_pcm_stale", tu.module, tu.pcm_path, tu.full_path);
        }

        // Rebuild when any transitive import PCM is newer than this object file.
        // Catches partition updates (e.g. tester:assertions) for test TUs that import an umbrella module.
        auto visited = std::flat_set<std::string>{};
        auto stale = output::rebuild_info{};
        if (any_transitive_pcm_newer_than_object(tu, object_timestamp, u2tu, visited, stale))
            return stale;

        // Rebuild if any imported modules have changed (their .pcm files are stale or they need recompiling)
        for (const auto& dependency_key : tu.imports) {
            if (u2tu.contains(dependency_key)) {
                const auto& dep_tu = *u2tu.at(dependency_key);
                // Check if the imported module's .pcm is stale compared to its source
                if (dep_tu.is_modular) {
                    if (not fs::exists(dep_tu.pcm_path) or 
                        fs::last_write_time(dep_tu.pcm_path) < dep_tu.last_modified) {
                        return make_rebuild("dependency_pcm_stale", dep_tu.module, dep_tu.pcm_path, dep_tu.full_path);
                    }
                }
                // Also recursively check if the imported module needs recompiling
                if (auto dep_reason = needs_recompile(dep_tu, c, u2tu))
                {
                    auto info = *dep_reason;
                    if(info.trigger_path.empty())
                        info.trigger_path = dep_tu.full_path;
                    if(info.module.empty() and not dep_tu.module.empty())
                        info.module = dep_tu.module;
                    return info;
                }
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
            if (fs::exists(executable_cache_path()))
                fs::remove(executable_cache_path());
            return;
        }
        auto tmp = executable_cache_path() + ".tmp";
        auto file = std::ofstream{tmp};
        if(not file)
            throw std::runtime_error{"Cannot open executable cache temporary file: " + tmp};
        for (const auto& [path, signature] : cache)
            file << path << "\t" << signature << "\n";
        file.close();
        if(not file)
        {
            auto ignored = std::error_code{};
            fs::remove(tmp, ignored);
            throw std::runtime_error{"Failed to write executable cache temporary file: " + tmp};
        }

        auto error = std::error_code{};
        fs::rename(tmp, executable_cache_path(), error);
        if(error)
        {
            auto ignored = std::error_code{};
            fs::remove(tmp, ignored);
            throw std::runtime_error{"Failed to replace executable cache: " + error.message()};
        }
    }

    std::optional<output::rebuild_info> needs_relinking(std::string_view executable_path,
                                                        const std::string& signature,
                                                        const executable_cache_map& link_cache) const
    {
        if(not fs::exists(executable_path))
            return finalize_link_rebuild(make_rebuild("missing_executable"), executable_path);

        if(not link_cache.contains(executable_path))
            return finalize_link_rebuild(make_rebuild("not_in_cache"), executable_path);

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
                return finalize_link_rebuild(make_rebuild("object_changed"), executable_path);
            if(previous.substr(previous_flags) != signature.substr(current_flags))
                return finalize_link_rebuild(make_rebuild("link_flags_changed"), executable_path);
        }

        return finalize_link_rebuild(make_rebuild("signature_changed"), executable_path);
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

                if (rel_path.contains("/test/") or rel_path.starts_with("test/") or
                    rel_path.contains("/tools/") or rel_path.starts_with("tools/") or
                    rel_path.contains("/.git/") or rel_path.starts_with(".git/"))
                    continue;

                // Exclude examples by default, include only if flag is set
                if (not include_examples and (rel_path.contains("/examples/") or rel_path.starts_with("examples/")))
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

        for (auto& tu : sorted) {
            // Attach builder-managed artifact paths once we know the full configuration.
            // Keeping them here keeps the translation unit metadata immutable while giving downstream
            // steps a single place to read object/PCM/binary locations from.
            tu.object_path = compute_object_path(tu);
            if (tu.is_modular) {
                tu.pcm_path = compute_pcm_path(tu);
            }
            if (tu.has_main) {
                tu.executable_path = compute_executable_path(tu);
            }
            validate_translation_unit(tu);
        }

        units_in_topological_order = std::move(sorted);
    }

    // ============================================================================
    // Standard Library Module Building
    // ============================================================================

    void build_std_pcm() {
        auto std_pcm = std_pcm_path();
        if (fs::exists(std_pcm) and fs::exists(std_module_source) and
            fs::last_write_time(std_pcm) >= fs::last_write_time(std_module_source))
            return;

        execute_system_command(build_std_pcm_argv());
    }

    void build_std_o() {
        auto std_pcm = std_pcm_path();
        auto std_obj = std_obj_path();
        if (not fs::exists(std_pcm)) build_std_pcm();
        if (fs::exists(std_obj) and fs::last_write_time(std_obj) >= fs::last_write_time(std_pcm))
            return;

        execute_system_command(build_std_o_argv());
    }

    // ============================================================================
    // Compilation
    // ============================================================================

    void compile_unit(const translation_unit& tu, const output::rebuild_info& rebuild) {
        emit_compile_start(tu, rebuild);
        const auto started = std::chrono::steady_clock::now();
        try {
            if (tu.is_modular) {
                execute_system_command(precompile_argv(tu));
                execute_system_command(pcm_object_argv(tu));
            } else {
                execute_system_command(source_object_argv(tu));
            }
        } catch (...) {
            emit_compile_end(tu, false, false, started, std::chrono::steady_clock::now(), rebuild);
            throw;
        }
        emit_compile_end(tu, true, false, started, std::chrono::steady_clock::now(), rebuild);
    }

    void update_module_flags()
    {
        const auto discovered_flags =
            units_in_topological_order
            | std::views::filter([](const translation_unit& tu) { return tu.is_modular; })
            | std::views::transform([](const translation_unit& tu) {
                return "-fmodule-file=" + tu.module + "=" + tu.pcm_path;
            })
            | std::ranges::to<string_list>();
        for(const auto& flag : discovered_flags)
            if(not std::ranges::contains(module_flags, flag))
                module_flags.push_back(flag);
    }

    void compile_units() {
        if (units_in_topological_order.empty()) return;
        auto cache = load_object_cache();
        emit_profile_changed();

        auto u2tu = unit_to_tu_map{};
        for (auto& tu : units_in_topological_order) {
            auto k = tu.unit;
            u2tu[k] = &tu;
        }

        auto levels = level_groups_map{};
        for (const auto& tu : units_in_topological_order)
            levels[tu.dependency_level >= 0 ? tu.dependency_level : INT_MAX].push_back(&tu);

        for (const auto& [lvl, group] : levels) {
            auto decisions = std::vector<std::pair<const translation_unit*, std::optional<output::rebuild_info>>>{};
            decisions.reserve(group.size());
            for(const auto* tu : group)
            {
                if(auto reason = needs_recompile(*tu, cache, u2tu))
                    decisions.emplace_back(tu, finalize_rebuild(std::move(*reason), *tu));
                else
                    decisions.emplace_back(tu, std::nullopt);
            }

            auto threads = thread_list{};
            auto failed = std::atomic_bool{false};
            auto failure = std::exception_ptr{};
            auto failure_mutex = std::mutex{};

            for (const auto& [tu, reason] : decisions) {
                if(reason) {
                    threads.emplace_back([this, tu, reason, &cache, &failed, &failure, &failure_mutex]() {
                        if(failed.load(std::memory_order_relaxed))
                            return;
                        try {
                            compile_unit(*tu, *reason);
                            auto lock = std::lock_guard<std::mutex>{cache_mutex};
                            cache[tu->full_path] = tu->last_modified;
                        } catch (...) {
                            failed.store(true, std::memory_order_relaxed);
                            auto lock = std::lock_guard<std::mutex>{failure_mutex};
                            if(not failure)
                                failure = std::current_exception();
                            return;
                        }
                    });
                } else {
                    const auto now = std::chrono::steady_clock::now();
                    emit_compile_start(*tu);
                    emit_compile_end(*tu, true, true, now, now);
                }
            }
            for (auto& thread : threads) thread.join();
            if(failure)
                std::rethrow_exception(failure);
        }
        save_object_cache(cache);
    }

    // ============================================================================
    // Linking
    // ============================================================================

    string_list linkable_object_paths() const
    {
        return units_in_topological_order
            | std::views::filter([](const translation_unit& tu) { return not tu.has_main and not tu.is_test; })
            | std::views::transform([](const translation_unit& tu) { return tu.object_path; })
            | std::ranges::to<string_list>();
    }

    void link_executable(const translation_unit& tu, const string_list& shared_objects) {
        if (not tu.has_main) return;
        execute_system_command(link_executable_argv(tu, shared_objects));
    }

    std::string dependency_signature(const std::string& path) const {
        if (path.empty() or not fs::exists(path))
            return path + ":missing";
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            fs::last_write_time(path).time_since_epoch()).count();
        return path + ":" + std::to_string(timestamp);
    }

    std::string dependency_signatures_joined(const string_list& paths) const
    {
        return paths
            | std::views::transform([&](const std::string& path) { return dependency_signature(path); })
            | std::views::join_with("|"sv)
            | std::ranges::to<std::string>();
    }

    std::string compute_link_signature(const translation_unit& tu, const string_list& shared_objects) const {
        auto paths = string_list{};
        paths.push_back(tu.object_path);
        paths.append_range(shared_objects);
        paths.push_back(std_obj_path());

        auto signature = dependency_signatures_joined(paths);
        signature += "|flags=";
        signature += detail::flags_profile_string(compile_flags);
        signature += "|link=";
        signature += detail::flags_profile_string(link_flags);
        signature += "|modules=";
        signature += detail::flags_profile_string(module_flags);
        signature += "|imports=";
        signature += detail::flags_profile_string(collect_module_ldflags(tu.imports));
        return signature;
    }

    void link_executables() {
        auto shared_objects = linkable_object_paths();
        auto link_cache = load_executable_cache();
        auto threads = thread_list{};
        auto failed = std::atomic_bool{false};
        auto failure = std::exception_ptr{};
        auto failure_mutex = std::mutex{};
        for (const auto& tu : units_in_topological_order)
            if (tu.has_main and not tu.filename.contains("test_runner")) {
                auto signature = compute_link_signature(tu, shared_objects);
                auto link_reason = needs_relinking(tu.executable_path, signature, link_cache);
                if (not link_reason) {
                        output::notify(&output::observer::info, "Skipping link (up-to-date): "s + tu.executable_path);
                        const auto now = std::chrono::steady_clock::now();
                        emit_link_end(tu.executable_path, true, true, now, now);
                        continue;
                }
                threads.emplace_back([this, tu, &shared_objects, signature, link_reason, &link_cache, &failed, &failure, &failure_mutex]() {
                    if(failed.load(std::memory_order_relaxed))
                        return;
                    const auto started = std::chrono::steady_clock::now();
                    auto linked = false;
                    try {
                        link_executable(tu, shared_objects);
                        linked = true;
                        const auto finished = std::chrono::steady_clock::now();
                        emit_link_end(tu.executable_path, true, false, started, finished, *link_reason);
                        auto lock = std::lock_guard<std::mutex>{link_cache_mutex};
                        link_cache[tu.executable_path] = signature;
                    } catch (...) {
                        if(not linked)
                            emit_link_end(tu.executable_path, false, false, started, std::chrono::steady_clock::now(), *link_reason);
                        failed.store(true, std::memory_order_relaxed);
                        auto lock = std::lock_guard<std::mutex>{failure_mutex};
                        if(not failure)
                            failure = std::current_exception();
                        return;
                    }
                });
            }
        for (auto& thread : threads) thread.join();
        if(failure)
            std::rethrow_exception(failure);
        save_executable_cache(link_cache);
    }

    // ============================================================================
    // Test Support
    // ============================================================================

    bool has_test_runner_link_inputs(bool has_runner_unit) const
    {
        if(has_runner_unit)
            return true;
        return std::ranges::any_of(units_in_topological_order, [](const translation_unit& tu) {
            return not tu.has_main;
        });
    }

    string_list collect_test_module_ldflags() const
    {
        return std::ranges::fold_left(
            units_in_topological_order | std::views::filter([](const translation_unit& tu) { return tu.is_test and not tu.has_main; }),
            string_list{},
            [&](string_list flags, const translation_unit& tu) {
                flags.append_range(collect_module_ldflags(tu.imports));
                return flags;
            });
    }

    std::string compute_test_runner_signature(const std::string& test_runner_obj,
                                              const string_list& signature_import_flags) const {
        auto paths = string_list{};
        if(not test_runner_obj.empty())
            paths.push_back(test_runner_obj);
        paths.append_range(
            units_in_topological_order
            | std::views::filter([](const translation_unit& tu) { return tu.is_test and not tu.has_main; })
            | std::views::transform([](const translation_unit& tu) { return tu.object_path; })
            | std::ranges::to<string_list>());
        paths.append_range(linkable_object_paths());
        paths.push_back(std_obj_path());

        auto signature = dependency_signatures_joined(paths);
        signature += "|flags=";
        signature += detail::flags_profile_string(compile_flags);
        signature += "|link=";
        signature += detail::flags_profile_string(link_flags);
        signature += "|modules=";
        signature += detail::flags_profile_string(module_flags);
        signature += "|imports=";
        signature += detail::flags_profile_string(signature_import_flags);

        return signature;
    }

    void link_test_runner() {
        const auto runner_it = std::ranges::find_if(units_in_topological_order, [](const translation_unit& tu) {
            return tu.has_main and tu.base_name.contains("test_runner");
        });
        const auto has_runner_unit = runner_it != units_in_topological_order.end();

        if(not has_test_runner_link_inputs(has_runner_unit)) {
            output::notify(&output::observer::info, "No objects to link for test_runner");
            return;
        }

        auto test_runner_path = binary_dir() + "/test_runner";
        auto test_runner_obj = std::string{};
        if(has_runner_unit)
        {
            test_runner_path = runner_it->executable_path;
            test_runner_obj = runner_it->object_path;
        }

        const auto link_module_ldflags = has_runner_unit
            ? collect_module_ldflags(runner_it->imports)
            : collect_test_module_ldflags();

        auto signature_import_flags = collect_test_module_ldflags();
        if(has_runner_unit)
            signature_import_flags.append_range(collect_module_ldflags(runner_it->imports));

        // Check if test_runner is up-to-date
        auto link_cache = load_executable_cache();
        auto signature = compute_test_runner_signature(test_runner_obj, signature_import_flags);
        auto link_reason = needs_relinking(test_runner_path, signature, link_cache);
        if(not link_reason)
        {
            output::notify(&output::observer::info, "Skipping link (up-to-date): "s + test_runner_path);
            const auto now = std::chrono::steady_clock::now();
            emit_link_end(test_runner_path, true, true, now, now);
            return;
        }

        const auto link_started = std::chrono::steady_clock::now();
        try {
            execute_system_command(link_test_runner_argv(test_runner_path, test_runner_obj, link_module_ldflags));
        } catch (...) {
            emit_link_end(test_runner_path, false, false, link_started, std::chrono::steady_clock::now(), *link_reason);
            throw;
        }
        if(has_runner_unit)
            output::notify(&output::observer::success, "test_runner linked with test objects");
        else
            output::notify(&output::observer::success, "test_runner linked successfully");
        
        emit_link_end(test_runner_path, true, false, link_started, std::chrono::steady_clock::now(), *link_reason);

        // Save signature to cache
        {
            auto lock = std::lock_guard<std::mutex>{link_cache_mutex};
            link_cache[test_runner_path] = signature;
        }
        save_executable_cache(link_cache);
    }

    void build_steps()
    {
        // Ensure build directories exist (they may have been removed by clean())
        fs::create_directories(module_cache_dir());
        fs::create_directories(object_dir());
        fs::create_directories(binary_dir());
        fs::create_directories(cache_dir());

        build_std_pcm();
        build_std_o();
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

        auto executable_entries = 0;
        if(fs::exists(executable_cache_path()))
        {
            auto file = std::ifstream{executable_cache_path()};
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
            cache_path,
            cache_exists,
            profile_match,
            object_entries,
            object_stale,
            executable_entries,
            current_profile);
    }

    static bool remove_if_exists(const std::string& path)
    {
        if(not fs::exists(path))
            return false;
        fs::remove(path);
        return true;
    }

    void cache_invalidate() const
    {
        fs::create_directories(cache_dir());

        const auto object_removed = remove_if_exists(object_cache_path());
        const auto executable_removed = remove_if_exists(executable_cache_path());
        const auto stamp_removed = remove_if_exists(cache_dir() + "/compiler-version.txt");

        output::notify(&output::observer::cache_invalidate_end, object_removed, executable_removed, stamp_removed);
    }

    void set_include_tests(bool value) {
        include_tests = value;
    }

    void build() {
        const auto build_started = std::chrono::steady_clock::now();
        current_phase = build_phase::build;
        phase_started = build_started;
        build_end_emitted = false;
        output::notify(&output::observer::build_start, detail::config_name(config), include_tests, include_examples);

        try {
            build_steps();
        } catch (...) {
            emit_failed_build_end();
            current_phase = build_phase::none;
            throw;
        }

        output::notify(&output::observer::build_end, true, build_started, std::chrono::steady_clock::now());
        build_end_emitted = true;
        current_phase = build_phase::none;

        output::notify(&output::observer::success, "Build completed: "s + build_root());
    }

    // Returns false when the test runner reports failures (normal outcome, not exceptional).
    bool run_tests(const std::vector<std::string>& args = {}) {
        output::notify(&output::observer::info, "=== Running tests ===");

        include_tests = true;
        const auto build_started = std::chrono::steady_clock::now();
        current_phase = build_phase::build;
        phase_started = build_started;
        build_end_emitted = false;
        output::notify(&output::observer::build_start, detail::config_name(config), true, include_examples);

        auto runner = binary_dir() + "/test_runner";
        try {
            build_steps();
            link_test_runner();
            if(not fs::exists(runner))
                throw std::runtime_error{"test_runner not found — make sure .test.c++ files or test_runner.c++ exist"};
        } catch (...) {
            emit_failed_build_end();
            current_phase = build_phase::none;
            throw;
        }

        const auto build_finished = std::chrono::steady_clock::now();
        output::notify(&output::observer::build_end, true, build_started, build_finished);
        build_end_emitted = true;
        current_phase = build_phase::none;
        output::notify(&output::observer::success, "Build completed: "s + build_root());

        static auto env_storage = std::array<std::string, 2>{};
        auto env_count = std::size_t{};
        const auto store_env = [&](std::string_view key, std::string_view value) {
            env_storage[env_count++] = std::string{key} + '=' + std::string{value};
        };
        store_env("TESTER_CONFIG", detail::config_name(config));
        const auto parent = output::run_id();
        if(not parent.empty())
            store_env("TESTER_PARENT_RUN_ID", parent);
        for(auto i = std::size_t{}; i < env_count; ++i)
            ::putenv(env_storage[i].data());

        const auto test_started = std::chrono::steady_clock::now();
        output::notify(&output::observer::test_start, runner);

        const auto r = invoke_shell(test_runner_argv(runner, args));
        const auto test_finished = std::chrono::steady_clock::now();
        output::notify(&output::observer::test_end, r == 0, r, r, false, 0, test_started, test_finished);
        current_phase = build_phase::none;
        if (r) {
            output::notify(&output::observer::error, "Some tests or assertions failed!");
            return false;
        }
        output::notify(&output::observer::success, "All tests passed!");
        return true;
    }

    void list_sources() {
        scan_and_order();
        auto inventory = output::source_inventory{
            std::string{detail::config_name(config)},
            include_tests,
            include_examples,
            source_dir,
            {},
            0,
            0,
            0,
        };
        inventory.units.reserve(units_in_topological_order.size());
        for(const auto& tu : units_in_topological_order)
        {
            inventory.units.push_back(output::source_unit{
                tu.unit,
                tu.path.empty() ? tu.filename : tu.path + "/" + tu.filename,
                tu.module,
                std::string{detail::unit_kind_name(tu.kind)},
                tu.imports,
                tu.dependency_level,
                tu.has_main,
                tu.is_test,
                tu.is_modular,
            });
            if(tu.has_main)
                ++inventory.main_count;
            if(tu.is_test)
                ++inventory.test_count;
            if(tu.dependency_level >= 0)
                inventory.max_level = std::max(inventory.max_level, tu.dependency_level);
        }
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
        || arg == "--include-examples" || arg == "--build-tests"
        || arg == "-I" || arg == "--include" || arg == "--link-flags"
        || arg == "--compile-flags" || arg == "--extra-compile-flags"
        || arg == "--jsonl" || arg.starts_with("--jsonl=") || arg == "--";
}

bool is_test_runner_token(std::string_view arg)
{
    return arg == "--list" || arg == "--jsonl" || arg == "--result" || arg == "--help"
        || arg.starts_with("--tags=")
        || arg.starts_with("--slowest=")
        || arg.starts_with("--jsonl=")
        || arg.starts_with("--jsonl-output-max-bytes=");
}

void note_test_runner_jsonl(std::string_view arg, std::string_view& output_name)
{
    if(arg == "--jsonl")
    {
        output_name = "jsonl";
        cb::jsonl_observer().set_mode(cb::output::jsonl::jsonl_mode::failures);
    }
    else if(arg.starts_with("--jsonl="))
    {
        output_name = "jsonl";
        const auto mode = arg.substr(std::string_view{"--jsonl="}.size());
        if(mode == "summary")
            cb::jsonl_observer().set_mode(cb::output::jsonl::jsonl_mode::summary);
        else if(mode == "trace")
            cb::jsonl_observer().set_mode(cb::output::jsonl::jsonl_mode::trace);
        else
            cb::jsonl_observer().set_mode(cb::output::jsonl::jsonl_mode::failures);
    }
}

} // namespace

using namespace std::string_literals;

int main(int argc, char* argv[])
try {
    auto output_name = std::string_view{"console"};
    cb::select_output_observer(output_name);
    auto stdcppm = ""s;  // Empty string triggers auto-detection
    auto arg_index = 1;
    if (argc > 1) {
        auto candidate = fs::path{argv[1]};
        if (fs::exists(candidate)) {
            stdcppm = candidate.string();
            ++arg_index;
        }
    }

    auto config = cb::build_config::debug;  // default to debug
    auto do_clean = false, do_list = false, do_build = false, do_run_tests = false;
    auto do_cache_status = false, do_cache_invalidate = false;
    auto test_filter = std::string{};
    auto test_runner_args = std::vector<std::string>{};
    auto static_linking = false;
    auto include_examples = false;
    auto build_tests = false;  // --build-tests flag: build tests but don't run them
    auto include_paths = std::vector<std::string>{};
    auto extra_compile_flags = cb::string_list{};
    auto extra_link_flags = cb::string_list{};

    for (int i = arg_index; i < argc; ++i) {
        auto argument = std::string_view{argv[i]};
        if (argument == "--") {
            // Everything after "--" is passed to test_runner (only meaningful with "test").
            test_runner_args.assign(argv + i + 1, argv + argc);
            for(const auto& arg : test_runner_args)
                note_test_runner_jsonl(arg, output_name);
            break;
        }
        if (argument == "--jsonl") {
            cb::jsonl_observer().set_mode(cb::output::jsonl::jsonl_mode::failures);
            output_name = "jsonl";
            cb::select_output_observer(output_name);
            continue;
        }
        if(argument.starts_with("--jsonl="))
        {
            const auto mode = argument.substr(std::string_view{"--jsonl="}.size());
            if(mode == "summary")
                cb::jsonl_observer().set_mode(cb::output::jsonl::jsonl_mode::summary);
            else if(mode == "failures")
                cb::jsonl_observer().set_mode(cb::output::jsonl::jsonl_mode::failures);
            else if(mode == "trace")
                cb::jsonl_observer().set_mode(cb::output::jsonl::jsonl_mode::trace);
            else
            {
                cb::output::notify(&cb::output::observer::error, "Unknown JSONL mode: "s + std::string{mode});
                return 1;
            }
            output_name = "jsonl";
            cb::select_output_observer(output_name);
            continue;
        }
        if (argument == "test") {
            do_run_tests = true;
            // Optional positional filter (substring); do not consume test_runner/CB flags.
            if (i + 1 < argc) {
                const auto next = std::string_view{argv[i + 1]};
                if (next != "--" && !is_test_runner_token(next) && !is_cb_token(next) && !next.starts_with("-"))
                    test_filter = argv[++i];
            }
        } else if (argument == "release") {
            config = cb::build_config::release;
        } else if (argument == "debug") {
            config = cb::build_config::debug;
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
        } else if (do_run_tests && is_test_runner_token(argument)) {
            // Convenience: forward test_runner flags without requiring "--"
            test_runner_args.emplace_back(argv[i]);
            note_test_runner_jsonl(argument, output_name);
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
                      << "  ci               Clean and run tests (shortcut for: clean test)\n"
                      << "  list             List all translation units\n"
                      << "  cache status     Inspect object-cache profile and entry counts\n"
                      << "  cache invalidate Remove object/link cache indexes (lighter than clean)\n"
                      << "  test [filter] [-- <args...>]  Build and run tests (optional filter)\n"
                      << "                 Forward test_runner flags directly (e.g. --tags=, --list)\n"
                      << "                 or pass any args after '--'\n"
                      << "  static           Enable static linking (C++ stdlib static)\n"
                      << "  --include-examples Include examples directory in build (excluded by default)\n"
                      << "  --build-tests    Build tests in release mode (useful for CI to verify compilation)\n"
                      << "  --jsonl[=<summary|failures|trace>]  Machine-readable output (default: failures)\n"
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
                      << "  " << argv[0] << " ci\n"
                      << "  " << argv[0] << " test\n"
                      << "  " << argv[0] << " test --tags=[module]\n"
                      << "  " << argv[0] << " test --jsonl=failures --tags=[module]\n"
                      << "  " << argv[0] << " debug build --jsonl=summary\n"
                      << "  " << argv[0] << " test -- --jsonl=trace --slowest=10\n"
                      << "  " << argv[0] << " clean\n";
            return 0;
        }
    }

    if(not cb::select_output_observer(output_name))
    {
        cb::output::notify(&cb::output::observer::error, "Unknown output observer: "s + std::string{output_name});
        return 1;
    }
    std::atexit(cb::output::finish);

    auto include_flags = cb::string_list{};
    for(const auto& path : include_paths)
    {
        include_flags.push_back("-I");
        include_flags.push_back(path);
    }

    auto build_system = cb::build_system{config, include_flags, {}, ".", stdcppm, static_linking, include_examples, extra_compile_flags, extra_link_flags};

    if (do_list) build_system.list_sources();
    if (do_cache_status) {
        build_system.cache_status();
        return 0;
    }
    if (do_cache_invalidate) {
        build_system.cache_invalidate();
        return 0;
    }
    if (do_clean) build_system.clean();
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

        const auto has_jsonl_mode = std::ranges::any_of(test_runner_args, [](const std::string& arg) {
            return arg == "--jsonl" || arg.starts_with("--jsonl=");
        });
        if(output_name == "jsonl" && not has_jsonl_mode)
        {
            const auto mode = cb::jsonl_observer().mode();
            args.emplace_back(mode == cb::output::jsonl::jsonl_mode::summary
                ? "--jsonl=summary"
                : mode == cb::output::jsonl::jsonl_mode::trace
                    ? "--jsonl=trace"
                    : "--jsonl=failures");
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
} catch (const std::exception& e) {
    cb::output::notify(&cb::output::observer::error, "Fatal error: "s + e.what());
    return 1;
} catch (...) {
    cb::output::notify(&cb::output::observer::error, "Fatal error: unknown exception");
    return 1;
}
