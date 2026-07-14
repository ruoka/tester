// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
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

class observer
{
public:
    virtual ~observer() = default;

    virtual bool machine_readable() const { return false; }
    virtual void activate() {}
    virtual void finish() {}
    virtual std::string_view run_id() const { return {}; }
    virtual void error(std::string_view) {}
    virtual void warning(std::string_view) {}
    virtual void info(std::string_view) {}
    virtual void success(std::string_view) {}
    virtual void command(std::string_view) {}
    virtual void profile_changed(std::string_view, const object_cache_profile_diff&) {}
    virtual void profile_change_rebuild(std::string_view) {}

    virtual void cache_status(
        std::string_view,
        bool,
        bool,
        int,
        int,
        int,
        std::string_view) {}

    virtual void cache_invalidate_end(bool, bool, bool) {}
    virtual void source_list(const source_inventory&) {}
    virtual void build_start(std::string_view, bool, bool) {}

    virtual void build_end(
        bool,
        std::chrono::steady_clock::time_point,
        std::chrono::steady_clock::time_point) {}

    virtual void test_start(std::string_view) {}

    virtual void test_end(
        bool,
        int,
        int,
        bool,
        int,
        std::chrono::steady_clock::time_point,
        std::chrono::steady_clock::time_point) {}

    virtual void command_start(std::string_view, std::span<const std::string>) {}

    virtual void command_end(
        std::string_view,
        std::span<const std::string>,
        bool,
        int,
        std::chrono::steady_clock::time_point,
        std::chrono::steady_clock::time_point) {}

    virtual void compile_start(
        std::string_view,
        std::string_view,
        std::string_view,
        std::string_view,
        std::string_view) {}

    virtual void compile_end(
        std::string_view,
        std::string_view,
        std::string_view,
        std::string_view,
        bool,
        bool,
        std::chrono::steady_clock::time_point,
        std::chrono::steady_clock::time_point,
        std::string_view) {}

    virtual void link_end(
        std::string_view,
        bool,
        bool,
        std::chrono::steady_clock::time_point,
        std::chrono::steady_clock::time_point) {}
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

inline bool machine_readable()
{
    return std::ranges::any_of(observers, [](const auto target)
    {
        return target.get().machine_readable();
    });
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
