// Copyright (c) 2025-2026 Kaius Ruokonen. All rights reserved.
// SPDX-License-Identifier: MIT
// See the LICENSE file in the project root for full license text.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace tester_selftest {

inline auto shell_quote(std::string_view arg)
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

inline auto find_test_runner()
{
    if(const auto* env = std::getenv("TEST_RUNNER"); env != nullptr && env[0] != '\0')
        return std::filesystem::path{env};

    // Prefer the newest build-*/bin/test_runner. directory_iterator order is
    // unspecified, and a stale Make tree (build-linux/) next to CB's
    // build-linux-debug/ would otherwise win and run probes against the wrong binary.
    const auto cwd = std::filesystem::current_path();
    auto best = std::filesystem::path{};
    auto best_write = std::filesystem::file_time_type::min();
    for(const auto& entry : std::filesystem::directory_iterator(cwd))
    {
        if(not entry.is_directory())
            continue;

        const auto name = entry.path().filename().string();
        if(not name.starts_with("build-"))
            continue;

        const auto candidate = entry.path() / "bin" / "test_runner";
        auto error = std::error_code{};
        const auto write = std::filesystem::last_write_time(candidate, error);
        if(error)
            continue;
        if(best.empty() or write > best_write)
        {
            best = candidate;
            best_write = write;
        }
    }

    if(not best.empty())
        return best;

    return cwd / "build-darwin-debug" / "bin" / "test_runner";
}

struct spawn_result
{
    std::string stdout_text;
    // The console observer writes to stderr, so assertions about human-facing
    // rendering need it captured separately from the JSONL stream.
    std::string stderr_text;
    int exit_code{-1};
    bool signaled{false};
    int signal{0};
};

// std::system returns a wait status, not an exit code: a runner exiting 1 yields 256.
// Decoding here keeps require_eq(exit_code, 1) meaningful in self tests.
inline auto decode_wait_status(int status)
{
    struct decoded { int exit_code; bool signaled; int signal; };

    if(status < 0)
        return decoded{status, false, 0};
    if((status & 0x7f) != 0 and (status & 0x7f) != 0x7f)
        return decoded{-1, true, status & 0x7f};
    return decoded{(status >> 8) & 0xff, false, 0};
}

// Minimal per-line JSONL access. Not a JSON parser: it locates a line by "type"
// and reads one top-level field verbatim, which is enough to assert on a specific
// event's fields instead of substring-matching the whole stream.
inline auto find_event(std::string_view text, std::string_view type, std::size_t skip = 0)
{
    const auto needle = "\"type\":\"" + std::string{type} + "\"";
    for(auto pos = std::size_t{0}; pos < text.size();)
    {
        const auto eol = text.find('\n', pos);
        const auto line = text.substr(pos, eol == std::string_view::npos ? eol : eol - pos);
        if(line.contains(needle))
        {
            if(skip == 0)
                return std::string{line};
            --skip;
        }
        if(eol == std::string_view::npos)
            break;
        pos = eol + 1;
    }
    return std::string{};
}

// Returns the raw JSON value for key: quoted for strings ("check_eq" keeps its
// quotes), bare for numbers, booleans, arrays, and objects.
inline auto field(std::string_view line, std::string_view key)
{
    const auto needle = "\"" + std::string{key} + "\":";
    const auto at = line.find(needle);
    if(at == std::string_view::npos)
        return std::string{};

    auto pos = at + needle.size();
    if(pos >= line.size())
        return std::string{};

    if(line[pos] == '"')
    {
        auto end = pos + 1;
        while(end < line.size() and line[end] != '"')
            end += line[end] == '\\' ? 2 : 1;
        return std::string{line.substr(pos, std::min(end + 1, line.size()) - pos)};
    }

    // Bare value: run to the delimiter that closes it at nesting depth zero.
    auto depth = 0;
    auto end = pos;
    for(; end < line.size(); ++end)
    {
        const auto ch = line[end];
        if(ch == '[' or ch == '{')
            ++depth;
        else if(ch == ']' or ch == '}')
        {
            if(depth == 0)
                break;
            --depth;
        }
        else if(ch == ',' and depth == 0)
            break;
    }
    return std::string{line.substr(pos, end - pos)};
}

// Convenience: field() on the first event of the given type.
inline auto event_field(std::string_view text, std::string_view type, std::string_view key)
{
    return field(find_event(text, type), key);
}

inline auto read_file_text(const std::filesystem::path& path)
{
    auto file = std::ifstream{path};
    if(not file)
        return std::string{};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

inline auto run_test_runner(const std::vector<std::string>& args, std::string_view extra_env = {})
{
    const auto stem = std::filesystem::temp_directory_path()
        / ("tester_selftest_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto out_path = std::filesystem::path{stem.string() + ".out"};
    const auto err_path = std::filesystem::path{stem.string() + ".err"};

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
    cmd += " 2> ";
    cmd += shell_quote(err_path.string());

    const auto status = std::system(cmd.c_str());
    auto output = read_file_text(out_path);
    auto errors = read_file_text(err_path);
    auto ec = std::error_code{};
    std::filesystem::remove(out_path, ec);
    std::filesystem::remove(err_path, ec);

    const auto decoded = decode_wait_status(status);
    return spawn_result{
        .stdout_text = std::move(output),
        .stderr_text = std::move(errors),
        .exit_code = decoded.exit_code,
        .signaled = decoded.signaled,
        .signal = decoded.signal};
}

} // namespace tester_selftest
