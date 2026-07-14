// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <algorithm>
#include <mutex>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "../tester/details/output-mux.h++"
#include "cb-jsonl_sink.h++"

namespace cb_console {

using namespace std::string_view_literals;

using string_list = std::vector<std::string>;

inline std::string format_token_list(const string_list& tokens, std::size_t max_tokens = 8)
{
    if(tokens.empty())
        return {};

    const auto count = std::min(tokens.size(), max_tokens);
    const string_list head{tokens.begin(), tokens.begin() + static_cast<std::ptrdiff_t>(count)};
    auto out = head | std::views::join_with(", "sv) | std::ranges::to<std::string>();
    if(tokens.size() > max_tokens)
        out += ", ... (" + std::to_string(tokens.size() - max_tokens) + " more)";
    return out;
}

inline std::string format_token_change_summary(std::string_view name, const cb_jsonl::profile_token_change& change, std::size_t max_tokens = 8)
{
    auto parts = string_list{};
    if(not change.added.empty())
        parts.push_back("+ " + format_token_list(change.added, max_tokens));
    if(not change.removed.empty())
        parts.push_back("- " + format_token_list(change.removed, max_tokens));
    if(parts.empty())
        return {};

    return std::string{name} + ": " + (parts | std::views::join_with(", "sv) | std::ranges::to<std::string>());
}

inline std::string format_profile_diff(const cb_jsonl::object_cache_profile_diff& diff, std::size_t max_tokens = 8)
{
    auto parts = string_list{};
    const auto append_scalar = [&](std::string_view name, const cb_jsonl::profile_scalar_change& change) {
        parts.push_back(std::string{name} + ": " + change.old_value + " -> " + change.new_value);
    };

    if(diff.format) append_scalar("format", *diff.format);
    if(diff.config) append_scalar("config", *diff.config);
    if(diff.static_link) append_scalar("static_link", *diff.static_link);
    if(diff.llvm) append_scalar("llvm", *diff.llvm);
    if(diff.cxx) append_scalar("cxx", *diff.cxx);
    if(diff.cxx_sig) append_scalar("cxx_sig", *diff.cxx_sig);
    if(diff.clang_ver) append_scalar("clang_ver", *diff.clang_ver);
    if(diff.std_cppm) append_scalar("std_cppm", *diff.std_cppm);
    if(diff.compile)
    {
        if(auto summary = format_token_change_summary("compile", *diff.compile, max_tokens); not summary.empty())
            parts.push_back(std::move(summary));
    }
    if(diff.cpp)
    {
        if(auto summary = format_token_change_summary("cpp", *diff.cpp, max_tokens); not summary.empty())
            parts.push_back(std::move(summary));
    }

    return parts | std::views::join_with("; "sv) | std::ranges::to<std::string>();
}

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

    void profile_changed(std::string_view reason, const cb_jsonl::object_cache_profile_diff* diff = nullptr)
    {
        auto msg = std::string{"Object cache profile changed; invalidating compile cache"};
        if(diff && !diff->empty())
        {
            msg += " (";
            msg += format_profile_diff(*diff);
            msg += ')';
        }
        info(msg);
    }

    void profile_change_rebuild(std::string_view tu_label)
    {
        info("Rebuilding " + std::string{tu_label} + " because compile profile changed");
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
            if (not tu.imports.empty()) {
                os << io::color::gray << "   imports: ";
                for (std::size_t i = 0; i < tu.imports.size(); ++i) {
                    if (i) os << ", ";
                    os << tu.imports[i];
                }
                os << io::color::reset << "\n";
            }
        }
        os << io::color::cyan << "\n" << io::color::reset;
    }
};

} // namespace cb_console

