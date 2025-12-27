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
#include <map>
#include <array>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <chrono>
#include <limits>
#include <iostream>
#include <algorithm>
#include <utility>
#include <stdexcept>
#include <unistd.h>
#include <sys/wait.h>
#include "cb-jsonl-sink.h++"

namespace fs = std::filesystem;

static std::string shell_quote(std::string_view arg);

namespace cb {

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace jsonl {
// JSONL state is owned by jsonl_context (no global flags)
enum class phase { none, build, test };
inline phase current_phase = phase::none;
inline std::chrono::steady_clock::time_point phase_started{};
inline bool build_end_emitted = false;

inline auto& io_mux()
{
    static auto mux = io::mux{std::cout, std::cerr, std::cerr};
    return mux;
}

inline auto& sink()
{
    static auto s = cb_jsonl::sink{io_mux()};
    return s;
}

// JSONL context using shared utilities from io.h++
inline auto& ctx()
{
    return io_mux().jsonl;
}

inline bool enabled() { return ctx().is_enabled(); }
inline void set_enabled(bool v) { io_mux().set_jsonl_enabled(v); }
inline void reset() { io_mux().reset_jsonl_state(); }

inline void emit_meta() { auto lock = std::lock_guard<std::mutex>{io_mux().mutex}; ctx().emit_meta(); }
inline void emit_event(std::string_view type, auto&& add_fields) {
    auto lock = std::lock_guard<std::mutex>{io_mux().mutex};
    ctx()(type) << std::forward<decltype(add_fields)>(add_fields);
}
inline void emit_eof() { auto lock = std::lock_guard<std::mutex>{io_mux().mutex}; ctx().emit_eof(); }
} // namespace jsonl

static void jsonl_atexit_handler()
{
    cb::jsonl::emit_eof();
}

namespace log {
inline void error(std::string_view msg) {
    // In JSONL mode, keep output machine-parseable: do not print human logs.
    if(!cb::jsonl::enabled())
        io::error(cb::jsonl::io_mux(), msg);

    // JSONL error event (machine output)
    if(cb::jsonl::enabled())
    {
        // If build fails mid-flight, emit a structured build_end before cb_error.
        if(cb::jsonl::current_phase == cb::jsonl::phase::build && !cb::jsonl::build_end_emitted)
        {
            const auto finished = std::chrono::steady_clock::now();
            cb::jsonl::sink().build_end(false, jsonl_util::duration_ms(cb::jsonl::phase_started, finished));
            cb::jsonl::build_end_emitted = true;
        }

        cb::jsonl::sink().cb_error(msg);
    }
}

inline void warning(std::string_view msg) { if(!cb::jsonl::enabled()) io::warning(cb::jsonl::io_mux(), msg); }
inline void info(std::string_view msg) { if(!cb::jsonl::enabled()) io::info(cb::jsonl::io_mux(), msg); }
inline void success(std::string_view msg) { if(!cb::jsonl::enabled()) io::success(cb::jsonl::io_mux(), msg); }
inline void command(std::string_view cmd) { if(!cb::jsonl::enabled()) io::command(cb::jsonl::io_mux(), cmd); }
} // namespace log

enum class build_config { debug, release };

inline std::string_view config_name(build_config cfg)
{
    switch(cfg)
    {
        case build_config::debug: return "debug";
        case build_config::release: return "release";
    }
    return "debug";
}

enum class unit_kind : unsigned {
    non_module,          // non-modular source, no module declaration at all  (.c++ .cpp)
    interface_unit,      // module interface, export module name;             (.c++m .cppm)
    partition_unit,      // module partition, export module name:part;        (.c++m .cppm)
    implementation_unit, // module implementation, module name;               (.impl.c++)
    global_fragment      // global module fragment, only contains "module;"   (.c++m .cppm)
};

using suffix_list = std::vector<std::string>;
using string_list = std::vector<std::string>;

inline const suffix_list supported_suffixes = {
    ".test.c++m",
    ".test.c++",
    ".impl.c++",
    ".c++m",
    ".cppm",
    ".c++",
    ".cpp"
};

inline std::string make_base_name(std::string_view filename) {
    for (const auto& suffix : supported_suffixes)
        if (filename.ends_with(suffix))
            return std::string{filename.substr(0, filename.size() - suffix.size())};
    return std::string{filename};
}

inline std::string normalize_relative_dir(const fs::path& dir) {
    if (dir.empty()) return "";
    auto str = dir.string();
    return str == "." ? "" : str;
}

inline bool determine_is_test(std::string_view rel_dir, std::string_view name, std::string_view suffix_value) {
    if (suffix_value == ".test.c++" or suffix_value == ".test.c++m")
        return true;
    auto combined = rel_dir.empty() ? std::string{name} : std::string{rel_dir} + "/" + std::string{name};
    // Exclude "tester/" framework directory - it's the testing framework, not test files
    if (combined.starts_with("tester/"))
        return false;
    // Check for test files/directories (but not the tester framework)
    return combined.contains("test");
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
};

inline bool translation_unit::match_supported_suffix(std::string_view filename, std::string& out_suffix) {
    for (const auto& suffix : supported_suffixes) {
        if (filename.ends_with(suffix)) {
            out_suffix.assign(suffix.data(), suffix.size());
            return true;
        }
    }
    return false;
}

inline std::string extract_suffix(std::string_view filename) {
    auto suffix = std::string{};
    if (not translation_unit::match_supported_suffix(filename, suffix))
        throw std::runtime_error{"unsupported source suffix"};
    return suffix;
}

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
      path(normalize_relative_dir(relative.parent_path())),
      suffix(extract_suffix(relative.filename().string())),
      base_name(make_base_name(this->filename)),
      full_path(make_full_path(full_path)),
      unit(make_unit(module_value, kind_value, this->filename)),
      module(std::move(module_value)),
      imports(std::move(imports_value)),
      kind(kind_value),
      has_main(has_main_flag),
      is_test(determine_is_test(this->path, this->filename, this->suffix)),
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
        if (not seen_real_code) {
            if (trimmed.find('{') != std::string::npos or
                trimmed.find("class") != std::string::npos or
                trimmed.find("struct") != std::string::npos or
                trimmed.find("namespace") != std::string::npos or
                trimmed.find("using namespace") != std::string::npos or
                trimmed.find("constexpr") != std::string::npos or
                trimmed.find("inline") != std::string::npos or
                trimmed.find("static") != std::string::npos) {
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
            kind = module_name.find(':') != std::string::npos ? unit_kind::partition_unit : unit_kind::interface_unit;
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

using dependency_graph = std::map<std::string, string_list>;
using indegree_map = std::map<std::string, int>;
using unit_to_tu_map = std::map<std::string, translation_unit*>;
using object_cache_map = std::map<std::string, fs::file_time_type>;
using translation_unit_list = std::vector<translation_unit>;
using topo_sort_queue = std::queue<std::string>;
using level_groups_map = std::map<int, std::vector<const translation_unit*>>;
using thread_list = std::vector<std::thread>;
using module_to_ldflags_map = std::map<std::string, std::string>;
using executable_cache_map = std::map<std::string, std::string>;

class build_system {
private:
    std::string source_dir;
    std::string compile_flags, link_flags, cpp_flags;
    module_to_ldflags_map module_ldflags;
    std::string module_flags;
    std::string std_module_source;
    std::string llvm_prefix, llvm_cxx;
    translation_unit_list units_in_topological_order;
    std::mutex cache_mutex;
    std::mutex link_cache_mutex;
    const build_config config;
    const bool static_link;
    bool include_tests = false;
    bool include_examples = false;
    std::string extra_compile_flags;
    std::string extra_link_flags;

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
                log::error("std.cppm path not provided. Pass it as the first argument or set LLVM_PATH.");
                std::exit(1);
            }
        }

        auto std_module_path = fs::path{std_module_source};
        if (not fs::exists(std_module_path)) {
            log::error("std.cppm not found at: " + std_module_source);
            std::exit(1);
        }

        // Determine LLVM prefix from std.cppm path
        // Navigate up from share/libc++/v1/std.cppm or include/c++/v1/std.cppm to get LLVM root
        auto p = std_module_path;
        for (int i = 0; i < 4 and p.has_parent_path(); ++i) p = p.parent_path();
        llvm_prefix = p.string();
        
        // Find clang++ compiler binary
        // This is the C++ compiler we'll use for all compilation and linking
        auto command_available = [](const std::string& candidate) {
            if (candidate.find('/') == std::string::npos) {
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
        if (try_env_compiler())
            return;

        llvm_cxx = llvm_prefix + "/bin/clang++";
        if (!command_available(llvm_cxx)) {
            log::error("clang++ not found. Expected: " + llvm_cxx + " (set LLVM_CXX to override).");
            std::exit(1);
        }
    }

    void initialize_build_flags()
    {
        auto os        = os_name();
        bool is_darwin = (os == "darwin");
        bool is_linux  = (os == "linux");
    
        // ------------------------------------------------------------------
        // Compile flags (common to all configs)
        // ------------------------------------------------------------------
        // Use -B to tell clang++ where to find binaries (like the linker)
        compile_flags = "-B" + llvm_prefix + "/bin "
                        "-fuse-ld=lld "
                        "-std=c++23 -stdlib=libc++ -pthread -fPIC "
                        "-fexperimental-library -Wall -Wextra "
                        "-Wno-reserved-module-identifier "
                        "-Wno-unused-command-line-argument ";
    
        if (is_linux) {
            // Linux: normal include path, no -nostdinc++
            compile_flags += "-I" + llvm_prefix + "/include/c++/v1 ";
        } else {
            // macOS + everything else: silence system headers, disable implicit modules
            compile_flags += "-nostdinc++ -isystem " + llvm_prefix + "/include/c++/v1 "
                             "-fno-implicit-modules -fno-implicit-module-maps ";
        }
    
        // ------------------------------------------------------------------
        // Optimization / debug flags
        // ------------------------------------------------------------------
        if (config == build_config::release) {
            compile_flags += "-O3 -DNDEBUG ";
            log::info("Building RELEASE configuration"s + (static_link ? " (static C++ stdlib)"s : ""s));
        } else {
            compile_flags += "-O0 -g3 ";
            log::info("Building DEBUG configuration"s + (static_link ? " (static C++ stdlib)"s : ""s));
        }
    
        // ------------------------------------------------------------------
        // Machine-readable diagnostics (JSONL mode)
        // ------------------------------------------------------------------
        // Keep output low-noise in machine runs (stderr only).
        if (cb::jsonl::enabled()) {
            compile_flags += "-fno-caret-diagnostics -fno-show-column -fno-show-source-location ";
        }
    
        // ------------------------------------------------------------------
        // Link flags
        // ------------------------------------------------------------------
        if (static_link) {
            if (is_darwin) {
                // macOS cannot fully static-link libc++ (it stays dynamic)
                link_flags = "-pthread -lc++ "
                             "-L" + llvm_prefix + "/lib "
                             "-Wl,-dead_strip";
                log::warning("Static linking on macOS is limited – libc++ remains dynamically linked");
            } else {
                // Linux – static libc++ / libc++abi only (glibc stays dynamic)
                auto arch = linux_arch();  // e.g. "aarch64"
                link_flags = "-Wl,-Bstatic -lc++ -lc++abi -lc++experimental "
                             "-Wl,-Bdynamic -pthread -ldl "
                             "-L/usr/lib/" + arch + "-linux-gnu "
                             "-L" + llvm_prefix + "/lib "
                             "-O3";
                if (config == build_config::debug) {
                    link_flags += " -g3";  // replace -O3 in debug builds if you want
                }
            }
        }
        else {
            // ---------- Dynamic linking ----------
            if (is_darwin) {
                // Workaround for llvm/llvm-project#92121 and #168287:
                // When using ld64.lld, we must explicitly link with LLVM libunwind
                // to avoid exception handling bugs on macOS ARM.
                // See: https://gist.github.com/ruoka/a62dbdddeacec52d75382791bdc0a2ba
                link_flags = "-pthread "
                             "-L" + llvm_prefix + "/lib "
                             "-Wl,-rpath," + llvm_prefix + "/lib "
                             "-lunwind "
                             "-Wl,-dead_strip ";
                if (fs::exists("/usr/lib/system/introspection/libunwind.reexported_symbols")) {
                    link_flags += "-Wl,-unexported_symbols_list,/usr/lib/system/introspection/libunwind.reexported_symbols";
                }
            } else {
                // Linux dynamic – clean, correct, no -lunwind ever
                auto arch = linux_arch();
                link_flags = "-pthread -lc++ -lc++abi -lc++experimental "
                             "-L/usr/lib/" + arch + "-linux-gnu "
                             "-L" + llvm_prefix + "/lib "
                             "-Wl,-rpath," + llvm_prefix + "/lib "
                             "-O3";
                if (config == build_config::debug) {
                    link_flags += " -g3";
                }
            }
        }
        
        // Append extra link flags if provided
        if (not extra_link_flags.empty()) {
            link_flags += " " + extra_link_flags;
            log::info("Added extra linker flags: "s + extra_link_flags);
        }
        
        // Append extra compile flags if provided
        if (not extra_compile_flags.empty()) {
            compile_flags += " " + extra_compile_flags;
            log::info("Added extra compile flags: "s + extra_compile_flags);
        }
    
        // ------------------------------------------------------------------
        // Module flags (used for import std; and your own modules)
        // ------------------------------------------------------------------
        module_flags = "-fno-implicit-modules -fno-implicit-module-maps "
                       "-fmodule-file=std=" + std_pcm_path() + " "
                       "-fprebuilt-module-path=" + module_cache_dir() + " ";
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
        log::error("Unsupported architecture. Only x86_64 and aarch64 are supported.");
        std::exit(1);
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

    static bool erase_once(std::string& s, std::string_view needle)
    {
        const auto pos = s.find(needle);
        if (pos == std::string::npos) return false;
        s.erase(pos, needle.size());
        return true;
    }

    void execute_system_command(std::string_view cmd) const
    {
        auto cmd_str = std::string{cmd};
        // Human logs are suppressed in JSONL mode; still emit machine-readable command events.
        if (cb::jsonl::enabled())
            cb::jsonl::sink().command_start(cmd_str);
        else
            log::command(cmd_str);

        const auto started = std::chrono::steady_clock::now();
        auto r = system(cmd_str.c_str());
        const auto finished = std::chrono::steady_clock::now();

        if (cb::jsonl::enabled())
            cb::jsonl::sink().command_end(cmd_str, r == 0, r, jsonl_util::duration_ms(started, finished));
        if (r) {
            log::error("Command failed: "s + cmd_str);
            std::exit(1);
        }
    }

    std::string collect_module_ldflags(const string_list& imp) const {
        auto f = ""s;
        for (const auto& m : imp)
            if (auto it = module_ldflags.find(m); it != module_ldflags.end())
                f += it->second + " ";
        return f;
    }

    // ============================================================================
    // Cache Management
    // ============================================================================

    object_cache_map load_object_cache() {
        auto cache = object_cache_map{};
        auto file = std::ifstream{object_cache_path()};
        if (file) {
            auto path = ""s;
            long long ticks = 0;
            while (std::getline(file, path, '\t') and file >> ticks) {
                cache[path] = fs::file_time_type{std::chrono::nanoseconds{ticks}};
                file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
        }
        return cache;
    }

    void save_object_cache(const object_cache_map& c) {
        auto tmp = object_cache_path() + ".tmp";
        auto file = std::ofstream{tmp};
        if (file) {
            for (const auto& [path, timestamp] : c) {
                auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    timestamp.time_since_epoch()).count();
                file << path << "\t" << ticks << "\n";
            }
        }
        fs::rename(tmp, object_cache_path());
    }

    bool needs_recompile(const translation_unit& tu, object_cache_map& c, const unit_to_tu_map& u2tu) const {
        // If we have never seen the file (or it changed since last compile), rebuild.
        auto cached = c.find(tu.full_path);
        if (cached == c.end() or cached->second < tu.last_modified)
            return true;

        // Ensure the object file exists and is up-to-date versus the source timestamp we cached.
        if (not fs::exists(tu.object_path))
            return true;

        auto object_timestamp = fs::last_write_time(tu.object_path);
        if (object_timestamp < cached->second)
            return true;

        // For modular units, also check if .pcm file is stale
        if (tu.is_modular) {
            if (not fs::exists(tu.pcm_path))
                return true;
            auto pcm_timestamp = fs::last_write_time(tu.pcm_path);
            if (pcm_timestamp < tu.last_modified)
                return true;
        }

        // Rebuild if any imported modules have changed (their .pcm files are stale or they need recompiling)
        for (const auto& dependency_key : tu.imports) {
            if (auto dependency = u2tu.find(dependency_key); dependency != u2tu.end()) {
                const auto& dep_tu = *dependency->second;
                // Check if the imported module's .pcm is stale compared to its source
                if (dep_tu.is_modular) {
                    if (not fs::exists(dep_tu.pcm_path) or 
                        fs::last_write_time(dep_tu.pcm_path) < dep_tu.last_modified) {
                        return true;  // Imported module's .pcm is stale, rebuild this unit
                    }
                }
                // Also recursively check if the imported module needs recompiling
                if (needs_recompile(dep_tu, c, u2tu))
                return true;
            }
        }

        return false;
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
        if (not file) return;
        for (const auto& [path, signature] : cache)
            file << path << "\t" << signature << "\n";
        file.close();
        fs::rename(tmp, executable_cache_path());
    }

    bool needs_relinking(const translation_unit& tu, const std::string& signature, const executable_cache_map& link_cache) const {
        if (not fs::exists(tu.executable_path))
            return true;

        if (auto it = link_cache.find(tu.executable_path); it != link_cache.end() and it->second == signature)
            return false;

        return true;
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
                    log::warning("Skipping "s + entry.path().string() + ": " + e.what());
                }
            }
        } catch (const std::exception& e) {
            log::error("Failed to scan project: "s + e.what());
            throw;
        } catch (...) {
            log::error("Failed to scan project: unknown error");
            throw;
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
            log::error(message);
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

        auto cmd = llvm_cxx + " " + compile_flags + cpp_flags +
                   " -nostdinc++ -isystem " + llvm_prefix + "/include/c++/v1 "
                   " -Wno-unused-command-line-argument -fno-implicit-modules "
                   " -fno-implicit-module-maps -Wno-reserved-module-identifier "
                   + std_module_source + " --precompile -o " + std_pcm;
        execute_system_command(cmd);
    }

    void build_std_o() {
        auto std_pcm = std_pcm_path();
        auto std_obj = std_obj_path();
        if (not fs::exists(std_pcm)) build_std_pcm();
        if (fs::exists(std_obj) and fs::last_write_time(std_obj) >= fs::last_write_time(std_pcm))
            return;

        auto os = os_name();
        auto is_darwin = (os == "darwin");
        
        std::string std_obj_flags = "-std=c++23 -pthread -fPIC -fexperimental-library -Wall -Wextra ";
        if (is_darwin)
            std_obj_flags += "-fapplication-extension ";
        if (config == build_config::release) {
            std_obj_flags += "-O3 -DNDEBUG ";
        } else {
            std_obj_flags += "-O0 -g ";
        }
        
        std_obj_flags += "-fno-implicit-modules -fno-implicit-module-maps ";
        std_obj_flags += "-fmodule-file=std=" + std_pcm + " ";
        
        auto cmd = llvm_cxx + " " + std_obj_flags + std_pcm + " -c -o " + std_obj;
        execute_system_command(cmd);
    }

    // ============================================================================
    // Compilation
    // ============================================================================

    void compile_unit(const translation_unit& tu) {
        if (tu.is_modular) {
            // Regular module interface/partition - create PCM file
            execute_system_command(
                llvm_cxx + " " + compile_flags + cpp_flags + " " + module_flags + " " + tu.full_path + " --precompile -o " + tu.pcm_path
            );
            execute_system_command(
                llvm_cxx + " " + compile_flags + module_flags + " " + tu.pcm_path + " -c -o " + tu.object_path
            );
        } else {
            auto extra = ""s;
            if (tu.kind == unit_kind::implementation_unit) {
                // Implementation units need the module PCM file
                auto module_pcm = compute_pcm_path(tu);
                extra = "-fmodule-file=" + tu.module + "=" + module_pcm + " ";
            }

            execute_system_command(
                llvm_cxx + " " + compile_flags + cpp_flags + " " + module_flags + " " + extra + tu.full_path + " -c -o " + tu.object_path
            );
        }
    }

    void update_module_flags() {
        auto flags = ""s;
        for (const auto& tu : units_in_topological_order) {
            if (tu.is_modular) {
                flags += "-fmodule-file=" + tu.module + "=" + tu.pcm_path + " ";
            }
        }
        module_flags += flags;
    }

    void compile_units() {
        if (units_in_topological_order.empty()) return;
        auto cache = load_object_cache();

        auto u2tu = unit_to_tu_map{};
        for (auto& tu : units_in_topological_order) {
            auto k = tu.unit;
            u2tu[k] = &tu;
        }

        auto levels = level_groups_map{};
        for (const auto& tu : units_in_topological_order)
            levels[tu.dependency_level >= 0 ? tu.dependency_level : INT_MAX].push_back(&tu);

        for (auto& [lvl, group] : levels) {
            auto threads = thread_list{};
                for (const auto* tu : group) {
                threads.emplace_back([this, tu, &cache, &u2tu]() {
                    if (needs_recompile(*tu, cache, u2tu)) {
                        compile_unit(*tu);
                        auto lock = std::lock_guard<std::mutex>{cache_mutex};
                        cache[tu->full_path] = tu->last_modified;
                    }
                });
            }
            for (auto& thread : threads) thread.join();
        }
        save_object_cache(cache);
    }

    // ============================================================================
    // Linking
    // ============================================================================

    string_list linkable_object_paths() const {
        auto objects = string_list{};
        for (const auto& tu : units_in_topological_order)
            if (not tu.has_main and not tu.is_test)
                objects.push_back(tu.object_path);
        return objects;
    }

    std::string collect_linkable_objects() const {
        auto buffer = ""s;
        for (const auto& object_path : linkable_object_paths())
            buffer += object_path + " "s;
        return buffer;
    }

    void link_executable(const translation_unit& tu, const string_list& shared_objects) {
        if (not tu.has_main) return;

        auto objects = tu.object_path + " ";
        for (const auto& object_path : shared_objects)
            objects += object_path + " ";

        auto cmd = llvm_cxx + " " + compile_flags + " " +
                collect_module_ldflags(tu.imports) + " " +
                module_flags + " " +
                objects +
                std_obj_path() + " " +
                link_flags + " -o " + tu.executable_path;

        execute_system_command(cmd);
    }

    std::string dependency_signature(const std::string& path) const {
        if (path.empty() or not fs::exists(path))
            return path + ":missing";
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            fs::last_write_time(path).time_since_epoch()).count();
        return path + ":" + std::to_string(timestamp);
    }

    std::string compute_link_signature(const translation_unit& tu, const string_list& shared_objects) const {
        auto signature = std::string{};
        signature.reserve(256);
        signature += dependency_signature(tu.object_path);
        for (const auto& object_path : shared_objects) {
            signature += "|";
            signature += dependency_signature(object_path);
        }
        signature += "|";
        signature += dependency_signature(std_obj_path());
        signature += "|flags=";
        signature += compile_flags;
        signature += "|link=";
        signature += link_flags;
        signature += "|modules=";
        signature += module_flags;
        signature += "|imports=";
        signature += collect_module_ldflags(tu.imports);
        return signature;
    }

    void link_executables() {
        auto shared_objects = linkable_object_paths();
        auto link_cache = load_executable_cache();
        auto threads = thread_list{};
        for (const auto& tu : units_in_topological_order)
            if (tu.has_main and not tu.filename.contains("test_runner")) {
                auto signature = compute_link_signature(tu, shared_objects);
                if (not needs_relinking(tu, signature, link_cache)) {
                    log::info("Skipping link (up-to-date): "s + tu.executable_path);
                    continue;
                }
                threads.emplace_back([this, tu, &shared_objects, signature, &link_cache]() {
                    link_executable(tu, shared_objects);
                    auto lock = std::lock_guard<std::mutex>{link_cache_mutex};
                    link_cache[tu.executable_path] = signature;
                });
            }
        for (auto& thread : threads) thread.join();
        save_executable_cache(link_cache);
    }

    // ============================================================================
    // Test Support
    // ============================================================================

    std::string collect_linkable_test_objects() const {
        auto objects = ""s;
        for (const auto& tu : units_in_topological_order)
            if (tu.is_test and not tu.has_main)
                objects += tu.object_path + " "s;
        return objects;
    }

    std::string collect_test_module_ldflags() const {
        auto f = ""s;
        for (const auto& tu : units_in_topological_order)
            if (tu.is_test and not tu.has_main)
                f += collect_module_ldflags(tu.imports);
        return f;
    }

    void link_test_runner() {
        auto objects = collect_linkable_test_objects();
        if (objects.empty()) {
            log::info("No objects to link for test_runner");
            return;
        }

        // Find test_runner translation unit if it exists
        auto test_runner_it = std::find_if(units_in_topological_order.begin(), units_in_topological_order.end(),
            [this](const translation_unit& tu) {
                return tu.has_main and tu.base_name.contains("test_runner");
            });

        if (test_runner_it != units_in_topological_order.end()) {
            auto cmd = llvm_cxx + " " + compile_flags + " " +
                    collect_module_ldflags(test_runner_it->imports) + " " +
                    module_flags + " " +
                    test_runner_it->object_path + " " +
                    collect_linkable_objects() + // Regular (non-main, non-test) objects
                    objects + // Test objects
                    std_obj_path()  + " " + link_flags +
                    " -o " + test_runner_it->executable_path;
            execute_system_command(cmd);
            log::success("test_runner linked with test objects");
        } else {
            // Auto-create test_runner (no source file, so compute path directly)
            auto cmd = llvm_cxx + " " + compile_flags + " " +
                    collect_test_module_ldflags() + " " +
                    module_flags + " " +
                    collect_linkable_objects() + // Regular (non-main, non-test) objects
                    objects + // Test objects
                    std_obj_path() + " " + link_flags +
                    " -o " + binary_dir() + "/test_runner";
            execute_system_command(cmd);
            log::success("test_runner linked successfully");
        }
    }

public:
    build_system(
        build_config cfg,
        const std::string& cpf = "",
        const module_to_ldflags_map& mlf = {},
        const std::string& src = ".",
        const std::string& stdcppm = "",
        bool static_linking = false,
        bool include_examples_flag = false,
        const std::string& extra_compile_flags_param = "",
        const std::string& extra_link_flags_param = ""
    ) : config(cfg), static_link(static_linking), source_dir(src), cpp_flags(cpf), module_ldflags(mlf), std_module_source(stdcppm), include_tests(config == build_config::debug), include_examples(include_examples_flag), extra_compile_flags(extra_compile_flags_param), extra_link_flags(extra_link_flags_param) {
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
            log::success("Removed "s + dir);
        } else {
            log::info("Nothing to clean for "s + dir);
        }
    }

    void set_include_tests(bool value) {
        include_tests = value;
    }

    void build() {    
        const auto build_started = std::chrono::steady_clock::now();
        cb::jsonl::current_phase = cb::jsonl::phase::build;
        cb::jsonl::phase_started = build_started;
        cb::jsonl::build_end_emitted = false;

        if (cb::jsonl::enabled()) {
            cb::jsonl::sink().build_start(config_name(config), false, include_examples);
        }

        // Ensure build directories exist (they may have been removed by clean())
        fs::create_directories(module_cache_dir());
        fs::create_directories(object_dir());
        fs::create_directories(binary_dir());
        fs::create_directories(cache_dir());

        build_std_pcm();
        build_std_o();
        scan_and_order();
        if (units_in_topological_order.empty()) {
            if (cb::jsonl::enabled()) {
                cb::jsonl::sink().build_end(false, jsonl_util::duration_ms(build_started, std::chrono::steady_clock::now()));
                cb::jsonl::build_end_emitted = true;
            }
            log::error("No sources found");
            std::exit(1);
        }

        update_module_flags();
        compile_units();
        link_executables();

        if (cb::jsonl::enabled()) {
            cb::jsonl::sink().build_end(true, jsonl_util::duration_ms(build_started, std::chrono::steady_clock::now()));
            cb::jsonl::build_end_emitted = true;
            cb::jsonl::current_phase = cb::jsonl::phase::none;
        }

        log::success("Build completed: "s + build_root());
    }

    void run_tests(const std::vector<std::string>& args = {}) {
        log::info("=== Running tests ===");

        const auto build_started = std::chrono::steady_clock::now();
        cb::jsonl::current_phase = cb::jsonl::phase::build;
        cb::jsonl::phase_started = build_started;
        cb::jsonl::build_end_emitted = false;
        if(cb::jsonl::enabled())
            cb::jsonl::sink().build_start(config_name(config), true, include_examples);

        include_tests = true;
        build();
        link_test_runner();

        auto runner = binary_dir() + "/test_runner";
        if (not fs::exists(runner)) {
            log::error("test_runner not found — no test files discovered");
            log::error("Make sure you have .test.c++ files or a test_runner.c++");
            std::exit(1);
        }
    
        const auto build_finished = std::chrono::steady_clock::now();
        cb::jsonl::sink().build_end(true, jsonl_util::duration_ms(build_started, build_finished));
        cb::jsonl::build_end_emitted = true;
        cb::jsonl::current_phase = cb::jsonl::phase::none;

        auto cmd = runner;
        for (const auto& a : args)
            cmd += " " + shell_quote(a);
        log::command(cmd);

        const auto test_started = std::chrono::steady_clock::now();
        cb::jsonl::current_phase = cb::jsonl::phase::test;
        cb::jsonl::phase_started = test_started;
        cb::jsonl::sink().test_start(runner);

        auto r = system(cmd.c_str());
        auto exit_code = r;
        auto signaled = false;
        auto signal_number = 0;
#if defined(WIFEXITED) && defined(WEXITSTATUS) && defined(WIFSIGNALED) && defined(WTERMSIG)
        if (r != -1)
        {
            if (WIFEXITED(r))
                exit_code = WEXITSTATUS(r);
            else if (WIFSIGNALED(r))
            {
                signaled = true;
                signal_number = WTERMSIG(r);
                exit_code = 128 + signal_number;
            }
        }
#endif
        const auto test_finished = std::chrono::steady_clock::now();
        cb::jsonl::sink().test_end(r == 0, exit_code, r, signaled, signal_number, jsonl_util::duration_ms(test_started, test_finished));
        cb::jsonl::current_phase = cb::jsonl::phase::none;
        if (r) {
            log::error("Some tests or assertions failed!");
            std::exit(1);
        }
        log::success("All tests passed!");
    }

    void print_sources() {
        scan_and_order();
        auto& mux = cb::jsonl::io_mux();
        auto lock = std::lock_guard<std::mutex>{mux.mutex};
        auto& os = mux.human_os();
        os << io::color::cyan << "\nFound " << units_in_topological_order.size() << " translation units:\n\n" << io::color::reset;
        int main_count = 0, test_count = 0;
        for (const auto& tu : units_in_topological_order) {
            if (tu.has_main) main_count++;
            if (tu.is_test) test_count++;
        }
        os << io::color::cyan << " Total: " << units_in_topological_order.size()
                  << " | Main: " << main_count
                  << " | Tests: " << test_count << "\n\n" << io::color::reset;

        for (const auto& tu : units_in_topological_order) {
            auto full = tu.path.empty() ? tu.filename : tu.path + "/" + tu.filename;
            os << io::color::cyan << " " << full << io::color::reset;
            if (not tu.module.empty()) os << " " << io::color::yellow << "[module: " << tu.module << "]" << io::color::reset;
            if (tu.has_main) os << " " << io::color::green << "[main]" << io::color::reset;
            if (tu.is_test) os << " " << io::color::magenta << "[TEST]" << io::color::reset;
            if (tu.dependency_level >= 0) os << " " << io::color::gray << "level=" << tu.dependency_level << io::color::reset;
            os << "\n";
        }
        os << io::color::cyan << "\n" << io::color::reset;
    }
};

} // namespace cb

using namespace std::string_literals;

static std::string shell_quote(std::string_view arg)
{
    // POSIX shell single-quote escaping: ' -> '\'' 
    auto out = std::string{};
    out.reserve(arg.size() + 2);
    out.push_back('\'');
    for(const char ch : arg)
    {
        if(ch == '\'')
            out.append("'\\''");
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

int main(int argc, char* argv[])
try {
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
    auto test_filter = std::string{};
    auto test_runner_args = std::vector<std::string>{};
    auto machine_output = false;
    auto static_linking = false;
    auto include_examples = false;
    auto build_tests = false;  // --build-tests flag: build tests but don't run them
    auto include_paths = std::vector<std::string>{};
    auto extra_compile_flags = std::string{};
    auto extra_link_flags = std::string{};

    for (int i = arg_index; i < argc; ++i) {
        auto argument = std::string_view{argv[i]};
        if (argument == "--output=jsonl" || argument == "--output=JSONL") {
            cb::jsonl::set_enabled(true);
            continue;
        }
        if (argument == "--") {
            // Everything after "--" is passed to test_runner (only meaningful with "test").
            for (int j = i + 1; j < argc; ++j)
                test_runner_args.emplace_back(argv[j]);
            for (const auto& a : test_runner_args)
                if (a == "--output=jsonl" || a == "--output=JSONL")
                    machine_output = true;
            break;
        }
        if (argument == "release") {
            config = cb::build_config::release;
        } else if (argument == "debug") {
            config = cb::build_config::debug;
        } else if (argument == "test") {
            do_run_tests = true;
            if (i+1 < argc and argv[i+1][0] != '-')
                test_filter = argv[++i];
        } else if (argument == "ci") {
            do_clean = true;
            do_run_tests = true;
        } else if (argument == "clean") {
            do_clean = true;
        } else if (argument == "build") {
            do_build = true;
        } else if (argument == "list") {
            do_list = true;
        } else if (argument == "static") {
            static_linking = true;
        } else if (argument == "--include-examples") {
            include_examples = true;
        } else if (argument == "--build-tests") {
            build_tests = true;
        } else if (do_run_tests && (argument.starts_with("--output=") ||
                                   argument.starts_with("--slowest=") ||
                                   argument.starts_with("--jsonl-output=") ||
                                   argument.starts_with("--jsonl-output-max-bytes=") ||
                                   argument == "--result")) {
            // Convenience: allow passing common test_runner CLI flags directly
            test_runner_args.emplace_back(argv[i]);
            if (argument == "--output=jsonl" || argument == "--output=JSONL")
                machine_output = true;
        } else if (argument == "-I" or argument == "--include") {
            if (i+1 < argc) {
                include_paths.push_back(argv[++i]);
            } else {
                cb::log::error("Missing path after -I/--include");
                std::exit(1);
            }
        } else if (argument == "--link-flags") {
            if (i+1 < argc) {
                extra_link_flags = argv[++i];
            } else {
                cb::log::error("Missing flags after --link-flags");
                std::exit(1);
            }
        } else if (argument == "--compile-flags" or argument == "--extra-compile-flags") {
            if (i+1 < argc) {
                extra_compile_flags = argv[++i];
            } else {
                cb::log::error("Missing flags after --compile-flags");
                std::exit(1);
            }
        } else if (argument == "help" or argument == "-h" or argument == "--help") {
            std::cout << "Usage: " << argv[0] << " [std.cppm] [options]\n\n"
                      << "Options:\n"
                      << "  release          Build in release mode (optimized, no tests)\n"
                      << "  debug            Build in debug mode (with debug symbols, includes tests)\n"
                      << "  build            Build the project (default if no action specified)\n"
                      << "  clean            Remove build directories\n"
                      << "  ci               Clean and run tests (shortcut for: clean test)\n"
                      << "  list             List all translation units\n"
                      << "  test [filter] [-- <args...>]  Build and run tests (optional filter)\n"
                      << "                 Pass extra args to test_runner after '--' (recommended)\n"
                      << "                 or pass common flags directly (e.g. --output=jsonl)\n"
                      << "  static           Enable static linking (C++ stdlib static)\n"
                      << "  --include-examples Include examples directory in build (excluded by default)\n"
                      << "  --build-tests    Build tests in release mode (useful for CI to verify compilation)\n"
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
                      << "  " << argv[0] << " test -- --output=jsonl --slowest=10\n"
                      << "  " << argv[0] << " clean\n";
            return 0;
        }
    }

    // If we are going to run tests in JSONL mode, or build in JSONL mode,
    // keep stdout machine-parseable by moving all CB human logs (including clean/build) to stderr.
    if(machine_output || cb::jsonl::enabled())
    {
        cb::jsonl::io_mux().set_human(std::cerr);
        cb::jsonl::io_mux().set_result(std::cerr);
    }
    else
    {
        cb::jsonl::io_mux().set_human(std::cout);
        cb::jsonl::io_mux().set_result(std::cout);
    }
    // For tests, JSONL is controlled by machine_output. For builds, it's already set.
    if (!cb::jsonl::enabled()) {
        cb::jsonl::set_enabled(machine_output);
    }
    cb::jsonl::reset();
    if (cb::jsonl::enabled())
        std::atexit(cb::jsonl_atexit_handler);

    // Build include flags from command-line arguments
    auto include_flags = std::string{};
    if (not include_paths.empty()) {
        // Use provided include paths
        for (const auto& path : include_paths) {
            include_flags += "-I " + path + " ";
        }
    }

    auto build_system = cb::build_system{config, include_flags, {}, ".", stdcppm, static_linking, include_examples, extra_compile_flags, extra_link_flags};

    if (do_list) build_system.print_sources();
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

        // If CB is in JSONL mode, automatically enable JSONL for test_runner
        // unless the user explicitly specified a different output format
        bool has_output_flag = false;
        for (const auto& arg : test_runner_args) {
            if (arg.starts_with("--output=")) {
                has_output_flag = true;
                break;
            }
        }
        if (cb::jsonl::enabled() && !has_output_flag) {
            args.emplace_back("--output=jsonl");
        } else if (cb::jsonl::enabled() && has_output_flag) {
            // User specified custom output format, respect it
            // Don't add --output=jsonl
        }

        for (auto& a : test_runner_args)
            args.emplace_back(a);

        // Build include_tests etc inside run_tests(), but pass args as tokens.
        build_system.run_tests(args);
    }
    if (not do_clean and not do_list and not do_run_tests and not do_build) build_system.build();

    return 0;
} catch (const std::exception& e) {
    cb::log::error("Fatal error: "s + e.what());
    std::exit(1);
} catch (...) {
    cb::log::error("Fatal error: unknown exception");
    std::exit(1);
}
