// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

// Shared ANSI terminal formatting constants used by both:
// - deps/tester/tools/cb.c++
// - deps/tester/tester/tester-output.c++m

namespace term {
    inline constexpr auto reset = "\033[0m";

    namespace text {
        inline constexpr auto black   = "\033[30m";
        inline constexpr auto red     = "\033[31m";
        inline constexpr auto green   = "\033[32m";
        inline constexpr auto yellow  = "\033[33m";
        inline constexpr auto blue    = "\033[34m";
        inline constexpr auto magenta = "\033[35m";
        inline constexpr auto cyan    = "\033[36m";
        inline constexpr auto white   = "\033[37m";
        inline constexpr auto gray    = "\033[0;90m";
    }

    // Convenience aliases (some code uses term::{color} directly, others term::text::{color})
    inline constexpr auto cyan    = text::cyan;
    inline constexpr auto yellow  = text::yellow;
    inline constexpr auto green   = text::green;
    inline constexpr auto magenta = text::magenta;
    inline constexpr auto gray    = text::gray;

    namespace background {
        inline constexpr auto black   = "\033[40m";
        inline constexpr auto red     = "\033[41m";
        inline constexpr auto green   = "\033[42m";
        inline constexpr auto yellow  = "\033[43m";
        inline constexpr auto blue    = "\033[44m";
        inline constexpr auto magenta = "\033[45m";
        inline constexpr auto cyan    = "\033[46m";
        inline constexpr auto white   = "\033[47m";
    }

    namespace bold {
        inline constexpr auto red    = "\033[1;31m";
        inline constexpr auto yellow = "\033[1;33m";
        inline constexpr auto blue   = "\033[1;34m";
        inline constexpr auto green  = "\033[1;32m";
    }
} // namespace term


