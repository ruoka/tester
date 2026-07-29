// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
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
        ++rebuild_by_kind[std::string{rebuild_kind_name(rebuild.kind)}];
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

    void profile_changed(rebuild_kind, const object_cache_profile_diff& diff) override
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

    void build_start(std::string_view, bool, bool) override
    {
        auto lock = std::lock_guard<std::mutex>{mutex};
        rebuild_by_kind.clear();
        rebuild_modules.clear();
    }

    void build_end(bool /*ok*/, const interval& /*timing*/) override
    {
        auto lock = std::lock_guard<std::mutex>{mutex};
        if(auto summary = format_rebuild_summary(); not summary.empty())
        {
            human << color::bold::blue << "INFO" << color::reset << " " << summary << '\n';
        }
    }

    void compile_start(const compile_unit& unit, const rebuild_info& rebuild) override
    {
        if(not rebuild.empty())
            info(compile_rebuild_message(unit, rebuild));
    }

    void compile_end(const compile_unit& /*unit*/, const step_result& step) override
    {
        if(not step.cache_hit)
        {
            auto lock = std::lock_guard<std::mutex>{mutex};
            note_rebuild(step.rebuild);
        }
    }

    // A skipped link is worth a line — there are one or two executables, and their absence from
    // the output would be indistinguishable from nothing having been asked. Cached compiles stay
    // silent on purpose: one line per up-to-date unit would be dozens of lines saying so.
    void link_end(std::string_view executable_path, const step_result& step) override
    {
        if(step.cache_hit)
            info("Skipping link (up-to-date): " + std::string{executable_path});
        else if(not step.rebuild.empty())
            info(link_rebuild_message(executable_path, step.rebuild));
    }

    // A passing run says so here rather than in the command, for the same reason the skipped-link
    // line does. A failing one is not ours alone to say: it goes out as `error`, which the JSONL
    // stream writes as cb_error, so test_scope raises it and this observer prints it like any other.
    void test_end(const process_result& result, const interval&) override
    {
        if(result.ok())
            success("All tests passed!");
    }

    // One paragraph per cache file, in the order a build consults them, so a reader can tell
    // which of the four is the one that will not be reused. Entry counts and profile matches
    // are printed only where they exist: a stamp has neither.
    void cache_status(const cache_inventory& caches) override
    {
        const auto yes_no = [](bool value) { return std::string{value ? "yes" : "no"}; };

        info("Object cache: " + std::string{caches.object_cache_path});
        info("  exists: " + yes_no(caches.object_cache_exists));
        if(caches.object_cache_exists)
        {
            info("  profile_match: " + yes_no(caches.profile_match));
            info("  object_entries: " + std::to_string(caches.object_entries));
            info("  object_stale_entries: " + std::to_string(caches.object_stale_entries));
        }

        info("Executable cache: " + std::string{caches.executable_cache_path});
        info("  exists: " + yes_no(caches.executable_cache_exists));
        info("  executable_entries: " + std::to_string(caches.executable_entries));

        info("Std module profile: " + std::string{caches.std_module_profile_path});
        info("  exists: " + yes_no(caches.std_module_profile_exists));
        if(caches.std_module_profile_exists)
            info("  profile_match: " + yes_no(caches.std_module_profile_match));

        info("Compiler stamp: " + std::string{caches.compiler_stamp_path});
        info("  exists: " + yes_no(caches.compiler_stamp_exists));
    }

    void cache_invalidate_end(const cache_removals& removed) override
    {
        const auto state = [](bool value) { return std::string{value ? "removed" : "absent"}; };

        info("Invalidated compile/link cache indexes:");
        info("  object_cache: " + state(removed.object_cache));
        info("  executable_cache: " + state(removed.executable_cache));
        info("  compiler_stamp: " + state(removed.compiler_stamp));
        info("  std_module_profile: " + state(removed.std_module_profile));
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

