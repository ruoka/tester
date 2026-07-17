// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
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

inline void write_rebuild(std::ostream& os, const rebuild_info& rebuild)
{
    os << '{';
    auto first = true;
    write_rebuild_field(os, "kind", rebuild.kind, first);
    write_rebuild_field(os, "module", rebuild.module, first);
    write_rebuild_field(os, "pcm_path", rebuild.pcm_path, first);
    write_rebuild_field(os, "object_path", rebuild.object_path, first);
    write_rebuild_field(os, "trigger_path", rebuild.trigger_path, first);
    write_rebuild_field(os, "hint", rebuild.hint, first);
    write_rebuild_field(os, "message", rebuild.message, first);
    write_rebuild_field(os, "see_event", rebuild.see_event, first);
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
            ++rebuild_by_kind[rebuild.kind];
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

    void build_end(bool ok, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("build_end") << [&](std::ostream& os){
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"duration_ms\":" << duration.count();
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

    void test_end(bool ok, int exit_code, int wait_status, bool signaled, int signal_number, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("test_end") << [&](std::ostream& os){
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"exit_code\":" << exit_code;
            os << ",\"wait_status\":" << wait_status;
            os << ",\"signaled\":" << (signaled ? "true" : "false");
            if(signaled) os << ",\"signal\":" << signal_number;
            os << ",\"duration_ms\":" << duration.count();
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

    void command_end(std::string_view cmd, std::span<const std::string> argv, bool ok, int exit_code, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        if(not ok)
            ++m.commands_failed;
        if(m.mode == jsonl_mode::summary || (m.mode == jsonl_mode::failures && ok))
            return;

        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("command_end") << [&](std::ostream& os){
            if(m.mode == jsonl_mode::trace)
                os << ",\"cmd\":\"" << escape(cmd) << "\"";
            write_argv(os, argv);
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"exit_code\":" << exit_code;
            os << ",\"duration_ms\":" << duration.count();
        };
    }

    void profile_changed(std::string_view reason, const object_cache_profile_diff& diff) override
    {
        if(m.mode == jsonl_mode::summary)
            return;

        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("profile_changed") << [&](std::ostream& os){
            os << ",\"reason\":\"" << escape(reason) << "\"";
            if(!diff.empty())
            {
                os << ",\"profile_diff\":";
                write_profile_diff(os, diff);
            }
        };
    }

    void cache_status(std::string_view object_cache_path,
                      bool object_cache_exists,
                      bool profile_match,
                      int object_entries,
                      int object_stale_entries,
                      int executable_entries,
                      std::string_view current_profile) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("cache_status") << [&](std::ostream& os){
            os << ",\"object_cache_path\":\"" << escape(object_cache_path) << "\"";
            os << ",\"object_cache_exists\":" << (object_cache_exists ? "true" : "false");
            os << ",\"profile_match\":" << (profile_match ? "true" : "false");
            os << ",\"object_entries\":" << object_entries;
            os << ",\"object_stale_entries\":" << object_stale_entries;
            os << ",\"executable_entries\":" << executable_entries;
            os << ",\"current_profile\":\"" << escape(current_profile) << "\"";
        };
    }

    void cache_invalidate_end(bool object_cache_removed,
                              bool executable_cache_removed,
                              bool compiler_stamp_removed) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("cache_invalidate_end") << [&](std::ostream& os){
            os << ",\"object_cache_removed\":" << (object_cache_removed ? "true" : "false");
            os << ",\"executable_cache_removed\":" << (executable_cache_removed ? "true" : "false");
            os << ",\"compiler_stamp_removed\":" << (compiler_stamp_removed ? "true" : "false");
        };
    }

    void compile_start(std::string_view source_path,
                       std::string_view object_path,
                       std::string_view pcm_path,
                       std::string_view module_name,
                       const rebuild_info& rebuild = {}) override
    {
        if(m.mode != jsonl_mode::trace)
            return;

        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("compile_start") << [&](std::ostream& os){
            os << ",\"source_path\":\"" << escape(source_path) << "\"";
            os << ",\"object_path\":\"" << escape(object_path) << "\"";
            if(!pcm_path.empty())
                os << ",\"pcm_path\":\"" << escape(pcm_path) << "\"";
            if(!module_name.empty())
                os << ",\"module_name\":\"" << escape(module_name) << "\"";
            if(not rebuild.empty())
            {
                os << ",\"rebuild_reason\":\"" << escape(rebuild.kind) << "\"";
                os << ",\"rebuild\":";
                write_rebuild(os, rebuild);
                if(not rebuild.message.empty())
                    os << ",\"message\":\"" << escape(rebuild.message) << "\"";
            }
        };
    }

    void link_end(std::string_view executable_path,
                  bool ok,
                  bool cache_hit,
                  std::chrono::steady_clock::time_point started,
                  std::chrono::steady_clock::time_point finished,
                  const rebuild_info& rebuild = {}) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        ++m.links_total;
        if(cache_hit)
            ++m.link_cache_hits;
        if(not ok)
            ++m.link_failed;
        if(m.mode == jsonl_mode::summary || (m.mode == jsonl_mode::failures && ok))
            return;

        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("link_end") << [&](std::ostream& os){
            os << ",\"executable_path\":\"" << escape(executable_path) << "\"";
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"cache_hit\":" << (cache_hit ? "true" : "false");
            if(not cache_hit and not rebuild.empty())
            {
                os << ",\"rebuild_reason\":\"" << escape(rebuild.kind) << "\"";
                os << ",\"rebuild\":";
                write_rebuild(os, rebuild);
            }
            os << ",\"duration_ms\":" << duration.count();
        };
    }

    void compile_end(std::string_view source_path,
                     std::string_view object_path,
                     std::string_view pcm_path,
                     std::string_view module_name,
                     bool ok,
                     bool cache_hit,
                     std::chrono::steady_clock::time_point started,
                     std::chrono::steady_clock::time_point finished,
                     const rebuild_info& rebuild = {}) override
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        ++m.compile_total;
        if(cache_hit)
            ++m.compile_cache_hits;
        else
        {
            ++m.compile_rebuilt;
            m.note_rebuild(rebuild);
        }
        if(not ok)
            ++m.compile_failed;
        if(m.mode == jsonl_mode::summary || (m.mode == jsonl_mode::failures && ok))
            return;

        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("compile_end") << [&](std::ostream& os){
            os << ",\"source_path\":\"" << escape(source_path) << "\"";
            os << ",\"object_path\":\"" << escape(object_path) << "\"";
            if(!pcm_path.empty())
                os << ",\"pcm_path\":\"" << escape(pcm_path) << "\"";
            if(!module_name.empty())
                os << ",\"module_name\":\"" << escape(module_name) << "\"";
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"cache_hit\":" << (cache_hit ? "true" : "false");
            if(not cache_hit and not rebuild.empty())
            {
                os << ",\"rebuild_reason\":\"" << escape(rebuild.kind) << "\"";
                os << ",\"rebuild\":";
                write_rebuild(os, rebuild);
            }
            os << ",\"duration_ms\":" << duration.count();
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


