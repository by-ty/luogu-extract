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
        std::string show = "00";    // 第 1 位=难度，第 2 位=标签；1 显示 0 隐藏。
                                    // 默认不显示难度；标签位为 0 时仅隐藏“算法”类
                                    // （type 2），其他类型（来源/年份/地区/特殊）始终显示

        // 目录条目是否带跳转到对应题目的超链接（--no-toc-links 置为 false；默认 true）
        bool toc_links = true;
        // 页眉页码是否为跳回目录页的超链接（--toc-backlinks 置为 true；默认 false）
        bool toc_backlinks = false;
        // bilibili 视频 URL 是否输出为超链接（--no-bilibili-link 置为 false；默认 true）
        bool bilibili_links = true;

        // 来源（竞赛来源 type 3）、时间（年份 type 4）、区域（地区/赛区 type 1）、
        // 特殊题目（type 5）标签是否显示（--no-show-source-tags 置为 false；
        // 默认 true）。上述四类标签与算法标签都隐藏时不输出「标签：」一栏。
        bool source_tags = true;
        // 算法（type 2）标签是否显示（--show-algorithm-tags 置为 true；
        // 默认 false）。显示时使用蓝色背景 rgb(41,73,180)，并排在其他标签之前。
        bool algorithm_tags = false;
        // 是否在时间限制、内存限制之下显示「难度：<难度>」
        // （--show-difficulty-tags 置为 true；默认 false）。
        // <难度> 的字体颜色与洛谷网页一致（luogu::difficulty_color）。
        bool difficulty = false;
        // 目录中的题目标题是否按难度着色（--show-contents-difficulty-tags
        // 置为 true；默认 false，即黑色）。与 difficulty 相互独立。
        bool toc_difficulty = false;

        // 字体设置：空串表示使用 ctex fontset / 代码字体回退链给出的默认字体。
        // 值既可以是系统已安装的字体名称，也可以是字体文件地址（main 中已规范化）。
        std::string font_cover;    // 封面标题字体（--set-font-cover-page）
        std::string font_body_zh;  // 正文中文字体（--set-font-body-zh-CN）
        std::string font_body_en;  // 正文及题目大标题西文字体，不作用于公式
                                    // （--set-font-body-en-US）
        std::string font_code;     // 代码块，以及正文黑体部分（小标题）西文字体
                                    // （--set-font-body-codes）
        std::string font_title_zh; // 题目大标题/小节标题/目录/页眉中文字体
                                    // （--set-font-title-zh-CN）
        std::string font_title_en; // 小节标题/目录/页眉西文字体（--set-font-title-en-US；
                                    // 题目大标题西文跟随 font_body_en）

        // 封面标题文字（--set-cover-title；空串表示默认 "luogu extract"）
        std::string cover_title;
    };

    // 把一段 markdown / HTML 文本转换为 LaTeX。
    // 标题映射为 \section 及更低层级；小节中文标题默认使用 ctex 预设
    // 黑体 \heiti（含 Markdown 的 ## / ### / ####），其西文使用代码块字体
    // （见 latex_fonts.h 的 write_font_setup）。图片链接映射为缓存中的文件
    // （crawler::image_cache_path），超宽/超高图片按比例缩小到版心内，小图片
    // 不放大；视频（Bilibili 等）只输出链接；
    // 数学公式原样保留。
    // 依赖的宏包：graphicx、hyperref、ulem（删除线）、amsmath/amssymb（公式/任务框）。
    std::string markdown_to_latex(const std::string &markdown);

    // 把一题转换为以 \section 开头的 LaTeX 内容（结构同 markdown 导出：
    // 难度/标签/作者/时空限制 + 背景/描述/输入输出格式/样例/提示）
    std::string problem_to_latex(const problem::Problem &p, const Options &opt = {});

    // 把一篇文章转换为以 \section 开头的 LaTeX 内容
    std::string article_to_latex(const article::Article &a);

    // 读取缓存并按条件筛选题目（与 -M 共用筛选逻辑），
    // 导出为一份完整的、可直接用 xelatex 编译的 LaTeX 文档。
    // 默认中文方案由 ctex fontset= 按操作系统选择；数学字体使用 unicode-math。
    // @param filter      筛选条件
    // @param output_path 输出 .tex 文件路径
    // @param error       失败时返回的错误信息
    // @param opt         显示选项（目录超链接/回链、字体、封面标题、bilibili 链接等）
    // @return 成功返回 true
    bool export_latex(const luogu::ExportFilter &filter,
                      const std::filesystem::path &output_path,
                      std::string &error,
                      const Options &opt = {});
}

#endif // LUOGU_EXTRACT_LATEX_H
