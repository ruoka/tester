// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <algorithm>
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
    std::optional<profile_scalar_change> module_phases;
    std::optional<profile_scalar_change> llvm;
    std::optional<profile_scalar_change> cxx;
    std::optional<profile_scalar_change> cxx_sig;
    std::optional<profile_scalar_change> clang_ver;
    std::optional<profile_scalar_change> std_cppm;
    std::optional<profile_token_change> compile;
    std::optional<profile_token_change> cpp;

    bool empty() const
    {
        return not format and not config and not static_link and not module_phases
            and not llvm and not cxx and not cxx_sig and not clang_ver and not std_cppm
            and not compile and not cpp;
    }
};

template<typename Diff, typename Callback>
void for_each_profile_scalar(Diff& diff, Callback&& callback)
{
    callback("format", diff.format);
    callback("config", diff.config);
    callback("static_link", diff.static_link);
    callback("module_phases", diff.module_phases);
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

// Built from a translation unit by source_unit_of; see compile_unit for the compile-event
// projection. Members default so a designated initializer can name only what it means.
struct source_unit
{
    std::string unit;
    std::string path;
    std::string module;
    std::string kind;
    std::vector<std::string> imports;
    int level = -1;
    bool has_main = false;
    bool is_test = false;
    bool is_modular = false;
};

struct source_inventory
{
    std::string config;
    bool include_tests = false;
    bool include_examples = false;
    std::string source_dir;
    std::vector<source_unit> units{}; // filled after construction; the rest warn if omitted
    int main_count = 0;
    int test_count = 0;
    int max_level = 0;
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

    // How reports name this unit: the source relative to the project root. Not written to
    // the wire — the rebuild sentence below is composed from it.
    std::string_view display_path;
};

// Rebuild telemetry kinds for compile/link (wire name is also rebuild_reason).
enum class rebuild_kind
{
    none,
    not_in_cache,
    source_stale,
    header_stale,
    header_missing,
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
        case rebuild_kind::header_missing: return "header_missing";
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

// The standing explanation of a rebuild kind. A pure function of the kind, so it is answered
// here on demand rather than carried in every rebuild_info — the same way see_event is.
constexpr std::string_view rebuild_hint(rebuild_kind kind)
{
    switch(kind)
    {
        case rebuild_kind::none:
            return {};
        case rebuild_kind::not_in_cache:
            return "Source path not present in object cache for this config.";
        case rebuild_kind::source_stale:
            return "Source mtime newer than cached compile timestamp.";
        case rebuild_kind::header_stale:
            return "An included header is newer than this object (from the compiler depfile).";
        case rebuild_kind::header_missing:
            return "An included project header from the compiler depfile is missing or unreadable.";
        case rebuild_kind::depfile_unusable:
            return "Compiler depfile missing, unreadable, or malformed; header freshness cannot be verified.";
        case rebuild_kind::object_missing:
            return "Object file missing on disk.";
        case rebuild_kind::object_stale:
            return "Object file older than cached source timestamp or its module PCM.";
        case rebuild_kind::own_pcm_missing:
            return "Module PCM missing on disk.";
        case rebuild_kind::own_pcm_stale:
            return "Module PCM older than its source.";
        case rebuild_kind::pcm_stale:
            return "Imported PCM newer than this object; recompile follows module graph.";
        case rebuild_kind::dependency_pcm_stale:
            return "Imported module PCM is missing or older than its source.";
        case rebuild_kind::profile_change:
            return "Object-cache toolchain profile changed; see profile_changed event.";
        case rebuild_kind::missing_executable:
            return "Linked executable missing on disk.";
        case rebuild_kind::object_changed:
            return "One or more input objects changed since the last link.";
        case rebuild_kind::link_flags_changed:
            return "Link/compile/module flags changed since the last link.";
        case rebuild_kind::signature_changed:
            return "Link signature changed since the last successful link.";
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

// Captured toolchain output. Failed commands always carry this so an agent reading stdout
// only can see why clang failed; successful ones carry it when the compiler or linker printed
// anything (warnings), so a project cannot accumulate `-Wall` noise invisibly.
struct diagnostics
{
    std::string path;      // file holding the full captured output
    std::string head;      // bounded excerpt for inline reporting
    std::size_t bytes = 0; // captured size before truncation
    bool truncated = false;

    bool empty() const { return head.empty() and path.empty(); }
};

// How a spawned toolchain process ended: the wait status, plus whatever it printed when that
// output is worth reporting (failure, or success with warnings). One value because ok is not
// independent of the status — it is status.ok() — and reporting them as separate arguments let
// an event claim success beside a status saying otherwise.
struct process_result
{
    process_status status{};
    diagnostics diag{};

    bool ok() const { return status.ok(); }
};

// Why a compile or link ran (kind is also rebuild_reason on the wire). Facts only: the hint
// and the sentence are derived from these by the functions below, so a producer cannot ship
// telemetry whose prose disagrees with its fields. Members default so a designated
// initializer can name only the ones a given reason has.
struct rebuild_info
{
    rebuild_kind kind = rebuild_kind::none;
    std::string module{};
    std::string pcm_path{};
    std::string trigger_path{};

    bool empty() const { return kind == rebuild_kind::none; }
};

// The sentences the reports show, in one place because both observers say them: the console
// prints them and the JSONL writes them as `message`, and composing them twice would let the
// two drift. The build system used to compose them and ship the result, which put report
// prose in the producer and made it untestable without running a build.
inline std::string compile_rebuild_message(const compile_unit& unit, const rebuild_info& info)
{
    const auto label = std::string{unit.display_path};
    switch(info.kind)
    {
        case rebuild_kind::profile_change:
            return "Rebuilding " + label + " because compile profile changed";
        case rebuild_kind::not_in_cache:
            return "Rebuilding " + label + " because it is not in the object cache";
        case rebuild_kind::source_stale:
            if(not info.trigger_path.empty() and info.trigger_path != unit.source)
                return "Rebuilding " + label + " because dependency " + info.trigger_path + " is newer than its cached object";
            return "Rebuilding " + label + " because source is newer than the cached object";
        case rebuild_kind::header_stale:
            return "Rebuilding " + label + " because included header " + info.trigger_path + " is newer than its object";
        case rebuild_kind::header_missing:
            return "Rebuilding " + label + " because included header " + info.trigger_path + " is missing or unreadable";
        case rebuild_kind::depfile_unusable:
            return "Rebuilding " + label + " because depfile " + info.trigger_path + " cannot be read; its header dependencies are unknown";
        case rebuild_kind::pcm_stale:
            return "Rebuilding " + label + " because PCM " + info.module + " is newer than the object (import graph)";
        case rebuild_kind::dependency_pcm_stale:
            return "Rebuilding " + label + " because imported module " + info.module + " PCM is missing or stale";
        case rebuild_kind::object_missing:
            return "Rebuilding " + label + " because object file is missing";
        case rebuild_kind::object_stale:
            if(not info.module.empty())
                return "Rebuilding " + label + " because object file is older than PCM " + info.module;
            return "Rebuilding " + label + " because object file is older than the cached source timestamp";
        case rebuild_kind::own_pcm_missing:
            return "Rebuilding " + label + " because its PCM is missing";
        case rebuild_kind::own_pcm_stale:
            return "Rebuilding " + label + " because its PCM is older than the source";
        default:
            return "Rebuilding " + label + " (" + std::string{rebuild_kind_name(info.kind)} + ")";
    }
}

inline std::string link_rebuild_message(std::string_view executable_path, const rebuild_info& info)
{
    switch(info.kind)
    {
        case rebuild_kind::missing_executable:
            return "Linking " + std::string{executable_path} + " because executable is missing";
        case rebuild_kind::not_in_cache:
            return "Linking " + std::string{executable_path} + " because it is not in the link cache";
        case rebuild_kind::object_changed:
            return "Linking " + std::string{executable_path} + " because input objects changed";
        case rebuild_kind::link_flags_changed:
            return "Linking " + std::string{executable_path} + " because link flags changed";
        default:
            return "Linking " + std::string{executable_path} + " ("
                + std::string{rebuild_kind_name(info.kind)} + ")";
    }
}

// Why a finished test run failed. Beside the rebuild sentences for the same reason: the console
// prints it and the JSONL writes it as a cb_error message, and composing it at the call site put
// report prose in the producer.
inline std::string test_failure_message(const process_result& result)
{
    if(result.status.signaled)
        return "Test runner was killed by signal " + std::to_string(result.status.signal) + '!';
    return "Some tests or assertions failed!";
}

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
    diagnostics diag{};     // toolchain output; empty when the step was silent
    std::string signature{}; // link input stamp; set on link_end (including cache hits)
};

// Every file CB keeps under cache/, in one value. The four are not interchangeable — only two
// hold a profile, only two hold entries — but a report that describes three of them and leaves
// the fourth out is how the std module profile stayed invisible to `cache status` while
// `cache invalidate` was quietly deleting it.
struct cache_inventory
{
    std::string_view object_cache_path;
    bool object_cache_exists = false;
    bool profile_match = false; // the object cache's stored profile against the current one
    int object_entries = 0;
    int object_stale_entries = 0;
    std::string_view executable_cache_path;
    bool executable_cache_exists = false;
    int executable_entries = 0;
    std::string_view std_module_profile_path;
    bool std_module_profile_exists = false;
    bool std_module_profile_match = false; // std.pcm was built by this profile
    std::string_view compiler_stamp_path;
    bool compiler_stamp_exists = false;
    std::string_view current_profile;
};

// What `cache invalidate` removed, one flag per file in cache_inventory. Absent is not failure:
// a cache that was never written has nothing to remove.
struct cache_removals
{
    bool object_cache = false;
    bool executable_cache = false;
    bool compiler_stamp = false;
    bool std_module_profile = false;
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
    // Parameter names are commented rather than declared: the default bodies ignore them, and
    // a named unused parameter warns under -Wextra. The names are the contract's only
    // documentation of which string_view is which, so they stay.
    virtual void cache_status(const cache_inventory& /*caches*/) {}

    virtual void cache_invalidate_end(const cache_removals& /*removed*/) {}

    virtual void source_list(const source_inventory& /*inventory*/) {}

    virtual void build_start(std::string_view /*config*/,
                             bool /*include_tests*/,
                             bool /*include_examples*/) {}

    virtual void build_end(bool /*ok*/, const interval& /*timing*/) {}

    virtual void test_start(std::string_view /*runner*/) {}

    virtual void test_end(const process_result& /*result*/, const interval& /*timing*/) {}

    virtual void command_start(std::string_view /*cmd*/,
                               std::span<const std::string> /*argv*/) {}

    virtual void command_end(
        std::string_view /*cmd*/,
        std::span<const std::string> /*argv*/,
        const process_result& /*result*/,
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
