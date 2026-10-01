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

// src/util/cookie.cpp
#include "luogu-extract/util/cookie.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include "luogu-extract/util/compat.h"

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

namespace
{
std::string trim(const std::string &s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string to_lower(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 去掉 domain 的前导 '.'（Netscape 格式用前导点表示包含子域）
std::string normalize_domain(const std::string &domain)
{
    std::string d = to_lower(trim(domain));
    while (!d.empty() && d.front() == '.')
        d.erase(d.begin());
    return d;
}

long long now_seconds()
{
    return static_cast<long long>(std::time(nullptr));
}

// 按 '\t' 拆分（Netscape 格式固定 7 列，字段内不含制表符）
std::vector<std::string> split_tabs(const std::string &line)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : line)
    {
        if (c == '\t')
        {
            out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// 权限建议：POSIX 下 cookies.txt 若对同组/其他用户可读则提示（不修改用户文件）
void check_file_permissions(const std::filesystem::path &path, std::string *warnings)
{
    if (!warnings)
        return;
#ifdef _WIN32
    (void)path;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return;
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        *warnings += "提示：'" + luogu::compat::path_to_utf8(path) +
                     "' 对同组或其他用户可读，建议执行 chmod 600 保护账号凭据。\n";
#endif
}
} // namespace

bool luogu::cookie::load_netscape_file(const std::filesystem::path &path, Jar &out,
                                       std::string &error, std::string *warnings)
{
    out.cookies.clear();
    error.clear();

    FILE *file = luogu::compat::fopen(path, "rb");
    if (!file)
    {
        error = "无法打开 Cookie 文件 '" + luogu::compat::path_to_utf8(path) +
                "'；请检查路径与读取权限";
        return false;
    }

    check_file_permissions(path, warnings);

    const long long now = now_seconds();
    int expired = 0;
    std::string line;
    while (luogu::compat::read_line(file, line) >= 0)
    {
        if (line.empty())
            continue;
        // 注释行；"#HttpOnly_" 前缀是 curl/浏览器标记 HttpOnly 的写法，
        // 去掉前缀后按普通数据行解析
        bool http_only = false;
        if (line[0] == '#')
        {
            static const std::string kHttpOnly = "#HttpOnly_";
            if (line.compare(0, kHttpOnly.size(), kHttpOnly) != 0)
                continue;
            line = line.substr(kHttpOnly.size());
            http_only = true;
        }

        const std::vector<std::string> f = split_tabs(line);
        if (f.size() < 7)
            continue;

        Cookie c;
        // 域字段带前导 '.' 表示包含子域（Netscape 的 includeSubdomains）
        const std::string raw_domain = trim(f[0]);
        c.domain = normalize_domain(raw_domain);
        c.path = f[2].empty() ? "/" : trim(f[2]);
        c.secure = to_lower(trim(f[3])) == "true" ||
                   to_lower(trim(f[1])) == "true";
        c.http_only = http_only;
        c.include_subdomains = !raw_domain.empty() && raw_domain[0] == '.';
        c.name = trim(f[5]);
        c.value = trim(f[6]);
        try
        {
            c.expires = std::stoll(trim(f[4]));
        }
        catch (const std::exception &)
        {
            c.expires = 0;
        }

        if (c.domain.empty() || c.name.empty())
            continue;
        if (c.expires > 0 && c.expires < now)
        {
            ++expired;
            continue;
        }
        out.cookies.push_back(std::move(c));
    }
    std::fclose(file);

    if (out.cookies.empty())
    {
        error = "Cookie 文件 '" + luogu::compat::path_to_utf8(path) +
                "' 中没有可用的 Cookie（可能已过期或格式不是 Netscape 格式）";
        return false;
    }
    if (expired > 0 && warnings)
        *warnings += "提示：Cookie 文件中有 " + std::to_string(expired) +
                     " 条已过期的 Cookie 被忽略。\n";
    return true;
}

bool luogu::cookie::load_cookie_string(const std::string &text, Jar &out,
                                       std::string &error)
{
    out.cookies.clear();
    error.clear();
    if (trim(text).empty())
    {
        error = "参数 '--cookie-string' 的值为空；正确用法：--cookie-string \"k=v; k2=v2\"";
        return false;
    }

    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t next = text.find(';', pos);
        if (next == std::string::npos)
            next = text.size();
        const std::string item = trim(text.substr(pos, next - pos));
        pos = next + 1;
        if (item.empty())
            continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            error = "参数 '--cookie-string' 的格式不正确；正确用法："
                    "--cookie-string \"k=v; k2=v2\"（各 Cookie 之间用分号分隔）";
            return false;
        }
        Cookie c;
        c.domain = "luogu.com.cn";
        c.path = "/";
        c.secure = true;
        c.name = trim(item.substr(0, eq));
        c.value = trim(item.substr(eq + 1));
        if (c.name.empty())
        {
            error = "参数 '--cookie-string' 中存在空的 Cookie 名；正确用法："
                    "--cookie-string \"k=v; k2=v2\"";
            return false;
        }
        out.cookies.push_back(std::move(c));
    }

    if (out.cookies.empty())
    {
        error = "参数 '--cookie-string' 中没有解析出任何 Cookie；正确用法："
                "--cookie-string \"k=v; k2=v2\"";
        return false;
    }
    return true;
}

bool luogu::cookie::host_allowed(const std::string &host, const std::string &extra_host)
{
    if (host.empty())
        return false;
    const std::string h = to_lower(host);
    const std::string extra = to_lower(extra_host);
    if (!extra.empty() && h == extra)
        return true;

    // 域名白名单：luogu.com.cn 与 luogu.org（含各级子域）
    static const char *kAllowed[] = {"luogu.com.cn", "luogu.org"};
    for (const char *base : kAllowed)
    {
        const std::string b = base;
        if (h == b)
            return true;
        if (h.size() > b.size() + 1 &&
            h.compare(h.size() - b.size() - 1, b.size() + 1, "." + b) == 0)
            return true;
    }
    return false;
}

std::string luogu::cookie::host_of_url(const std::string &url)
{
    size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos)
        end = url.size();
    std::string host = url.substr(start, end - start);
    // 去掉 userinfo（user:pass@host）
    const size_t at = host.find('@');
    if (at != std::string::npos)
        host = host.substr(at + 1);
    // 去掉端口（IPv6 字面量的方括号形式对本项目不适用）
    const size_t colon = host.find(':');
    if (colon != std::string::npos)
        host = host.substr(0, colon);
    return to_lower(host);
}

std::string luogu::cookie::netscape_line(const Cookie &c)
{
    // 交给 libcurl 的 Netscape 行格式：
    // domain \t includeSubdomains \t path \t secure \t expires \t name \t value
    // 会话 Cookie 的 expires 用 0 表示。
    std::string line = c.domain;
    line += c.include_subdomains ? "\tTRUE\t" : "\tFALSE\t";
    line += c.path.empty() ? "/" : c.path;
    line += c.secure ? "\tTRUE\t" : "\tFALSE\t";
    line += std::to_string(c.expires);
    line += "\t" + c.name + "\t" + c.value;
    return line;
}
