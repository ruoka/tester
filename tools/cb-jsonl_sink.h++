// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <chrono>
#include <mutex>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../tester/details/output-mux.h++"

namespace cb_jsonl {

using ::jsonl::escape;

inline void write_argv(std::ostream& os, std::span<const std::string> argv)
{
    os << ",\"argv\":[";
    for(std::size_t i = 0; i < argv.size(); ++i)
    {
        if(i) os << ',';
        os << '"' << escape(argv[i]) << '"';
    }
    os << ']';
}

inline void write_string_array(std::ostream& os, std::string_view field, std::span<const std::string> values)
{
    os << ",\"" << field << "\":[";
    for(std::size_t i = 0; i < values.size(); ++i)
    {
        if(i) os << ',';
        os << '"' << escape(values[i]) << '"';
    }
    os << ']';
}

struct sink
{
    io::mux& m;

    explicit sink(io::mux& mux) : m(mux) {}

    void build_start(std::string_view config, bool include_tests, bool include_examples)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("build_start") << [&](std::ostream& os){
            os << ",\"config\":\"" << escape(config) << "\"";
            os << ",\"include_tests\":" << (include_tests ? "true" : "false");
            os << ",\"include_examples\":" << (include_examples ? "true" : "false");
        };
    }

    void build_end(bool ok, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("build_end") << [&](std::ostream& os){
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"duration_ms\":" << duration.count();
        };
    }

    void test_start(std::string_view runner)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("test_start") << [&](std::ostream& os){
            os << ",\"runner\":\"" << escape(runner) << "\"";
        };
    }

    void test_end(bool ok, int exit_code, int wait_status, bool signaled, int signal_number, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished)
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

    void cb_error(std::string_view message)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("cb_error") << [&](std::ostream& os){
            os << ",\"message\":\"" << escape(message) << "\"";
        };
    }

    void command_start(std::string_view cmd, std::span<const std::string> argv)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("command_start") << [&](std::ostream& os){
            os << ",\"cmd\":\"" << escape(cmd) << "\"";
            write_argv(os, argv);
        };
    }

    void command_end(std::string_view cmd, std::span<const std::string> argv, bool ok, int exit_code, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("command_end") << [&](std::ostream& os){
            os << ",\"cmd\":\"" << escape(cmd) << "\"";
            write_argv(os, argv);
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"exit_code\":" << exit_code;
            os << ",\"duration_ms\":" << duration.count();
        };
    }

    void profile_changed(std::string_view reason, std::string_view profile_diff_json = {})
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("profile_changed") << [&](std::ostream& os){
            os << ",\"reason\":\"" << escape(reason) << "\"";
            if(!profile_diff_json.empty())
                os << ",\"profile_diff\":" << profile_diff_json;
        };
    }

    void cache_status(std::string_view object_cache_path,
                      bool object_cache_exists,
                      bool legacy_header,
                      bool profile_match,
                      int object_entries,
                      int object_stale_entries,
                      int executable_entries,
                      std::string_view current_profile)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("cache_status") << [&](std::ostream& os){
            os << ",\"object_cache_path\":\"" << escape(object_cache_path) << "\"";
            os << ",\"object_cache_exists\":" << (object_cache_exists ? "true" : "false");
            os << ",\"legacy_header\":" << (legacy_header ? "true" : "false");
            os << ",\"profile_match\":" << (profile_match ? "true" : "false");
            os << ",\"object_entries\":" << object_entries;
            os << ",\"object_stale_entries\":" << object_stale_entries;
            os << ",\"executable_entries\":" << executable_entries;
            os << ",\"current_profile\":\"" << escape(current_profile) << "\"";
        };
    }

    void cache_invalidate_end(bool object_cache_removed,
                              bool executable_cache_removed,
                              bool compiler_stamp_removed)
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
                       std::string_view rebuild_reason = {})
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("compile_start") << [&](std::ostream& os){
            os << ",\"source_path\":\"" << escape(source_path) << "\"";
            os << ",\"object_path\":\"" << escape(object_path) << "\"";
            if(!pcm_path.empty())
                os << ",\"pcm_path\":\"" << escape(pcm_path) << "\"";
            if(!module_name.empty())
                os << ",\"module_name\":\"" << escape(module_name) << "\"";
            if(!rebuild_reason.empty())
                os << ",\"rebuild_reason\":\"" << escape(rebuild_reason) << "\"";
        };
    }

    void link_end(std::string_view executable_path,
                  bool ok,
                  bool cache_hit,
                  std::chrono::steady_clock::time_point started,
                  std::chrono::steady_clock::time_point finished)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("link_end") << [&](std::ostream& os){
            os << ",\"executable_path\":\"" << escape(executable_path) << "\"";
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"cache_hit\":" << (cache_hit ? "true" : "false");
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
                     std::string_view rebuild_reason = {},
                     std::string_view profile_diff_json = {})
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
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
            if(!cache_hit && !rebuild_reason.empty())
                os << ",\"rebuild_reason\":\"" << escape(rebuild_reason) << "\"";
            if(!profile_diff_json.empty())
                os << ",\"profile_diff\":" << profile_diff_json;
            os << ",\"duration_ms\":" << duration.count();
        };
    }

    void list_start(std::string_view config, bool include_tests, bool include_examples, std::string_view source_dir)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("list_start") << [&](std::ostream& os){
            os << ",\"config\":\"" << escape(config) << "\"";
            os << ",\"include_tests\":" << (include_tests ? "true" : "false");
            os << ",\"include_examples\":" << (include_examples ? "true" : "false");
            os << ",\"source_dir\":\"" << escape(source_dir) << "\"";
        };
    }

    void unit(std::string_view unit_id,
              std::string_view path,
              std::string_view module_name,
              std::string_view kind,
              std::span<const std::string> imports,
              int level,
              bool has_main,
              bool is_test,
              bool is_modular)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("unit") << [&](std::ostream& os){
            os << ",\"unit\":\"" << escape(unit_id) << "\"";
            os << ",\"path\":\"" << escape(path) << "\"";
            if(!module_name.empty())
                os << ",\"module\":\"" << escape(module_name) << "\"";
            os << ",\"kind\":\"" << escape(kind) << "\"";
            write_string_array(os, "imports", imports);
            if(level >= 0)
                os << ",\"level\":" << level;
            os << ",\"has_main\":" << (has_main ? "true" : "false");
            os << ",\"is_test\":" << (is_test ? "true" : "false");
            os << ",\"is_modular\":" << (is_modular ? "true" : "false");
        };
    }

    void list_summary(int units_total, int main_count, int test_count, int max_level)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("list_summary") << [&](std::ostream& os){
            os << ",\"units_total\":" << units_total;
            os << ",\"main_count\":" << main_count;
            os << ",\"test_count\":" << test_count;
            os << ",\"max_level\":" << max_level;
        };
    }
};

} // namespace cb_jsonl


