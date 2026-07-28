// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

// JSONL utilities for this project:
// - JSON escaping
// - unix time helpers
// - jsonl_context for emitting JSONL events (meta/event/eof) to a stream

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <format>
#include <iostream>
#include <chrono>
#include <random>
#include <type_traits>
#include <unistd.h>

namespace jsonl {

// Length of the well-formed UTF-8 sequence starting at index, or 0 if the bytes
// there are not valid UTF-8. Rejects overlong encodings, surrogates, and anything
// above U+10FFFF, so the result is always a legal JSON string body.
inline std::size_t utf8_sequence_length(std::string_view sv, std::size_t index)
{
    const auto byte = static_cast<unsigned char>(sv[index]);

    auto length = std::size_t{0};
    auto code_point = 0u;
    if(byte < 0x80)
        return 1;
    else if((byte & 0xe0) == 0xc0) { length = 2; code_point = byte & 0x1fu; }
    else if((byte & 0xf0) == 0xe0) { length = 3; code_point = byte & 0x0fu; }
    else if((byte & 0xf8) == 0xf0) { length = 4; code_point = byte & 0x07u; }
    else
        return 0; // continuation byte or invalid lead byte

    if(index + length > sv.size())
        return 0;

    for(auto offset = std::size_t{1}; offset < length; ++offset)
    {
        const auto continuation = static_cast<unsigned char>(sv[index + offset]);
        if((continuation & 0xc0) != 0x80)
            return 0;
        code_point = (code_point << 6) | (continuation & 0x3fu);
    }

    if(length == 2 && code_point < 0x80) return 0;
    if(length == 3 && code_point < 0x800) return 0;
    if(length == 4 && code_point < 0x10000) return 0;
    if(code_point > 0x10ffff) return 0;
    if(code_point >= 0xd800 && code_point <= 0xdfff) return 0;

    return length;
}

// JSON requires valid UTF-8. Test data is arbitrary bytes, so invalid sequences are
// replaced with U+FFFD rather than passed through — a single stray byte otherwise
// makes the whole line unparseable for every downstream consumer.
inline std::string escape(std::string_view sv)
{
    constexpr auto replacement_character = "\xef\xbf\xbd";

    auto out = std::string{};
    out.reserve(sv.size() + 16);
    for(std::size_t index = 0; index < sv.size();)
    {
        const auto ch = static_cast<unsigned char>(sv[index]);
        switch(ch)
        {
            case '\\': out += "\\\\"; ++index; continue;
            case '"':  out += "\\\""; ++index; continue;
            case '\b': out += "\\b"; ++index; continue;
            case '\f': out += "\\f"; ++index; continue;
            case '\n': out += "\\n"; ++index; continue;
            case '\r': out += "\\r"; ++index; continue;
            case '\t': out += "\\t"; ++index; continue;
            default: break;
        }

        if(ch < 0x20)
        {
            out += std::format("\\u{:04x}", static_cast<unsigned int>(ch));
            ++index;
            continue;
        }

        if(ch < 0x80)
        {
            out.push_back(static_cast<char>(ch));
            ++index;
            continue;
        }

        if(const auto length = utf8_sequence_length(sv, index); length > 0)
        {
            out.append(sv.substr(index, length));
            index += length;
            continue;
        }

        // Invalid byte: emit one replacement character and resynchronise by one byte.
        out += replacement_character;
        ++index;
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

inline std::string generate_run_id()
{
    auto rd = std::random_device{};
    auto out = std::string{};
    out.reserve(32);
    for(int i = 0; i < 16; ++i)
        out += std::format("{:02x}", static_cast<unsigned>(rd() & 0xff));
    return out;
}

template<typename Stream>
inline void write_run_ids(Stream& os, std::string_view run_id, std::string_view parent_run_id)
{
    if(!run_id.empty())
        os << ",\"run_id\":\"" << escape(run_id) << "\"";
    if(!parent_run_id.empty())
        os << ",\"parent_run_id\":\"" << escape(parent_run_id) << "\"";
}

template<typename Stream, typename F>
void emit_event_raw(Stream& os,
                    std::string_view type,
                    std::string_view schema,
                    int version,
                    std::chrono::milliseconds ts_unix_ms,
                    unsigned pid_value,
                    std::string_view run_id,
                    std::string_view parent_run_id,
                    F&& add_fields)
{
    os << "{\"type\":\"" << type << "\"";
    os << ",\"schema\":\"" << escape(schema) << "\"";
    os << ",\"version\":" << version;
    os << ",\"pid\":" << pid_value;
    os << ",\"ts_unix_ms\":" << ts_unix_ms.count();
    write_run_ids(os, run_id, parent_run_id);
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
    std::string run_id{};
    std::string parent_run_id{};

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

    [[nodiscard]] std::string_view get_run_id() const { return run_id; }
    [[nodiscard]] std::string_view get_parent_run_id() const { return parent_run_id; }

    void assign_new_run_id() { run_id = generate_run_id(); }

    void set_parent_run_id(std::string_view id)
    {
        parent_run_id.assign(id);
    }

    void emit_meta()
    {
        if(!enabled || meta_printed) return;
        meta_printed = true;
        emit_event_raw(stream, "meta", schema, version, unix_ms_now(), pid(), run_id, parent_run_id, [](auto&){});
        stream << std::flush;
    }

    template<typename F>
    void emit_event(std::string_view type, F&& add_fields)
    {
        if(!enabled) return;
        emit_meta();
        emit_event_raw(stream, type, schema, version, unix_ms_now(), pid(), run_id, parent_run_id, std::forward<F>(add_fields));
        stream << std::flush;
    }

    template<typename F>
    void emit_event_with_ts(std::string_view type, std::chrono::milliseconds ts, F&& add_fields)
    {
        if(!enabled) return;
        emit_meta();
        emit_event_raw(stream, type, schema, version, ts, pid(), run_id, parent_run_id, std::forward<F>(add_fields));
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
    event_builder event_at(std::string_view type, std::chrono::system_clock::time_point tp) { return event_with_ts(type, unix_ms(tp)); }

    event_builder operator()(std::string_view type) { return event(type); }
    event_builder operator()(std::string_view type, std::chrono::milliseconds ts) { return event_with_ts(type, ts); }
    event_builder operator()(std::string_view type, std::chrono::system_clock::time_point tp) { return event_at(type, tp); }
};

} // namespace jsonl