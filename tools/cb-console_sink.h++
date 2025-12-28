// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <mutex>
#include <ostream>
#include <string_view>
#include <vector>

#include "../tester/details/output-mux.h++"

namespace cb_console {

struct sink
{
    io::mux& m;

    explicit sink(io::mux& mux) : m(mux) {}

    void error(std::string_view msg)
    {
        io::error(m, msg);
    }

    void warning(std::string_view msg)
    {
        io::warning(m, msg);
    }

    void info(std::string_view msg)
    {
        io::info(m, msg);
    }

    void success(std::string_view msg)
    {
        io::success(m, msg);
    }

    void command(std::string_view cmd)
    {
        io::command(m, cmd);
    }

    template<typename TranslationUnit>
    void print_sources(const std::vector<TranslationUnit>& units)
    {
        auto lock = std::lock_guard<std::mutex>{m.mutex};
        auto& os = m.human_os();
        os << io::color::cyan << "\nFound " << units.size() << " translation units:\n\n" << io::color::reset;
        
        int main_count = 0, test_count = 0;
        for (const auto& tu : units) {
            if (tu.has_main) main_count++;
            if (tu.is_test) test_count++;
        }
        
        os << io::color::cyan << " Total: " << units.size()
                  << " | Main: " << main_count
                  << " | Tests: " << test_count << "\n\n" << io::color::reset;

        for (const auto& tu : units) {
            auto full = tu.path.empty() ? tu.filename : tu.path + "/" + tu.filename;
            os << io::color::cyan << " " << full << io::color::reset;
            if (not tu.module.empty()) os << " " << io::color::yellow << "[module: " << tu.module << "]" << io::color::reset;
            if (tu.has_main) os << " " << io::color::green << "[main]" << io::color::reset;
            if (tu.is_test) os << " " << io::color::magenta << "[TEST]" << io::color::reset;
            if (tu.dependency_level >= 0) os << " " << io::color::gray << "level=" << tu.dependency_level << io::color::reset;
            os << "\n";
        }
        os << io::color::cyan << "\n" << io::color::reset;
    }
};

} // namespace cb_console

