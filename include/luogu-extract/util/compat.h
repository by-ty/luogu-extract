// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 by-ty
//
// This file is part of luogu-export-next
// (https://github.com/by-ty/luogu-export-next), a fork of luogu-export
// (https://github.com/sacharei/luogu-export) which is licensed under the
// MIT License (Copyright (c) 2026 sacharei); see the "Original MIT License"
// section in the LICENSE file.
//
// luogu-export-next is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation, either version 3 of the License, or (at
// your option) any later version. See the LICENSE file or
// https://www.gnu.org/licenses/lgpl-3.0.html for the full license text.
//
// luogu-export-next is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
// License for more details.

// include/luogu-export/util/compat.h
// 跨平台（Windows / macOS / Linux）兼容性工具：
// - Windows 的 CRT fopen/gzopen/getenv 按 ANSI 代码页解释窄字符，
//   这里统一提供按 UTF-8 处理路径的封装；
// - POSIX getline 在 MSVC 上不存在，这里提供等价实现；
// - Windows 传统控制台默认不解析 ANSI 转义序列，这里提供初始化封装。
#ifndef LUOGU_EXPORT_UTIL_COMPAT_H
#define LUOGU_EXPORT_UTIL_COMPAT_H

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
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

    // 初始化控制台输出。Windows 传统控制台默认不解析 ANSI 转义序列
    // （彩色、\033[K 清行、\033[s/\033[u 光标保存恢复等会乱码），
    // 这里对 stdout/stderr 启用 ENABLE_VIRTUAL_TERMINAL_PROCESSING，
    // 并把控制台代码页设为 UTF-8（中文输出不乱码）；
    // 输出被重定向（非控制台）或启用失败时保持原样。其他平台为空操作。
    void init_console();
} // namespace compat
} // namespace luogu

#endif // LUOGU_EXPORT_UTIL_COMPAT_H
