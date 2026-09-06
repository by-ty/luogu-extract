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
#include <limits>
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

// 按空白拆成多个 token（空 token 忽略）
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

// 把 --tag 参数规范成标签名：数字 ID 优先按 tags.json 翻译，其余按名称原样处理
// 返回 false 仅当输入是数字 ID 但缺少 tags.json 无法翻译
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
        return false; // 数字 ID 需要 tags.json 才能翻译

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

// 题号数字部分（用于按题号从小到大排序）
long pid_number(const std::string &pid)
{
    long n = 0;
    bool any = false;
    const long kMax = std::numeric_limits<long>::max();
    for (char c : pid)
    {
        if (c >= '0' && c <= '9')
        {
            const long d = c - '0';
            // 溢出即封顶返回 LONG_MAX：畸形缓存里的超长数字不再触发
            // 有符号溢出 UB，且排序仍稳定（全部归到最大档）
            if (n > (kMax - d) / 10)
                return kMax;
            n = n * 10 + d;
            any = true;
        }
    }
    return any ? n : -1;
}

// ---- 原始文本快速预筛 -------------------------------------------------
// 目标：跳过“确定不可能命中筛选条件”的行，避免为它们构造完整 JSON DOM。
// 原则：只有能严格证明不命中时才跳过；任何不确定情况一律返回“可能命中”，
// 交给后面的完整 JSON 解析与精确筛选，保证筛选结果与原来完全一致。

inline bool is_json_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 扫描整行的 "key" 键值对：只要任一出现处的整数值命中 allowed[0..allowed_max]
// 就返回 true；键未出现或所有值都不命中返回 false。
// 值带小数点/指数（如 1.0、1e0，nlohmann 不会当作整数）或无法解析时保守返回 true。
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
                    v = static_cast<long>(allowed_max) + 1; // 超出范围即无需精确值
                ++q;
            }
            // 浮点形式不会被当作整数，但无法可靠判断，保守交给完整解析
            if (q < line.size() && (line[q] == '.' || line[q] == 'e' || line[q] == 'E'))
                return true;
            const long value = neg ? -v : v;
            if (value >= 0 && value <= allowed_max && allowed[static_cast<size_t>(value)])
                return true;
        }
        // null / 字符串等类型或值不在允许集合内：继续找下一个同名字段
        pos = q;
    }
    return false;
}

// 扫描整行的 "key" 键值对：只要任一出现处的字符串值（不区分大小写）命中
// allowed 之一就返回 true；键未出现或所有值都不命中返回 false。
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
            pos = q; // null / 数字等非字符串值：不命中
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
            return true; // 字符串未闭合，保守
        const std::string_view value = line.substr(value_start, q - value_start);
        ++q;
        if (has_escape)
            return true; // 含转义无法可靠比较，保守

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

// 跳过从 q 开始的 JSON token（对象/数组/普通值），返回其后的位置。
// 用于在 tags 数组里跳过对象元素，避免误把对象字符串里的 ']' 当成数组结束。
size_t skip_json_token(std::string_view line, size_t q)
{
    if (q >= line.size())
        return q;
    const char open_c = line[q];
    if (open_c == '"')
        return q; // 字符串由调用方处理
    const char close_c = (open_c == '{') ? '}' : ((open_c == '[') ? ']' : '\0');
    if (!close_c)
    {
        while (q < line.size() && line[q] != ',' && line[q] != ']' && line[q] != '}')
            ++q;
        return q;
    }

    ++q; // 跳过开括号
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

// 把 JSON 字符串 token（引号内的原始字节）解码成 UTF-8 后与 name 比较。
// 返回 1=匹配，0=确定不匹配，-1=含无法可靠解码的内容（调用方应保守放行）。
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
        ++i; // 跳过反斜杠
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
                return -1; // 孤立的低代理
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
            return -1; // 未知转义：不确定
        }
    }

    if (decoded.size() >= 3 && std::memcmp(decoded.data(), kBom, 3) == 0)
        decoded.erase(0, 3);
    return decoded == name ? 1 : 0;
}

// 检查整行里是否存在包含全部 filter_tags 的 "tags" 数组。
// 每个标签在数组里以“名字字符串”（去 BOM 后比较）或“数字 ID”任一种形式
// 出现都算命中；返回 false 表示确定不命中，true 表示可能命中或无法可靠判断。
bool raw_tags_match(std::string_view line,
                    const std::vector<std::string> &names,
                    const std::vector<long> &ids)
{
    if (names.empty())
        return true;
    if (names.size() > 64)
        return true; // 数量过多时保守处理，直接完整解析
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
            // tags 不是数组（如 null）：这一处不命中，继续找其它同名键
            pos = q;
            continue;
        }
        ++q; // 进入数组

        uint64_t found = 0;
        while (q < line.size())
        {
            while (q < line.size() && is_json_ws(line[q]))
                ++q;
            if (q >= line.size())
                return true; // 数组未闭合，保守
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
                    return true; // 字符串未闭合，保守
                std::string_view tok = line.substr(tok_start, q - tok_start);
                ++q;
                for (size_t i = 0; i < names.size(); ++i)
                {
                    const int m = json_string_token_match(tok, names[i]);
                    if (m == 1)
                        found |= (uint64_t{1} << i);
                    else if (m == -1)
                        return true; // 无法可靠解码：保守交给完整解析
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
                q = skip_json_token(line, q); // 对象等其它元素
            }

            if (q < line.size() && line[q] == ',')
                ++q;
        }
        if (found == need)
            return true;
        if (q < line.size() && line[q] == ']')
            ++q;
        pos = q; // 继续找其它 "tags" 键
    }
    return false;
}

// ---- --pid / --pid-range 相关辅助 -------------------------------------

// 已解析的 --pid-range 区间（端点已规范化为大写，且两端属于同一题库）
struct PidRange
{
    std::string lo_prefix;  // 题库前缀（如 "P" / "B"），两端相同
    unsigned long long lo_num = 0;
    std::string lo_suffix;
    std::string hi_prefix;  // 恒等于 lo_prefix（仅用于校验时暂存）
    unsigned long long hi_num = 0;
    std::string hi_suffix;
};

// 从一行原始 JSON 文本中提取第一个 "pid" 字符串值（不解码转义；
// 值含反斜杠或格式异常时保守返回 false，交由完整 JSON 解析处理）。
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
                return false; // 含转义：无法可靠比较
            ++q;
        }
        if (q >= line.size())
            return false; // 字符串未闭合
        out.assign(line.substr(value_start, q - value_start));
        return true;
    }
    return false;
}

// 题号（已大写）是否命中 --pid 集合或任一 --pid-range 区间
bool pid_matches(const std::string &pid,
                 const std::set<std::string> &pids,
                 const std::vector<PidRange> &ranges)
{
    if (pids.count(pid) != 0)
        return true;
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

// 综合预筛：难度、类型、标签、题号任一条件在原始文本上就确定不满足时返回 false。
bool raw_may_match(std::string_view line,
                   const std::vector<int> &difficulties,
                   const std::vector<std::string> &filter_tags,
                   const std::vector<long> &filter_tag_ids,
                   const std::vector<std::string> &types,
                   const std::set<std::string> &pids,
                   const std::vector<PidRange> &pid_ranges)
{
    if (!difficulties.empty())
    {
        bool allowed[9] = {false};
        for (int d : difficulties)
            if (d >= 0 && d <= 8)
                allowed[static_cast<size_t>(d)] = true;
        if (!raw_int_value_match(line, "\"difficulty\"", allowed, 8))
            return false;
    }

    if (!types.empty())
    {
        const char *allowed_types[2] = {nullptr, nullptr};
        size_t n = 0;
        for (const auto &t : types)
            if (n < 2)
                allowed_types[n++] = t.c_str();
        if (!raw_string_value_match(line, "\"type\"", allowed_types, n))
            return false;
    }

    if (!filter_tags.empty() && !raw_tags_match(line, filter_tags, filter_tag_ids))
        return false;

    if (!pids.empty() || !pid_ranges.empty())
    {
        std::string raw_pid;
        if (!raw_pid_value(line, raw_pid))
            return true; // 无法可靠提取：保守交给完整解析
        if (!pid_matches(to_upper_ascii(raw_pid), pids, pid_ranges))
            return false;
    }

    return true;
}

} // namespace

bool luogu::select_problems(const ExportFilter &filter,
                            std::vector<problem::Problem> &problems,
                            std::vector<std::string> *resolved_tags,
                            std::string &error)
{
    error.clear();
    problems.clear();
    if (resolved_tags)
        resolved_tags->clear();

    // 1. 标签缓存（-U 生成：ID <-> 名称，以及标签分类 type）
    //    复用进程内共享缓存，tags.json 只读取一次
    const tagcache::Cache &tag_cache = tagcache::shared_cache();
    const bool has_tag_map = tagcache::shared_cache_loaded();

    // 2. 解析 --tag 参数
    std::vector<std::string> filter_tags;
    for (const auto &raw : filter.tags)
    {
        // 含空格的参数先整体匹配已知标签名（如 "NOIP 普及组"）：命中就按一个标签，
        // 否则按空格拆成多个标签（如 --tag "模拟 贪心"），保持原有写法。
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

    // 2.5 --pid / --pid-range 的题号规范化与区间解析
    // pids_set：大写题号集合（--pid，精确匹配）；
    // wanted_pids：需要存在性检查的题号（--pid 值与 --pid-range 两端点）；
    // pid_ranges：已解析的闭区间（多组取“或”）
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

    // 3. 打开题目缓存
    std::filesystem::path ndjson_path = crawler::get_cache_dir() / "latest.ndjson";
    FILE *in = luogu::compat::fopen(ndjson_path, "rb");
    if (!in)
    {
        error = "找不到题目缓存 '" + luogu::compat::path_to_utf8(ndjson_path) + "'，请先运行 -U 更新缓存";
        return false;
    }

    // 3.5 --pid / --pid-range：先在题目列表缓存中查找题号是否存在，
    //     不存在时指出具体题号并停止执行（原始文本扫描，避免整份缓存做 JSON 解析）
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

    // 4. 逐行扫描并筛选（统一用 Problem 结构承载题目）
    // 预计算每个 --tag 名字对应的数字 ID，供原始文本快速预筛使用（-1 表示查不到）
    std::vector<long> filter_tag_ids;
    filter_tag_ids.reserve(filter_tags.size());
    for (const auto &name : filter_tags)
    {
        const auto it = tag_cache.name_to_id.find(name);
        filter_tag_ids.push_back(it != tag_cache.name_to_id.end()
                                     ? static_cast<long>(it->second)
                                     : -1L);
    }

    // 用跨平台 read_line 替代 POSIX getline（MSVC 没有 getline），
    // 语义一致：读入一行（不含末尾换行），EOF 且无内容时返回 -1
    std::string line;
    while (luogu::compat::read_line(in, line) >= 0)
    {
        if (line.empty())
            continue;

        // 快速预筛：原始文本上就确定不可能命中的行，跳过 JSON 解析
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
            continue; // 跳过损坏行
        }

        try
        {
            // 难度：多个值取“或”
            if (!filter.difficulties.empty())
            {
                if (!data.contains("difficulty") || !data["difficulty"].is_number_integer())
                    continue;
                const int difficulty = data["difficulty"].get<int>();
                if (std::find(filter.difficulties.begin(), filter.difficulties.end(), difficulty) ==
                    filter.difficulties.end())
                    continue;
            }

            // 类型：多个值取“或”（B / P，不区分大小写）
            if (!filter.types.empty())
            {
                std::string ptype = data.value("type", "");
                for (auto &c : ptype)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (std::find(filter.types.begin(), filter.types.end(), ptype) == filter.types.end())
                    continue;
            }

            // 构造 Problem（标签名称、题面、样例、时空限制、多语言都在这里解析）
            problem::Problem p(data, &tag_cache.id_to_name);

            // --pid：题号精确匹配（不区分大小写）；--pid-range：题号落在任一闭区间
            if (!pids_set.empty() || !pid_ranges.empty())
            {
                if (!pid_matches(to_upper_ascii(p.pid), pids_set, pid_ranges))
                    continue;
            }

            // 标签：多个值取“且”
            if (!filter_tags.empty())
            {
                bool all = true;
                for (const auto &wanted : filter_tags)
                {
                    const std::string key = to_lower_ascii(wanted);
                    if (std::find_if(p.tags.begin(), p.tags.end(),
                                     [&key](const std::string &t) { return to_lower_ascii(t) == key; }) ==
                        p.tags.end())
                    {
                        all = false;
                        break;
                    }
                }
                if (!all)
                    continue;
            }

            problems.push_back(std::move(p));
        }
        catch (...)
        {
            continue; // 字段类型异常时跳过该题
        }
    }
    // 5. 校验 --tag 名称确实存在。与难度/类型筛选解耦：
    //    - 首选官方标签表 tags.json（O(1) 查找）；
    //    - 标签不在表中（tags.json 缺失，或名称只出现在题目缓存，如个别
    //      年份/旧版标签）时，独立全量扫描题目缓存收集全部标签再比对，
    //      避免“标签存在于缓存、只是不满足难度/类型条件”时被误报不存在
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

    // 6. 排序：按题号从小到大（先按数字部分升序，前缀字母作为次级排序）
    std::sort(problems.begin(), problems.end(), [](const problem::Problem &a, const problem::Problem &b) {
        const long na = pid_number(a.pid);
        const long nb = pid_number(b.pid);
        if (na != nb)
            return na < nb;
        return a.pid < b.pid;
    });
    return true;
}

std::string luogu::describe_filter(const ExportFilter &filter,
                                   const std::vector<std::string> &resolved_tags)
{
    const bool use_en = (filter.lang == "en");
    const bool show_difficulty = (filter.show.size() >= 2 && filter.show[0] == '1');
    const bool show_tags = (filter.show.size() >= 2 && filter.show[1] == '1');

    std::vector<std::string> conds;
    if (!filter.pids.empty())
        conds.push_back("题号为 " + join_strings(filter.pids, "、"));
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
    if (!show_difficulty)
        conds.push_back("不显示难度");
    if (!show_tags)
        conds.push_back("不显示算法类标签");
    return join_strings(conds, "；");
}
