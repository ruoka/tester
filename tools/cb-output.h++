// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cb::output {

struct profile_scalar_change
{
    std::string old_value;
    std::string new_value;
};

struct profile_token_change
{
    std::vector<std::string> added;
    std::vector<std::string> removed;

    bool changed() const { return not added.empty() or not removed.empty(); }
};

struct object_cache_profile_diff
{
    std::optional<profile_scalar_change> format;
    std::optional<profile_scalar_change> config;
    std::optional<profile_scalar_change> static_link;
    std::optional<profile_scalar_change> llvm;
    std::optional<profile_scalar_change> cxx;
    std::optional<profile_scalar_change> cxx_sig;
    std::optional<profile_scalar_change> clang_ver;
    std::optional<profile_scalar_change> std_cppm;
    std::optional<profile_token_change> compile;
    std::optional<profile_token_change> cpp;

    bool empty() const
    {
        return not format and not config and not static_link and not llvm
            and not cxx and not cxx_sig and not clang_ver and not std_cppm
            and not compile and not cpp;
    }
};

template<typename Diff, typename Callback>
void for_each_profile_scalar(Diff& diff, Callback&& callback)
{
    callback("format", diff.format);
    callback("config", diff.config);
    callback("static_link", diff.static_link);
    callback("llvm", diff.llvm);
    callback("cxx", diff.cxx);
    callback("cxx_sig", diff.cxx_sig);
    callback("clang_ver", diff.clang_ver);
    callback("std_cppm", diff.std_cppm);
}

template<typename Diff, typename Callback>
void for_each_profile_tokens(Diff& diff, Callback&& callback)
{
    callback("compile", diff.compile);
    callback("cpp", diff.cpp);
}

struct source_unit
{
    std::string unit;
    std::string path;
    std::string module;
    std::string kind;
    std::vector<std::string> imports;
    int level;
    bool has_main;
    bool is_test;
    bool is_modular;
};

struct source_inventory
{
    std::string config;
    bool include_tests;
    bool include_examples;
    std::string source_dir;
    std::vector<source_unit> units;
    int main_count;
    int test_count;
    int max_level;
};

} // namespace cb::output
