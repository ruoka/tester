// Copyright (c) 2025 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sys/wait.h>

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

inline auto run_test_runner(const std::vector<std::string>& args, std::string_view extra_env = {}) -> spawn_result
{
    auto cmd = std::string{};
    if(not extra_env.empty())
        cmd += std::string{extra_env} + " ";

    cmd += shell_quote(find_test_runner().string());
    for(const auto& arg : args)
    {
        cmd += ' ';
        cmd += shell_quote(arg);
    }

    cmd += " 2>/dev/null";

    auto pipe = ::popen(cmd.c_str(), "r");
    if(pipe == nullptr)
        return {.stdout_text = {}, .exit_code = -1};

    auto output = std::string{};
    auto buffer = std::array<char, 4096>{};
    while(true)
    {
        const auto n = std::fread(buffer.data(), 1, buffer.size(), pipe);
        if(n == 0)
            break;
        output.append(buffer.data(), n);
    }

    const auto status = ::pclose(pipe);
    auto exit_code = status;
#if defined(WIFEXITED) && defined(WEXITSTATUS)
    if(status != -1 && WIFEXITED(status))
        exit_code = WEXITSTATUS(status);
#endif
    return {.stdout_text = std::move(output), .exit_code = exit_code};
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