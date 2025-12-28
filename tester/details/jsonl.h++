// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

// JSONL utilities for this project:
// - JSON escaping
// - unix time helpers
// - jsonl_context for emitting JSONL events (meta/event/eof) to a stream

#pragma once

#include <string>
#include <string_view>
#include <format>
#include <iostream>
#include <chrono>
#include <type_traits>
#include <unistd.h>

namespace jsonl {

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
                    out += std::format("\\u{:04x}", static_cast<unsigned int>(ch));
                else
                    out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

inline std::chrono::milliseconds unix_ms_now()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch());
}

inline std::chrono::milliseconds unix_ms(std::chrono::system_clock::time_point tp)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(tp.time_since_epoch());
}

inline unsigned pid()
{
    return static_cast<unsigned>(::getpid());
}

template<typename Stream, typename F>
void emit_event_raw(Stream& os, std::string_view type, std::string_view schema, int version, std::chrono::milliseconds ts_unix_ms, unsigned pid_value, F&& add_fields)
{
    os << "{\"type\":\"" << type << "\"";
    os << ",\"schema\":\"" << escape(schema) << "\"";
    os << ",\"version\":" << version;
    os << ",\"pid\":" << pid_value;
    os << ",\"ts_unix_ms\":" << ts_unix_ms.count();
    add_fields(os);
    os << "}\n";
}

template<typename Stream>
struct jsonl_context
{
    using stream_type = std::remove_reference_t<Stream>;

    static constexpr auto schema = "tester-jsonl";
    static constexpr int version = 1;

    stream_type& stream;
    bool enabled = false;
    bool meta_printed = false;
    bool eof_emitted = false;

    struct event_builder
    {
        jsonl_context* ctx = nullptr;
        std::string_view type{};
        std::chrono::milliseconds ts_unix_ms{0};
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

    explicit jsonl_context(stream_type& s) : stream(s) {}

    bool is_enabled() const { return enabled; }
    void set_enabled(bool v) { enabled = v; }
    void reset_stream_state()
    {
        meta_printed = false;
        eof_emitted = false;
    }

    void emit_meta()
    {
        if(!enabled || meta_printed) return;
        meta_printed = true;
        emit_event_raw(stream, "meta", schema, version, unix_ms_now(), pid(), [](auto&){});
        stream << std::flush;
    }

    template<typename F>
    void emit_event(std::string_view type, F&& add_fields)
    {
        if(!enabled) return;
        emit_meta();
        emit_event_raw(stream, type, schema, version, unix_ms_now(), pid(), std::forward<F>(add_fields));
        stream << std::flush;
    }

    template<typename F>
    void emit_event_with_ts(std::string_view type, std::chrono::milliseconds ts, F&& add_fields)
    {
        if(!enabled) return;
        emit_meta();
        emit_event_raw(stream, type, schema, version, ts, pid(), std::forward<F>(add_fields));
        stream << std::flush;
    }

    template<typename F>
    void emit_event_at(std::string_view type, std::chrono::system_clock::time_point tp, F&& add_fields)
    {
        emit_event_with_ts(type, unix_ms(tp), std::forward<F>(add_fields));
    }

    void emit_eof()
    {
        if(!enabled || eof_emitted) return;
        eof_emitted = true;
        emit_event("eof", [](auto&){});
    }

    event_builder event(std::string_view type) { return event_builder{this, type, std::chrono::milliseconds{0}, false}; }
    event_builder event_with_ts(std::string_view type, std::chrono::milliseconds ts) { return event_builder{this, type, ts, true}; }
    event_builder event_at(std::string_view type, std::chrono::system_clock::time_point tp) { return event_builder{this, type, unix_ms(tp), true}; }

    event_builder operator()(std::string_view type) { return event(type); }
    event_builder operator()(std::string_view type, std::chrono::milliseconds ts) { return event_with_ts(type, ts); }
    event_builder operator()(std::string_view type, std::chrono::system_clock::time_point tp) { return event_at(type, tp); }
};

} // namespace jsonl


