// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 by-ty
//
// This file is part of luogu-extract
// (https://github.com/by-ty/luogu-extract), a fork of luogu-export
// (https://github.com/sacharei/luogu-export) which is licensed under the
// MIT License (Copyright (c) 2026 sacharei); see the "Original MIT License"
// section in the LICENSE file.
//
// luogu-extract is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation, either version 3 of the License, or (at
// your option) any later version. See the LICENSE file or
// https://www.gnu.org/licenses/lgpl-3.0.html for the full license text.
//
// luogu-extract is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
// License for more details.

// include/luogu-extract/util/compat.h
// 跨平台兼容层：Windows 的 CRT、argv、环境变量、system 均按 ANSI 代码页解释窄字符，这里统一按 UTF-8 处理；
// 另含 POSIX getline 的等价实现与 Windows 控制台 ANSI 初始化。
#ifndef LUOGU_EXTRACT_UTIL_COMPAT_H
#define LUOGU_EXTRACT_UTIL_COMPAT_H

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <zlib.h>

namespace luogu
{
namespace compat
{
    // UTF-8 路径 fopen：Windows 转宽字符走 _wfopen（中文路径可用），其他平台 std::fopen
    FILE *fopen(const std::filesystem::path &path, const char *mode);

    // zlib 的 UTF-8 路径封装；Windows 用 gzopen_w
    gzFile gzopen(const std::filesystem::path &path, const char *mode);

    // UTF-8 -> path：Windows 必须经 u8path，否则按 ANSI 代码页解释字节
    inline std::filesystem::path path_from_utf8(const std::string &utf8)
    {
#ifdef _WIN32
        return std::filesystem::u8path(utf8);
#else
        return std::filesystem::path(utf8);
#endif
    }

    // path -> UTF-8：Windows 用 u8string()（string() 按 ANSI 代码页解释）
    inline std::string path_to_utf8(const std::filesystem::path &path)
    {
#ifdef _WIN32
        return path.u8string();
#else
        return path.string();
#endif
    }

    // argc/argv -> UTF-8 副本：Windows 的 CRT 按 ANSI 转换，改用 GetCommandLineW + CommandLineToArgvW
    std::vector<std::string> get_argv_utf8(int argc, char **argv);

    // 读环境变量（UTF-8）：Windows 用 GetEnvironmentVariableW（getenv 按 ANSI 解释）
    std::string getenv_utf8(const char *name);

    // 经 shell 执行命令（命令串为 UTF-8，Windows 用 _wsystem）。
    // 返回码：0/正数为程序退出码，信号终止为 128+信号，无法执行为 -1。
    // 命令串会被 shell 解释，用户可控参数请改用 run_command_utf8。
    int system_utf8(const std::string &command);

    // 不经 shell 执行命令：POSIX 为 fork + execvp，Windows 为 CreateProcessW（参数自行加引号）；
    // " & | ` $( ) %VAR% 等元字符不会被解释，适合用户可控文件名（如 --compile 的 latexmk）。
    // argv 为空返回 -1；返回码语义同 system_utf8；失败原因（UTF-8）写入 error，成功时清空
    int run_command_utf8(const std::vector<std::string> &argv, std::string &error);

    // 读一行（不含末尾换行），替代 POSIX getline：返回字符数，EOF 且无内容返回 -1
    long long read_line(FILE *in, std::string &out);

    // 过滤 C0 控制字符（保留 \t \n \r）与 DEL：JSON 里的 \u0000 等会静默截断 C 字符串或破坏 LaTeX
    std::string strip_control_chars(std::string s);

    // 与目标同目录的临时文件路径（<目标名>.tmp.<时间戳>.<进程内计数>），用于「临时文件 + rename」原子写
    inline std::filesystem::path temp_sibling_path(const std::filesystem::path &target)
    {
        static std::atomic<unsigned long long> counter{0};
        const auto now = std::chrono::steady_clock::now()
                             .time_since_epoch()
                             .count();
        std::filesystem::path tmp = target;
        tmp += std::string(".tmp.") + std::to_string(now) + "." +
               std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        return tmp;
    }

    // 落盘：POSIX 为 fflush + fsync，Windows 为 fflush + _commit；返回 true 表示成功
    // （平台/文件系统不支持时按成功处理，尽力而为）
    bool flush_and_sync(FILE *file);

    // 路径存在且可写；不存在或无法访问返回 false
    bool file_exists(const std::filesystem::path &path);

    // 原子替换（from -> to，覆盖已存在）：POSIX rename / Windows MoveFileExW(REPLACE_EXISTING)
    // （MinGW-w64 的 rename 会因目标存在而失败）。from 与 to 必须同目录；失败写 error
    bool atomic_replace(const std::filesystem::path &from,
                        const std::filesystem::path &to, std::string &error);

    // ---- 终端交互（题解抓取的风险确认与限流暂停使用）----

    // 标准输入是否交互式终端（POSIX isatty / Windows _isatty）。
    // 无 TTY 时所有确认一律失败闭合（拒绝执行），绝不默认继续
    bool stdin_is_tty();

    // 标准输出是否终端（决定是否输出颜色与转义序列）
    bool stdout_is_tty();

    // 标准错误是否终端；需与 stdout 分开判断（stdout 重定向到文件时错误信息仍应带颜色）
    bool stderr_is_tty();

    // 睡眠指定毫秒（仅用于极短的分片睡眠，返回后不补足剩余时间）
    void sleep_ms(long ms);

    // 等待最多 ms 毫秒，每 200ms 检查一次按键（只识别 ASCII 单字符）：on_key 为 true 时立即返回 true；
    // 无 TTY 或 on_key 为空时退化为纯睡眠并返回 false；不使用线程，也不注册信号处理器
    bool sleep_interruptible_ms(long ms, const std::function<bool(char)> &on_key);

    // Windows：对 stdout/stderr 启用 ENABLE_VIRTUAL_TERMINAL_PROCESSING（否则 ANSI 彩色与光标序列乱码）
    // 并把代码页设为 UTF-8；输出被重定向或启用失败时保持原样，其他平台空操作
    void init_console();
} // namespace compat
} // namespace luogu

#endif // LUOGU_EXTRACT_UTIL_COMPAT_H
