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
#include <cstdlib>
#include <mutex>
#include <cwchar>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#include <process.h>
#include <io.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
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
    // UTF-8 转宽字符；转换失败（含非法字节序列）返回空串
    std::wstring utf8_to_wide(const std::string &utf8)
    {
        if (utf8.empty())
            return L"";
        const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()),
                                             nullptr, 0);
        if (need <= 0)
            return std::wstring();
        std::wstring out(static_cast<size_t>(need), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                            static_cast<int>(utf8.size()), out.data(), need);
        return out;
    }

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

    // 按 CommandLineToArgvW 规则加引号，保证子进程还原一致：反斜杠仅在紧邻引号或末尾时翻倍
    std::wstring quote_windows_argument(const std::wstring &arg)
    {
        std::wstring out = L"\"";
        size_t i = 0;
        while (i < arg.size())
        {
            size_t backslashes = 0;
            while (i < arg.size() && arg[i] == L'\\')
            {
                ++backslashes;
                ++i;
            }
            if (i == arg.size())
            {
                out.append(backslashes * 2, L'\\');
                break;
            }
            const wchar_t c = arg[i++];
            if (c == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\');
                out += L'"';
            }
            else
            {
                out.append(backslashes, L'\\');
                out += c;
            }
        }
        out += L'"';
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
    // 取宽字符原始命令行：main(char**) 的 argv 已被 ANSI 代码页转换，UTF-8 字节会被破坏
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv)
    {
        // 极少见的失败：退回逐参数转换（可能乱码）
        for (int i = 0; i < argc; ++i)
        {
            std::string utf8;
            const int need = MultiByteToWideChar(CP_ACP, 0, argv[i], -1, nullptr, 0);
            if (need > 0)
            {
                std::wstring wide(static_cast<size_t>(need), L'\0');
                MultiByteToWideChar(CP_ACP, 0, argv[i], -1, wide.data(), need);
                // need 含结尾 L'\0'：只转到它之前，否则结果末尾多一个内嵌 '\0'
                utf8 = wide_to_utf8(wide.c_str(), std::wcslen(wide.c_str()));
            }
            out.push_back(utf8);
        }
        return out;
    }
    for (int i = 0; i < wargc; ++i)
        out.push_back(wide_to_utf8(wargv[i], std::wcslen(wargv[i])));
    LocalFree(wargv);
    // GetCommandLineW 无法区分空串参数、引号规则也与 CRT 略有差异：参数个数不一致时退回 argv
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

int system_utf8(const std::string &command)
{
#ifdef _WIN32
    const std::wstring wide = utf8_to_wide(command);
    if (wide.empty())
        return -1;
    return ::_wsystem(wide.c_str());
#else
    const int status = std::system(command.c_str());
    if (status == -1)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return status;
#endif
}

int run_command_utf8(const std::vector<std::string> &argv, std::string &error)
{
    error.clear();
    if (argv.empty() || argv[0].empty())
    {
        error = "命令为空";
        return -1;
    }
#ifdef _WIN32
    // 自行拼命令行调用 CreateProcessW（不经 cmd.exe）：参数按 CommandLineToArgvW 规则加
    // 引号，%VAR% / & / | / ` 不被解释；但它不按 PATHEXT 解析 .bat/.cmd，目标须是可执行文件
    std::wstring command_line;
    for (const auto &arg : argv)
    {
        if (!command_line.empty())
            command_line += L' ';
        command_line += quote_windows_argument(utf8_to_wide(arg));
    }
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // bInheritHandles = TRUE：子进程继承 stdin/stdout，编译输出直接显示在控制台
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &startup, &process))
    {
        error = "系统错误码 " +
                std::to_string(static_cast<unsigned long>(GetLastError()));
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    const bool got_code = GetExitCodeProcess(process.hProcess, &code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!got_code)
    {
        error = "无法获取子进程退出码";
        return -1;
    }
    return static_cast<int>(code);
#else
    std::vector<char *> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto &arg : argv)
        cargv.push_back(const_cast<char *>(arg.c_str()));
    cargv.push_back(nullptr);

    // exec 失败时把 errno 写回管道（写端 CLOEXEC，exec 成功后自动关闭），父进程据此报出原因
    int pipefd[2];
    if (::pipe(pipefd) != 0)
    {
        error = std::strerror(errno);
        return -1;
    }
    // 设置失败必须直接返回：否则 exec 成功后写端仍打开，父进程会永久阻塞
    if (::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) != 0)
    {
        error = std::strerror(errno);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return -1;
    }

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        error = std::strerror(errno);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return -1;
    }
    if (pid == 0)
    {
        // 子进程：execvp 不经 shell，参数不会被 shell 解释
        ::close(pipefd[0]);
        ::execvp(cargv[0], cargv.data());
        const int exec_errno = errno;
        // 只做异步信号安全的调用（write + _exit，后者不刷新 stdio 缓冲）；write 可能
        // 被信号打断而部分写入，必须重试写完，否则父进程会当成没有 exec 错误
        const char *p = reinterpret_cast<const char *>(&exec_errno);
        size_t left = sizeof(exec_errno);
        while (left > 0)
        {
            const ssize_t n = ::write(pipefd[1], p, left);
            if (n > 0)
            {
                p += n;
                left -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        ::_exit(127);
    }
    ::close(pipefd[1]);

    int exec_errno = 0;
    ssize_t got = 0;
    do
    {
        got = ::read(pipefd[0], &exec_errno, sizeof(exec_errno));
    } while (got < 0 && errno == EINTR);
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0)
    {
        if (errno == EINTR)
            continue;
        error = std::strerror(errno);
        return -1;
    }
    if (got == static_cast<ssize_t>(sizeof(exec_errno)))
    {
        error = std::strerror(exec_errno);
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return status;
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
    // 文件结束：末行无换行符时仍返回其内容；完全没有内容则返回 -1
    return out.empty() ? -1 : static_cast<long long>(out.size());
}

std::string strip_control_chars(std::string s)
{
    size_t w = 0;
    for (size_t r = 0; r < s.size(); ++r)
    {
        const unsigned char c = static_cast<unsigned char>(s[r]);
        // 0x7F（DEL）也按控制字符删除：它不是 UTF-8 多字节序列的一部分（续字节为
        // 0x80~0xBF），删掉不破坏编码，但留下会让 LaTeX 报 invalid character
        if ((c >= 0x20 && c != 0x7F) || c == '\t' || c == '\n' || c == '\r')
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
    // MoveFileExW 默认不覆盖已存在的目标，必须显式给 REPLACE_EXISTING 才与 POSIX
    // rename() 一致（MinGW-w64 的 std::filesystem::rename 走 _wrename，会直接失败）
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

bool stderr_is_tty()
{
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
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
    // POSIX 终端默认规范模式（行缓冲），单键要等回车，无法「按 S 立即停止」：轮询期间临时
    // 切到 cbreak（关 ICANON/ECHO、保留 ISIG 让 Ctrl+C 仍中断），由析构函数还原
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
    // 非阻塞取一个按键（只认 ASCII；无输入或 stdin 非 TTY 时返回 false）。
    // POSIX 一次 read 可能拿到多个字节，多余字节留在内部缓冲逐字节返回，避免丢预输入
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
            // 方向键/功能键先给 0 或 224 再给扫描码：两个字节都丢掉
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
            // 多字节 UTF-8 输入只取首字节（按键只认 ASCII）
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
        CbreakGuard cbreak;
#endif
        sleep_ms(chunk);
        remaining -= chunk;
        char key = 0;
        // 一次可能积累多个按键，逐个消费，任一命中即中断；上限 32 个/轮，避免输入
        // 被灌入（如 `yes |`）时消费循环一直有数据可读而永不返回
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
    // 启用虚拟终端处理让传统 Windows 控制台渲染 ANSI 转义序列（stdout/stderr 各设一次）
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
    // 代码页设为 UTF-8：程序内所有输出（含中文）都是 UTF-8 字节
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    (void)0;
#endif
}

} // namespace compat
} // namespace luogu
