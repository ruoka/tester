// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <algorithm>
#include <flat_map>
#include <mutex>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "../tester/details/terminal-colors.h++"
#include "cb-observer.h++"

namespace cb::output::console {

using namespace std::string_literals;
using namespace std::string_view_literals;
namespace color = ::term;

using string_list = std::vector<std::string>;

inline std::string format_token_list(const string_list& tokens, std::size_t max_tokens = 8)
{
    if(tokens.empty())
        return {};

    const auto count = std::min(tokens.size(), max_tokens);
    const string_list head{tokens.begin(), tokens.begin() + static_cast<std::ptrdiff_t>(count)};
    auto out = head | std::views::join_with(", "sv) | std::ranges::to<std::string>();
    if(tokens.size() > max_tokens)
        out += ", ... (" + std::to_string(tokens.size() - max_tokens) + " more)";
    return out;
}

inline std::string format_token_change_summary(std::string_view name, const profile_token_change& change, std::size_t max_tokens = 8)
{
    auto parts = string_list{};
    if(not change.added.empty())
        parts.push_back("+ " + format_token_list(change.added, max_tokens));
    if(not change.removed.empty())
        parts.push_back("- " + format_token_list(change.removed, max_tokens));
    if(parts.empty())
        return {};

    return std::string{name} + ": " + (parts | std::views::join_with(", "sv) | std::ranges::to<std::string>());
}

inline std::string format_profile_diff(const object_cache_profile_diff& diff, std::size_t max_tokens = 8)
{
    auto parts = string_list{};
    const auto append_scalar = [&](std::string_view name, const profile_scalar_change& change) {
        parts.push_back(std::string{name} + ": " + change.old_value + " -> " + change.new_value);
    };

    for_each_profile_scalar(diff, [&](std::string_view name, const auto& change) {
        if(change)
            append_scalar(name, *change);
    });
    for_each_profile_tokens(diff, [&](std::string_view name, const auto& change) {
        if(change)
        {
            if(auto summary = format_token_change_summary(name, *change, max_tokens); not summary.empty())
                parts.push_back(std::move(summary));
        }
    });

    return parts | std::views::join_with("; "sv) | std::ranges::to<std::string>();
}

struct observer final : cb::output::observer
{
    std::ostream& human;
    std::mutex mutex{};
    std::flat_map<std::string, std::size_t, std::less<>> rebuild_by_kind{};
    std::flat_map<std::string, std::size_t, std::less<>> rebuild_modules{};

    explicit observer(std::ostream& human_stream) : human{human_stream} {}

    void write(std::string_view prefix, std::string_view color_code, std::string_view message)
    {
        auto lock = std::lock_guard<std::mutex>{mutex};
        human << color_code << prefix << color::reset << " " << message << '\n';
    }

    void note_rebuild(const rebuild_info& rebuild)
    {
        if(rebuild.empty())
            return;
        ++rebuild_by_kind[rebuild.kind];
        if(not rebuild.module.empty())
            ++rebuild_modules[rebuild.module];
    }

    std::string format_rebuild_summary() const
    {
        if(rebuild_by_kind.empty())
            return {};

        auto parts = string_list{};
        for(const auto& [kind, count] : rebuild_by_kind)
            parts.push_back(kind + '=' + std::to_string(count));

        auto msg = "Rebuild summary: "s + (parts | std::views::join_with(", "sv) | std::ranges::to<std::string>());
        if(not rebuild_modules.empty())
        {
            auto ranked = std::vector<std::pair<std::string, std::size_t>>{
                rebuild_modules.begin(),
                rebuild_modules.end()};
            std::ranges::sort(ranked, [](const auto& a, const auto& b) {
                if(a.second != b.second)
                    return a.second > b.second;
                return a.first < b.first;
            });
            const auto limit = std::min<std::size_t>(ranked.size(), 8);
            auto modules = ranked
                | std::views::take(limit)
                | std::views::keys
                | std::ranges::to<string_list>();
            msg += "; top modules: ";
            msg += modules | std::views::join_with(", "sv) | std::ranges::to<std::string>();
        }
        return msg;
    }

    void error(std::string_view msg) override
    {
        write("ERROR", color::bold::red, msg);
    }

    void warning(std::string_view msg) override
    {
        write("WARNING", color::bold::yellow, msg);
    }

    void info(std::string_view msg) override
    {
        write("INFO", color::bold::blue, msg);
    }

    void success(std::string_view msg) override
    {
        write("SUCCESS", color::bold::green, msg);
    }

    void command(std::string_view cmd) override
    {
        write("COMMAND", color::bold::blue, cmd);
    }

    void profile_changed(std::string_view reason, const object_cache_profile_diff& diff) override
    {
        auto msg = std::string{"Object cache profile changed; invalidating compile cache"};
        if(!diff.empty())
        {
            msg += " (";
            msg += format_profile_diff(diff);
            msg += ')';
        }
        info(msg);
    }

    void profile_change_rebuild(std::string_view tu_label) override
    {
        info("Rebuilding " + std::string{tu_label} + " because compile profile changed");
    }

    void build_start(std::string_view, bool, bool) override
    {
        auto lock = std::lock_guard<std::mutex>{mutex};
        rebuild_by_kind.clear();
        rebuild_modules.clear();
    }

    void build_end(bool,
                   std::chrono::steady_clock::time_point,
                   std::chrono::steady_clock::time_point) override
    {
        auto lock = std::lock_guard<std::mutex>{mutex};
        if(auto summary = format_rebuild_summary(); not summary.empty())
        {
            human << color::bold::blue << "INFO" << color::reset << " " << summary << '\n';
        }
    }

    void compile_start(std::string_view,
                       std::string_view,
                       std::string_view,
                       std::string_view,
                       const rebuild_info& rebuild = {}) override
    {
        if(not rebuild.empty() and not rebuild.message.empty())
            info(rebuild.message);
    }

    void compile_end(std::string_view,
                     std::string_view,
                     std::string_view,
                     std::string_view,
                     bool,
                     bool cache_hit,
                     std::chrono::steady_clock::time_point,
                     std::chrono::steady_clock::time_point,
                     const rebuild_info& rebuild = {}) override
    {
        if(not cache_hit)
        {
            auto lock = std::lock_guard<std::mutex>{mutex};
            note_rebuild(rebuild);
        }
    }

    void link_end(std::string_view,
                  bool,
                  bool cache_hit,
                  std::chrono::steady_clock::time_point,
                  std::chrono::steady_clock::time_point,
                  const rebuild_info& rebuild = {}) override
    {
        if(not cache_hit and not rebuild.message.empty())
            info(rebuild.message);
    }

    void cache_status(std::string_view object_cache_path,
                      bool object_cache_exists,
                      bool profile_match,
                      int object_entries,
                      int object_stale_entries,
                      int executable_entries,
                      std::string_view) override
    {
        info("Object cache: " + std::string{object_cache_path});
        info("  exists: " + std::string{object_cache_exists ? "yes" : "no"});
        if(object_cache_exists)
        {
            info("  profile_match: " + std::string{profile_match ? "yes" : "no"});
            info("  object_entries: " + std::to_string(object_entries));
            info("  object_stale_entries: " + std::to_string(object_stale_entries));
        }
        info("  executable_entries: " + std::to_string(executable_entries));
    }

    void cache_invalidate_end(bool object_cache_removed,
                              bool executable_cache_removed,
                              bool compiler_stamp_removed) override
    {
        info("Invalidated compile/link cache indexes:");
        info("  object_cache: " + std::string{object_cache_removed ? "removed" : "absent"});
        info("  executable_cache: " + std::string{executable_cache_removed ? "removed" : "absent"});
        info("  compiler_stamp: " + std::string{compiler_stamp_removed ? "removed" : "absent"});
    }

    void source_list(const source_inventory& inventory) override
    {
        auto lock = std::lock_guard<std::mutex>{mutex};
        auto& os = human;
        os << color::cyan << "\nFound " << inventory.units.size() << " translation units:\n\n" << color::reset;
        os << color::cyan << " Total: " << inventory.units.size()
                  << " | Main: " << inventory.main_count
                  << " | Tests: " << inventory.test_count << "\n\n" << color::reset;

        for (const auto& unit : inventory.units) {
            os << color::cyan << " " << unit.path << color::reset;
            if (not unit.module.empty()) os << " " << color::yellow << "[module: " << unit.module << "]" << color::reset;
            if (unit.has_main) os << " " << color::green << "[main]" << color::reset;
            if (unit.is_test) os << " " << color::magenta << "[TEST]" << color::reset;
            if (unit.level >= 0) os << " " << color::gray << "level=" << unit.level << color::reset;
            os << "\n";
            if (not unit.imports.empty()) {
                os << color::gray << "   imports: ";
                for (std::size_t i = 0; i < unit.imports.size(); ++i) {
                    if (i) os << ", ";
                    os << unit.imports[i];
                }
                os << color::reset << "\n";
            }
        }
        os << color::cyan << "\n" << color::reset;
    }
};

} // namespace cb::output::console

