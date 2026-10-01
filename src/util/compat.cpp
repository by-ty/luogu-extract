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

// src/util/compat.cpp
#include "luogu-extract/util/compat.h"

#include <cstring>
#include <cerrno>
#include <mutex>
#include <cwchar>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#include <io.h>
#include <shellapi.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace luogu
{
namespace compat
{

FILE *fopen(const std::filesystem::path &path, const char *mode)
{
#ifdef _WIN32
    std::wstring wmode;
    for (const char *p = mode; *p; ++p)
        wmode += static_cast<wchar_t>(static_cast<unsigned char>(*p));
    return _wfopen(path.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

gzFile gzopen(const std::filesystem::path &path, const char *mode)
{
#ifdef _WIN32
    std::wstring wmode;
    for (const char *p = mode; *p; ++p)
        wmode += static_cast<wchar_t>(static_cast<unsigned char>(*p));
    return gzopen_w(path.c_str(), wmode.c_str());
#else
    return ::gzopen(path.c_str(), mode);
#endif
}

#ifdef _WIN32
namespace
{
    // Windows: 把宽字符串转成 UTF-8
    std::string wide_to_utf8(const wchar_t *wide, size_t len)
    {
        if (len == 0)
            return "";
        const int need = WideCharToMultiByte(CP_UTF8, 0, wide,
                                              static_cast<int>(len),
                                              nullptr, 0, nullptr, nullptr);
        if (need <= 0)
            return "";
        std::string out(static_cast<size_t>(need), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len),
                            out.data(), need, nullptr, nullptr);
        return out;
    }
} // namespace
#endif

std::vector<std::string> get_argv_utf8(int argc, char **argv)
{
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(argc));
#ifdef _WIN32
    (void)argv;
    // 用 GetCommandLineW 拿到原始宽字符命令行再按 Windows 规则拆分，
    // 避免 main(char**) 参数被 ANSI 代码页转换破坏 UTF-8 字节
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv)
    {
        // 极少数失败场景：退回逐参数转换（可能乱码，但不会崩溃）
        for (int i = 0; i < argc; ++i)
        {
            std::string utf8;
            const int need = MultiByteToWideChar(CP_ACP, 0, argv[i], -1, nullptr, 0);
            if (need > 0)
            {
                std::wstring wide(static_cast<size_t>(need), L'\0');
                MultiByteToWideChar(CP_ACP, 0, argv[i], -1, wide.data(), need);
                utf8 = wide_to_utf8(wide.c_str(), wide.size());
            }
            out.push_back(utf8);
        }
        return out;
    }
    for (int i = 0; i < wargc; ++i)
        out.push_back(wide_to_utf8(wargv[i], std::wcslen(wargv[i])));
    LocalFree(wargv);
    // GetCommandLineW 无法区分空字符串参数，且引号规则与 CRT 略有差异；
    // 但解析出的参数数量/内容与 argc/argv 不一致时退回 CRT 的 argv
    if (static_cast<int>(out.size()) != argc)
    {
        out.clear();
        for (int i = 0; i < argc; ++i)
            out.push_back(argv[i]);
    }
#else
    for (int i = 0; i < argc; ++i)
        out.push_back(argv[i] ? argv[i] : "");
#endif
    return out;
}

std::string getenv_utf8(const char *name)
{
#ifdef _WIN32
    std::wstring wname;
    for (const char *p = name; *p; ++p)
        wname += static_cast<wchar_t>(static_cast<unsigned char>(*p));

    const DWORD need = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (need == 0)
        return "";
    std::wstring buffer(need, L'\0');
    const DWORD got = GetEnvironmentVariableW(wname.c_str(), buffer.data(), need);
    if (got == 0 || got > need)
        return "";
    buffer.resize(got);

    const int len = WideCharToMultiByte(CP_UTF8, 0, buffer.data(),
                                        static_cast<int>(buffer.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return "";
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer.data(),
                        static_cast<int>(buffer.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
#else
    const char *value = std::getenv(name);
    return value ? value : "";
#endif
}

long long read_line(FILE *in, std::string &out)
{
    out.clear();
    char buffer[65536];
    while (std::fgets(buffer, sizeof(buffer), in))
    {
        const size_t n = std::strlen(buffer);
        out.append(buffer, n);
        if (n > 0 && buffer[n - 1] == '\n')
        {
            out.pop_back();
            return static_cast<long long>(out.size());
        }
    }
    // 文件结束：最后一行为内容但无换行符时，返回该行
    return out.empty() ? -1 : static_cast<long long>(out.size());
}

std::string strip_control_chars(std::string s)
{
    size_t w = 0;
    for (size_t r = 0; r < s.size(); ++r)
    {
        const unsigned char c = static_cast<unsigned char>(s[r]);
        if (c >= 0x20 || c == '\t' || c == '\n' || c == '\r')
            s[w++] = s[r];
    }
    s.resize(w);
    return s;
}

bool flush_and_sync(FILE *file)
{
    if (!file)
        return false;
    if (std::fflush(file) != 0)
        return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    const int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0;
#endif
}

bool file_exists(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

bool atomic_replace(const std::filesystem::path &from,
                    const std::filesystem::path &to, std::string &error)
{
    error.clear();
#ifdef _WIN32
    // MoveFileExW 默认不覆盖已存在的目标：必须显式给出 REPLACE_EXISTING，
    // 才能与 POSIX rename() 的语义一致（MinGW-w64 的 std::filesystem::rename
    // 走 _wrename，目标存在时直接失败）
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    const DWORD code = GetLastError();
    error = "系统错误码 " + std::to_string(static_cast<unsigned long>(code));
    return false;
#else
    // POSIX rename() 在同一文件系统内是原子的，且目标存在时覆盖
    if (std::rename(from.c_str(), to.c_str()) == 0)
        return true;
    error = std::strerror(errno);
    return false;
#endif
}

bool stdin_is_tty()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

bool stdout_is_tty()
{
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

void sleep_ms(long ms)
{
    if (ms <= 0)
        return;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#ifndef _WIN32
namespace
{
    // POSIX 终端默认是规范模式（行缓冲）：单个按键要等到回车才会交给进程，
    // 「按 S 立即停止」就无从谈起。这里在每次按键轮询期间临时切到 cbreak
    // 模式（关掉 ICANON 与 ECHO，保留 ISIG 让 Ctrl+C 仍然是中断信号），
    // 离开作用域立刻还原。作用域只有一次 200 毫秒的轮询，异常路径由
    // 析构函数兜底；不注册信号处理器。
    class CbreakGuard
    {
    public:
        CbreakGuard()
        {
            if (!isatty(STDIN_FILENO))
                return;
            if (tcgetattr(STDIN_FILENO, &saved_) != 0)
                return;
            termios raw = saved_;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
                return;
            active_ = true;
        }
        ~CbreakGuard()
        {
            if (active_)
                tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        }
        CbreakGuard(const CbreakGuard &) = delete;
        CbreakGuard &operator=(const CbreakGuard &) = delete;

    private:
        bool active_ = false;
        termios saved_{};
    };
} // namespace
#endif

namespace
{
    // 非阻塞读取一个按键（只识别 ASCII 单字符）。
    // 没有待处理输入时返回 false。stdin 非 TTY 时始终返回 false。
    //
    // POSIX 下一次 read 可能一次拿到多个字节（用户连按或提前输入），
    // 只取首字节会把其余字节丢掉（可能拆散一行预输入的文本），
    // 因此把多读到的字节留在内部缓冲里，逐字节返回。
    bool read_key_async(char &out)
    {
        // auto 模式下有两条通道并行等待，可能同时轮询按键：加锁串行化
        static std::mutex key_mutex;
        std::lock_guard<std::mutex> key_lock(key_mutex);
#ifdef _WIN32
        if (!_kbhit())
            return false;
        const int ch = _getch();
        if (ch == 0 || ch == 224)
        {
            // 功能键/方向键会先给出 0 或 224，再给出扫描码：两个字节都丢掉
            if (_kbhit())
                _getch();
            return false;
        }
        out = static_cast<char>(ch & 0x7F);
        return true;
#else
        static unsigned char pending[256];
        static size_t pending_len = 0;
        static size_t pending_pos = 0;

        if (pending_pos < pending_len)
        {
            // 多字节 UTF-8 输入：只取首字节（不解析中文，确认只认 ASCII 单字符）
            out = static_cast<char>(pending[pending_pos++] & 0x7F);
            return true;
        }

        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int ready = ::poll(&pfd, 1, 0);
        if (ready <= 0 || !(pfd.revents & (POLLIN | POLLHUP)))
            return false;
        const ssize_t n = ::read(STDIN_FILENO, pending, sizeof(pending));
        if (n <= 0)
            return false;
        pending_len = static_cast<size_t>(n);
        pending_pos = 1;
        out = static_cast<char>(pending[0] & 0x7F);
        return true;
#endif
    }
} // namespace

bool sleep_interruptible_ms(long ms, const std::function<bool(char)> &on_key)
{
    if (ms <= 0)
        return false;
    if (!on_key || !stdin_is_tty())
    {
        sleep_ms(ms);
        return false;
    }

    const long kStepMs = 200;
    long remaining = ms;
    while (remaining > 0)
    {
        const long chunk = remaining < kStepMs ? remaining : kStepMs;
#ifndef _WIN32
        // 轮询期间临时切到 cbreak，让单键（S / C）无需回车即可被读到
        CbreakGuard cbreak;
#endif
        sleep_ms(chunk);
        remaining -= chunk;
        char key = 0;
        // 一次可能积累多个按键：逐个消费，任一命中即中断。
        // 上限 32 个/轮：输入被重定向或被人为灌入时（如 `yes |`），
        // 无上限的消费循环会一直有数据可读而永不返回
        int consumed = 0;
        while (consumed < 32 && read_key_async(key))
        {
            ++consumed;
            if (on_key(key))
                return true;
        }
    }
    return false;
}

void init_console()
{
#ifdef _WIN32
    // 启用虚拟终端处理：让传统 Windows 控制台正确渲染 ANSI 转义序列。
    // stdout 与 stderr 分别处理（彩色错误信息走 stderr）。
    for (const DWORD id : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE})
    {
        const HANDLE handle = GetStdHandle(id);
        if (!handle || handle == INVALID_HANDLE_VALUE)
            continue;
        DWORD mode = 0;
        if (!GetConsoleMode(handle, &mode))
            continue; // 输出被重定向（非控制台）时保持原样
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(handle, mode);
    }
    // 控制台代码页设为 UTF-8：程序内所有输出（含中文）都是 UTF-8 字节
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    (void)0;
#endif
}

} // namespace compat
} // namespace luogu
