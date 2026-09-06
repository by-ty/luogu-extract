// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 by-ty
//
// This file is part of luogu-export-next
// (https://github.com/by-ty/luogu-export-next), a fork of luogu-export
// (https://github.com/sacharei/luogu-export) which is licensed under the
// MIT License (Copyright (c) 2026 sacharei); see the "Original MIT License"
// section in the LICENSE file.
//
// luogu-export-next is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation, either version 3 of the License, or (at
// your option) any later version. See the LICENSE file or
// https://www.gnu.org/licenses/lgpl-3.0.html for the full license text.
//
// luogu-export-next is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
// License for more details.

// include/luogu-export/export/common.h
#ifndef LUOGU_EXPORT_EXPORT_COMMON_H
#define LUOGU_EXPORT_EXPORT_COMMON_H

#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "luogu-export/contents/problem.h"

namespace luogu
{
    // -M / -L 共用的筛选条件
    struct ExportFilter
    {
        std::vector<std::string> tags;       // 标签：多个取“且”（题目必须全部包含）
        std::vector<int> difficulties;       // 难度：多个取“或”，已展开为单个数字
        std::vector<std::string> types;      // 题目类型：B / P，空表示全部
        std::string lang = "zh-CN";          // 题面语言：zh-CN / en
        std::string show = "11";             // 显示开关：第 1 位=难度，第 2 位=标签
        std::vector<std::string> pids;       // --pid：按题号精确筛选（多个取“或”）
        // --pid-range：按题号闭区间筛选（多组取“或”，两端点均包含；
        // 每组端点已规范化为大写且属于同一题库）
        std::vector<std::pair<std::string, std::string>> pid_ranges;
    };

    // 解析题号的组成部分：prefix（前导 ASCII 字母，转大写）、num（紧随的
    // 十进制数字，防溢出）、suffix（数字后的剩余部分，转大写；如 CF1000E
    // 的 "E"，通常为空）。前缀为空、前缀后无数字或数字溢出时返回 false。
    inline bool parse_pid_parts(const std::string &pid, std::string &prefix,
                                unsigned long long &num, std::string &suffix)
    {
        prefix.clear();
        num = 0;
        suffix.clear();
        size_t i = 0;
        while (i < pid.size() &&
               std::isalpha(static_cast<unsigned char>(pid[i])))
        {
            prefix += static_cast<char>(
                std::toupper(static_cast<unsigned char>(pid[i])));
            ++i;
        }
        if (prefix.empty() || i >= pid.size() ||
            !std::isdigit(static_cast<unsigned char>(pid[i])))
            return false;
        const unsigned long long kMax =
            std::numeric_limits<unsigned long long>::max();
        while (i < pid.size() &&
               std::isdigit(static_cast<unsigned char>(pid[i])))
        {
            const unsigned long long d =
                static_cast<unsigned long long>(pid[i] - '0');
            if (num > (kMax - d) / 10)
                return false; // 数字溢出
            num = num * 10 + d;
            ++i;
        }
        while (i < pid.size())
        {
            suffix += static_cast<char>(
                std::toupper(static_cast<unsigned char>(pid[i])));
            ++i;
        }
        return true;
    }

    // 按 (num, suffix) 比较两个题号组成部分（假设前缀已相同）：
    // 返回 <0 / 0 / >0 表示 a 在 b 之前 / 相等 / 之后。
    inline int compare_pid_parts(unsigned long long num_a,
                                 const std::string &suffix_a,
                                 unsigned long long num_b,
                                 const std::string &suffix_b)
    {
        if (num_a != num_b)
            return num_a < num_b ? -1 : 1;
        if (suffix_a != suffix_b)
            return suffix_a < suffix_b ? -1 : 1;
        return 0;
    }

    // 从缓存 latest.ndjson 中筛选题目并按题号排序（-M / -L 共用）。
    // resolved_tags 可选：返回解析后的标签名（数字 ID 已翻译成名称），
    // 用于在导出文件头描述筛选条件。
    bool select_problems(const ExportFilter &filter,
                         std::vector<problem::Problem> &problems,
                         std::vector<std::string> *resolved_tags,
                         std::string &error);

    // 生成筛选条件的中文说明；无筛选时返回空字符串
    std::string describe_filter(const ExportFilter &filter,
                                const std::vector<std::string> &resolved_tags);
}

#endif // LUOGU_EXPORT_EXPORT_COMMON_H
