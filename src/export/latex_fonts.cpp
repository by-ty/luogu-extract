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

#include "luogu-extract/export/latex_fonts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "luogu-extract/util/compat.h"

namespace
{

std::string lower_ascii(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_ascii(const std::string &s)
{
    const std::string::size_type a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    const std::string::size_type b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 去掉 /etc/os-release 值两侧的引号与空白。
std::string clean_os_release_value(const std::string &raw)
{
    std::string v = trim_ascii(raw);
    if (v.size() >= 2 &&
        ((v.front() == '"' && v.back() == '"') ||
         (v.front() == '\'' && v.back() == '\'')))
        v = v.substr(1, v.size() - 2);
    return v;
}

// 判断发行版标识或其派生标识是否属于 Ubuntu 系列。
// Ubuntu 官方衍生版的 ID 大多以 ubuntu 开头（Ubuntu Budgie、Ubuntu Kylin），
// 部分名称把 ubuntu 放在中间或结尾（Kubuntu、Xubuntu、Edubuntu），因此
// 这里统一检查 ID 是否包含 ubuntu；Linux Mint、elementary OS 等则在
// ID_LIKE 中声明 ubuntu。两种情形都选择 ctex 的 ubuntu 字体方案。
bool is_ubuntu_family(const std::string &id, const std::string &id_like)
{
    const std::string a = lower_ascii(id);
    if (a.find("ubuntu") != std::string::npos)
        return true;

    std::istringstream stream(lower_ascii(id_like));
    std::string token;
    while (stream >> token)
    {
        if (token == "ubuntu" || token.rfind("ubuntu", 0) == 0)
            return true;
    }
    return false;
}

std::string font_spec_escape(const std::string &t)
{
    std::string out;
    out.reserve(t.size());
    for (char c : t)
    {
        switch (c)
        {
        case '{': out += "\\{"; break;
        case '}': out += "\\}"; break;
        case '#': out += "\\#"; break;
        case '%': out += "\\%"; break;
        case '&': out += "\\&"; break;
        case '^': out += "\\textasciicircum{}"; break;
        case '~': out += "\\textasciitilde{}"; break;
        case '$': out += "\\$"; break;
        default: out += c;
        }
    }
    return out;
}

} // namespace

namespace latex
{

std::string ctex_fontset_name()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#else
    // Linux：运行阶段读取 /etc/os-release。该文件无法访问或缺少 ID 信息时
    // 使用 ctex 自带的 fandol 字体集（Fandol 字体随 TeX Live 分发，覆盖最好）。
    std::ifstream release("/etc/os-release");
    if (!release)
        return "fandol";

    std::string id;
    std::string id_like;
    std::string line;
    while (std::getline(release, line))
    {
        const std::string t = trim_ascii(line);
        if (t.empty() || t[0] == '#')
            continue;
        const std::string::size_type eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim_ascii(t.substr(0, eq));
        const std::string value = clean_os_release_value(t.substr(eq + 1));
        if (key == "ID")
            id = value;
        else if (key == "ID_LIKE")
            id_like = value;
    }

    if (id.empty())
        return "fandol";
    return is_ubuntu_family(id, id_like) ? "ubuntu" : "fandol";
#endif
}

std::string ctex_package_options()
{
    return "[UTF8,fontset=" + ctex_fontset_name() + "]";
}

std::string font_argument(const std::string &spec)
{
    std::string t = spec;
    std::replace(t.begin(), t.end(), '\\', '/');

    // 不含路径分隔符 -> 按已安装字体名称传给 fontspec。
    if (t.find('/') == std::string::npos)
        return "{" + font_spec_escape(t) + "}";

    // 字体文件地址：拆成 Path= / Extension= / 字体名三段，这是 fontspec
    // 加载字体文件最稳妥的写法，可避免路径中空格、中文等造成解析歧义。
    const std::filesystem::path p = luogu::compat::path_from_utf8(t);
    std::string dir = luogu::compat::path_to_utf8(p.parent_path());
    if (dir.empty())
        dir = ".";
    if (dir.back() != '/')
        dir += '/';

    std::string base = luogu::compat::path_to_utf8(p.filename());
    const std::string ext = luogu::compat::path_to_utf8(p.extension());
    if (ext.empty())
        return "[Path={" + font_spec_escape(dir) + "}]{./" + font_spec_escape(base) + "}";

    base = base.substr(0, base.size() - ext.size());
    return "[Path={" + font_spec_escape(dir) + "},Extension=" + font_spec_escape(ext) +
           "]{" + font_spec_escape(base) + "}";
}

void write_font_setup(FILE *out, const Options &opt)
{
    // 1. 正文：ctex fontset 已经提供默认 CJK 主字体；仅在用户显式指定时
    //    覆盖西文 / 中文主字体。先设置西文再设置中文，避免 xeCJK 重复定义
    //    默认 CJK 字体族。
    if (!opt.font_body_en.empty())
        std::fprintf(out, "\\setmainfont%s\n", font_argument(opt.font_body_en).c_str());
    if (!opt.font_body_zh.empty())
        std::fprintf(out, "\\setCJKmainfont%s\n", font_argument(opt.font_body_zh).c_str());

    // 2. 代码块西文等宽字体与“正文黑体部分”（Markdown 的 # 小标题、
    //    “题目描述”“输入输出样例”等固定小标题）的西文字体使用同一套
    //    选择逻辑：默认按“Consolas -> Menlo -> DejaVu Sans Mono”顺序
    //    回退（Windows 常见 Consolas，macOS 常见 Menlo，Linux TeX Live
    //    几乎必带 DejaVu Sans Mono），三个都不存在时保留 fontspec 默认
    //    等宽字体；用户传 --set-font-body-codes 时两者都用该字体。
    //    \luogoheadinglatin 用 \newfontfamily 定义，只切换西文字族，
    //    中文字体仍由 \heiti 等 CJK 字体命令控制，逻辑不变。
    if (opt.font_code.empty())
    {
        std::fputs(
            "\\IfFontExistsTF{Consolas}%\n"
            "  {\\setmonofont{Consolas}\\newfontfamily{\\luogoheadinglatin}{Consolas}}%\n"
            "  {\\IfFontExistsTF{Menlo}%\n"
            "    {\\setmonofont{Menlo}\\newfontfamily{\\luogoheadinglatin}{Menlo}}%\n"
            "    {\\IfFontExistsTF{DejaVu Sans Mono}%\n"
            "      {\\setmonofont{DejaVu Sans Mono}\\newfontfamily{\\luogoheadinglatin}{DejaVu Sans Mono}}%\n"
            "      {\\global\\let\\luogoheadinglatin\\relax}}}\n",
            out);
    }
    else
    {
        const std::string code_font = font_argument(opt.font_code);
        std::fprintf(out, "\\setmonofont%s\n", code_font.c_str());
        std::fprintf(out, "\\newfontfamily{\\luogoheadinglatin}%s\n", code_font.c_str());
    }

    // CJK 等宽字体不再单独强制 SimHei：ctex fontset 已经设置了与当前
    // 中文字体方案匹配的 \CJKmonofont，避免重新定义 CJKttdefault 的警告，
    // 也让 Linux/Windows/macOS 的默认行为各自一致。

    // 3. 标签徽章字体跟随正文字体：不再按操作系统单独指定思源黑体 /
    //    文泉驿微米黑，标签直接使用当前正文中西文字体（可由
    //    --set-font-body-zh-CN / --set-font-body-en-US 指定）。
    //    \tagsfonts 保留为空定义，标签渲染代码无需改动。
    std::fputs("\\newcommand{\\tagsfonts}{}\n", out);

    // 4. 用户指定的封面 / 标题字体。命令在标题格式中按需展开。
    if (!opt.font_cover.empty())
    {
        std::fprintf(out, "\\newfontfamily{\\luogocoverfont}%s\n",
                     font_argument(opt.font_cover).c_str());
        // 封面标题同时切换西文与 CJK 字体族，确保指定字体对中文封面也生效。
        std::fprintf(out, "\\newCJKfontfamily{\\luogocoverfontcjk}%s\n",
                     font_argument(opt.font_cover).c_str());
        std::fputs(
            "\\newcommand{\\luogocoverfontall}{%\n"
            "  \\luogocoverfont\\luogocoverfontcjk\n"
            "}\n",
            out);
    }
    if (!opt.font_title_zh.empty())
        std::fprintf(out, "\\newCJKfontfamily{\\luogotitlezh}%s\n",
                     font_argument(opt.font_title_zh).c_str());
    if (!opt.font_title_en.empty())
        std::fprintf(out, "\\newfontfamily{\\luogotitleen}%s\n",
                     font_argument(opt.font_title_en).c_str());

    // Markdown ## / ### / #### 小标题：中文默认使用 ctex fontset 预设黑体；
    // 西文默认与代码块同字体（\luogoheadinglatin）；若用户传了
    // --set-font-title-zh-CN / --set-font-title-en-US，则按对应维度改用
    // 用户标题字体，维持 --set-font-* 系列参数的最高优先级。
    std::fprintf(out, "\\newcommand{\\luogomarkdownheading}{%s%s}\n",
                 opt.font_title_zh.empty() ? "\\heiti" : "\\luogotitlezh",
                 opt.font_title_en.empty() ? "\\luogoheadinglatin" : "\\luogotitleen");
}

} // namespace latex
