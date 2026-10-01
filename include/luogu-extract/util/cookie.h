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
// 凭据通道：解析 Netscape 格式 cookies.txt（与 --cookie-string），
// 并保证 Cookie 只会附加到洛谷自己的域名上（域名白名单硬校验）。
//
// 安全约定：
// - Cookie 值不打印、不写日志、不写缓存；错误信息只输出文件名；
// - 保存站（第三方镜像）请求一律不携带 Cookie；
// - 交给 libcurl 的统一走 Cookie 引擎（按域匹配），因此重定向到其它
//   域名时不会被带上，不存在跨域泄露。
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
        std::string domain;      // 小写；可能带前导 '.'（表示含子域）
        std::string path = "/";
        bool secure = false;
        bool http_only = false;
        bool include_subdomains = true; // 域字段带前导 '.'（Netscape 的 includeSubdomains）
        long long expires = 0;   // Unix 时间戳；0 表示会话 Cookie
        std::string name;
        std::string value;
    };

    struct Jar
    {
        std::vector<Cookie> cookies;
        bool empty() const { return cookies.empty(); }
        size_t size() const { return cookies.size(); }
    };

    // 读取 Netscape 格式的 cookies.txt（curl/wget/浏览器导出均可）。
    // 支持 "#HttpOnly_" 前缀；忽略注释行、空行与过期 Cookie。
    // @param warnings 可选：非致命提示（如过期条目数、权限建议）
    // @return 解析成功（至少 1 条有效 Cookie）返回 true
    bool load_netscape_file(const std::filesystem::path &path, Jar &out,
                            std::string &error, std::string *warnings = nullptr);

    // 解析 --cookie-string 的 "k=v; k2=v2" 形式（不带域信息，
    // 一律视为洛谷主域 Cookie）
    bool load_cookie_string(const std::string &text, Jar &out, std::string &error);

    // host 是否在 Cookie 白名单内：luogu.com.cn / luogu.org 及其子域，
    // 或 extra_host（端点被环境变量覆盖用于本地测试时传入）
    bool host_allowed(const std::string &host, const std::string &extra_host);

    // 从 URL 中取出小写 host（含端口时去掉端口）；解析失败返回空串
    std::string host_of_url(const std::string &url);

    // 生成 libcurl CURLOPT_COOKIELIST 所需的 Netscape 行。
    // 交给 Cookie 引擎而不是 CURLOPT_COOKIE：引擎按域匹配，
    // 重定向到第三方域名时不会带上凭据。
    std::string netscape_line(const Cookie &c);
} // namespace cookie
} // namespace luogu

#endif // LUOGU_EXTRACT_UTIL_COOKIE_H
