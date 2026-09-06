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

// src/contents/problem.cpp
#include <cctype>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "luogu-extract/contents/problem.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/image_util.h"
#include "luogu-extract/util/tag_cache.h"

using nlohmann::json;
using problem::Problem;

namespace
{

// 删除 ::anti-ai[...] 指令块（洛谷用它向 AI 隐藏题面内容，导出时应把整段
// 内容连同指令名与方括号一起删除）。方括号允许嵌套；未闭合的块删除到
// 字符串末尾；指令名不区分大小写，未跟 '[' 的指令名按普通文本保留。
std::string strip_anti_ai(std::string s)
{
    static constexpr char kMarker[] = "::anti-ai";
    static constexpr size_t kMarkerLen = sizeof(kMarker) - 1;
    std::string out;
    size_t i = 0;
    while (i < s.size())
    {
        // 不区分大小写（仅 ASCII）查找指令名
        size_t p = std::string::npos;
        for (size_t q = i; q + kMarkerLen <= s.size(); ++q)
        {
            bool eq = true;
            for (size_t k = 0; k < kMarkerLen; ++k)
            {
                if (std::tolower(static_cast<unsigned char>(s[q + k])) !=
                    std::tolower(static_cast<unsigned char>(kMarker[k])))
                {
                    eq = false;
                    break;
                }
            }
            if (eq)
            {
                p = q;
                break;
            }
        }
        if (p == std::string::npos)
        {
            out += s.substr(i);
            break;
        }
        out += s.substr(i, p - i);
        size_t q = p + kMarkerLen;
        if (q >= s.size() || s[q] != '[')
        {
            // 指令名后没有紧跟 '['：不是指令块，保留原文并继续向后查找
            out += s.substr(p, q - p);
            i = q;
            continue;
        }
        // 找与 '[' 配对的 ']'（块内容里允许出现嵌套的方括号）
        int depth = 1;
        size_t r = q + 1;
        while (r < s.size() && depth > 0)
        {
            if (s[r] == '[')
                ++depth;
            else if (s[r] == ']')
                --depth;
            ++r;
        }
        if (depth != 0)
        {
            // 未闭合：视为直到字符串末尾都处于块内，全部删除
            i = s.size();
            break;
        }
        i = r; // 整块（含 ']'）被删除
    }
    return out;
}

// 键存在但值为 null 时也返回缺省值（官方数据里 background/hint 等可能为 null）。
// 同时过滤控制字符（\u0000 等）：fputs/fprintf("%s") 依赖 C 字符串终止符，
// 含 NUL 的内容会被静默截断，且控制字符会破坏 LaTeX 编译
std::string get_string(const json &j, const char *key)
{
    if (!j.contains(key) || !j[key].is_string())
        return "";
    return luogu::compat::strip_control_chars(
        strip_anti_ai(j[key].get<std::string>()));
}

int get_int(const json &j, const char *key, int def)
{
    if (!j.contains(key) || !j[key].is_number_integer())
        return def;
    return j[key].get<int>();
}

} // namespace

problem::Problem::Problem() { return; }

problem::Problem::Problem(const json &data,
                          const std::unordered_map<int, std::string> *tag_id_to_name)
{
    pid = luogu::compat::strip_control_chars(get_string(data, "pid"));
    type = luogu::compat::strip_control_chars(get_string(data, "type"));
    difficulty = get_int(data, "difficulty", 0);

    // 标签：批量缓存里一般是中文名；若是数字 ID 则用 tag_id_to_name 翻译
    if (data.contains("tags") && data["tags"].is_array())
    {
        for (const auto &t : data["tags"])
        {
            if (t.is_string())
            {
                tags.push_back(luogu::compat::strip_control_chars(
                    tagcache::strip_bom(t.get<std::string>())));
            }
            else if (t.is_number_integer())
            {
                const int id = t.get<int>();
                if (tag_id_to_name)
                {
                    auto it = tag_id_to_name->find(id);
                    if (it != tag_id_to_name->end())
                    {
                        tags.push_back(it->second);
                        continue;
                    }
                }
                tags.push_back("tag#" + std::to_string(id));
            }
        }
    }

    // 中文题面：优先取 translations.zh-CN（部分题目默认语言是英文，如 P1561），
    // 缺失时回退到顶层字段（批量缓存里中文键名是 title/inputFormat/outputFormat）
    auto zh_field = [&](const char *key) -> std::string {
        if (data.contains("translations") && data["translations"].is_object())
        {
            const json &tr = data["translations"];
            if (tr.contains("zh-CN") && tr["zh-CN"].is_object())
            {
                const std::string zh = get_string(tr["zh-CN"], key);
                if (!zh.empty())
                    return zh;
            }
        }
        return get_string(data, key);
    };
    name = zh_field("title");
    background = zh_field("background");
    description = zh_field("description");
    formatI = zh_field("inputFormat");
    formatO = zh_field("outputFormat");
    hint = zh_field("hint");

    // 样例
    if (data.contains("samples") && data["samples"].is_array())
    {
        for (const auto &sample : data["samples"])
        {
            if (sample.is_array() && sample.size() >= 2 &&
                sample[0].is_string() && sample[1].is_string())
            {
                samples.emplace_back(
                    luogu::compat::strip_control_chars(sample[0].get<std::string>()),
                    luogu::compat::strip_control_chars(sample[1].get<std::string>()));
            }
        }
    }

    // 时空限制
    if (data.contains("limits") && data["limits"].is_object())
    {
        const json &limits = data["limits"];
        if (limits.contains("time") && limits["time"].is_array())
            for (const auto &v : limits["time"])
                if (v.is_number_integer()) time.push_back(v.get<int>());
        if (limits.contains("memory") && limits["memory"].is_array())
            for (const auto &v : limits["memory"])
                if (v.is_number_integer()) memory.push_back(v.get<int>());
    }

    // 多语言题面：只保留 en（zh-CN 与顶层字段重复）；
    // 英文题面同样删除 ::anti-ai 指令块（-M / -L 的 --lang en
    // 直接读取该对象的字符串字段）
    if (data.contains("translations") && data["translations"].is_object() &&
        data["translations"].contains("en") && data["translations"]["en"].is_object())
    {
        translations = data["translations"]["en"];
        for (auto it = translations.begin(); it != translations.end(); ++it)
            if (it.value().is_string())
                it.value() = strip_anti_ai(it.value().get<std::string>());
    }
}

std::vector<std::string> Problem::image_urls() const
{
    // 同时扫描中文题面与英文题面（translations.en）中的图片：
    // --lang en 导出时题面字段改用英文，英文独有的图片也要进入
    // “缺失图片”列表被下载，否则会被 \IfFileExists 静默跳过
    std::string all;
    auto add = [&](const std::string &field) {
        all += field;
        all += '\n';
    };
    all.reserve(background.size() + description.size() + formatI.size() +
                formatO.size() + hint.size() + name.size() + 64);
    add(name);
    add(background);
    add(description);
    add(formatI);
    add(formatO);
    add(hint);
    if (translations.is_object())
    {
        auto en_field = [&](const char *key) {
            if (translations.contains(key) && translations[key].is_string())
                add(translations[key].get<std::string>());
        };
        en_field("title");
        en_field("background");
        en_field("description");
        en_field("inputFormat");
        en_field("outputFormat");
        en_field("hint");
    }
    return image_util::extract_urls(all);
}
