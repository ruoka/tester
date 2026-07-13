// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tester_selftest {

inline auto shell_quote(std::string_view arg) -> std::string
{
    auto out = std::string{"'"};
    for(const char ch : arg)
    {
        if(ch == '\'')
            out += "'\\''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

inline auto find_test_runner() -> std::filesystem::path
{
    if(const auto* env = std::getenv("TEST_RUNNER"); env != nullptr && env[0] != '\0')
        return std::filesystem::path{env};

    const auto cwd = std::filesystem::current_path();
    for(const auto& entry : std::filesystem::directory_iterator(cwd))
    {
        if(not entry.is_directory())
            continue;

        const auto name = entry.path().filename().string();
        if(not name.starts_with("build-"))
            continue;

        const auto candidate = entry.path() / "bin" / "test_runner";
        if(std::filesystem::exists(candidate))
            return candidate;
    }

    return cwd / "build-darwin-debug" / "bin" / "test_runner";
}

struct spawn_result
{
    std::string stdout_text;
    int exit_code = -1;
};

inline auto read_file_text(const std::filesystem::path& path) -> std::string
{
    auto file = std::ifstream{path};
    if(not file)
        return {};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

inline auto run_test_runner(const std::vector<std::string>& args, std::string_view extra_env = {}) -> spawn_result
{
    const auto out_path = std::filesystem::temp_directory_path()
        / ("tester_selftest_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + ".out");

    auto cmd = std::string{};
    if(not extra_env.empty())
        cmd += std::string{extra_env} + " ";

    cmd += shell_quote(find_test_runner().string());
    for(const auto& arg : args)
    {
        cmd += ' ';
        cmd += shell_quote(arg);
    }

    cmd += " > ";
    cmd += shell_quote(out_path.string());
    cmd += " 2>/dev/null";

    const auto status = std::system(cmd.c_str());
    auto output = read_file_text(out_path);
    std::error_code ec;
    std::filesystem::remove(out_path, ec);

    return {.stdout_text = std::move(output), .exit_code = status};
}

inline auto jsonl_events_contain(std::string_view jsonl, std::string_view needle) -> bool
{
    return jsonl.contains(needle);
}

inline auto last_summary_passed(std::string_view jsonl) -> std::optional<bool>
{
    auto pos = std::string_view::npos;
    while(true)
    {
        const auto found = jsonl.find("\"type\":\"summary\"", pos == std::string_view::npos ? 0 : pos + 1);
        if(found == std::string_view::npos)
            break;
        pos = found;
    }

    if(pos == std::string_view::npos)
        return std::nullopt;

    const auto slice = jsonl.substr(pos);
    if(slice.contains("\"passed\":true"))
        return true;
    if(slice.contains("\"passed\":false"))
        return false;
    return std::nullopt;
}

} // namespace tester_selftest