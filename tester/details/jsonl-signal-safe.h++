// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <cstddef>
#include <cstdint>
#include <unistd.h>

namespace jsonl_util::signal_safe {

inline std::size_t append_cstr(char* buf, std::size_t n, std::size_t cap, const char* s)
{
    while(*s && n < cap)
        buf[n++] = *s++;
    return n;
}

inline std::size_t append_u(char* buf, std::size_t n, std::size_t cap, unsigned v)
{
    char tmp[32];
    int m = 0;
    do { tmp[m++] = static_cast<char>('0' + (v % 10)); v /= 10; } while(v);
    while(m-- > 0 && n < cap)
        buf[n++] = tmp[m];
    return n;
}

inline void emit_crash_event_jsonl(
    int stdout_fd,
    int stderr_fd,
    unsigned signal_number,
    const char* schema,
    std::size_t schema_len,
    int version,
    bool emit_result_line)
{
    char buf[256];
    std::size_t n = 0;

    n = append_cstr(buf, n, sizeof(buf), "{\"type\":\"crash\",\"schema\":\"");
    for(std::size_t i = 0; i < schema_len && n < sizeof(buf); ++i)
        buf[n++] = schema[i];
    n = append_cstr(buf, n, sizeof(buf), "\",\"version\":");
    n = append_u(buf, n, sizeof(buf), static_cast<unsigned>(version));
    n = append_cstr(buf, n, sizeof(buf), ",\"pid\":");
    n = append_u(buf, n, sizeof(buf), static_cast<unsigned>(::getpid()));
    n = append_cstr(buf, n, sizeof(buf), ",\"signal\":");
    n = append_u(buf, n, sizeof(buf), static_cast<unsigned>(signal_number));
    n = append_cstr(buf, n, sizeof(buf), "}\n");

    (void)::write(stdout_fd, buf, n);

    if(emit_result_line)
    {
        const char result[] = "RESULT: passed=false crashed=true\n";
        (void)::write(stderr_fd, result, sizeof(result) - 1);
    }
}

} // namespace jsonl_util::signal_safe


