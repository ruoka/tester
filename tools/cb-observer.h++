// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cb::output {

struct profile_scalar_change
{
    std::string old_value;
    std::string new_value;
};

struct profile_token_change
{
    std::vector<std::string> added;
    std::vector<std::string> removed;

    bool changed() const { return not added.empty() or not removed.empty(); }
};

struct object_cache_profile_diff
{
    std::optional<profile_scalar_change> format;
    std::optional<profile_scalar_change> config;
    std::optional<profile_scalar_change> static_link;
    std::optional<profile_scalar_change> llvm;
    std::optional<profile_scalar_change> cxx;
    std::optional<profile_scalar_change> cxx_sig;
    std::optional<profile_scalar_change> clang_ver;
    std::optional<profile_scalar_change> std_cppm;
    std::optional<profile_token_change> compile;
    std::optional<profile_token_change> cpp;

    bool empty() const
    {
        return not format and not config and not static_link and not llvm
            and not cxx and not cxx_sig and not clang_ver and not std_cppm
            and not compile and not cpp;
    }
};

template<typename Diff, typename Callback>
void for_each_profile_scalar(Diff& diff, Callback&& callback)
{
    callback("format", diff.format);
    callback("config", diff.config);
    callback("static_link", diff.static_link);
    callback("llvm", diff.llvm);
    callback("cxx", diff.cxx);
    callback("cxx_sig", diff.cxx_sig);
    callback("clang_ver", diff.clang_ver);
    callback("std_cppm", diff.std_cppm);
}

template<typename Diff, typename Callback>
void for_each_profile_tokens(Diff& diff, Callback&& callback)
{
    callback("compile", diff.compile);
    callback("cpp", diff.cpp);
}

struct source_unit
{
    std::string unit;
    std::string path;
    std::string module;
    std::string kind;
    std::vector<std::string> imports;
    int level;
    bool has_main;
    bool is_test;
    bool is_modular;
};

struct source_inventory
{
    std::string config;
    bool include_tests;
    bool include_examples;
    std::string source_dir;
    std::vector<source_unit> units;
    int main_count;
    int test_count;
    int max_level;
};

// A span of monotonic work. Every phase event carries one, and passing the pair as a value
// removes the chance of transposing the endpoints at a call site that has nothing else to
// distinguish them.
struct interval
{
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point finished{};

    std::chrono::milliseconds::rep elapsed_ms() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
    }
};

// What a compile event is about: the four fields observers format, projected out of the
// build system's translation unit so this contract stays free of build internals — the same
// reason source_unit exists for the list command.
struct compile_unit
{
    std::string_view source;
    std::string_view object;
    std::string_view pcm;    // empty unless the unit is modular
    std::string_view module; // empty unless the unit belongs to a module
};

// Rebuild telemetry kinds for compile/link (wire name is also rebuild_reason).
enum class rebuild_kind
{
    none,
    not_in_cache,
    source_stale,
    header_stale,
    depfile_unusable,
    object_missing,
    object_stale,
    own_pcm_missing,
    own_pcm_stale,
    pcm_stale,
    dependency_pcm_stale,
    profile_change,
    missing_executable,
    object_changed,
    link_flags_changed,
    signature_changed,
};

constexpr std::string_view rebuild_kind_name(rebuild_kind kind)
{
    switch(kind)
    {
        case rebuild_kind::none: return {};
        case rebuild_kind::not_in_cache: return "not_in_cache";
        case rebuild_kind::source_stale: return "source_stale";
        case rebuild_kind::header_stale: return "header_stale";
        case rebuild_kind::depfile_unusable: return "depfile_unusable";
        case rebuild_kind::object_missing: return "object_missing";
        case rebuild_kind::object_stale: return "object_stale";
        case rebuild_kind::own_pcm_missing: return "own_pcm_missing";
        case rebuild_kind::own_pcm_stale: return "own_pcm_stale";
        case rebuild_kind::pcm_stale: return "pcm_stale";
        case rebuild_kind::dependency_pcm_stale: return "dependency_pcm_stale";
        case rebuild_kind::profile_change: return "profile_change";
        case rebuild_kind::missing_executable: return "missing_executable";
        case rebuild_kind::object_changed: return "object_changed";
        case rebuild_kind::link_flags_changed: return "link_flags_changed";
        case rebuild_kind::signature_changed: return "signature_changed";
    }
    return {};
}

// std::system returns a wait status, not an exit code: a child exiting 1 yields 256.
// Decoding once at the shell boundary keeps exit_code meaningful and makes a child
// killed by a signal distinguishable from one that exited with that number.
struct process_status
{
    int exit_code = -1;
    int wait_status = -1;
    bool signaled = false;
    int signal = 0;

    bool ok() const { return not signaled and exit_code == 0; }
};

// Captured output of a failed toolchain command. `command_end.ok:false` alone is not
// actionable — an agent told to parse stdout only has no way to see why clang failed.
struct diagnostics
{
    std::string path;      // file holding the full captured output
    std::string head;      // bounded excerpt for inline reporting
    std::size_t bytes = 0; // captured size before truncation
    bool truncated = false;

    bool empty() const { return head.empty() and path.empty(); }
};

// Structured rebuild telemetry for compile/link JSONL (kind is also rebuild_reason).
struct rebuild_info
{
    rebuild_kind kind = rebuild_kind::none;
    std::string module;
    std::string pcm_path;
    std::string object_path;
    std::string trigger_path;
    std::string hint;
    std::string message;

    bool empty() const { return kind == rebuild_kind::none; }
};

// How a compile or link step finished. Trailing defaults cannot help here — notify() calls
// through a pointer to member, and those never apply default arguments — so the optional
// tail lives in the struct instead: a cache hit names only ok, cache_hit, and timing, and
// the two bools stop being interchangeable positions.
struct step_result
{
    bool ok = false;
    bool cache_hit = false;
    interval timing{};
    rebuild_info rebuild{}; // why the step ran; empty on a cache hit
    diagnostics diag{};     // what the toolchain said; empty unless the step failed
};

class observer
{
public:
    virtual ~observer() = default;

    virtual void activate() {}
    virtual void finish() {}
    virtual std::string_view run_id() const { return {}; }
    virtual void error(std::string_view) {}
    virtual void warning(std::string_view) {}
    virtual void info(std::string_view) {}
    virtual void success(std::string_view) {}
    virtual void command(std::string_view) {}
    virtual void profile_changed(rebuild_kind, const object_cache_profile_diff&) {}
    virtual void profile_change_rebuild(std::string_view) {}

    // Parameter names are commented rather than declared: the default bodies ignore them, and
    // a named unused parameter warns under -Wextra. The names are the contract's only
    // documentation of which string_view is which, so they stay.
    virtual void cache_status(
        std::string_view /*object_cache_path*/,
        bool /*object_cache_exists*/,
        bool /*profile_match*/,
        int /*object_entries*/,
        int /*object_stale_entries*/,
        int /*executable_entries*/,
        std::string_view /*current_profile*/) {}

    virtual void cache_invalidate_end(bool /*object_cache_removed*/,
                                      bool /*executable_cache_removed*/,
                                      bool /*compiler_stamp_removed*/) {}

    virtual void source_list(const source_inventory& /*inventory*/) {}

    virtual void build_start(std::string_view /*config*/,
                             bool /*include_tests*/,
                             bool /*include_examples*/) {}

    virtual void build_end(bool /*ok*/, const interval& /*timing*/) {}

    virtual void test_start(std::string_view /*runner*/) {}

    virtual void test_end(bool /*ok*/,
                          const process_status& /*status*/,
                          const interval& /*timing*/) {}

    virtual void command_start(std::string_view /*cmd*/,
                               std::span<const std::string> /*argv*/) {}

    virtual void command_end(
        std::string_view /*cmd*/,
        std::span<const std::string> /*argv*/,
        bool /*ok*/,
        const process_status& /*status*/,
        const diagnostics& /*diag*/,
        const interval& /*timing*/) {}

    virtual void compile_start(const compile_unit& /*unit*/, const rebuild_info& /*rebuild*/) {}

    virtual void compile_end(const compile_unit& /*unit*/, const step_result& /*step*/) {}

    virtual void link_end(std::string_view /*executable_path*/, const step_result& /*step*/) {}
};

inline auto observers = std::vector<std::reference_wrapper<observer>>{};
inline auto named_observers = std::vector<std::pair<std::string, std::reference_wrapper<observer>>>{};

inline void register_observer(std::string_view name, observer& value)
{
    if(const auto found = std::ranges::find_if(named_observers, [name](const auto& entry)
    {
        return entry.first == name;
    });
       found != named_observers.end())
    {
        found->second = value;
        return;
    }
    named_observers.emplace_back(name, value);
}

inline void finish()
{
    for(auto target : observers)
        target.get().finish();
}

inline void clear_observers()
{
    finish();
    observers.clear();
}

inline void observe(observer& value)
{
    observers.emplace_back(value);
    value.activate();
}

inline bool observing(const observer& value)
{
    return std::ranges::any_of(observers, [&value](const auto target)
    {
        return std::addressof(target.get()) == std::addressof(value);
    });
}

inline bool select_observer(std::string_view name)
{
    const auto found = std::ranges::find_if(named_observers, [name](const auto& entry)
    {
        return entry.first == name;
    });
    if(found == named_observers.end())
        return false;
    if(observing(found->second.get()))
        return true;

    clear_observers();
    observe(found->second.get());
    return true;
}

template<typename Callback>
void notify(Callback&& callback)
{
    for(auto target : observers)
        callback(target.get());
}

template<typename Method, typename... Args>
void notify(Method method, Args&&... args)
{
    for(auto target : observers)
        std::invoke(method, target.get(), args...);
}

inline std::string_view run_id()
{
    for(auto target : observers)
    {
        if(const auto value = target.get().run_id(); not value.empty())
            return value;
    }
    return {};
}

} // namespace cb::output
