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

// src/export/common.cpp
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/export/common.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/problem_info.h"
#include "luogu-extract/util/tag_cache.h"

using nlohmann::json;

namespace
{

std::string to_lower_ascii(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string to_upper_ascii(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string join_strings(const std::vector<std::string> &v, const std::string &sep)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i)
            out += sep;
        out += v[i];
    }
    return out;
}

std::vector<std::string> split_whitespace(const std::string &s)
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

// 数字 ID 按 tags.json 翻译，其余按名称原样；输入是 ID 但缺少 tags.json 时返回 false
bool resolve_tag(const std::string &raw, bool has_tag_map,
                 const tagcache::Cache &cache, std::string &out)
{
    const bool numeric = !raw.empty() &&
        std::all_of(raw.begin(), raw.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c));
        });
    if (!numeric)
    {
        out = tagcache::strip_bom(raw);
        return true;
    }
    if (!has_tag_map)
        return false;

    int id = 0;
    try
    {
        id = std::stoi(raw);
    }
    catch (...)
    {
        out = tagcache::strip_bom(raw);
        return true;
    }

    auto it = cache.id_to_name.find(id);
    if (it != cache.id_to_name.end())
    {
        out = it->second;
        return true;
    }
    // 数字也可能是年份等标签名（如 "1997"）
    out = tagcache::strip_bom(raw);
    return true;
}

// 题号排序用的数字：只取第一段连续数字（P1001→1001、AT_abc123_4→123，不能拼接
// 所有数字段），无数字返回 0；溢出封顶 ULLONG_MAX，畸形缓存下排序仍是全序。
unsigned long long pid_number(const std::string &pid)
{
    const unsigned long long kMax =
        std::numeric_limits<unsigned long long>::max();
    unsigned long long n = 0;
    bool overflow = false;
    size_t i = 0;
    while (i < pid.size() && !std::isdigit(static_cast<unsigned char>(pid[i])))
        ++i;
    for (; i < pid.size() && std::isdigit(static_cast<unsigned char>(pid[i])); ++i)
    {
        if (overflow)
            continue;
        const unsigned long long d =
            static_cast<unsigned long long>(pid[i] - '0');
        if (n > (kMax - d) / 10)
        {
            overflow = true;
            continue;
        }
        n = n * 10 + d;
    }
    return overflow ? kMax : n;
}

// 原始文本快速预筛：只有能严格证明不命中时才跳过（省去构造完整 JSON DOM），
// 任何不确定情况一律放行，交给后续精确筛选，保证筛选结果与原来完全一致。

inline bool is_json_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 行内任一 "key" 的整数值命中 allowed 即返回 true，确定不命中才返回 false；
// 值带小数点/指数（如 1.0、1e0，nlohmann 不当作整数）或无法解析时保守放行。
bool raw_int_value_match(std::string_view line, const char *key,
                         const bool *allowed, int allowed_max)
{
    const size_t key_len = std::strlen(key);
    size_t pos = 0;
    while ((pos = line.find(key, pos)) != std::string_view::npos)
    {
        size_t q = pos + key_len;
        while (q < line.size() && is_json_ws(line[q]))
            ++q;
        if (q >= line.size() || line[q] != ':')
        {
            pos = q;
            continue;
        }
        ++q;
        while (q < line.size() && is_json_ws(line[q]))
            ++q;
        if (q >= line.size())
            break;

        const bool neg = line[q] == '-';
        if (neg)
            ++q;
        if (q < line.size() && line[q] >= '0' && line[q] <= '9')
        {
            long v = 0;
            while (q < line.size() && line[q] >= '0' && line[q] <= '9')
            {
                v = v * 10 + (line[q] - '0');
                if (v > allowed_max)
                    v = static_cast<long>(allowed_max) + 1;
                ++q;
            }
            // 浮点形式不会命中整数比较，但无法可靠判断，保守放行
            if (q < line.size() && (line[q] == '.' || line[q] == 'e' || line[q] == 'E'))
                return true;
            const long value = neg ? -v : v;
            if (value >= 0 && value <= allowed_max && allowed[static_cast<size_t>(value)])
                return true;
        }
        pos = q;
    }
    return false;
}

// 行内任一 "key" 的字符串值（不区分大小写）命中 allowed 之一即返回 true；
// 值含转义或无法解析时保守返回 true。
bool raw_string_value_match(std::string_view line, const char *key,
                            const char *const *allowed, size_t allowed_count)
{
    const size_t key_len = std::strlen(key);
    size_t pos = 0;
    while ((pos = line.find(key, pos)) != std::string_view::npos)
    {
        size_t q = pos + key_len;
        while (q < line.size() && is_json_ws(line[q]))
            ++q;
        if (q >= line.size() || line[q] != ':')
        {
            pos = q;
            continue;
        }
        ++q;
        while (q < line.size() && is_json_ws(line[q]))
            ++q;
        if (q >= line.size() || line[q] != '"')
        {
            pos = q;
            continue;
        }
        ++q;
        const size_t value_start = q;
        bool has_escape = false;
        while (q < line.size() && line[q] != '"')
        {
            if (line[q] == '\\')
            {
                has_escape = true;
                ++q;
                if (q < line.size())
                    ++q;
            }
            else
            {
                ++q;
            }
        }
        if (q >= line.size())
            return true;
        const std::string_view value = line.substr(value_start, q - value_start);
        ++q;
        if (has_escape)
            return true;

        for (size_t i = 0; i < allowed_count; ++i)
        {
            const std::string_view want(allowed[i]);
            if (value.size() != want.size())
                continue;
            bool eq = true;
            for (size_t j = 0; j < value.size(); ++j)
            {
                if (std::tolower(static_cast<unsigned char>(value[j])) !=
                    std::tolower(static_cast<unsigned char>(want[j])))
                {
                    eq = false;
                    break;
                }
            }
            if (eq)
                return true;
        }
        pos = q;
    }
    return false;
}

// 跳过从 q 开始的整个 JSON token：避免把对象/数组内部字符串里的 ']' 当成数组结束。
size_t skip_json_token(std::string_view line, size_t q)
{
    if (q >= line.size())
        return q;
    const char open_c = line[q];
    if (open_c == '"')
        return q;
    const char close_c = (open_c == '{') ? '}' : ((open_c == '[') ? ']' : '\0');
    if (!close_c)
    {
        while (q < line.size() && line[q] != ',' && line[q] != ']' && line[q] != '}')
            ++q;
        return q;
    }

    ++q;
    int depth = 1;
    while (q < line.size() && depth > 0)
    {
        const char c = line[q];
        if (c == '"')
        {
            ++q;
            while (q < line.size())
            {
                if (line[q] == '\\')
                {
                    q += 2;
                    if (q > line.size())
                        q = line.size();
                }
                else if (line[q] == '"')
                {
                    ++q;
                    break;
                }
                else
                {
                    ++q;
                }
            }
            continue;
        }
        if (c == open_c)
            ++depth;
        else if (c == close_c)
            --depth;
        ++q;
    }
    return q;
}

// JSON 字符串 token 解码后与 name 比较：1=匹配、0=不匹配、-1=无法可靠解码（保守放行）。
int json_string_token_match(std::string_view raw, const std::string &name)
{
    static const char kBom[] = "\xEF\xBB\xBF";
    if (raw.find('\\') == std::string_view::npos)
    {
        if (raw.size() >= 3 && std::memcmp(raw.data(), kBom, 3) == 0)
            raw.remove_prefix(3);
        return raw == std::string_view(name) ? 1 : 0;
    }

    std::string decoded;
    decoded.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size())
    {
        const char c = raw[i];
        if (c != '\\')
        {
            decoded += c;
            ++i;
            continue;
        }
        ++i;
        if (i >= raw.size())
            return -1;
        const char e = raw[i];
        ++i;
        switch (e)
        {
        case '"': decoded += '"'; break;
        case '\\': decoded += '\\'; break;
        case '/': decoded += '/'; break;
        case 'b': decoded += '\b'; break;
        case 'f': decoded += '\f'; break;
        case 'n': decoded += '\n'; break;
        case 'r': decoded += '\r'; break;
        case 't': decoded += '\t'; break;
        case 'u':
        {
            if (i + 4 > raw.size())
                return -1;
            uint32_t cp = 0;
            for (int k = 0; k < 4; ++k)
            {
                const char h = raw[i + static_cast<size_t>(k)];
                cp <<= 4;
                if (h >= '0' && h <= '9')
                    cp |= static_cast<uint32_t>(h - '0');
                else if (h >= 'a' && h <= 'f')
                    cp |= static_cast<uint32_t>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                    cp |= static_cast<uint32_t>(h - 'A' + 10);
                else
                    return -1;
            }
            i += 4;

            if (cp >= 0xD800 && cp <= 0xDBFF)
            {
                // 高代理：需后随 \uXXXX 低代理才能组成非 BMP 字符
                if (i + 6 <= raw.size() && raw[i] == '\\' && raw[i + 1] == 'u')
                {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k)
                    {
                        const char h = raw[i + 2 + static_cast<size_t>(k)];
                        lo <<= 4;
                        if (h >= '0' && h <= '9')
                            lo |= static_cast<uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            lo |= static_cast<uint32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            lo |= static_cast<uint32_t>(h - 'A' + 10);
                        else
                        {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok || lo < 0xDC00 || lo > 0xDFFF)
                        return -1;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
                else
                {
                    return -1;
                }
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF)
            {
                return -1;
            }

            if (cp < 0x80)
                decoded += static_cast<char>(cp);
            else if (cp < 0x800)
            {
                decoded += static_cast<char>(0xC0 | (cp >> 6));
                decoded += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                decoded += static_cast<char>(0xE0 | (cp >> 12));
                decoded += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                decoded += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                decoded += static_cast<char>(0xF0 | (cp >> 18));
                decoded += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                decoded += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                decoded += static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            return -1;
        }
    }

    if (decoded.size() >= 3 && std::memcmp(decoded.data(), kBom, 3) == 0)
        decoded.erase(0, 3);
    return decoded == name ? 1 : 0;
}

// 整行的 "tags" 数组里，每个待筛标签以“名字字符串”（去 BOM 比较）或“数字 ID”
// 任一种形式出现都算命中；false 只在确定不命中时返回，标签数 >64 时也一律放行。
bool raw_tags_match(std::string_view line,
                    const std::vector<std::string> &names,
                    const std::vector<long> &ids)
{
    if (names.empty())
        return true;
    if (names.size() > 64)
        return true;
    const uint64_t need = (names.size() == 64)
                              ? ~uint64_t{0}
                              : ((uint64_t{1} << names.size()) - 1);

    constexpr size_t kKeyLen = 6; // "\"tags\""
    size_t pos = 0;
    while ((pos = line.find("\"tags\"", pos)) != std::string_view::npos)
    {
        size_t q = pos + kKeyLen;
        while (q < line.size() && is_json_ws(line[q]))
            ++q;
        if (q >= line.size() || line[q] != ':')
        {
            pos = q;
            continue;
        }
        ++q;
        while (q < line.size() && is_json_ws(line[q]))
            ++q;
        if (q >= line.size() || line[q] != '[')
        {
            pos = q;
            continue;
        }
        ++q;

        uint64_t found = 0;
        while (q < line.size())
        {
            while (q < line.size() && is_json_ws(line[q]))
                ++q;
            if (q >= line.size())
                return true;
            if (line[q] == ']')
                break;

            if (line[q] == '"')
            {
                ++q;
                const size_t tok_start = q;
                while (q < line.size() && line[q] != '"')
                {
                    if (line[q] == '\\')
                    {
                        ++q;
                        if (q < line.size())
                            ++q;
                    }
                    else
                    {
                        ++q;
                    }
                }
                if (q >= line.size())
                    return true;
                std::string_view tok = line.substr(tok_start, q - tok_start);
                ++q;
                for (size_t i = 0; i < names.size(); ++i)
                {
                    const int m = json_string_token_match(tok, names[i]);
                    if (m == 1)
                        found |= (uint64_t{1} << i);
                    else if (m == -1)
                        return true;
                }
            }
            else if (line[q] >= '0' && line[q] <= '9')
            {
                long v = 0;
                while (q < line.size() && line[q] >= '0' && line[q] <= '9')
                {
                    v = v * 10 + (line[q] - '0');
                    if (v > 1000000000L)
                        v = 1000000001L;
                    ++q;
                }
                for (size_t i = 0; i < ids.size(); ++i)
                    if (ids[i] >= 0 && v == ids[i])
                        found |= (uint64_t{1} << i);
            }
            else
            {
                q = skip_json_token(line, q);
            }

            if (q < line.size() && line[q] == ',')
                ++q;
        }
        if (found == need)
            return true;
        if (q < line.size() && line[q] == ']')
            ++q;
        pos = q;
    }
    return false;
}

// 已解析的 --pid-range 区间：端点已大写，两端同题库（校验后 hi_prefix == lo_prefix）。
struct PidRange
{
    std::string lo_prefix;
    unsigned long long lo_num = 0;
    std::string lo_suffix;
    std::string hi_prefix;
    unsigned long long hi_num = 0;
    std::string hi_suffix;
};

// 取一行原始 JSON 里第一个 "pid" 字符串值（不解码转义）；含转义或格式异常返回 false。
bool raw_pid_value(std::string_view line, std::string &out)
{
    static constexpr std::string_view kKey = "\"pid\"";
    size_t pos = 0;
    while ((pos = line.find(kKey, pos)) != std::string_view::npos)
    {
        size_t q = pos + kKey.size();
        while (q < line.size() && is_json_ws(line[q]))
            ++q;
        if (q >= line.size() || line[q] != ':')
        {
            pos = q;
            continue;
        }
        ++q;
        while (q < line.size() && is_json_ws(line[q]))
            ++q;
        if (q >= line.size() || line[q] != '"')
        {
            pos = q;
            continue;
        }
        ++q;
        const size_t value_start = q;
        while (q < line.size() && line[q] != '"')
        {
            if (line[q] == '\\')
                return false;
            ++q;
        }
        if (q >= line.size())
            return false;
        out.assign(line.substr(value_start, q - value_start));
        return true;
    }
    return false;
}

// 题号（已大写）是否落在任一 --pid-range 闭区间内
bool pid_in_ranges(const std::string &pid, const std::vector<PidRange> &ranges)
{
    std::string prefix, suffix;
    unsigned long long num = 0;
    if (!luogu::parse_pid_parts(pid, prefix, num, suffix))
        return false;
    for (const auto &r : ranges)
    {
        if (prefix != r.lo_prefix)
            continue;
        if (luogu::compare_pid_parts(num, suffix, r.lo_num, r.lo_suffix) >= 0 &&
            luogu::compare_pid_parts(num, suffix, r.hi_num, r.hi_suffix) <= 0)
            return true;
    }
    return false;
}

// 综合预筛：确定不可能被导出的行返回 false，语义与 select_problems 一致。
// --pid 是「额外追加」，命中的题目无条件保留；其余条件（难度/类型/标签/题号范围）
// 取「且」，任一确定不满足即排除；无条件时全部保留，不确定的一律放行。
bool raw_may_match(std::string_view line,
                   const std::vector<int> &difficulties,
                   const std::vector<std::string> &filter_tags,
                   const std::vector<long> &filter_tag_ids,
                   const std::vector<std::string> &types,
                   const std::set<std::string> &pids,
                   const std::vector<PidRange> &pid_ranges)
{
    std::string raw_pid;
    const bool have_pid = raw_pid_value(line, raw_pid);
    const std::string pid_upper = have_pid ? to_upper_ascii(raw_pid) : std::string();

    if (have_pid && pids.count(pid_upper) != 0)
        return true;

    const bool has_other_filters = !difficulties.empty() || !types.empty() ||
                                   !filter_tags.empty() || !pid_ranges.empty();
    if (has_other_filters)
    {
        bool other_possible = true;
        if (!difficulties.empty())
        {
            bool allowed[9] = {false};
            for (int d : difficulties)
                if (d >= 0 && d <= 8)
                    allowed[static_cast<size_t>(d)] = true;
            if (!raw_int_value_match(line, "\"difficulty\"", allowed, 8))
                other_possible = false;
        }

        if (other_possible && !types.empty())
        {
            const char *allowed_types[2] = {nullptr, nullptr};
            size_t n = 0;
            for (const auto &t : types)
                if (n < 2)
                    allowed_types[n++] = t.c_str();
            if (!raw_string_value_match(line, "\"type\"", allowed_types, n))
                other_possible = false;
        }

        if (other_possible && !filter_tags.empty() &&
            !raw_tags_match(line, filter_tags, filter_tag_ids))
            other_possible = false;

        if (other_possible && !pid_ranges.empty() && have_pid &&
            !pid_in_ranges(pid_upper, pid_ranges))
            other_possible = false;

        if (other_possible)
            return true;
    }
    else if (pids.empty())
    {
        return true;
    }

    // 到这里：其它条件确定不满足，或只给出了 --pid。题号提取不到时保守放行
    return !have_pid;
}

} // namespace

std::string luogu::bilibili_video_url(const std::string &url)
{
    static const std::string kScheme = "bilibili:";
    static const std::string kBase = "https://www.bilibili.com/video/";
    if (url.compare(0, kScheme.size(), kScheme) != 0)
        return "";

    // 拆出 id 与查询串（?page=4&t=82）：分 P 与起始位置由查询串表达，原样保留
    std::string id = url.substr(kScheme.size());
    std::string query;
    const size_t q = id.find_first_of("?#");
    if (q != std::string::npos)
    {
        query = id.substr(q);
        id.resize(q);
    }
    if (id.empty())
        return "";

    auto is_all = [](const std::string &s, size_t from, int (*pred)(int)) {
        return from < s.size() &&
               std::all_of(s.begin() + static_cast<std::ptrdiff_t>(from), s.end(),
                           [pred](char c) { return pred(static_cast<unsigned char>(c)) != 0; });
    };

    const std::string low = to_lower_ascii(id);
    if (low.rfind("bv", 0) == 0)
    {
        if (!is_all(id, 2, std::isalnum))
            return "";
    }
    else if (low.rfind("av", 0) == 0)
    {
        if (!is_all(id, 2, std::isdigit))
            return "";
    }
    else if (is_all(id, 0, std::isdigit))
    {
        id = "av" + id;
    }
    else
    {
        return "";
    }

    // 查询串只允许 URL 常见字符，避免把 } { \ " 等注入到 \url{} / \href{}
    // （Markdown 的 ]( ) 同理）
    static const std::string kQueryExtra = "-._~=&%?#+/:";
    for (char c : query)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && kQueryExtra.find(static_cast<char>(u)) == std::string::npos)
            return "";
    }
    return kBase + id + query;
}

std::string luogu::markdown_bilibili_links(const std::string &markdown)
{
    // 洛谷的视频写法是指向 bilibili: 伪协议的坏图，改写为
    // [文字](https://www.bilibili.com/video/...)：图片/链接语法取原文字（为空或用
    // 完整 URL），自动链接用完整 URL；围栏代码块与行内代码原样保留。
    static const std::regex kImage(R"(!\[([^\]]*)\]\(\s*(bilibili:[^\s)]+)\s*\))");
    static const std::regex kLink(R"(\[([^\]]*)\]\(\s*(bilibili:[^\s)]+)\s*\))");
    static const std::regex kAutolink(R"(<(bilibili:[^>\s]+)>)");

    auto expand = [](const std::string &label, const std::string &raw) -> std::string {
        const std::string full = luogu::bilibili_video_url(raw);
        if (full.empty())
            return ""; // 非可识别视频链接：保留原文
        // 文字含会破坏 Markdown 链接语法的字符时，改用完整 URL 作链接文字
        const bool label_ok =
            !label.empty() && label.find_first_of("[]()\\") == std::string::npos;
        return "[" + (label_ok ? label : full) + "](" + full + ")";
    };

    auto rewrite = [](const std::string &s, const std::regex &re,
                      const std::function<std::string(const std::smatch &)> &convert) {
        std::string out;
        size_t last = 0;
        for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
        {
            out += s.substr(last, static_cast<size_t>(it->position()) - last);
            const std::string replaced = convert(*it);
            out += replaced.empty() ? it->str() : replaced;
            last = static_cast<size_t>(it->position() + it->length());
        }
        out += s.substr(last);
        return out;
    };

    // 图片语法必须最先改写，否则内层 [..](..) 会被链接规则先匹配到
    auto rewrite_plain = [&](const std::string &s) {
        std::string r = rewrite(s, kImage, [&expand](const std::smatch &m) {
            return expand(m[1].str(), m[2].str());
        });
        r = rewrite(r, kLink, [&expand](const std::smatch &m) {
            return expand(m[1].str(), m[2].str());
        });
        return rewrite(r, kAutolink, [&expand](const std::smatch &m) {
            return expand("", m[1].str());
        });
    };

    // 行内代码（`...`）是示例内容，原样保留
    auto rewrite_line = [&rewrite_plain](const std::string &line) {
        std::string out;
        size_t i = 0;
        while (i < line.size())
        {
            const size_t open = line.find('`', i);
            if (open == std::string::npos)
            {
                out += rewrite_plain(line.substr(i));
                break;
            }
            size_t run = 0;
            while (open + run < line.size() && line[open + run] == '`')
                ++run;
            const std::string ticks = line.substr(open, run);
            const size_t close = line.find(ticks, open + run);
            if (close == std::string::npos)
            {
                out += rewrite_plain(line.substr(i));
                break;
            }
            out += rewrite_plain(line.substr(i, open - i));
            out += line.substr(open, close + run - open);
            i = close + run;
        }
        return out;
    };

    // 逐行改写，围栏代码块（``` / ~~~）里的示例内容原样保留
    std::string out;
    bool in_fence = false;
    size_t pos = 0;
    while (pos < markdown.size())
    {
        size_t eol = markdown.find('\n', pos);
        if (eol == std::string::npos)
            eol = markdown.size();
        const std::string line = markdown.substr(pos, eol - pos);
        const size_t first = line.find_first_not_of(" \t");
        const bool is_fence =
            first != std::string::npos &&
            (line.compare(first, 3, "```") == 0 || line.compare(first, 3, "~~~") == 0);
        if (is_fence)
            in_fence = !in_fence;
        out += (in_fence || is_fence) ? line : rewrite_line(line);
        if (eol < markdown.size())
            out += '\n';
        pos = eol + 1;
    }
    return out;
}

bool luogu::select_problems(const ExportFilter &filter,
                            std::vector<problem::Problem> &problems,
                            std::vector<std::string> *resolved_tags,
                            std::string &error)
{
    error.clear();
    problems.clear();
    if (resolved_tags)
        resolved_tags->clear();

    // 标签缓存（-U 生成的 ID <-> 名称与分类）复用进程内共享缓存，只读取一次
    const tagcache::Cache &tag_cache = tagcache::shared_cache();
    const bool has_tag_map = tagcache::shared_cache_loaded();

    std::vector<std::string> filter_tags;
    for (const auto &raw : filter.tags)
    {
        // 参数先整体匹配已知标签名（如 "NOIP 普及组"），未命中再按空格拆成多个标签
        const std::string raw_stripped = tagcache::strip_bom(raw);
        if (raw_stripped.find_first_of(" \t") != std::string::npos &&
            has_tag_map &&
            tag_cache.name_to_id.find(raw_stripped) != tag_cache.name_to_id.end())
        {
            filter_tags.push_back(raw_stripped);
            continue;
        }

        for (const auto &tok : split_whitespace(raw))
        {
            std::string name;
            if (!resolve_tag(tok, has_tag_map, tag_cache, name))
            {
                error = "缺少 tags.json，无法把标签 ID 翻译成名称，请先运行 -U: " + tok;
                return false;
            }
            filter_tags.push_back(name);
        }
    }
    if (resolved_tags)
        *resolved_tags = filter_tags;

    // 题号统一转大写：pids_set 为 --pid 精确匹配集合，wanted_pids 为需做存在性
    // 检查的题号（含 --pid-range 两端点），pid_ranges 为已解析闭区间（多组取或）
    std::set<std::string> pids_set;
    std::set<std::string> wanted_pids;
    std::vector<PidRange> pid_ranges;
    for (const auto &pid : filter.pids)
    {
        pids_set.insert(to_upper_ascii(pid));
        wanted_pids.insert(to_upper_ascii(pid));
    }
    for (const auto &r : filter.pid_ranges)
    {
        const std::string lo = to_upper_ascii(r.first);
        const std::string hi = to_upper_ascii(r.second);
        PidRange pr;
        if (!luogu::parse_pid_parts(lo, pr.lo_prefix, pr.lo_num, pr.lo_suffix) ||
            !luogu::parse_pid_parts(hi, pr.hi_prefix, pr.hi_num, pr.hi_suffix) ||
            pr.lo_prefix != pr.hi_prefix ||
            luogu::compare_pid_parts(pr.lo_num, pr.lo_suffix,
                                     pr.hi_num, pr.hi_suffix) > 0)
        {
            error = "参数 --pid-range 的题号范围 '" + r.first + "-" + r.second +
                    "' 无效（两端应为同一题库的合法题号，且左端点不超过右端点，"
                    "如 P1001-P1010）";
            return false;
        }
        pid_ranges.push_back(pr);
        wanted_pids.insert(lo);
        wanted_pids.insert(hi);
    }

    std::filesystem::path ndjson_path = crawler::get_cache_dir() / "latest.ndjson";
    FILE *in = luogu::compat::fopen(ndjson_path, "rb");
    if (!in)
    {
        error = "找不到题目缓存 '" + luogu::compat::path_to_utf8(ndjson_path) + "'，请先运行 -U 更新缓存";
        return false;
    }

    // --pid / --pid-range：先在缓存中确认题号存在（原始文本扫描，不做 JSON 解析），
    // 不存在时列出具体题号并停止
    if (!wanted_pids.empty())
    {
        std::set<std::string> found;
        if (std::fseek(in, 0, SEEK_SET) == 0)
        {
            std::string scan_line;
            while (luogu::compat::read_line(in, scan_line) >= 0)
            {
                if (scan_line.empty())
                    continue;
                std::string pid;
                if (raw_pid_value(std::string_view(scan_line), pid))
                {
                    const std::string up = to_upper_ascii(pid);
                    if (wanted_pids.count(up))
                        found.insert(up);
                }
            }
        }
        std::vector<std::string> missing;
        for (const auto &w : wanted_pids)
            if (!found.count(w))
                missing.push_back(w);
        if (!missing.empty())
        {
            std::fclose(in);
            error = "题目列表缓存中不存在题号 " + join_strings(missing, "、") +
                    "（无此题号的题目）；请检查题号拼写，或先运行 -U 更新缓存";
            return false;
        }
        // 存在性检查扫描到文件末尾，回到开头供筛选使用
        if (std::fseek(in, 0, SEEK_SET) != 0)
        {
            std::fclose(in);
            error = "读取题目缓存 '" + luogu::compat::path_to_utf8(ndjson_path) + "' 失败";
            return false;
        }
    }

    // 预计算 --tag 名字对应的数字 ID，供原始文本快速预筛使用（-1 表示查不到）
    std::vector<long> filter_tag_ids;
    filter_tag_ids.reserve(filter_tags.size());
    for (const auto &name : filter_tags)
    {
        const auto it = tag_cache.name_to_id.find(name);
        filter_tag_ids.push_back(it != tag_cache.name_to_id.end()
                                     ? static_cast<long>(it->second)
                                     : -1L);
    }

    // compat::read_line：读入一行（不含末尾换行），EOF 且无内容时返回 -1；MSVC 无 getline
    std::string line;
    while (luogu::compat::read_line(in, line) >= 0)
    {
        if (line.empty())
            continue;

        if (!raw_may_match(std::string_view(line),
                           filter.difficulties, filter_tags, filter_tag_ids,
                           filter.types, pids_set, pid_ranges))
            continue;

        json data;
        try
        {
            data = json::parse(line);
        }
        catch (...)
        {
            continue;
        }

        try
        {
            // 其它条件是否命中：难度/类型（多个取或，类型不区分大小写）、
            // 题号范围（落在任一闭区间）、标签（多个取且）
            auto matches_other_filters = [&](const json &item,
                                             const problem::Problem &p) -> bool {
                if (!filter.difficulties.empty())
                {
                    if (!item.contains("difficulty") || !item["difficulty"].is_number_integer())
                        return false;
                    const int difficulty = item["difficulty"].get<int>();
                    if (std::find(filter.difficulties.begin(), filter.difficulties.end(),
                                  difficulty) == filter.difficulties.end())
                        return false;
                }

                if (!filter.types.empty())
                {
                    std::string ptype = item.value("type", "");
                    for (auto &c : ptype)
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (std::find(filter.types.begin(), filter.types.end(), ptype) ==
                        filter.types.end())
                        return false;
                }

                if (!pid_ranges.empty() &&
                    !pid_in_ranges(to_upper_ascii(p.pid), pid_ranges))
                    return false;

                if (!filter_tags.empty())
                {
                    for (const auto &wanted : filter_tags)
                    {
                        const std::string key = to_lower_ascii(wanted);
                        if (std::find_if(p.tags.begin(), p.tags.end(),
                                         [&key](const std::string &t) {
                                             return to_lower_ascii(t) == key;
                                         }) == p.tags.end())
                            return false;
                    }
                }

                return true;
            };

            // Problem 构造负责解析标签名称、题面、样例、时空限制与多语言
            problem::Problem p(data, &tag_cache.id_to_name);
            const std::string pid_upper = to_upper_ascii(p.pid);

            const bool pid_hit = !pids_set.empty() && pids_set.count(pid_upper) != 0;
            const bool has_pid_filter = !pids_set.empty();
            const bool has_other_filter = !filter.difficulties.empty() ||
                                          !filter.types.empty() ||
                                          !filter_tags.empty() ||
                                          !pid_ranges.empty();

            bool selected = pid_hit;
            if (!selected && has_other_filter)
                selected = matches_other_filters(data, p);
            if (!selected && !has_pid_filter && !has_other_filter)
                selected = true;
            if (!selected)
                continue;

            problems.push_back(std::move(p));
        }
        catch (...)
        {
            continue;
        }
    }
    // 校验 --tag 名称确实存在：先查 tags.json；不在表中（缺失或只出现在题目缓存，
    // 如个别年份/旧版标签）时独立全量扫描题目缓存收集全部标签再比对，避免误报
    std::vector<std::string> not_found;
    {
        std::set<std::string> not_found_lower;
        for (const auto &wanted : filter_tags)
        {
            const bool in_tag_map = has_tag_map &&
                                    tag_cache.name_to_id.find(wanted) !=
                                        tag_cache.name_to_id.end();
            if (!in_tag_map && not_found_lower.insert(to_lower_ascii(wanted)).second)
                not_found.push_back(wanted);
        }
    }
    if (!not_found.empty())
    {
        std::set<std::string> all_tags;
        if (std::fseek(in, 0, SEEK_SET) == 0)
        {
            std::string scan_line;
            while (luogu::compat::read_line(in, scan_line) >= 0)
            {
                if (scan_line.empty())
                    continue;
                json data;
                try
                {
                    data = json::parse(scan_line);
                }
                catch (...)
                {
                    continue;
                }
                try
                {
                    if (!data.contains("tags") || !data["tags"].is_array())
                        continue;
                    for (const auto &t : data["tags"])
                    {
                        if (t.is_string())
                        {
                            all_tags.insert(to_lower_ascii(
                                tagcache::strip_bom(t.get<std::string>())));
                        }
                        else if (t.is_number_integer() && has_tag_map)
                        {
                            const auto it = tag_cache.id_to_name.find(t.get<int>());
                            if (it != tag_cache.id_to_name.end())
                                all_tags.insert(to_lower_ascii(it->second));
                        }
                    }
                }
                catch (...)
                {
                    continue;
                }
            }
        }
        not_found.erase(
            std::remove_if(not_found.begin(), not_found.end(),
                           [&](const std::string &w) {
                               return all_tags.count(to_lower_ascii(w)) != 0;
                           }),
            not_found.end());
    }
    std::fclose(in);

    if (!not_found.empty())
    {
        error = "以下标签不存在: " + join_strings(not_found, "、") +
                "（请检查拼写，或先运行 -U 更新缓存）";
        return false;
    }

    // 排序：题号数字部分升序（取法见 pid_number），相同则比较题号原文；
    // (数字, 原文) 构成全序，满足严格弱序，std::sort 不会因比较不合法而 UB
    std::sort(problems.begin(), problems.end(), [](const problem::Problem &a, const problem::Problem &b) {
        const unsigned long long na = pid_number(a.pid);
        const unsigned long long nb = pid_number(b.pid);
        if (na != nb)
            return na < nb;
        return a.pid < b.pid;
    });
    return true;
}

std::string luogu::describe_filter(const ExportFilter &filter,
                                   const std::vector<std::string> &resolved_tags,
                                   const DisplayOptions &display)
{
    const bool use_en = (filter.lang == "en");

    std::vector<std::string> conds;
    // --pid 是「额外追加」：与其它条件同时给出时描述为追加，单独给出时按普通筛选
    const bool pid_appends = !filter.pids.empty() &&
                             (!filter.pid_ranges.empty() ||
                              !resolved_tags.empty() ||
                              !filter.difficulties.empty() ||
                              !filter.types.empty());
    if (!filter.pids.empty())
        conds.push_back(pid_appends ? "另追加题号为 " + join_strings(filter.pids, "、") + " 的题目"
                                    : "题号为 " + join_strings(filter.pids, "、"));
    if (!filter.pid_ranges.empty())
    {
        std::vector<std::string> rs;
        for (const auto &r : filter.pid_ranges)
            rs.push_back(r.first + "-" + r.second);
        conds.push_back("题号范围为 " + join_strings(rs, " 或 "));
    }
    if (!resolved_tags.empty())
        conds.push_back("标签包含 " + join_strings(resolved_tags, "、"));
    if (!filter.difficulties.empty())
    {
        std::vector<std::string> ds;
        for (int d : filter.difficulties)
            ds.push_back(std::string(luogu::difficulty_label(d)) + "(" + std::to_string(d) + ")");
        conds.push_back("难度为 " + join_strings(ds, " 或 "));
    }
    if (!filter.types.empty())
        conds.push_back("类型为 " + join_strings(filter.types, "、"));
    if (use_en)
        conds.push_back("题面语言为英文（缺失时回退中文）");
    // 显示设置只记录「不显示」的项，默认设置下说明更简洁
    if (!display.difficulty)
        conds.push_back("不显示难度");
    if (!display.algorithm_tags)
        conds.push_back("不显示算法类标签");
    if (!display.source_tags)
        conds.push_back("不显示来源类标签");
    return join_strings(conds, "；");
}

std::string luogu::truncate_utf8(const std::string &text, size_t max_chars)
{
    size_t chars = 0;
    size_t i = 0;
    while (i < text.size())
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0x80) == 0x00)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        if (i + len > text.size())
            break; // 末尾残片不完整：丢弃，避免输出非法 UTF-8
        ++chars;
        if (chars > max_chars)
            return text.substr(0, i) + "…";
        i += len;
    }
    return text;
}

std::string luogu::strip_solution_title_prefix(const std::string &title)
{
    size_t i = 0;
    while (i < title.size() && (title[i] == ' ' || title[i] == '\t'))
        ++i;
    static const char *kPrefixes[] = {"题解：", "题解:", "题解 "};
    for (const char *prefix : kPrefixes)
    {
        const std::string p = prefix;
        if (title.compare(i, p.size(), p) != 0)
            continue;
        size_t j = i + p.size();
        while (j < title.size() && (title[j] == ' ' || title[j] == '\t'))
            ++j;
        if (j >= title.size())
            return title; // 只有前缀：保持原样，避免空标题
        return title.substr(j);
    }
    return title;
}

std::string luogu::solution_heading(const std::string &title)
{
    std::string name = title.empty() ? "（无标题）"
                                     : strip_solution_title_prefix(title);
    // 去掉标题里可能出现的换行，避免破坏目录条目与书签
    for (char &c : name)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return "题解：" + truncate_utf8(name, 60);
}

std::string luogu::strip_article_title_prefix(const std::string &title)
{
    size_t i = 0;
    while (i < title.size() && (title[i] == ' ' || title[i] == '\t'))
        ++i;
    static const char *kPrefixes[] = {"文章：", "文章:", "文章 "};
    for (const char *prefix : kPrefixes)
    {
        const std::string p = prefix;
        if (title.compare(i, p.size(), p) != 0)
            continue;
        size_t j = i + p.size();
        while (j < title.size() && (title[j] == ' ' || title[j] == '\t'))
            ++j;
        if (j >= title.size())
            return title;
        return title.substr(j);
    }
    return title;
}

std::string luogu::article_heading(const std::string &title)
{
    std::string name = title.empty() ? "（无标题）"
                                     : strip_article_title_prefix(title);
    for (char &c : name)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return "文章：" + truncate_utf8(name, 60);
}

namespace
{

// 锚点片段白名单化：锚点会写进 LaTeX 的 \hypertarget / \pdfbookmark 与
// Markdown 的 <a id="...">，而题号来自题目列表缓存（只过滤控制字符、不校验形状），
// 含 # % \ { } " 换行等字符会破坏结构甚至注入命令。这里只保留字母、数字与 - _ .，
// 其余替换为 '_' 并追加原文哈希后缀（避免 P1#2 与 P1_2 撞同一锚点）。
std::string anchor_safe_segment(const std::string &raw)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() + 9);
    bool replaced = false;
    for (unsigned char c : raw)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            out += static_cast<char>(c);
        else
        {
            out += '_';
            replaced = true;
        }
    }
    if (!replaced)
        return out;

    // FNV-1a 32 位：只用于区分被替换过的片段，同一原文得到同一后缀
    uint32_t hash = 2166136261u;
    for (unsigned char c : raw)
    {
        hash ^= c;
        hash *= 16777619u;
    }
    out += '-';
    for (int shift = 28; shift >= 0; shift -= 4)
        out += kHex[(hash >> shift) & 0x0Fu];
    return out;
}

} // namespace

// 供 latex.cpp 的 \pdfbookmark 标签等其它模块复用同一套白名单化
std::string luogu::anchor_segment(const std::string &raw)
{
    return anchor_safe_segment(raw);
}

std::string luogu::problem_anchor(const std::string &pid)
{
    return "sol-problem-" + anchor_safe_segment(pid);
}

std::string luogu::solution_anchor(const std::string &pid, const std::string &lid)
{
    return "sol-" + anchor_safe_segment(pid) + "-" + anchor_safe_segment(lid);
}

std::string luogu::article_anchor(const std::string &lid)
{
    return "sol-art-" + anchor_safe_segment(lid);
}
