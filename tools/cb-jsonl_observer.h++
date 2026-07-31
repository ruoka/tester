// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <algorithm>
#include <chrono>
#include <flat_map>
#include <mutex>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../tester/details/jsonl.h++"
#include "cb-observer.h++"

namespace cb::output::jsonl {

enum class jsonl_mode { summary, failures, trace };

using ::jsonl::escape;

inline std::string join_json_strings(std::span<const std::string> values)
{
    return values
        | std::views::transform([](const std::string& value) {
            return '"' + escape(value) + '"';
        })
        | std::views::join_with(',')
        | std::ranges::to<std::string>();
}

inline void write_argv(std::ostream& os, std::span<const std::string> argv)
{
    os << ",\"argv\":[" << join_json_strings(argv) << ']';
}

inline void write_string_array(std::ostream& os, std::string_view field, std::span<const std::string> values)
{
    os << ",\"" << field << "\":[" << join_json_strings(values) << ']';
}

inline void write_profile_diff_scalar(std::ostream& os, const profile_scalar_change& change)
{
    os << "{\"old\":\"" << escape(change.old_value) << "\",\"new\":\"" << escape(change.new_value) << "\"}";
}

inline void write_profile_diff_tokens(std::ostream& os, const profile_token_change& change)
{
    os << "{\"added\":[" << join_json_strings(change.added)
       << "],\"removed\":[" << join_json_strings(change.removed) << "]}";
}

inline void write_profile_diff(std::ostream& os, const object_cache_profile_diff& diff)
{
    os << '{';
    auto first = true;
    const auto field = [&](std::string_view name, const auto& write_value) {
        if(not first)
            os << ',';
        first = false;
        os << '"' << name << "\":";
        write_value();
    };

    for_each_profile_scalar(diff, [&](std::string_view name, const auto& change) {
        if(change)
            field(name, [&]{ write_profile_diff_scalar(os, *change); });
    });
    for_each_profile_tokens(diff, [&](std::string_view name, const auto& change) {
        if(change)
            field(name, [&]{ write_profile_diff_tokens(os, *change); });
    });
    os << '}';
}

inline void write_rebuild_field(std::ostream& os, std::string_view name, std::string_view value, bool& first)
{
    if(value.empty())
        return;
    if(not first)
        os << ',';
    first = false;
    os << '"' << name << "\":\"" << escape(value) << '"';
}

inline void write_process_status(std::ostream& os, const process_status& status)
{
    os << ",\"exit_code\":" << status.exit_code;
    os << ",\"wait_status\":" << status.wait_status;
    os << ",\"signaled\":" << (status.signaled ? "true" : "false");
    if(status.signaled)
        os << ",\"signal\":" << status.signal;
}

// Toolchain output travels inline so that a consumer reading stdout only has the
// error or warning text, with a path to the untruncated capture for anything longer.
inline void write_diagnostics(std::ostream& os, const diagnostics& diag)
{
    if(diag.empty())
        return;
    os << ",\"diagnostics\":{";
    os << "\"text\":\"" << escape(diag.head) << '"';
    if(not diag.path.empty())
        os << ",\"path\":\"" << escape(diag.path) << '"';
    os << ",\"bytes\":" << diag.bytes;
    os << ",\"truncated\":" << (diag.truncated ? "true" : "false");
    os << '}';
}

// compile_start and compile_end describe the same unit, so the field set is written once.
// The optional paths stay absent rather than empty for units that have no module.
inline void write_compile_unit(std::ostream& os, const compile_unit& unit)
{
    os << ",\"source_path\":\"" << escape(unit.source) << "\"";
    os << ",\"object_path\":\"" << escape(unit.object) << "\"";
    if(not unit.pcm.empty())
        os << ",\"pcm_path\":\"" << escape(unit.pcm) << "\"";
    if(not unit.module.empty())
        os << ",\"module_name\":\"" << escape(unit.module) << "\"";
}

// The object path and the sentence come from the event, not from the reason: a compile names
// the object it was producing, a link has none to name, and the hint follows from the kind.
inline void write_rebuild(std::ostream& os,
                          const rebuild_info& rebuild,
                          std::string_view object_path,
                          std::string_view message)
{
    os << '{';
    auto first = true;
    write_rebuild_field(os, "kind", rebuild_kind_name(rebuild.kind), first);
    write_rebuild_field(os, "module", rebuild.module, first);
    write_rebuild_field(os, "pcm_path", rebuild.pcm_path, first);
    write_rebuild_field(os, "object_path", object_path, first);
    write_rebuild_field(os, "trigger_path", rebuild.trigger_path, first);
    write_rebuild_field(os, "hint", rebuild_hint(rebuild.kind), first);
    write_rebuild_field(os, "message", message, first);
    if(rebuild.kind == rebuild_kind::profile_change)
        write_rebuild_field(os, "see_event", "profile_changed", first);
    os << '}';
}

struct observer final : cb::output::observer
{
    struct state
    {
        std::ostream& json;
        ::jsonl::jsonl_context<std::ostream> jsonl;
        std::mutex mutex{};
        jsonl_mode mode = jsonl_mode::failures;
        std::size_t compile_total = 0;
        std::size_t compile_rebuilt = 0;
        std::size_t compile_cache_hits = 0;
        std::size_t compile_failed = 0;
        std::size_t links_total = 0;
        std::size_t link_cache_hits = 0;
        std::size_t link_failed = 0;
        std::size_t commands_failed = 0;
        std::flat_map<std::string, std::size_t, std::less<>> rebuild_by_kind{};
        std::flat_map<std::string, std::size_t, std::less<>> rebuild_modules{};

        explicit state(std::ostream& stream) : json{stream}, jsonl{stream} {}

        void note_rebuild(const rebuild_info& rebuild)
        {
            if(rebuild.empty())
                return;
            ++rebuild_by_kind[std::string{rebuild_kind_name(rebuild.kind)}];
            if(not rebuild.module.empty())
                ++rebuild_modules[rebuild.module];
        }

        void write_rebuild_summary(std::ostream& os) const
        {
            if(rebuild_by_kind.empty())
                return;

            os << ",\"rebuild_summary\":{";
            auto first = true;
            for(const auto& [kind, count] : rebuild_by_kind)
            {
                if(not first)
                    os << ',';
                first = false;
                os << '"' << escape(kind) << "\":" << count;
            }

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
                os << ",\"top_modules\":[";
                for(std::size_t i = 0; i < limit; ++i)
                {
                    if(i)
                        os << ',';
                    os << '"' << escape(ranked[i].first) << '"';
                }
                os << ']';
            }
            os << '}';
        }
    };

    state m;

    explicit observer(std::ostream& stream) : m{stream} {}

    void set_mode(jsonl_mode mode)
    {
        m.mode = mode;
    }

    auto mode() const
    {
        return m.mode;
    }

    void activate() override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.jsonl.set_enabled(true);
        m.jsonl.reset_stream_state();
        m.jsonl.assign_new_run_id();
    }

    void finish() override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.jsonl.emit_eof();
    }

    std::string_view run_id() const override
    {
        return m.jsonl.get_run_id();
    }

    void build_start(std::string_view config, bool include_tests, bool include_examples) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.compile_total = 0;
        m.compile_rebuilt = 0;
        m.compile_cache_hits = 0;
        m.compile_failed = 0;
        m.links_total = 0;
        m.link_cache_hits = 0;
        m.link_failed = 0;
        m.commands_failed = 0;
        m.rebuild_by_kind.clear();
        m.rebuild_modules.clear();
        m.json << m.jsonl("build_start") << [&](std::ostream& os){
            os << ",\"config\":\"" << escape(config) << "\"";
            os << ",\"include_tests\":" << (include_tests ? "true" : "false");
            os << ",\"include_examples\":" << (include_examples ? "true" : "false");
        };
    }

    void build_end(bool ok, const interval& timing) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("build_end") << [&](std::ostream& os){
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"duration_ms\":" << timing.elapsed_ms();
            if(m.mode != jsonl_mode::trace)
            {
                os << ",\"compile_total\":" << m.compile_total;
                os << ",\"compile_rebuilt\":" << m.compile_rebuilt;
                os << ",\"compile_cache_hits\":" << m.compile_cache_hits;
                os << ",\"compile_failed\":" << m.compile_failed;
                os << ",\"links_total\":" << m.links_total;
                os << ",\"link_cache_hits\":" << m.link_cache_hits;
                os << ",\"link_failed\":" << m.link_failed;
                os << ",\"commands_failed\":" << m.commands_failed;
            }
            m.write_rebuild_summary(os);
        };
    }

    void test_start(std::string_view runner) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("test_start") << [&](std::ostream& os){
            os << ",\"runner\":\"" << escape(runner) << "\"";
        };
    }

    void test_end(const process_result& result, const interval& timing) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("test_end") << [&](std::ostream& os){
            os << ",\"ok\":" << (result.ok() ? "true" : "false");
            write_process_status(os, result.status);
            os << ",\"duration_ms\":" << timing.elapsed_ms();
        };
    }

    void error(std::string_view message) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("cb_error") << [&](std::ostream& os){
            os << ",\"message\":\"" << escape(message) << "\"";
        };
    }

    void command_start(std::string_view cmd, std::span<const std::string> argv) override
    {
        if(m.mode != jsonl_mode::trace)
            return;

        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("command_start") << [&](std::ostream& os){
            os << ",\"cmd\":\"" << escape(cmd) << "\"";
            write_argv(os, argv);
        };
    }

    void command_end(std::string_view cmd,
                     std::span<const std::string> argv,
                     const process_result& result,
                     const interval& timing) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        const auto ok = result.ok();
        if(not ok)
            ++m.commands_failed;
        // Failures mode keeps silent successes off the wire, but a success that printed
        // warnings is actionable — the same rule as compile_end / link_end below.
        if(m.mode == jsonl_mode::summary
           || (m.mode == jsonl_mode::failures && ok && result.diag.empty()))
            return;

        m.json << m.jsonl("command_end") << [&](std::ostream& os){
            if(m.mode == jsonl_mode::trace)
                os << ",\"cmd\":\"" << escape(cmd) << "\"";
            write_argv(os, argv);
            os << ",\"ok\":" << (ok ? "true" : "false");
            write_process_status(os, result.status);
            write_diagnostics(os, result.diag);
            os << ",\"duration_ms\":" << timing.elapsed_ms();
        };
    }

    void profile_changed(rebuild_kind reason, const object_cache_profile_diff& diff) override
    {
        if(m.mode == jsonl_mode::summary)
            return;

        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("profile_changed") << [&](std::ostream& os){
            os << ",\"reason\":\"" << escape(rebuild_kind_name(reason)) << "\"";
            if(!diff.empty())
            {
                os << ",\"profile_diff\":";
                write_profile_diff(os, diff);
            }
        };
    }

    void cache_status(const cache_inventory& caches) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("cache_status") << [&](std::ostream& os){
            os << ",\"object_cache_path\":\"" << escape(caches.object_cache_path) << "\"";
            os << ",\"object_cache_exists\":" << (caches.object_cache_exists ? "true" : "false");
            os << ",\"profile_match\":" << (caches.profile_match ? "true" : "false");
            os << ",\"object_entries\":" << caches.object_entries;
            os << ",\"object_stale_entries\":" << caches.object_stale_entries;
            os << ",\"executable_cache_path\":\"" << escape(caches.executable_cache_path) << "\"";
            os << ",\"executable_cache_exists\":" << (caches.executable_cache_exists ? "true" : "false");
            os << ",\"executable_entries\":" << caches.executable_entries;
            os << ",\"std_module_profile_path\":\"" << escape(caches.std_module_profile_path) << "\"";
            os << ",\"std_module_profile_exists\":" << (caches.std_module_profile_exists ? "true" : "false");
            os << ",\"std_module_profile_match\":" << (caches.std_module_profile_match ? "true" : "false");
            os << ",\"compiler_stamp_path\":\"" << escape(caches.compiler_stamp_path) << "\"";
            os << ",\"compiler_stamp_exists\":" << (caches.compiler_stamp_exists ? "true" : "false");
            os << ",\"current_profile\":\"" << escape(caches.current_profile) << "\"";
        };
    }

    void cache_invalidate_end(const cache_removals& removed) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("cache_invalidate_end") << [&](std::ostream& os){
            os << ",\"object_cache_removed\":" << (removed.object_cache ? "true" : "false");
            os << ",\"executable_cache_removed\":" << (removed.executable_cache ? "true" : "false");
            os << ",\"compiler_stamp_removed\":" << (removed.compiler_stamp ? "true" : "false");
            os << ",\"std_module_profile_removed\":" << (removed.std_module_profile ? "true" : "false");
        };
    }

    void compile_start(const compile_unit& unit, const rebuild_info& rebuild) override
    {
        if(m.mode != jsonl_mode::trace)
            return;

        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("compile_start") << [&](std::ostream& os){
            write_compile_unit(os, unit);
            if(not rebuild.empty())
            {
                const auto message = compile_rebuild_message(unit, rebuild);
                os << ",\"rebuild_reason\":\"" << escape(rebuild_kind_name(rebuild.kind)) << "\"";
                os << ",\"rebuild\":";
                write_rebuild(os, rebuild, unit.object, message);
                os << ",\"message\":\"" << escape(message) << "\"";
            }
        };
    }

    void link_end(std::string_view executable_path, const step_result& step) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        ++m.links_total;
        if(step.cache_hit)
            ++m.link_cache_hits;
        if(not step.ok)
            ++m.link_failed;
        if(m.mode == jsonl_mode::summary
           || (m.mode == jsonl_mode::failures && step.ok && step.diag.empty()))
            return;

        m.json << m.jsonl("link_end") << [&](std::ostream& os){
            os << ",\"executable_path\":\"" << escape(executable_path) << "\"";
            os << ",\"ok\":" << (step.ok ? "true" : "false");
            os << ",\"cache_hit\":" << (step.cache_hit ? "true" : "false");
            if(not step.cache_hit and not step.rebuild.empty())
            {
                os << ",\"rebuild_reason\":\"" << escape(rebuild_kind_name(step.rebuild.kind)) << "\"";
                os << ",\"rebuild\":";
                write_rebuild(os, step.rebuild, {}, link_rebuild_message(executable_path, step.rebuild));
            }
            write_diagnostics(os, step.diag);
            os << ",\"duration_ms\":" << step.timing.elapsed_ms();
        };
    }

    void compile_end(const compile_unit& unit, const step_result& step) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        ++m.compile_total;
        if(step.cache_hit)
            ++m.compile_cache_hits;
        else
        {
            ++m.compile_rebuilt;
            m.note_rebuild(step.rebuild);
        }
        if(not step.ok)
            ++m.compile_failed;
        if(m.mode == jsonl_mode::summary
           || (m.mode == jsonl_mode::failures && step.ok && step.diag.empty()))
            return;

        m.json << m.jsonl("compile_end") << [&](std::ostream& os){
            write_compile_unit(os, unit);
            os << ",\"ok\":" << (step.ok ? "true" : "false");
            os << ",\"cache_hit\":" << (step.cache_hit ? "true" : "false");
            if(not step.cache_hit and not step.rebuild.empty())
            {
                os << ",\"rebuild_reason\":\"" << escape(rebuild_kind_name(step.rebuild.kind)) << "\"";
                os << ",\"rebuild\":";
                write_rebuild(os, step.rebuild, unit.object,
                              compile_rebuild_message(unit, step.rebuild));
            }
            write_diagnostics(os, step.diag);
            os << ",\"duration_ms\":" << step.timing.elapsed_ms();
        };
    }

    void source_list(const source_inventory& inventory) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("list_start") << [&](std::ostream& os){
            os << ",\"config\":\"" << escape(inventory.config) << "\"";
            os << ",\"include_tests\":" << (inventory.include_tests ? "true" : "false");
            os << ",\"include_examples\":" << (inventory.include_examples ? "true" : "false");
            os << ",\"source_dir\":\"" << escape(inventory.source_dir) << "\"";
        };
        for(const auto& unit : inventory.units)
        {
            m.json << m.jsonl("unit") << [&](std::ostream& os){
                os << ",\"unit\":\"" << escape(unit.unit) << "\"";
                os << ",\"path\":\"" << escape(unit.path) << "\"";
                if(!unit.module.empty())
                    os << ",\"module\":\"" << escape(unit.module) << "\"";
                os << ",\"kind\":\"" << escape(unit.kind) << "\"";
                write_string_array(os, "imports", unit.imports);
                if(unit.level >= 0)
                    os << ",\"level\":" << unit.level;
                os << ",\"has_main\":" << (unit.has_main ? "true" : "false");
                os << ",\"is_test\":" << (unit.is_test ? "true" : "false");
                os << ",\"is_modular\":" << (unit.is_modular ? "true" : "false");
            };
        }
        m.json << m.jsonl("list_summary") << [&](std::ostream& os){
            os << ",\"units_total\":" << inventory.units.size();
            os << ",\"main_count\":" << inventory.main_count;
            os << ",\"test_count\":" << inventory.test_count;
            os << ",\"max_level\":" << inventory.max_level;
        };
    }
};

} // namespace cb::output::jsonl


