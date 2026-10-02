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
// 跨平台（Windows / macOS / Linux）兼容性工具：
// - Windows 的 CRT fopen/gzopen/getenv 按 ANSI 代码页解释窄字符，
//   这里统一提供按 UTF-8 处理路径的封装；
// - POSIX getline 在 MSVC 上不存在，这里提供等价实现；
// - Windows 传统控制台默认不解析 ANSI 转义序列，这里提供初始化封装。
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
    // 用 UTF-8 路径打开文件。
    // Windows 下把路径转成宽字符后调用 _wfopen（中文路径可用）；
    // 其他平台直接透传 std::fopen。
    FILE *fopen(const std::filesystem::path &path, const char *mode);

    // zlib 的 gzopen 同样存在窄字符路径问题；Windows 下用 gzopen_w。
    gzFile gzopen(const std::filesystem::path &path, const char *mode);

    // 把 UTF-8 字符串转换为 filesystem::path。
    // Windows 下直接按窄字符构造会按 ANSI 代码页解释字节，必须经 u8path
    // （本工程为 C++17，u8path 可用）。
    inline std::filesystem::path path_from_utf8(const std::string &utf8)
    {
#ifdef _WIN32
        return std::filesystem::u8path(utf8);
#else
        return std::filesystem::path(utf8);
#endif
    }

    // 把 filesystem::path 转回 UTF-8 字符串（用于写进 UTF-8 文件
    // 或输出到终端）。Windows 下 path::string() 按当前 ANSI 代码页
    // 解释窄字符，这里统一用 u8string()；其他平台与 string() 等价。
    inline std::string path_to_utf8(const std::filesystem::path &path)
    {
#ifdef _WIN32
        return path.u8string();
#else
        return path.string();
#endif
    }

    // 把 main() 的 argc/argv 转成 UTF-8 字符串数组（返回 argv 的一个副本）。
    // Windows 下 CRT 的 main(char**) 参数按系统 ANSI 代码页转换而非 UTF-8，
    // 这里改用 GetCommandLineW + CommandLineToArgvW 重新解析命令行
    // 并转成 UTF-8（中文参数不乱码）；其他平台直接复制原 argv。
    std::vector<std::string> get_argv_utf8(int argc, char **argv);

    // 读取环境变量（返回 UTF-8）。
    // Windows 下用 GetEnvironmentVariableW 再转 UTF-8（getenv 按 ANSI 解释）；
    // 其他平台直接 std::getenv。
    std::string getenv_utf8(const char *name);

    // 执行外部命令（命令串为 UTF-8）。
    // Windows 下把命令串转成宽字符后调用 _wsystem（system 按 ANSI 代码页解释
    // 命令行，含中文的路径会乱码）；其他平台与 std::system 等价。
    // 返回命令的退出码（已被信号终止时返回 128 + 信号编号；
    // 无法执行命令时返回 -1）。
    int system_utf8(const std::string &command);

    // 从 FILE* 读取一行（结果不含末尾换行符），替代 POSIX getline。
    // 返回读取到的字符数；文件结束且未读到任何内容时返回 -1。
    // 语义与 getline + 去掉末尾 '\n' 一致。
    long long read_line(FILE *in, std::string &out);

    // 过滤字符串中的 C0 控制字符（保留 \t \n \r）与 DEL。
    // 缓存 JSON 中可能含 \u0000 等控制字符，直接 fputs/fprintf("%s")
    // 输出会被 C 字符串终止符静默截断（或破坏 LaTeX 编译），
    // 在解析阶段统一清除这些字节。
    std::string strip_control_chars(std::string s);

    // 生成与目标文件同目录的临时文件路径（文件名 = 目标名 +
    // ".tmp.<时间戳>.<进程内计数>"，后缀为纯 ASCII，各平台均安全），
    // 用于“临时文件 + rename”原子写。
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

    // 把已写缓冲落盘并 fsync（临时文件 + rename 原子写的一部分）。
    // POSIX 用 fflush + fsync(fileno)；Windows 用 fflush + _commit。
    // 返回 true 表示成功（某些平台/文件系统不支持时按成功处理，尽力而为）。
    bool flush_and_sync(FILE *file);

    // 目标文件是否已存在且可写（用于「覆盖前确认」一类的判断）。
    // 路径不存在或无法访问时返回 false。
    bool file_exists(const std::filesystem::path &path);

    // 原子替换：把 from 改名到 to（to 已存在时覆盖）。
    // POSIX 用 rename()；Windows 用 MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)，
    // 保证与 POSIX 一致的覆盖语义（MinGW-w64 的 std::filesystem::rename 会因
    // 目标已存在而失败）。from 必须与 to 在同一目录（同一文件系统）。
    // 失败时返回 false 并把系统错误写入 error。
    bool atomic_replace(const std::filesystem::path &from,
                        const std::filesystem::path &to, std::string &error);

    // ---- 终端交互（题解抓取的风险确认与限流暂停使用）----

    // 标准输入是否连接到交互式终端（POSIX: isatty；Windows: _isatty）。
    // 无 TTY 时所有确认一律失败闭合（拒绝执行），绝不默认继续。
    bool stdin_is_tty();

    // 判断标准输出是否连接到终端（彩色/转义序列是否值得输出）
    bool stdout_is_tty();

    // 睡眠指定毫秒（可被 std::this_thread::sleep_for 之外的信号打断，
    // 返回后剩余时间不再补足）。仅用于极短的分片睡眠。
    void sleep_ms(long ms);

    // 以毫秒为单位等待，期间每 200 毫秒检查一次按键（只识别 ASCII 单字符，
    // 规避 Windows 与 POSIX 终端编码差异）：
    // - on_key 返回 true 时立即结束等待并返回 true（被按键中断）；
    // - 无 TTY（输入被重定向）时不检测按键，睡满后返回 false；
    // - on_key 为空时退化为纯睡眠；
    // - 不使用线程，也不注册信号处理器（Ctrl+C 交给系统默认行为）。
    bool sleep_interruptible_ms(long ms, const std::function<bool(char)> &on_key);

    // 初始化控制台输出。Windows 传统控制台默认不解析 ANSI 转义序列
    // （彩色、\033[K 清行、\033[s/\033[u 光标保存恢复等会乱码），
    // 这里对 stdout/stderr 启用 ENABLE_VIRTUAL_TERMINAL_PROCESSING，
    // 并把控制台代码页设为 UTF-8（中文输出不乱码）；
    // 输出被重定向（非控制台）或启用失败时保持原样。其他平台为空操作。
    void init_console();
} // namespace compat
} // namespace luogu

#endif // LUOGU_EXTRACT_UTIL_COMPAT_H
