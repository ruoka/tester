// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

// XML 1.0 escaping for report sinks (JUnit / xUnit XML).
// Independent of the JSONL escape helpers — the two formats disagree on
// what is special and how controls are represented.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace xml {

// Length of the well-formed UTF-8 sequence starting at index, or 0 if invalid.
// Same acceptance rules as the JSONL helper: reject overlong encodings,
// surrogates, and anything above U+10FFFF.
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
        return 0;

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

inline bool is_xml10_char(unsigned code_point)
{
    // Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
    if(code_point == 0x9 || code_point == 0xa || code_point == 0xd)
        return true;
    if(code_point >= 0x20 && code_point <= 0xd7ff)
        return true;
    if(code_point >= 0xe000 && code_point <= 0xfffd)
        return true;
    if(code_point >= 0x10000 && code_point <= 0x10ffff)
        return true;
    return false;
}

inline unsigned decode_utf8(std::string_view sv, std::size_t index, std::size_t length)
{
    const auto lead = static_cast<unsigned char>(sv[index]);
    if(length == 1)
        return lead;

    auto code_point = 0u;
    if(length == 2) code_point = lead & 0x1fu;
    else if(length == 3) code_point = lead & 0x0fu;
    else code_point = lead & 0x07u;

    for(auto offset = std::size_t{1}; offset < length; ++offset)
    {
        const auto continuation = static_cast<unsigned char>(sv[index + offset]);
        code_point = (code_point << 6) | (continuation & 0x3fu);
    }
    return code_point;
}

// Escape text for XML attribute values and element bodies. Invalid UTF-8 and
// characters outside the XML 1.0 Char production become U+FFFD so the report
// stays well-formed even when assertion data carries arbitrary bytes.
inline std::string escape(std::string_view sv)
{
    constexpr auto replacement = "\xef\xbf\xbd";

    auto out = std::string{};
    out.reserve(sv.size() + 16);

    for(std::size_t index = 0; index < sv.size();)
    {
        const auto length = utf8_sequence_length(sv, index);
        if(length == 0)
        {
            out += replacement;
            ++index;
            continue;
        }

        const auto code_point = decode_utf8(sv, index, length);
        if(length == 1)
        {
            switch(static_cast<char>(code_point))
            {
                case '&':  out += "&amp;";  ++index; continue;
                case '<':  out += "&lt;";   ++index; continue;
                case '>':  out += "&gt;";   ++index; continue;
                case '"':  out += "&quot;"; ++index; continue;
                case '\'': out += "&apos;"; ++index; continue;
                default: break;
            }
        }

        if(not is_xml10_char(code_point))
        {
            out += replacement;
            index += length;
            continue;
        }

        out.append(sv.data() + index, length);
        index += length;
    }

    return out;
}

} // namespace xml
