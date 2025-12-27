// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

// Shared IO multiplexing for this repository:
// - JSONL events go to the "json" stream (typically stdout)
// - Human logs go to the "human" stream (typically stderr/clog)
// - RESULT: line goes to the "result" stream
//
// The JSONL state (enabled/meta/eof) is owned by jsonl_context.

#include <ostream>
#include <mutex>
#include <string_view>

#include "jsonl-format.h++"
#include "term.h++"

namespace io {

namespace color = ::term;

struct mux
{
    std::ostream& json;
    std::ostream* human = nullptr;
    std::ostream* result = nullptr;

    jsonl_util::jsonl_context<std::ostream> jsonl;

    // Optional shared mutex for coordinating mixed human logs from multiple threads.
    std::mutex mutex{};

    mux(std::ostream& json_os, std::ostream& human_os, std::ostream& result_os)
        : json(json_os), human(&human_os), result(&result_os), jsonl(json_os)
    {}

    auto& human_os() { return *human; }
    auto& result_os() { return *result; }
    void set_human(std::ostream& os) { human = &os; }
    void set_result(std::ostream& os) { result = &os; }

    void set_jsonl_enabled(bool v) { jsonl.set_enabled(v); }
    auto jsonl_enabled() const -> bool { return jsonl.is_enabled(); }
    void reset_jsonl_state() { jsonl.reset_stream_state(); }
};

// Shared CB-style prefix logs (human-only).
inline void log_prefixed(std::ostream& os, std::string_view prefix, std::string_view color_code, std::string_view msg)
{
    os << color_code << prefix << color::reset << " " << msg << "\n";
}

inline void error(mux& m, std::string_view msg)
{
    auto lock = std::lock_guard<std::mutex>{m.mutex};
    log_prefixed(m.human_os(), "ERROR", color::bold::red, msg);
}

inline void warning(mux& m, std::string_view msg)
{
    auto lock = std::lock_guard<std::mutex>{m.mutex};
    log_prefixed(m.human_os(), "WARNING", color::bold::yellow, msg);
}

inline void info(mux& m, std::string_view msg)
{
    auto lock = std::lock_guard<std::mutex>{m.mutex};
    log_prefixed(m.human_os(), "INFO", color::bold::blue, msg);
}

inline void success(mux& m, std::string_view msg)
{
    auto lock = std::lock_guard<std::mutex>{m.mutex};
    log_prefixed(m.human_os(), "SUCCESS", color::bold::green, msg);
}

inline void command(mux& m, std::string_view cmdline)
{
    auto lock = std::lock_guard<std::mutex>{m.mutex};
    log_prefixed(m.human_os(), "COMMAND", color::bold::blue, cmdline);
}

} // namespace io


