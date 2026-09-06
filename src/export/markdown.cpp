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

// src/export/markdown.cpp
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "luogu-extract/contents/problem.h"
#include "luogu-extract/export/common.h"
#include "luogu-extract/export/markdown.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/problem_info.h"

using nlohmann::json;

namespace
{

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

// 多语言字段取值：键存在但为 null 时按缺省处理
std::string safe_string(const json &j, const char *key)
{
    if (!j.contains(key) || !j[key].is_string())
        return "";
    return j[key].get<std::string>();
}

} // namespace

bool markdown::export_markdown(const luogu::ExportFilter &filter,
                               const std::filesystem::path &output_path,
                               std::string &error,
                               const std::string &cover_title)
{
    error.clear();

    // 筛选（-M / -L 共用），结果已按题号排序
    std::vector<problem::Problem> problems;
    std::vector<std::string> resolved_tags;
    if (!luogu::select_problems(filter, problems, &resolved_tags, error))
        return false;

    // 显示开关：第 1 位 = 难度，第 2 位 = 标签
    const bool show_difficulty = (filter.show.size() >= 2 && filter.show[0] == '1');
    const bool show_tags = (filter.show.size() >= 2 && filter.show[1] == '1');
    const bool use_en = (filter.lang == "en");

    // 输出采用“临时文件 + fsync + rename”的原子写：
    // 导出中途失败不会留下半截文件覆盖旧输出
    const std::filesystem::path tmp_path = luogu::compat::temp_sibling_path(output_path);
    FILE *out = luogu::compat::fopen(tmp_path, "w");
    if (!out)
    {
        error = "无法打开输出文件 '" + luogu::compat::path_to_utf8(output_path) + "'";
        return false;
    }

    // 把字符串内容按字节数完整写出：内容里意外出现 NUL 等控制字符时
    // 不会被 C 字符串终止符静默截断（解析阶段已过滤控制字符，这里兜底）
    auto write_str = [&](const std::string &s) -> bool {
        return std::fwrite(s.data(), 1, s.size(), out) == s.size();
    };
    auto fail_write = [&]() {
        std::fclose(out);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
        return false;
    };

    // 一级标题：--set-cover-title 指定时使用指定标题，否则用默认标题
    const std::string cover = cover_title.empty() ? "洛谷题目导出" : cover_title;
    std::fprintf(out, "# %s（共 %zu 道题）\n\n", cover.c_str(), problems.size());

    const std::string conds = luogu::describe_filter(filter, resolved_tags);
    std::fputs("筛选条件：", out);
    if (!write_str(conds.empty() ? "无（导出全部题目）" : conds))
        return fail_write();
    std::fputs("\n\n", out);

    for (const auto &p : problems)
    {
        // 题面语言：英文优先取 translations，缺失时回退中文
        std::string title = p.name;
        std::string background = p.background;
        std::string description = p.description;
        std::string formatI = p.formatI;
        std::string formatO = p.formatO;
        std::string hint = p.hint;
        if (use_en)
        {
            std::string en = safe_string(p.translations, "title");
            if (!en.empty()) title = en;
            en = safe_string(p.translations, "background");
            if (!en.empty()) background = en;
            en = safe_string(p.translations, "description");
            if (!en.empty()) description = en;
            en = safe_string(p.translations, "inputFormat");
            if (!en.empty()) formatI = en;
            en = safe_string(p.translations, "outputFormat");
            if (!en.empty()) formatO = en;
            en = safe_string(p.translations, "hint");
            if (!en.empty()) hint = en;
        }

        std::fputs("---\n\n", out);
        std::fprintf(out, "# %s %s\n\n", p.pid.c_str(), title.c_str());

        if (show_difficulty)
            std::fprintf(out, "难度：%s\n\n", luogu::difficulty_label(p.difficulty));

        // 标签：--show 末位为 0 时仅隐藏“算法”类（type 2）标签，其余类型始终显示
        std::vector<std::string> shown_tags = luogu::filter_display_tags(p.tags, show_tags);
        if (!shown_tags.empty())
            std::fprintf(out, "标签：%s\n\n", join_strings(shown_tags, "、").c_str());

        // 时空限制：多组限制输出最小-最大范围；Markdown 用纯文本 "~"
        // （format_limits 的 LaTeX 数学写法 $\sim$ 不适用于 Markdown）。
        // 没有时空限制数据时不输出这两行
        const pss limits = luogu::format_limits(p.time, p.memory, false);
        if (!limits.first.empty() || !limits.second.empty())
        {
            std::string limits_text;
            if (!limits.first.empty())
                limits_text += "时间限制: " + limits.first;
            if (!limits.second.empty())
            {
                if (!limits_text.empty())
                    limits_text += "\n";
                limits_text += "内存限制: " + limits.second;
            }
            limits_text += "\n";
            if (!write_str(limits_text))
                return fail_write();
            std::fputs("\n", out);
        }

        if (!background.empty())
        {
            std::fputs("## 题目背景\n\n", out);
            if (!write_str(background))
                return fail_write();
            std::fputs("\n\n", out);
        }
        if (!description.empty())
        {
            std::fputs("## 题目描述\n\n", out);
            if (!write_str(description))
                return fail_write();
            std::fputs("\n\n", out);
        }
        if (!formatI.empty())
        {
            std::fputs("## 输入格式\n\n", out);
            if (!write_str(formatI))
                return fail_write();
            std::fputs("\n\n", out);
        }
        if (!formatO.empty())
        {
            std::fputs("## 输出格式\n\n", out);
            if (!write_str(formatO))
                return fail_write();
            std::fputs("\n\n", out);
        }

        int sample_no = 1;
        for (const auto &s : p.samples)
        {
            std::fprintf(out, "## 输入输出样例 #%d\n\n", sample_no);
            std::fprintf(out, "### 输入 #%d\n\n```\n", sample_no);
            if (!write_str(s.first))
                return fail_write();
            // 样例末尾没有换行时补一个，否则闭合代码围栏会紧跟内容（1 2```）
            if (s.first.empty() || s.first.back() != '\n')
                std::fputs("\n", out);
            std::fputs("```\n\n", out);
            std::fprintf(out, "### 输出 #%d\n\n```\n", sample_no);
            if (!write_str(s.second))
                return fail_write();
            if (s.second.empty() || s.second.back() != '\n')
                std::fputs("\n", out);
            std::fputs("```\n\n", out);
            ++sample_no;
        }

        if (!hint.empty())
        {
            std::fputs("## 说明/提示\n\n", out);
            if (!write_str(hint))
                return fail_write();
            std::fputs("\n\n", out);
        }
    }

    if (std::ferror(out))
        return fail_write();
    // 落盘并 fsync；失败时清理临时文件（注意避免对已关闭的流二次 fclose）
    if (!luogu::compat::flush_and_sync(out))
    {
        std::fclose(out);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
        return false;
    }
    if (std::fclose(out) != 0)
    {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
        return false;
    }

    // 原子替换目标文件
    std::error_code ec;
    std::filesystem::rename(tmp_path, output_path, ec);
    if (ec)
    {
        std::filesystem::remove(tmp_path, ec);
        error = "无法把输出文件写入 '" + luogu::compat::path_to_utf8(output_path) +
                "': " + ec.message();
        return false;
    }
    return true;
}
