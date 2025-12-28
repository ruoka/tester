// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <chrono>
#include <mutex>
#include <ostream>
#include <string_view>

#include "../tester/details/output-mux.h++"

namespace cb_jsonl {

using ::jsonl::escape;

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

    void command_start(std::string_view cmd)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        m.json << m.jsonl("command_start") << [&](std::ostream& os){
            os << ",\"cmd\":\"" << escape(cmd) << "\"";
        };
    }

    void command_end(std::string_view cmd, bool ok, int exit_code, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
        m.json << m.jsonl("command_end") << [&](std::ostream& os){
            os << ",\"cmd\":\"" << escape(cmd) << "\"";
            os << ",\"ok\":" << (ok ? "true" : "false");
            os << ",\"exit_code\":" << exit_code;
            os << ",\"duration_ms\":" << duration.count();
        };
    }
};

} // namespace cb_jsonl


