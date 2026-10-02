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

// include/luogu-extract/export/latex.h
#ifndef LUOGU_EXTRACT_LATEX_H
#define LUOGU_EXTRACT_LATEX_H

#include <filesystem>
#include <string>
#include "luogu-extract/contents/article.h"
#include "luogu-extract/contents/problem.h"
#include "luogu-extract/export/common.h"

namespace latex
{
    // -L 导出的显示选项
    struct Options
    {
        std::string lang = "zh-CN"; // 题面语言：zh-CN / en（英文缺失时回退中文）

        // 目录条目是否带跳转到对应题目的超链接（--no-toc-links 置为 false；默认 true）
        bool toc_links = true;
        // 页眉页码是否为跳回目录页的超链接（--toc-backlinks 置为 true；默认 false）
        bool toc_backlinks = false;
        // bilibili 视频 URL 是否输出为超链接（--no-bilibili-link 置为 false；默认 true）
        bool bilibili_links = true;

        // Markdown 一级标题（#）是否渲染成 \section（进目录、写页眉）而非 \section*。
        // --local 转写本地 Markdown 时置 true（一级标题即本文档章节标题）；
        // 题面/题解/文章正文里的一级标题保持默认，以免污染整册目录与页眉
        bool h1_as_section = false;

        // 题目信息显示开关（与 -M 共用同一组参数）
        luogu::DisplayOptions display;

        // 目录中的题目标题按难度着色（--show-contents-difficulty-tags；
        // 与 display.difficulty 相互独立）
        bool toc_difficulty = false;

        // 题目之间、文章之间分页（--paginate）：只在两次写入之间插入 \newpage，
        // 不写 \addcontentsline、不生成书签，因此目录与 PDF 书签不受影响
        bool paginate = false;

        // 下载题面图片时忽略缓存、全部重新下载（-RD, --new-download）。
        // 新图片先写临时文件再原子替换同名缓存，重新下载失败不影响原缓存
        bool new_download = false;

        // 字体设置：空串表示使用 ctex fontset / 代码字体回退链给出的默认字体；
        // 值可以是系统字体名，也可以是字体文件地址（main 中已规范化）
        std::string font_cover;    // 封面标题字体（--set-font-cover-page）
        std::string font_body_zh;  // 正文中文字体（--set-font-body-zh-CN）
        std::string font_body_en;  // 正文与题目大标题西文字体，不作用于公式
                                   // （--set-font-body-en-US）
        std::string font_code;     // 代码块与正文黑体部分（小标题）西文字体
                                   // （--set-font-body-codes）
        std::string font_title_zh; // 题目大标题/小节标题/目录/页眉中文字体
                                   // （--set-font-title-zh-CN）
        std::string font_title_en; // 小节标题/目录/页眉西文字体（--set-font-title-en-US；
                                   // 题目大标题西文跟随 font_body_en）

        // 封面标题文字（--set-cover-title；空串表示默认 "luogu extract"）
        std::string cover_title;

        // 题解与文章导出：题解包（nullptr 表示不导出题解），与题面同文件，
        // 位置由 solution_export.document_end 决定
        const luogu::SolutionBundle *solutions = nullptr;
        // --article 单独下载的文章（nullptr 表示没有），统一放在文档最后
        const luogu::ArticleBundle *articles = nullptr;
        luogu::SolutionExportOptions solution_export;
    };

    // 把一段 markdown / HTML 转为 LaTeX：标题映射为 \section 及更低层级（小节中文标题用
    // ctex 的 \heiti，西文用代码块字体）；图片映射为缓存文件（超宽/超高按比例缩小，小图
    // 不放大）；视频只输出链接；数学公式原样保留。
    // 折叠框（:::info / :::success / :::warning / :::error，可带 [标题]）用 mdframed 渲染，
    // 嵌套框按估算高度切成若干一页以内的小块（第二块起标题加「（续）」）以免被截断。
    // 依赖宏包：graphicx、hyperref、ulem、amsmath/amssymb、mdframed
    std::string markdown_to_latex(const std::string &markdown);

    // 把一题转为以 \section 开头的 LaTeX（难度/标签/作者/时空限制 + 各部分题面）。
    // first_solution_lid 非空且启用题解导出时，标题行右侧生成「查看题解」按钮
    std::string problem_to_latex(const problem::Problem &p, const Options &opt = {},
                                 const std::string &first_solution_lid = "");

    // 把一篇文章转为以 \section 开头的 LaTeX
    std::string article_to_latex(const article::Article &a);

    // 读取缓存并按条件筛选题目（与 -M 共用筛选逻辑），导出为可直接用 xelatex 编译的
    // LaTeX 文档（中文方案由 ctex fontset= 按系统选择，数学字体用 unicode-math）。
    // preselected 非空时复用已筛选的结果，避免重复解析题目列表缓存
    bool export_latex(const luogu::ExportFilter &filter,
                      const std::filesystem::path &output_path,
                      std::string &error,
                      const Options &opt = {},
                      const luogu::ProblemSelection *preselected = nullptr);

    // --local：把本地 Markdown 转写为 LaTeX 文档（不访问网络、不使用缓存）。
    // 编码自适应（BOM 判定 UTF-8/UTF-16/UTF-32，无 BOM 时先严格校验 UTF-8，
    // 否则按 GB18030 转码）；doc_only（--doc-only）时不输出封面、目录与页眉标题
    bool export_local_markdown(const std::filesystem::path &input_path,
                               const std::filesystem::path &output_path,
                               std::string &error,
                               const Options &opt = {},
                               bool doc_only = false);
}

#endif // LUOGU_EXTRACT_LATEX_H
