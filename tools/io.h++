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

#include "jsonl-format.hpp"
#include "term.hpp"

namespace io {

namespace color = ::term;

struct mux
{
    std::ostream& json;
    std::ostream& human;
    std::ostream& result;

    jsonl_util::jsonl_context<std::ostream> jsonl;

    // Optional shared mutex for coordinating mixed human logs from multiple threads.
    std::mutex mutex{};

    mux(std::ostream& json_os, std::ostream& human_os, std::ostream& result_os)
        : json(json_os), human(human_os), result(result_os), jsonl(json_os)
    {}

    void set_jsonl_enabled(bool v) { jsonl.set_enabled(v); }
    auto jsonl_enabled() const -> bool { return jsonl.is_enabled(); }
    void reset_jsonl_state() { jsonl.reset_stream_state(); }
};

// A tiny helper for CB-style prefix logs (human-only).
inline void log_prefixed(std::ostream& os, std::string_view prefix, std::string_view color_code, std::string_view msg)
{
    os << color_code << prefix << color::reset << " " << msg << "\n";
}

} // namespace io


