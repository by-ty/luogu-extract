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

// include/luogu-extract/util/problem_info.h
#ifndef LUOGU_EXTRACT_PROBLEM_INFO_H
#define LUOGU_EXTRACT_PROBLEM_INFO_H

#include <algorithm>
#include <string>
#include <vector>

namespace luogu
{
    // 洛谷官方新版难度表 ProblemDifficulty（0-8）
    inline const char *difficulty_label(int difficulty)
    {
        switch (difficulty)
        {
        case 0: return "暂无评定";
        case 1: return "入门";
        case 2: return "普及−";
        case 3: return "普及";
        case 4: return "普及+/提高−";
        case 5: return "提高";
        case 6: return "提高+/省选−";
        case 7: return "省选/NOI−";
        case 8: return "NOI/NOI+/CTS";
        default: return "未知";
        }
    }

    // 洛谷网页题目难度对应的字体颜色（HTML 十六进制，与洛谷网页一致）：
    // 暂无评定 rgb(191,191,191)、入门 rgb(254,76,97)、普及− rgb(243,156,17)、
    // 普及 rgb(255,193,22)、普及+/提高− rgb(83,196,26)、提高 rgb(19,194,194)、
    // 提高+/省选− rgb(52,152,219)、省选/NOI− rgb(156,61,207)、
    // NOI/NOI+/CTS rgb(14,29,105)；未知难度返回黑色。
    inline const char *difficulty_color(int difficulty)
    {
        switch (difficulty)
        {
        case 0: return "bfbfbf";
        case 1: return "fe4c61";
        case 2: return "f39c11";
        case 3: return "ffc116";
        case 4: return "53c41a";
        case 5: return "13c2c2";
        case 6: return "3498db";
        case 7: return "9c3dcf";
        case 8: return "0e1d69";
        default: return "000000";
        }
    }

    // 时空限制文本：多组限制取最小-最大范围，单组输出单个值
    // 时间单位 ms；内存单位 MiB（缓存里的 KB 值除以 1024）
    // latex_math 为 true 时范围用 $\sim$ 连接（LaTeX 语法），
    // 为 false 时用普通 "~"（Markdown 纯文本，避免输出 LaTeX 语法）
    inline std::pair<std::string, std::string> format_limits(const std::vector<int> &time,
                                                             const std::vector<int> &memory,
                                                             bool latex_math = true)
    {
        const std::string sim = latex_math ? "$\\sim $" : "~";
        std::pair<std::string, std::string> res; res.first = res.second = "";
        if (!time.empty())
        {
            auto [mn, mx] = std::minmax_element(time.begin(), time.end());
            res.first = (*mn == *mx ? std::to_string(*mn)
                                         : std::to_string(*mn) + sim + std::to_string(*mx)) +
                   "ms";
        }
        if (!memory.empty())
        {
            auto [mn, mx] = std::minmax_element(memory.begin(), memory.end());
            const long lo = *mn / 1024;
            const long hi = *mx / 1024;
            res.second  = (lo == hi ? std::to_string(lo)
                                        : std::to_string(lo) + sim + std::to_string(hi)) +
                              "MiB";
        }
        return res;
    }

    // 按 --show 规则过滤要显示的标签：show_all 为 true 时原样返回；
    // 为 false 时隐藏“算法”类（官方 type 2）标签，其余类型保留。
    // 标签类型来自 -U 生成的 tags.json，缓存缺失时全部保留。
    std::vector<std::string> filter_display_tags(const std::vector<std::string> &tags, bool show_all);

    // 返回 tags 中属于指定官方 type 的标签子集（顺序保持原样）。
    // 类型来自 -U 生成的 tags.json；tags.json 缺失或标签不在表中时不返回该标签。
    std::vector<std::string> filter_tags_by_type(const std::vector<std::string> &tags, int type);
}

#endif // LUOGU_EXTRACT_PROBLEM_INFO_H
