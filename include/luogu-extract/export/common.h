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

// include/luogu-extract/export/common.h
#ifndef LUOGU_EXTRACT_EXPORT_COMMON_H
#define LUOGU_EXTRACT_EXPORT_COMMON_H

#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "luogu-extract/contents/problem.h"

namespace luogu
{
    // -M / -L 共用的筛选条件
    struct ExportFilter
    {
        std::vector<std::string> tags;       // 标签：多个取“且”（题目必须全部包含）
        std::vector<int> difficulties;       // 难度：多个取“或”，已展开为单个数字
        std::vector<std::string> types;      // 题目类型：B / P，空表示全部
        std::string lang = "zh-CN";          // 题面语言：zh-CN / en
        std::vector<std::string> pids;       // --pid：按题号精确筛选（多个取“或”）
        // --pid-range：按题号闭区间筛选（多组取“或”，两端点均包含；
        // 每组端点已规范化为大写且属于同一题库）
        std::vector<std::pair<std::string, std::string>> pid_ranges;
    };

    // -M / -L 共用的题目信息显示开关（默认值与 -L 一致）：
    // 显示来源、时间、区域、特殊题目标签，隐藏算法标签，不显示难度
    struct DisplayOptions
    {
        // --no-show-source-tags 置为 false：不显示来源（type 3）、时间（type 4）、
        // 区域（type 1）、特殊题目（type 5）标签
        bool source_tags = true;
        // --show-algorithm-tags 置为 true：显示算法（type 2）标签
        bool algorithm_tags = false;
        // --show-difficulty-tags 置为 true：显示「难度：<难度>」
        bool difficulty = false;
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

    // 洛谷题面用「图片语法 + bilibili: 伪协议」插入 B 站视频，支持的写法：
    //   ![](bilibili:221107)                  纯数字：省略 av 前缀的 av 号
    //   ![](bilibili:av53851218)              av 号
    //   ![](bilibili:BV1GJ411x7h7)            BV 号
    //   ![](bilibili:BV1bv411p7U5?page=4&t=82) 可带分 P（page）与起始位置（t，秒）
    // 本函数把这种伪链接补全成完整的视频网页 URL
    // （https://www.bilibili.com/video/<id>[?<query>]，query 原样保留）；
    // 不是可识别的 bilibili 伪链接（前缀不对、id/query 含非法字符）时返回空串，
    // 调用方据此保留原文。
    std::string bilibili_video_url(const std::string &url);

    // Markdown 导出用：把题面中的 B 站视频伪链接补全成指向视频网页的
    // Markdown 超链接。洛谷的视频写法在 Markdown 里只是指向 bilibili: 协议的
    // 坏图，这里改成 [文字](https://www.bilibili.com/video/...)：
    // - 图片语法 ![文字](bilibili:...) 的文字非空时作为链接文字，为空时用完整 URL；
    // - 普通链接语法 [文字](bilibili:...) 保留原有链接文字；
    // - 自动链接 <bilibili:...> 用完整 URL 作为链接文字；
    // 其余内容（含普通图片、普通链接、围栏代码块与行内代码）一律原样保留。
    std::string markdown_bilibili_links(const std::string &markdown);

    // 从缓存 latest.ndjson 中筛选题目并按题号排序（-M / -L 共用）。
    // resolved_tags 可选：返回解析后的标签名（数字 ID 已翻译成名称），
    // 用于在导出文件头描述筛选条件。
    bool select_problems(const ExportFilter &filter,
                         std::vector<problem::Problem> &problems,
                         std::vector<std::string> *resolved_tags,
                         std::string &error);

    // 生成筛选条件的中文说明；无筛选时返回空字符串。
    // display 可选：把「不显示…」一类的题目信息显示设置也一并写入说明
    std::string describe_filter(const ExportFilter &filter,
                                const std::vector<std::string> &resolved_tags,
                                const DisplayOptions &display = {});
}

#endif // LUOGU_EXTRACT_EXPORT_COMMON_H
