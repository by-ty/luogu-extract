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
#include <set>
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

// 按显示开关从题目标签中筛出要输出的标签：
// 算法（官方 type 2）标签仅在 show_algorithm 时显示，其余标签（来源、时间、
// 区域、特殊题目等）仅在 show_others 时显示；结果保持缓存中的原顺序。
// 标签类型来自 -U 生成的 tags.json，缓存缺失时按“非算法标签”处理。
std::vector<std::string> visible_tags(const std::vector<std::string> &tags,
                                      bool show_algorithm, bool show_others)
{
    if (!show_algorithm && !show_others)
        return {};

    const std::vector<std::string> algorithm =
        show_algorithm ? luogu::filter_tags_by_type(tags, 2)
                       : std::vector<std::string>();
    const std::vector<std::string> others =
        show_others ? luogu::filter_display_tags(tags, false)
                    : std::vector<std::string>();

    std::set<std::string> visible(algorithm.begin(), algorithm.end());
    visible.insert(others.begin(), others.end());

    std::vector<std::string> out;
    for (const auto &t : tags)
    {
        if (visible.count(t))
            out.push_back(t);
    }
    return out;
}


// 把远端文本压成单行：题号、标题、原文链接等来自缓存的数据可能夹带换行
// （problem.cpp 的控制字符过滤保留了 \t \n \r），直接写进「# <题号> <标题>」
// 这类由我们生成的单行结构会把标题断成两行，甚至凭空多出一个标题行。
// 只替换换行与制表符，不转义 Markdown 特殊字符：正文仍按原文输出，
// 避免破坏正常排版。
std::string single_line(std::string text)
{
    for (char &c : text)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return text;
}

// 输出一篇题解或文章（Markdown 侧，设计 §10.4）：
// - 显式 HTML 锚点：中文标题的自动锚点在不同渲染器下不一致，必须显式指定；
// - 题解 → 题目 的「返回题目」与 题目 → 题解 的「查看题解」两个开关独立；
//   按文章下载的文章（--article）不与题目绑定，没有「返回题目」链接；
// - 元信息块（来源 / 原文）由 --no-article-meta 关闭；
// - 正文按洛谷 Markdown 原文输出（Markdown 侧既有语义）。
bool write_markdown_article(FILE *out,
                            const std::function<bool(const std::string &)> &write_str,
                            const std::function<bool()> &fail_write,
                            const std::string &heading, const std::string &anchor,
                            const std::string &problem_pid,
                            const luogu::SolutionView &view,
                            const luogu::SolutionExportOptions &opt)
{
    std::fprintf(out, "<a id=\"%s\"></a>\n\n", anchor.c_str());
    std::fprintf(out, "### %s\n\n", heading.c_str());
    if (!problem_pid.empty() && opt.article_to_problem_link)
        std::fprintf(out, "<a href=\"#%s\">返回题目</a>\n\n",
                     luogu::problem_anchor(problem_pid).c_str());
    if (opt.article_meta)
    {
        std::fprintf(out, "> 来源：%s\n", view.source_name.c_str());
        std::fprintf(out, "> 原文：%s\n\n", single_line(view.source_url).c_str());
    }
    if (!view.content_full)
        std::fputs("> 注意：本篇正文不完整（因 --allow-partial 导出）。\n\n", out);

    const std::string content = luogu::markdown_bilibili_links(view.content);
    if (!content.empty())
    {
        if (!write_str(content))
            return fail_write();
        if (content.back() != '\n')
            std::fputs("\n", out);
        std::fputs("\n", out);
    }
    return true;
}

} // namespace

bool markdown::export_markdown(const luogu::ExportFilter &filter,
                               const std::filesystem::path &output_path,
                               std::string &error,
                               const std::string &cover_title,
                               const luogu::DisplayOptions &display,
                               const luogu::SolutionBundle *solutions,
                               const luogu::SolutionExportOptions &solution_export,
                               const luogu::ProblemSelection *preselected,
                               const luogu::ArticleBundle *articles)
{
    error.clear();

    // 筛选（-M / -L 共用），结果已按题号排序。
    // 题解流程已筛选过一次时直接复用（preselected），避免重复解析题目列表缓存
    std::vector<problem::Problem> local_problems;
    std::vector<std::string> resolved_tags;
    if (preselected)
    {
        local_problems = preselected->problems;
        resolved_tags = preselected->resolved_tags;
    }
    else if (!luogu::select_problems(filter, local_problems, &resolved_tags, error))
    {
        return false;
    }
    const std::vector<problem::Problem> &problems = local_problems;

    // 显示开关（与 -L 共用同一组参数）：
    // --show-difficulty-tags 显示难度（默认不显示）；
    // --show-algorithm-tags 显示算法（type 2）标签（默认隐藏）；
    // --no-show-source-tags 隐藏算法以外的标签（来源/时间/区域/特殊等，默认显示）
    const bool show_difficulty = display.difficulty;
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

    // 题解导出相关（设计 §十）：题解与题面同文件，默认统一置于文档最后
    const bool with_solutions = solutions != nullptr && solution_export.enabled;
    // --article 的文章（与题解同文件，统一放在文档最后）
    const bool with_articles = articles != nullptr && !articles->items.empty();
    size_t solution_total = 0;
    if (with_solutions)
        for (const auto &item : solutions->items)
            solution_total += item.solutions.size();

    // 一级标题：--set-cover-title 指定时使用指定标题，否则用默认标题
    const std::string cover = cover_title.empty() ? "洛谷题目导出" : cover_title;
    std::string header_count;
    if (solution_export.articles_only)
    {
        // --solutions-only：文档里没有题面，只统计题解与文章
        header_count = "共 " + std::to_string(solution_total) + " 篇题解";
        if (with_articles)
            header_count += "，" + std::to_string(articles->items.size()) + " 篇文章";
    }
    else if (solution_export.export_problems)
    {
        header_count = "共 " + std::to_string(problems.size()) + " 道题";
        if (with_solutions)
            header_count += "，" + std::to_string(solution_total) + " 篇题解";
        if (with_articles)
            header_count += "，" + std::to_string(articles->items.size()) + " 篇文章";
    }
    else
    {
        // --article 单独使用：文档只含文章
        header_count = "共 " +
                       std::to_string(with_articles ? articles->items.size() : 0) +
                       " 篇文章";
    }
    std::fprintf(out, "# %s（%s）\n\n", cover.c_str(), header_count.c_str());

    std::string conds = luogu::describe_filter(filter, resolved_tags, display);
    if (!solution_export.export_problems)
        conds = "无（未指定题目筛选条件，仅导出文章）";
    else if (solution_export.articles_only && conds.empty())
        conds = "无（未指定题目筛选条件，仅导出题解）";
    else if (conds.empty())
        conds = "无（导出全部题目）";
    std::fputs("筛选条件：", out);
    if (!write_str(conds))
        return fail_write();
    std::fputs("\n\n", out);

    // 渲染一道题的题面（难度 / 标签 / 时空限制 + 背景 / 描述 / 输入输出格式 /
    // 样例 / 说明提示）。--solutions-only 只导出题解与文章、不导出题面，这时
    // 不调用本函数（题解仍按题目顺序单独输出，见下方 per-problem 分支）。
    // sol_set 为该题的题解（题面处的「查看题解」链接用），可为 nullptr。
    auto write_problem = [&](const problem::Problem &p,
                             const luogu::ProblemSolutionSet *sol_set) -> bool
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

        // B 站视频在题面里写作 ![](bilibili:221107) 这类图片语法，Markdown 里
        // 会变成指向 bilibili: 伪协议的坏图；补全为指向视频网页的超链接
        // （-L 的 --no-bilibili-link 仅对 LaTeX 生效，Markdown 始终输出链接）
        background = luogu::markdown_bilibili_links(background);
        description = luogu::markdown_bilibili_links(description);
        formatI = luogu::markdown_bilibili_links(formatI);
        formatO = luogu::markdown_bilibili_links(formatO);
        hint = luogu::markdown_bilibili_links(hint);

        std::fputs("---\n\n", out);
        std::fprintf(out, "# %s %s\n\n", single_line(p.pid).c_str(),
                     single_line(title).c_str());

        // 题面处的「查看题解」链接：目标为文末该题第一篇题解的显式锚点
        // （中文标题的自动锚点在不同 Markdown 渲染器下不一致，必须显式指定）
        if (with_solutions && solution_export.problem_to_article_link && sol_set &&
            !sol_set->solutions.empty())
        {
            const std::string anchor = luogu::solution_anchor(
                p.pid, sol_set->solutions.front().lid);
            std::fprintf(out, "<a href=\"#%s\">查看题解</a>\n\n", anchor.c_str());
        }

        if (show_difficulty)
            std::fprintf(out, "难度：%s\n\n", luogu::difficulty_label(p.difficulty));

        // 标签：算法（type 2）标签默认隐藏（--show-algorithm-tags 时显示），
        // 其余标签默认显示（--no-show-source-tags 时隐藏）；两类都不显示时
        // 不输出「标签」一栏
        const std::vector<std::string> shown_tags =
            visible_tags(p.tags, display.algorithm_tags, display.source_tags);
        if (!shown_tags.empty())
            std::fprintf(out, "标签：%s\n\n",
                         single_line(join_strings(shown_tags, "、")).c_str());

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
        return true;
    };

    for (const auto &p : problems)
    {
        const luogu::ProblemSolutionSet *sol_set =
            with_solutions ? solutions->find(p.pid) : nullptr;

        // --solutions-only：只导出题解与文章、不导出题面（与 -L 一致）
        if (!solution_export.articles_only && !write_problem(p, sol_set))
            return false;

        // --solution-placement per-problem：该题的题解紧跟题面之后
        if (with_solutions && !solution_export.document_end && sol_set)
        {
            for (const auto &view : sol_set->solutions)
            {
                if (!write_markdown_article(
                        out, write_str, fail_write,
                        luogu::solution_heading(view.title),
                        luogu::solution_anchor(p.pid, view.lid), p.pid, view,
                        solution_export))
                    return false;
            }
        }
    }

    // --solution-placement document-end（默认）：题解统一置于文档最后，
    // 每题一组、同题题解连续排列
    if (with_solutions && solution_export.document_end)
    {
        std::fputs("---\n\n# 题解\n\n", out);
        for (const auto &item : solutions->items)
        {
            if (item.solutions.empty())
                continue; // 该题无可用题解：不生成小节（也就不产生死链）
            std::fprintf(out, "## %s %s\n\n", single_line(item.pid).c_str(),
                         single_line(item.problem_title).c_str());
            for (const auto &view : item.solutions)
            {
                if (!write_markdown_article(
                        out, write_str, fail_write,
                        luogu::solution_heading(view.title),
                        luogu::solution_anchor(item.pid, view.lid), item.pid, view,
                        solution_export))
                    return false;
            }
        }
    }

    // --article：文章统一置于文档最后（同时下载题解与文章时，文章在题解之后）。
    // 文章不与题目绑定，因此没有题目分组标题，也没有「返回题目」链接
    if (with_articles)
    {
        std::fputs("---\n\n# 文章\n\n", out);
        for (const auto &view : articles->items)
        {
            if (!write_markdown_article(out, write_str, fail_write,
                                        luogu::article_heading(view.title),
                                        luogu::article_anchor(view.lid),
                                        std::string(), view, solution_export))
                return false;
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

    // 原子替换目标文件（覆盖已存在的旧输出）。
    // Windows 下必须用 compat::atomic_replace：MinGW-w64 的
    // std::filesystem::rename 走 _wrename，目标已存在时会直接失败，
    // 第二次导出同名文件就会报错（见 util/compat.h 的 atomic_replace）
    std::string replace_error;
    if (!luogu::compat::atomic_replace(tmp_path, output_path, replace_error))
    {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "无法把输出文件写入 '" + luogu::compat::path_to_utf8(output_path) +
                "': " + replace_error;
        return false;
    }
    return true;
}
