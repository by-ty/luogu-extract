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

// include/luogu-extract/util/cookie.h
// cookies.txt（Netscape 格式）与 --cookie-string 的解析及域名白名单：Cookie 只发洛谷域名
// （走 libcurl Cookie 引擎按域匹配，重定向到第三方与保存站请求都不会带上）；值不打印、不写日志与缓存。
#ifndef LUOGU_EXTRACT_UTIL_COOKIE_H
#define LUOGU_EXTRACT_UTIL_COOKIE_H

#include <filesystem>
#include <string>
#include <vector>

namespace luogu
{
namespace cookie
{
    struct Cookie
    {
        std::string domain;      // 小写；前导 '.' 表示含子域
        std::string path = "/";
        bool secure = false;
        bool http_only = false;
        bool include_subdomains = true; // 域字段带前导 '.'（Netscape includeSubdomains）
        long long expires = 0;   // Unix 时间戳；0 = 会话 Cookie
        std::string name;
        std::string value;
    };

    struct Jar
    {
        std::vector<Cookie> cookies;
        bool empty() const { return cookies.empty(); }
        size_t size() const { return cookies.size(); }
    };

    // 读 Netscape 格式 cookies.txt：支持 "#HttpOnly_" 前缀，忽略注释/空行与过期项；
    // 至少 1 条有效 Cookie 才返回 true。warnings 可选，收非致命提示
    bool load_netscape_file(const std::filesystem::path &path, Jar &out,
                            std::string &error, std::string *warnings = nullptr);

    // 解析 --cookie-string 的 "k=v; k2=v2"（无域信息，视为洛谷主域 Cookie）
    bool load_cookie_string(const std::string &text, Jar &out, std::string &error);

    // host 是否在白名单：luogu.com.cn / luogu.org 及子域，或 extra_host（本地测试用）
    bool host_allowed(const std::string &host, const std::string &extra_host);

    // 取 URL 的小写 host（去掉端口）；解析失败返回空串
    std::string host_of_url(const std::string &url);

    // 生成 CURLOPT_COOKIELIST 的 Netscape 行：必须走 Cookie 引擎（按域匹配），用 CURLOPT_COOKIE 会在重定向时泄露
    std::string netscape_line(const Cookie &c);
} // namespace cookie
} // namespace luogu

#endif // LUOGU_EXTRACT_UTIL_COOKIE_H
