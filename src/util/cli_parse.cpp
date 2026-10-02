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

// src/util/cli_parse.cpp
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/export/common.h"
#include "luogu-extract/util/cli_parse.h"
#include "luogu-extract/util/compat.h"

std::string cliparse::to_lower_ascii(const std::string &s)
{
    std::string out = s;
    for (auto &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string cliparse::to_upper_ascii(const std::string &s)
{
    std::string out = s;
    for (auto &c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> cliparse::split_whitespace(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        const size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool cliparse::parse_positive_int(const std::string &value, long min_value,
                                  long max_value, long &out)
{
    // 先显式去掉两端空白再交给 strtol：strtol 只跳前导空白，而交互模式直接读入
    // 整行、尾随空格很常见，需保证 "5 "、" 5 " 与 "5" 等价
    size_t first = 0;
    size_t last = value.size();
    while (first < last && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    const std::string text = value.substr(first, last - first);
    if (text.empty())
        return false;
    char *end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || (end && *end != '\0'))
        return false;
    if (parsed < min_value || parsed > max_value)
        return false;
    out = parsed;
    return true;
}

bool cliparse::parse_difficulty_spec(const std::string &spec,
                                     std::vector<int> &difficulties)
{
    auto parse_num = [](const std::string &s, long &out) -> bool {
        if (s.empty())
            return false;
        char *end = nullptr;
        out = std::strtol(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || out < 0 || out > 8)
            return false;
        return true;
    };

    const size_t dash = spec.find('-');
    if (dash == std::string::npos)
    {
        long v = 0;
        if (!parse_num(spec, v))
            return false;
        difficulties.push_back(static_cast<int>(v));
        return true;
    }

    // 区间 A-B；再出现 '-'（如 "1-2-3"）视为非法
    const std::string a = spec.substr(0, dash);
    const std::string b = spec.substr(dash + 1);
    if (b.find('-') != std::string::npos)
        return false;

    long lo = 0, hi = 0;
    if (!parse_num(a, lo) || !parse_num(b, hi))
        return false;
    if (lo > hi)
        return false;
    for (long v = lo; v <= hi; ++v)
        difficulties.push_back(static_cast<int>(v));
    return true;
}

bool cliparse::parse_pid_range_arg(const std::string &spec,
                                   std::pair<std::string, std::string> &out,
                                   std::string &err)
{
    const size_t dash = spec.find('-');
    if (dash == std::string::npos)
    {
        err = "参数错误：'--pid-range' 的值 '" + spec +
              "' 缺少 '-'；正确用法：--pid-range <题号>-<题号>"
              "（如 P1001-P1010，两端点均包含）";
        return false;
    }
    const std::string a = spec.substr(0, dash);
    const std::string b = spec.substr(dash + 1);
    if (a.empty() || b.empty() || b.find('-') != std::string::npos)
    {
        err = "参数错误：'--pid-range' 的值 '" + spec +
              "' 不是合法的题号范围；正确用法：--pid-range <题号>-<题号>"
              "（如 P1001-P1010，两端点均包含，且只含一个 '-'）";
        return false;
    }

    std::string a_prefix, a_suffix, b_prefix, b_suffix;
    unsigned long long a_num = 0, b_num = 0;
    if (!luogu::parse_pid_parts(a, a_prefix, a_num, a_suffix))
    {
        err = "参数错误：'--pid-range' 的端点 '" + a +
              "' 不是合法题号（应为 1 个或多个字母 + 数字，如 P1001）";
        return false;
    }
    if (!luogu::parse_pid_parts(b, b_prefix, b_num, b_suffix))
    {
        err = "参数错误：'--pid-range' 的端点 '" + b +
              "' 不是合法题号（应为 1 个或多个字母 + 数字，如 P1010）";
        return false;
    }
    if (a_prefix != b_prefix)
    {
        err = "参数错误：'--pid-range' 的一组范围两端必须为同一题库的题目"
              "（例如 P1001-P1010 或 B2000-B2010）；'" + spec +
              "' 跨越了不同题库（" + a_prefix + " 题库与 " + b_prefix +
              " 题库），不同题库请分成多组范围分别传入";
        return false;
    }
    if (luogu::compare_pid_parts(a_num, a_suffix, b_num, b_suffix) > 0)
    {
        err = "参数错误：'--pid-range' 的范围左端点不能大于右端点（'" + spec +
              "'）；正确用法：--pid-range <题号>-<题号>，两端点均包含且"
              "左端点不超过右端点";
        return false;
    }
    out = {to_upper_ascii(a), to_upper_ascii(b)};
    return true;
}

std::string cliparse::default_local_output(const std::string &input)
{
    std::filesystem::path p = luogu::compat::path_from_utf8(input);
    p.replace_extension(".tex");
    return luogu::compat::path_to_utf8(p);
}

namespace
{
// 按文件头魔数识别字体格式，返回应使用的扩展名（含点号），无法识别返回空串
std::string detect_font_extension(const std::filesystem::path &path)
{
    FILE *f = luogu::compat::fopen(path, "rb");
    if (!f)
        return "";
    unsigned char head[4] = {0};
    const size_t n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    if (n < 4)
        return "";
    // 魔数：TrueType 为 00 01 00 00 / 'true' / 'typ1'，OpenType(CFF) 为 'OTTO'，TTC 为 'ttcf'
    if ((head[0] == 0x00 && head[1] == 0x01 && head[2] == 0x00 && head[3] == 0x00) ||
        std::memcmp(head, "true", 4) == 0 ||
        std::memcmp(head, "typ1", 4) == 0)
        return ".ttf";
    if (std::memcmp(head, "OTTO", 4) == 0)
        return ".otf";
    if (std::memcmp(head, "ttcf", 4) == 0)
        return ".ttc";
    return "";
}
} // namespace

bool cliparse::font_value_is_file_address(const std::string &value)
{
    const bool has_separator = value.find('/') != std::string::npos ||
                               value.find('\\') != std::string::npos;
    const std::string lower = to_lower_ascii(value);
    static const char *kFontExts[] = {".ttf", ".otf", ".ttc", ".dfont",
                                      ".pfb", ".woff", ".woff2"};
    bool has_ext = false;
    for (const char *e : kFontExts)
    {
        const size_t n = std::strlen(e);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0)
        {
            has_ext = true;
            break;
        }
    }

    std::error_code ec;
    const bool file_exists_now =
        std::filesystem::exists(luogu::compat::path_from_utf8(value), ec) && !ec;

    return has_separator || has_ext || file_exists_now;
}

std::string cliparse::validate_font_option(const std::string &option_name,
                                           const std::string &value,
                                           std::string &font_out)
{
    if (value.empty() || value[0] == '-')
        return "参数 '" + option_name + "' 后缺少字体名称或字体文件地址；正确用法：" +
               option_name + " <字体名称或字体文件地址>";

    if (!font_value_is_file_address(value))
    {
        font_out = value;
        return "";
    }

    std::error_code ec;
    std::filesystem::path p =
        std::filesystem::absolute(luogu::compat::path_from_utf8(value), ec);
    if (ec || !std::filesystem::exists(p, ec))
        return "参数 '" + option_name + "' 指定的字体文件 '" + value +
               "' 不存在；请检查文件路径，或改用系统已安装的字体名称";

    // 无扩展名字体 fontspec 无法加载：按文件头识别格式，复制到缓存目录并补上扩展名
    if (!p.has_extension())
    {
        const std::string ext = detect_font_extension(p);
        if (ext.empty())
            return "参数 '" + option_name + "' 指定的字体文件 '" + value +
                   "' 无法识别其字体格式；请使用 .ttf / .otf / .ttc 格式的字体文件，"
                   "或改用系统已安装的字体名称";
        const std::filesystem::path cache_fonts = crawler::get_cache_dir() / "fonts";
        std::error_code ec2;
        if (!std::filesystem::exists(cache_fonts, ec2) &&
            !std::filesystem::create_directories(cache_fonts, ec2))
        {
            return "无法创建字体缓存目录 '" + luogu::compat::path_to_utf8(cache_fonts) + "': " + ec2.message();
        }
        std::filesystem::path target = cache_fonts / p.filename();
        target += ext;
        for (int i = 1; std::filesystem::exists(target, ec2); ++i)
        {
            target = cache_fonts / p.filename();
            target += "_" + std::to_string(i) + ext;
        }
        std::filesystem::copy_file(p, target, std::filesystem::copy_options::none, ec2);
        if (ec2)
            return "无法把字体文件复制到缓存目录: " + ec2.message();
        p = std::filesystem::absolute(target, ec2);
    }

    // 输出 UTF-8 路径（Windows 下 string() 按 ANSI 解释会乱码），并把 '\' 换成 '/'
    // 便于写入 LaTeX
    std::string spec = luogu::compat::path_to_utf8(p);
    std::replace(spec.begin(), spec.end(), '\\', '/');
    font_out = std::move(spec);
    return "";
}
