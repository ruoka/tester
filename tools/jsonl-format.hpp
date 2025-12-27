// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

// Header-only JSONL formatting utilities
// Used by both cb.c++ and tester-output.c++m to avoid code duplication
// Provides JSON escaping and event emission utilities

#pragma once

#include <string>
#include <string_view>
#include <format>
#include <iostream>
#include <chrono>
#include <type_traits>
#include <unistd.h>

namespace jsonl_util {

// Escape a string for JSON. Produces a JSON-encoded string value without surrounding quotes.
// Handles all JSON-required escapes: \b, \f, \n, \r, \t, \\, \", and Unicode escapes for control characters.
inline std::string escape(std::string_view sv)
{
    auto out = std::string{};
    out.reserve(sv.size() + 16);
    for(const unsigned char ch : sv)
    {
        switch(ch)
        {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if(ch < 0x20)
                {
                    // Control characters: escape as \uXXXX
                    out += std::format("\\u{:04x}", static_cast<unsigned int>(ch));
                }
                else
                {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    return out;
}

// Get current Unix timestamp in milliseconds
inline long long unix_ms_now()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Convert a system_clock::time_point to Unix timestamp in milliseconds
inline long long unix_ms(std::chrono::system_clock::time_point tp)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(tp.time_since_epoch()).count();
}

inline long long duration_ms(std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(finished - started).count();
}

inline long long duration_ms(std::chrono::system_clock::time_point started, std::chrono::system_clock::time_point finished)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(finished - started).count();
}

// Get current process ID as unsigned
inline unsigned pid()
{
    return static_cast<unsigned>(::getpid());
}

// Emit a JSONL event to a stream (low-level function, no meta emission)
// Stream: output stream (e.g., std::cout)
// type: event type (e.g., "test_start", "build_end")
// schema: schema name (e.g., "cb-jsonl", "tester-jsonl")
// version: schema version number
// ts_unix_ms: Unix timestamp in milliseconds (use unix_ms_now() for current time)
// pid: process ID (use pid() for current process)
// add_fields: callable that adds additional fields to the event (receives stream reference)
template<typename Stream, typename F>
void emit_event_raw(Stream& os, std::string_view type, std::string_view schema, int version, long long ts_unix_ms, unsigned pid_value, F&& add_fields)
{
    os << "{\"type\":\"" << type << "\"";
    os << ",\"schema\":\"" << escape(schema) << "\"";
    os << ",\"version\":" << version;
    os << ",\"pid\":" << pid_value;
    os << ",\"ts_unix_ms\":" << ts_unix_ms;
    add_fields(os);
    os << "}\n";
}

// Convenience function for direct event emission (backward compatibility)
template<typename Stream, typename F>
void emit_event(Stream& os, std::string_view type, std::string_view schema, int version, long long ts_unix_ms, unsigned pid_value, F&& add_fields)
{
    emit_event_raw(os, type, schema, version, ts_unix_ms, pid_value, std::forward<F>(add_fields));
}

// JSONL context manager for emitting events with automatic meta emission
// Manages state for meta emission and provides convenience methods
// Context owns schema and version constants - they define the JSONL format used by this project
template<typename Stream>
struct jsonl_context
{
    using stream_type = std::remove_reference_t<Stream>;

    // Schema name - defines the JSONL format used by this project
    // This is a constant - users cannot customize it since the project controls the JSONL structure
    static constexpr auto schema = "tester-jsonl";
    
    // Schema version - increment when making breaking changes to the JSONL format
    static constexpr int version = 1;

    stream_type& stream;
    bool enabled = false;
    bool meta_printed = false;
    bool eof_emitted = false;

    struct event_builder
    {
        jsonl_context* ctx = nullptr;
        std::string_view type{};
        long long ts_unix_ms = 0;
        bool has_ts = false;

        template<typename F>
        void operator<<(F&& add_fields) const
        {
            if(ctx == nullptr) return;
            if(has_ts) ctx->emit_event_with_ts(type, ts_unix_ms, std::forward<F>(add_fields));
            else ctx->emit_event(type, std::forward<F>(add_fields));
        }
    };

    struct pending_event
    {
        stream_type* os = nullptr;
        event_builder ev{};

        template<typename F>
        stream_type& operator<<(F&& add_fields) const
        {
            ev << std::forward<F>(add_fields);
            return *os;
        }
    };

    friend auto operator<<(stream_type& os, event_builder ev) -> pending_event
    {
        return pending_event{&os, ev};
    }

    // Constructor: context owns its state; stream is provided by the caller.
    explicit jsonl_context(stream_type& s) : stream(s) {}

    auto is_enabled() const -> bool { return enabled; }
    void set_enabled(bool v) { enabled = v; }
    void reset_stream_state()
    {
        meta_printed = false;
        eof_emitted = false;
    }

    // Emit meta event if not already emitted
    void emit_meta()
    {
        if(!enabled || meta_printed) return;
        meta_printed = true;
        emit_event_raw(stream, "meta", schema, version, unix_ms_now(), pid(), [](auto&){});
        stream << std::flush;
    }

    // Emit a JSONL event (automatically emits meta first if needed)
    template<typename F>
    void emit_event(std::string_view type, F&& add_fields)
    {
        if(!enabled) return;
        emit_meta();
        emit_event_raw(stream, type, schema, version, unix_ms_now(), pid(), std::forward<F>(add_fields));
        stream << std::flush;
    }

    // Emit a JSONL event with custom timestamp (automatically emits meta first if needed)
    template<typename F>
    void emit_event_with_ts(std::string_view type, long long ts_unix_ms, F&& add_fields)
    {
        if(!enabled) return;
        emit_meta();
        emit_event_raw(stream, type, schema, version, ts_unix_ms, pid(), std::forward<F>(add_fields));
        stream << std::flush;
    }

    // Emit a JSONL event at a specific system_clock time_point
    template<typename F>
    void emit_event_at(std::string_view type, std::chrono::system_clock::time_point tp, F&& add_fields)
    {
        emit_event_with_ts(type, unix_ms(tp), std::forward<F>(add_fields));
    }

    // Emit EOF event (only once)
    void emit_eof()
    {
        if(!enabled || eof_emitted) return;
        eof_emitted = true;
        emit_event("eof", [](auto&){});
    }

    auto event(std::string_view type) -> event_builder { return event_builder{this, type, 0, false}; }
    auto event_with_ts(std::string_view type, long long ts_unix_ms) -> event_builder { return event_builder{this, type, ts_unix_ms, true}; }
    auto event_at(std::string_view type, std::chrono::system_clock::time_point tp) -> event_builder { return event_with_ts(type, unix_ms(tp)); }

    auto operator()(std::string_view type) -> event_builder { return event(type); }
    auto operator()(std::string_view type, long long ts_unix_ms) -> event_builder { return event_with_ts(type, ts_unix_ms); }
    auto operator()(std::string_view type, std::chrono::system_clock::time_point tp) -> event_builder { return event_at(type, tp); }
};

} // namespace jsonl_util

