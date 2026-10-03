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

// src/export/latex.cpp
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <charconv>
#include <functional>
#include <limits>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include "luogu-extract/contents/article.h"
#include "luogu-extract/contents/problem.h"
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/export/common.h"
#include "luogu-extract/export/latex.h"
#include "luogu-extract/export/latex_fonts.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/image_util.h"
#include "luogu-extract/util/problem_info.h"
#include "luogu-extract/util/prompt.h"
#include "luogu-extract/util/text_encoding.h"
#include "luogu-extract/util/version.h"

namespace
{

// 解析 [first, last) 内的非负十进制数字；失败或溢出返回 false（std::stoi 对缓存里的畸形内容会抛异常）。
bool parse_nonneg_int(const char *first, const char *last, size_t &out)
{
    if (first >= last)
        return false;
    size_t v = 0;
    for (const char *p = first; p != last; ++p)
    {
        if (*p < '0' || *p > '9')
            return false;
        const size_t d = static_cast<size_t>(*p - '0');
        if (v > (std::numeric_limits<size_t>::max() - d) / 10)
            return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos)
        return "";
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// 是否代码围栏的收尾行（传入已 trim 的行）：CommonMark 要求 >= fence_len 个同类围栏字符且其后只有空白；
// 只按行首计数会把块内的 ```cpp 误判为收尾行，导致代码块提前结束、块内内容错位到正文。
bool is_fence_closer(const std::string &trimmed_line, char fence, size_t fence_len)
{
    if (fence_len == 0 || trimmed_line.size() < fence_len)
        return false;
    size_t n = 0;
    while (n < trimmed_line.size() && trimmed_line[n] == fence)
        ++n;
    if (n < fence_len)
        return false;
    for (size_t k = n; k < trimmed_line.size(); ++k)
        if (!std::isspace(static_cast<unsigned char>(trimmed_line[k])))
            return false;
    return true;
}

// 表格单元格空白清理：洛谷用全角空格（U+3000）等 Unicode 空白对齐源码，残留填充会把居中/右对齐的文字
// 挤偏（如「|测试点编号　　|」让表头相对列中心左偏一个全角空格）。下面列出所有要剥掉的空白字符。
std::string trim_cell(const std::string &s)
{
    static const char *kSpaces[] = {
        "\xE3\x80\x80",
        "\xC2\xA0",
        "\xE2\x80\x80",
        "\xE2\x80\x81",
        "\xE2\x80\x82",
        "\xE2\x80\x83",
        "\xE2\x80\x84",
        "\xE2\x80\x85",
        "\xE2\x80\x86",
        "\xE2\x80\x87",
        "\xE2\x80\x88",
        "\xE2\x80\x89",
        "\xE2\x80\x8A",
        "\xE2\x80\x8B",
        "\xE2\x80\xAF",
        "\xE2\x81\xA0",
        "\xEF\xBB\xBF",
    };
    std::string out = trim(s);
    bool changed = true;
    while (changed && !out.empty())
    {
        changed = false;
        for (const char *space : kSpaces)
        {
            const size_t n = std::strlen(space);
            if (out.size() >= n && out.compare(0, n, space) == 0)
            {
                out.erase(0, n);
                changed = true;
            }
            if (out.size() >= n && out.compare(out.size() - n, n, space) == 0)
            {
                out.erase(out.size() - n);
                changed = true;
            }
        }
        if (changed)
            out = trim(out);
    }
    return out;
}

std::string to_lower_ascii(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 跳过 s[pos]（必须是 '{'）开始的配对花括号组，返回组后第一个字符的下标；找不到配对时返回 s.size()
size_t skip_brace_group(const std::string &s, size_t pos)
{
    int d = 1;
    size_t i = pos + 1;
    while (i < s.size() && d > 0)
    {
        if (s[i] == '{')
            ++d;
        else if (s[i] == '}')
            --d;
        ++i;
    }
    return d == 0 ? i : s.size();
}

// 统计 array/subarray 列规格的列数：只数花括号外的列字母（l/c/r/p/m/b 与 *{n}{...} 展开的列），
// @{...}/>{...}/<{...} 的内容与 p{2cm} 这类参数里的字母都不算列。
size_t count_array_columns(const std::string &spec)
{
    size_t columns = 0;
    size_t i = 0;
    while (i < spec.size())
    {
        const char c = spec[i];
        if ((c == '@' || c == '>' || c == '<') && i + 1 < spec.size() &&
            spec[i + 1] == '{')
        {
            i = skip_brace_group(spec, i + 1);
            continue;
        }
        if (c == '{')
        {
            i = skip_brace_group(spec, i);
            continue;
        }
        if (c == '*' && i + 1 < spec.size() && spec[i + 1] == '{')
        {
            const size_t after_n = skip_brace_group(spec, i + 1);
            size_t repeat = 1;
            size_t digits = 0;
            for (size_t k = i + 2; k + 1 < after_n && k < spec.size(); ++k)
            {
                if (!std::isdigit(static_cast<unsigned char>(spec[k])))
                    break;
                if (repeat > 1000)
                    break; // 畸形规格：不再放大，避免计数溢出
                repeat = repeat * 10 + static_cast<size_t>(spec[k] - '0');
                ++digits;
            }
            if (digits == 0)
                repeat = 1;
            if (after_n < spec.size() && spec[after_n] == '{')
            {
                const size_t after_spec = skip_brace_group(spec, after_n);
                columns += repeat * count_array_columns(
                                       spec.substr(after_n + 1, after_spec - after_n - 2));
                i = after_spec;
                continue;
            }
            i = after_n;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)))
            ++columns;
        ++i;
    }
    return columns;
}

// 列规格是否只用了 array 认识的写法：是则原样保留（保住 l/c/r 对齐与 p{} 定宽列），
// 否则退回「全部 c 列」——丢对齐，但一定不会因为未加载的列类型/命令而编译失败。
bool array_spec_is_safe(const std::string &spec)
{
    static const std::string kAllowed = "lcrpmb|@{}<>*0123456789. ";
    for (char c : spec)
        if (kAllowed.find(c) == std::string::npos)
            return false;
    return true;
}

// 对齐环境行归一化：洛谷题面里 \begin{array}{c} 等常有多余/缺少的 &（报 Extra alignment tab），
// 把每行统一到目标列数（array 按列规格，矩阵按最大行宽）。
std::string regex_transform(const std::string &s, const std::regex &re,
                            const std::function<std::string(const std::smatch &)> &convert);

std::string normalize_alignment(std::string s, int depth = 0)
{
    // 嵌套对齐环境的最大递归深度（正常题面远低于此值）
    static const int kMaxEnvDepth = 32;
    (void)depth;
    static const std::set<std::string> kEnvs = {
        "array", "matrix", "pmatrix", "bmatrix", "vmatrix", "Vmatrix",
        "smallmatrix", "aligned", "alignedat", "gathered", "cases", "dcases",
        "rcases", "split", "subarray", "matrix*", "pmatrix*", "bmatrix*",
        "cases*",
    };
    // 这些环境不接受 &（如 gathered），行内多余的 & 只能去掉
    static const std::set<std::string> kNoAmpEnvs = {"gathered"};

    std::string out;
    size_t p = 0;
    while (p < s.size())
    {
        if (s.compare(p, 7, "\\begin{") != 0)
        {
            out += s[p];
            ++p;
            continue;
        }

        const size_t name_end = s.find('}', p + 7);
        if (name_end == std::string::npos)
        {
            out += s[p];
            ++p;
            continue;
        }
        const std::string name = s.substr(p + 7, name_end - p - 7);
        if (!kEnvs.count(name))
        {
            out += s[p];
            ++p;
            continue;
        }

        size_t body_start = name_end + 1;
        size_t spec_cols = 0;
        std::string spec;
        if (name == "array" || name == "subarray")
        {
            if (body_start < s.size() && s[body_start] == '{')
            {
                // 列规格里可以再嵌花括号（p{2cm}、@{...}、>{\cmd}），必须按配对找真正的收尾 }：
                // 取第一个 } 会把 {p{2cm}} 截成 "p{2cm"，多余的 } 还会落进表格正文（报 Extra }）。
                const size_t spec_end = skip_brace_group(s, body_start);
                if (spec_end <= s.size() && spec_end > body_start + 1 &&
                    s[spec_end - 1] == '}')
                {
                    spec = s.substr(body_start + 1, spec_end - body_start - 2);
                    spec_cols = count_array_columns(spec);
                    body_start = spec_end;
                }
            }
        }

        const std::string endtag = "\\end{" + name + "}";
        // 用同名环境深度找匹配的 \end{name}（洛谷题面有嵌套同名环境）
        size_t end_pos = std::string::npos;
        {
            size_t q = body_start;
            int name_depth = 1;
            while (q < s.size())
            {
                if (s.compare(q, 7, "\\begin{") == 0)
                {
                    const size_t ne = s.find('}', q + 7);
                    if (ne != std::string::npos &&
                        s.substr(q + 7, ne - q - 7) == name)
                        ++name_depth;
                    q = (ne != std::string::npos) ? ne + 1 : q + 7;
                    continue;
                }
                if (s.compare(q, endtag.size(), endtag) == 0)
                {
                    --name_depth;
                    if (name_depth == 0)
                    {
                        end_pos = q;
                        break;
                    }
                    q += endtag.size();
                    continue;
                }
                ++q;
            }
        }
        if (end_pos == std::string::npos)
        {
            out += s[p];
            ++p;
            continue;
        }
        // 先递归处理嵌套的对齐环境（内层列规格、行内 & 等）；限制递归深度，防止恶意超深嵌套导致栈溢出。
        std::string body = (depth >= kMaxEnvDepth)
                               ? s.substr(body_start, end_pos - body_start)
                               : normalize_alignment(
                                     s.substr(body_start, end_pos - body_start),
                                     depth + 1);

        // 去掉多余的 &&（洛谷常见写法，LaTeX 报 Extra alignment tab）；只处理本层，跳过嵌套环境内部。
        {
            std::string t;
            int d = 0;
            int ed = 0;
            size_t k = 0;
            while (k < body.size())
            {
                if (body.compare(k, 7, "\\begin{") == 0)
                {
                    ++ed;
                    t += body.substr(k, 7);
                    k += 7;
                    continue;
                }
                if (body.compare(k, 5, "\\end{") == 0)
                {
                    if (ed > 0)
                        --ed;
                    t += body.substr(k, 5);
                    k += 5;
                    continue;
                }
                if (body[k] == '{') ++d;
                else if (body[k] == '}' && d > 0) --d;
                if (body[k] == '&' && d == 0 && ed == 0 &&
                    k + 1 < body.size() && body[k + 1] == '&')
                {
                    ++k; // 合并连续 &&：跳过第一个，保留第二个
                    continue;
                }
                t += body[k];
                ++k;
            }
            body = std::move(t);
        }

        // 按 \\ 或 \cr 拆行：忽略花括号内与嵌套环境内部，否则内层 aligned/array 的 \\ 会被误当外层换行。
        std::vector<std::string> rows;
        std::string cur;
        // 花括号深度：与外层函数的参数 depth（嵌套环境递归深度）无关，故分开命名
        int brace_depth = 0;
        int env_depth = 0;
        size_t k = 0;
        while (k < body.size())
        {
            if (body.compare(k, 7, "\\begin{") == 0)
            {
                ++env_depth;
                cur += body.substr(k, 7);
                k += 7;
                continue;
            }
            if (body.compare(k, 5, "\\end{") == 0)
            {
                if (env_depth > 0)
                    --env_depth;
                cur += body.substr(k, 5);
                k += 5;
                continue;
            }
            if (body[k] == '{') ++brace_depth;
            else if (body[k] == '}') --brace_depth;
            if (brace_depth <= 0 && env_depth == 0)
            {
                if (body.compare(k, 2, "\\\\") == 0)
                {
                    rows.push_back(cur);
                    cur.clear();
                    k += 2;
                    // 跳过 \\ 的可选间距参数 [-..pt]
                    if (k < body.size() && body[k] == '[')
                    {
                        const size_t close = body.find(']', k);
                        if (close != std::string::npos)
                            k = close + 1;
                    }
                    continue;
                }
                if (body.compare(k, 3, "\\cr") == 0 &&
                    (k + 3 >= body.size() || !std::isalpha(static_cast<unsigned char>(body[k + 3]))))
                {
                    rows.push_back(cur);
                    cur.clear();
                    k += 3;
                    continue;
                }
            }
            cur += body[k];
            ++k;
        }
        if (!trim(cur).empty() || rows.empty())
            rows.push_back(cur);

        auto count_cells = [](const std::string &row) {
            size_t n = 1;
            int d = 0;
            int ed = 0;
            size_t i = 0;
            while (i < row.size())
            {
                if (row.compare(i, 7, "\\begin{") == 0)
                {
                    ++ed;
                    i += 7;
                    continue;
                }
                if (row.compare(i, 5, "\\end{") == 0)
                {
                    if (ed > 0)
                        --ed;
                    i += 5;
                    continue;
                }
                if (row[i] == '{') ++d;
                else if (row[i] == '}' && d > 0) --d;
                else if (row[i] == '&' && (i == 0 || row[i - 1] != '\\') &&
                         d == 0 && ed == 0) ++n;
                ++i;
            }
            return n;
        };
        // 目标列数：array/subarray 以显式列规格为准（规格为空时取最宽行），cases 系列固定 2 列，
        // matrix/aligned 等按最宽的一行。
        size_t target;
        if (name == "array" || name == "subarray")
        {
            target = spec_cols;
            if (target == 0)
                for (const auto &r : rows)
                    target = std::max(target, count_cells(r));
        }
        else if (name == "cases" || name == "dcases" || name == "rcases" ||
                 name == "cases*")
        {
            target = 2;
        }
        else
        {
            target = 1;
            for (const auto &r : rows)
                target = std::max(target, count_cells(r));
        }

        std::string new_body;
        for (size_t ri = 0; ri < rows.size(); ++ri)
        {
            if (ri)
                new_body += "\\\\";

            std::string row = rows[ri];
            std::string hline;
            size_t start = 0;
            // 连续多个 \hline / \noalign{\hline} 都要作为行前缀取走，否则第二个会变成单元格内容触发 Misplaced \noalign。
            while (true)
            {
                if (row.compare(start, 6, "\\hline") == 0 &&
                    (start + 6 >= row.size() ||
                     !std::isalpha(static_cast<unsigned char>(row[start + 6]))))
                {
                    hline += "\\hline";
                    start += 6;
                    continue;
                }
                if (row.compare(start, 16, "\\noalign{\\hline}") == 0)
                {
                    hline += "\\noalign{\\hline}";
                    start += 16;
                    continue;
                }
                break;
            }

            std::vector<std::string> cells;
            std::string cell;
            int d = 0;
            int ed = 0;
            size_t i = start;
            while (i < row.size())
            {
                if (row.compare(i, 7, "\\begin{") == 0)
                {
                    ++ed;
                    cell += row.substr(i, 7);
                    i += 7;
                    continue;
                }
                if (row.compare(i, 5, "\\end{") == 0)
                {
                    if (ed > 0)
                        --ed;
                    cell += row.substr(i, 5);
                    i += 5;
                    continue;
                }
                if (row[i] == '{') ++d;
                else if (row[i] == '}' && d > 0) --d;
                if (row[i] == '&' && (i == 0 || row[i - 1] != '\\') &&
                    d == 0 && ed == 0)
                {
                    cells.push_back(cell);
                    cell.clear();
                    ++i;
                }
                else
                {
                    cell += row[i];
                    ++i;
                }
            }
            cells.push_back(cell);

            if (cells.size() > target)
                cells.resize(target);
            while (cells.size() < target)
                cells.push_back("");

            new_body += hline;
            for (size_t ci = 0; ci < cells.size(); ++ci)
            {
                if (ci && !kNoAmpEnvs.count(name))
                    new_body += " & ";
                else if (ci)
                    new_body += " ";
                if (!kNoAmpEnvs.count(name) || !cells[ci].empty())
                    new_body += cells[ci];
            }
        }

        // amsmath 的 matrix 环境最多 10 列，超过时改用 array
        const bool matrix_family = (name != "array" && name != "cases" &&
                                    name != "dcases" && name != "rcases");
        if (name == "array" || (matrix_family && target > 10))
        {
            const std::string new_spec =
                array_spec_is_safe(spec) && !spec.empty()
                    ? spec
                    : std::string(target, 'c');
            out += "\\begin{array}{" + new_spec + "}" + new_body + "\\end{array}";
        }
        else
        {
            out += "\\begin{" + name + "}" + new_body + endtag;
        }
        p = end_pos + endtag.size();
    }
    return out;
}

// 数学公式里是否有「顶层」（不在任何环境与花括号内）的 & 或 \\ / \cr。
// 洛谷常把两段矩阵用顶层 & 和 \\ 拼在一行（KaTeX 能渲染），标准 LaTeX 必须包进对齐环境才能编译。
bool has_top_level_align(const std::string &s)
{
    int brace = 0;
    int env = 0;
    size_t i = 0;
    while (i < s.size())
    {
        if (s.compare(i, 7, "\\begin{") == 0)
        {
            ++env;
            i += 7;
            continue;
        }
        if (s.compare(i, 5, "\\end{") == 0)
        {
            if (env > 0)
                --env;
            i += 5;
            continue;
        }
        if (s[i] == '{')
        {
            ++brace;
            ++i;
            continue;
        }
        if (s[i] == '}')
        {
            if (brace > 0)
                --brace;
            ++i;
            continue;
        }
        if (brace == 0 && env == 0)
        {
            if (s[i] == '&' && (i == 0 || s[i - 1] != '\\'))
                return true;
            if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\')
                return true;
            if (s.compare(i, 3, "\\cr") == 0 &&
                (i + 3 >= s.size() ||
                 !std::isalpha(static_cast<unsigned char>(s[i + 3]))))
                return true;
        }
        ++i;
    }
    return false;
}

bool is_tex_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 编译期能读写文件、执行命令或重建控制词的 TeX 控制词黑名单（整词匹配、大小写敏感）。题面/题解/文章/样例
// 都是不可信远程文本，其中的 \write18、\openout、\input、\catcode、\csname 等会随数学片段写进 .tex 并在编译期生效。
// 只匹配完整控制词，\in、\infty、\int 这类以黑名单词为前缀的正常命令不受影响；\end 只针对 \end{document}。
const std::set<std::string> &dangerous_tex_commands()
{
    static const std::set<std::string> kCommands = {
        // 文件 / 进程 I/O 与命令执行
        "write", "openout", "closeout", "read", "openin", "closein",
        "newwrite", "newread", "immediate", "special", "directlua", "latelua",
        "input", "include", "includeonly", "endinput", "scantokens",
        "InputIfFileExists", "IfFileExists",
        "usepackage", "RequirePackage", "documentclass", "documentstyle",
        "LoadClass", "bibliography", "bibliographystyle",
        "shipout", "output", "everyjob", "everypar", "everymath",
        "everydisplay", "everyhbox", "everyvbox", "everycr",
        // \catcode 改字符类别、\csname 拼命令名，两者都能重建上面被禁的控制词，必须一并禁用
        "csname", "endcsname", "catcode", "lccode", "uccode", "mathcode",
        "delcode", "sfcode", "escapechar", "endlinechar", "newlinechar",
        "lowercase", "uppercase", "expandafter", "noexpand", "string",
        "meaning", "aftergroup", "afterassignment", "futurelet", "scantokens",
        // 字体（XeTeX 会把 [] 里的任意文件当字体读取）
        "font", "nullfont", "newfont", "letterspacefont", "fontspec",
        "setmainfont", "setsansfont", "setmonofont", "newfontfamily",
        // 无条件循环：配合条件判断可写出永不结束的 \loop...\repeat，让排版引擎挂死（KaTeX 也不支持）
        "loop", "repeat",
    };
    return kCommands;
}

// 按前缀整类禁用的引擎原语：XeTeX 的 \XeTeXpicfile 等能读取任意文件，pdfTeX / LuaTeX 的 \pdf... \luatex... 同理；
// 正常 KaTeX 内容不会出现这些前缀。
bool has_forbidden_tex_prefix(const std::string &name)
{
    return name.rfind("XeTeX", 0) == 0 || name.rfind("pdf", 0) == 0 ||
           name.rfind("luatex", 0) == 0;
}

// 拆掉不可信文本里能控制编译器的 TeX 控制词：在反斜杠后插入一个空格，原控制词就变成控制符号 "\ " + 普通字母，
// 不再执行；该写法在数学模式与文本模式里都合法，也不会引入新的花括号不平衡。只拆「读写文件 / 执行命令 /
// 改字符类别 / 重建控制词 / 加载字体 / 无条件循环」这些原语；\def、\newcommand、\let 等宏定义命令保留
// ——它们的定义体在同一个字符串里，体内出现的危险原语同样会被拆掉，无法绕过防护，而洛谷确实有用宏定义的正常公式。
std::string defuse_tex_commands(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size())
    {
        if (s[i] != '\\' || i + 1 >= s.size() || !is_tex_letter(s[i + 1]))
        {
            out += s[i++];
            continue;
        }
        size_t j = i + 1;
        while (j < s.size() && is_tex_letter(s[j]))
            ++j;
        const std::string name = s.substr(i + 1, j - i - 1);
        bool danger = dangerous_tex_commands().count(name) > 0 ||
                      has_forbidden_tex_prefix(name);
        if (!danger && name == "end")
        {
            // 只处理 \end{document}：它会提前结束文档、丢弃剩余内容；\end{cases} 之类的正常环境原样保留。
            // 允许 \end 与 {document} 之间出现空格/制表符（TeX 允许）。
            size_t k = j;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t'))
                ++k;
            static const std::string kDoc = "document";
            if (k < s.size() && s[k] == '{' &&
                s.compare(k + 1, kDoc.size(), kDoc) == 0)
            {
                size_t m = k + 1 + kDoc.size();
                while (m < s.size() && (s[m] == ' ' || s[m] == '\t'))
                    ++m;
                if (m < s.size() && s[m] == '}')
                    danger = true;
            }
        }
        if (danger)
        {
            out += "\\ ";
            out.append(s, i + 1, j - i - 1);
        }
        else
        {
            out.append(s, i, j - i);
        }
        i = j;
    }
    return out;
}

// 送进 std::regex 的单个文本块上限：libstdc++ 的 std::regex 用回溯式 DFS，重复片段每迭代一层递归，
// 超长无空白输入会把栈耗尽直接 SIGSEGV（实测 3 万字符公式、一行 5 万个 *a* 可触发）；8192 远低于崩溃点。
const size_t kMaxRegexChunk = 8192;

// 把超长文本切成 <= limit 的块供正则管道处理：优先最近的换行，其次最近的空白（TeX 里换行与空格等价），
// 都没有就硬切（宁可让这一块按纯文本处理，也不能让正则崩掉进程）。
std::vector<std::string> split_for_regex(const std::string &s, size_t limit)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size())
    {
        const size_t remain = s.size() - start;
        if (remain <= limit)
        {
            out.push_back(s.substr(start));
            break;
        }
        const size_t cut = start + limit;
        // 从 start 扫到 cut，记录「配对完整」的切点：$公式$ 与 `行内代码` 内部的空白不能切（切开后两半都不再匹配
        // 公式/代码语法）；优先换行（跨行结构更少），其次是任意不在这些结构内部的空白。
        size_t split = cut;
        {
            // 跟踪 $...$、`...` 与未转义的反斜杠：判断切点是否落在这些结构内部
            bool in_math = false;
            bool in_code = false;
            bool backslash = false;
            size_t last_nl = std::string::npos;
            size_t last_safe = std::string::npos;
            for (size_t k = start; k <= cut && k < s.size(); ++k)
            {
                const char c = s[k];
                if (c == '$' && !backslash && !in_code)
                    in_math = !in_math;
                else if (c == '`' && !backslash && !in_math)
                    in_code = !in_code;
                backslash = (c == '\\' && !backslash);
                if (std::isspace(static_cast<unsigned char>(c)) && !in_math && !in_code)
                {
                    last_safe = k;
                    if (c == '\n')
                        last_nl = k;
                }
            }
            if (last_nl != std::string::npos)
                split = last_nl;
            else if (last_safe != std::string::npos)
                split = last_safe;
        }
        out.push_back(s.substr(start, split - start + 1));
        start = split + 1;
    }
    return out;
}

std::string sanitize_math_chunk(std::string s);

// 数学片段入口：超长片段先按 kMaxRegexChunk 切块再走修复管线（见那里的说明：不切块会让 std::regex 递归爆栈）；
// 切块只影响畸形超长公式的修复效果，正常长度的公式逐字节等价。
std::string sanitize_math(std::string s)
{
    if (s.size() <= kMaxRegexChunk)
        return sanitize_math_chunk(std::move(s));
    std::string out;
    std::vector<std::string> pieces = split_for_regex(s, kMaxRegexChunk);
    for (auto &piece : pieces)
        out += sanitize_math_chunk(std::move(piece));
    return out;
}

std::string sanitize_math_chunk(std::string s)
{
    // \verb 内容是字面文本：先用占位符整体保护，等所有转换结束后再按文本模式转义还原，
    // 避免中间的 \color / px / % # 等转换污染 verb 内容。
    std::vector<std::string> verb_raws;
    auto verb_placeholder = [&](size_t i) {
        return std::string("\x02V") + std::to_string(i) + "\x02";
    };
    {
        static const std::regex kVerb(R"(\\verb(.)(.*?)\1)");
        s = regex_transform(s, kVerb, [&](const std::smatch &m) {
            verb_raws.push_back(m[2].str());
            return "{\\texttt{" + verb_placeholder(verb_raws.size() - 1) + "}}";
        });
    }

    // 源数据里的 \text{\\}（KaTeX 允许文本内换行）在表格/矩阵里会触发 Misplaced \cr；\newline 在文本模式处处合法
    static const std::regex kTextNewline(R"(\\text\{\s*\\\\\s*\})");
    s = std::regex_replace(s, kTextNewline, "\\text{\\newline}");

    // Unicode 数学符号（∑ 等）是普通字符，\limits 要求数学算子，转成对应的 LaTeX 命令
    static const std::map<std::string, std::string> kUnicodeMath = {
        {"\u2211", "\\sum"}, {"\u220f", "\\prod"}, {"\u222b", "\\int"},
        {"\u222e", "\\oint"}, {"\u221e", "\\infty"}, {"\u2264", "\\le"},
        {"\u2265", "\\ge"}, {"\u2260", "\\neq"}, {"\u00d7", "\\times"},
        {"\u00f7", "\\div"}, {"\u2200", "\\forall"}, {"\u2203", "\\exists"},
        {"\u2208", "\\in"}, {"\u2209", "\\notin"}, {"\u2229", "\\cap"},
        {"\u222a", "\\cup"}, {"\u2286", "\\subseteq"},
        {"\u2287", "\\supseteq"}, {"\u2295", "\\oplus"},
        {"\u2297", "\\otimes"}, {"\u2192", "\\rightarrow"},
        {"\u2190", "\\leftarrow"}, {"\u21d2", "\\Rightarrow"},
        {"\u21d0", "\\Leftarrow"}, {"\u21d4", "\\Leftrightarrow"},
        {"\u221a", "\\sqrt"}, {"\u00b1", "\\pm"}, {"\u2213", "\\mp"},
        {"\u22c5", "\\cdot"}, {"\u223c", "\\sim"}, {"\u2248", "\\approx"},
        {"\u2261", "\\equiv"}, {"\u2202", "\\partial"}, {"\u2207", "\\nabla"},
        {"\u2220", "\\angle"}, {"\u22a5", "\\bot"}, {"\u2227", "\\wedge"},
        {"\u2228", "\\vee"}, {"\u230a", "\\lfloor"}, {"\u230b", "\\rfloor"},
        {"\u2308", "\\lceil"}, {"\u2309", "\\rceil"}, {"\u2225", "\\parallel"},
        {"\u2223", "\\mid"},
    };
    for (const auto &kv : kUnicodeMath)
    {
        std::string t;
        size_t p = 0;
        const std::string &u = kv.first;
        const std::string &rep = kv.second;
        while ((p = s.find(u, p)) != std::string::npos)
        {
            s.replace(p, u.size(), rep);
            p += rep.size();
        }
    }

    // 公式末尾悬空的 ^ / _（如「……则省略 ^」）没有指数/下标参数，直接当符号输出，避免 Missing { inserted。
    // ^ 用 \wedge（∧）；_ 是下划线，用字面下划线 \text{\_}（已转义的 \_ 不受影响）。
    static const std::regex kTrailingCaret(R"((^|[^\\])([\^_])(?=\s*\$?\s*$))");
    s = regex_transform(s, kTrailingCaret, [&](const std::smatch &m) {
        return m[1].str() + (m[2].str() == "^" ? "\\wedge" : "\\text{\\_}");
    });

    // KaTeX 兼容：\colorbox{#hex} / \textcolor{#hex} / \color{#hex} → xcolor 的 HTML 颜色模型；
    // 3 位十六进制色值（#fff）按 xcolor 要求补齐成 6 位。
    auto hex_pad = [](const std::string &h) {
        if (h.size() == 3)
            return std::string() + h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
        return h;
    };
    static const std::regex kColorBox(R"(\\colorbox\{#?([0-9a-fA-F]{3}|[0-9a-fA-F]{6})\})");
    static const std::regex kTextColor(R"(\\textcolor\{#?([0-9a-fA-F]{3}|[0-9a-fA-F]{6})\})");
    static const std::regex kColor(R"(\\color\{#?([0-9a-fA-F]{3}|[0-9a-fA-F]{6})\})");
    s = regex_transform(s, kColorBox, [&](const std::smatch &m) {
        return "\\colorbox[HTML]{" + hex_pad(m[1].str()) + "}";
    });
    s = regex_transform(s, kTextColor, [&](const std::smatch &m) {
        return "\\textcolor[HTML]{" + hex_pad(m[1].str()) + "}";
    });
    s = regex_transform(s, kColor, [&](const std::smatch &m) {
        return "\\color[HTML]{" + hex_pad(m[1].str()) + "}";
    });

    // 2 位十六进制（洛谷的 \color{ff}）按白色处理，避免 Undefined color
    static const std::regex kColor2Hex(R"(\\color\{([0-9a-fA-F]{2})\})");
    s = std::regex_replace(s, kColor2Hex, "\\color{white}");

    static const std::regex kFColorBox(R"(\\fcolorbox\{([^}]*)\}\{([^}]*)\}\{)");
    static const std::map<std::string, std::string> kNamedHex = {
        {"black", "000000"}, {"white", "FFFFFF"}, {"red", "FF0000"},
        {"green", "00FF00"}, {"blue", "0000FF"}, {"yellow", "FFFF00"},
        {"cyan", "00FFFF"}, {"magenta", "FF00FF"}, {"orange", "FFA500"},
        {"purple", "800080"}, {"gray", "808080"}, {"grey", "808080"},
        {"brown", "A52A2A"}, {"pink", "FFC0CB"}, {"teal", "008080"},
        {"violet", "EE82EE"}, {"lime", "00FF00"}, {"olive", "808000"},
        {"gold", "FFD700"},
    };
    auto is_hex = [](const std::string &c) {
        return c.size() == 3 || c.size() == 6;
    };
    s = regex_transform(s, kFColorBox, [&](const std::smatch &m) {
        auto to_hex = [&](const std::string &c) -> std::string {
            std::string x = c;
            if (!x.empty() && x[0] == '#')
                x = x.substr(1);
            if (is_hex(x) &&
                std::all_of(x.begin(), x.end(), [](char ch) {
                    return std::isxdigit(static_cast<unsigned char>(ch));
                }))
                return hex_pad(x);
            const auto it = kNamedHex.find(to_lower_ascii(x));
            return it != kNamedHex.end() ? it->second : std::string();
        };
        const std::string f = to_hex(m[1].str());
        const std::string b = to_hex(m[2].str());
        if (!f.empty() && !b.empty())
            return "\\fcolorbox[HTML]{" + f + "}{" + b + "}{";
        return m[0].str();
    });

    // 常见笔误：$k^[th}$ → $k^{th}$
    static const std::regex kCaretBracket(R"(\^\[)");
    s = std::regex_replace(s, kCaretBracket, "^{");

    // KaTeX 支持 px 单位，LaTeX 不支持；统一转成 pt
    static const std::regex kPx(R"((\d+(?:\.\d+)?)px)");
    s = std::regex_replace(s, kPx, "$1pt");

    // \hspace 不接受 mu（数学单位），必须用 \mkern
    static const std::regex kHspaceMu(R"(\\hspace\{(\d+(?:\.\d+)?)mu\})");
    s = std::regex_replace(s, kHspaceMu, "\\mkern$1mu");

    // 洛谷常见笔误 \\end{cases} / \\\\end{cases}：行分隔符会吃掉 \end 的反斜杠，统一还原成单个 \end{
    static const std::regex kRowEnd(R"(\\+end\{)");
    s = std::regex_replace(s, kRowEnd, "\\end{");

    // \overline\texttt{ab} 这类「重音命令直接跟另一个命令」的写法：重音命令需要花括号参数，
    // 把后面的命令连同参数一起包进 {}
    {
        static const std::set<std::string> kAccents = {
            "overline", "underline", "overbrace", "underbrace", "widehat",
            "widetilde", "overrightarrow", "overleftarrow",
            "overleftrightarrow",
        };
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            bool matched = false;
            if (s[p] == '\\')
            {
                size_t w = p + 1;
                while (w < s.size() &&
                       std::isalpha(static_cast<unsigned char>(s[w])))
                    ++w;
                const std::string name = s.substr(p + 1, w - p - 1);
                if (kAccents.count(name) && w < s.size() && s[w] == '\\')
                {
                    size_t q = w;
                    size_t w2 = q + 1;
                    while (w2 < s.size() &&
                           std::isalpha(static_cast<unsigned char>(s[w2])))
                        ++w2;
                    size_t end = w2;
                    while (end < s.size() && s[end] == '{')
                    {
                        size_t d = 1;
                        size_t m = end + 1;
                        while (m < s.size() && d > 0)
                        {
                            if (s[m] == '{')
                                ++d;
                            else if (s[m] == '}')
                                --d;
                            ++m;
                        }
                        if (d != 0)
                            break;
                        end = m;
                    }
                    if (end > w2)
                    {
                        t += s.substr(p, w - p);
                        t += "{";
                        t += s.substr(w, end - w);
                        t += "}";
                        p = end;
                        matched = true;
                    }
                }
            }
            if (!matched)
            {
                t += s[p];
                ++p;
            }
        }
        s = std::move(t);
    }

    // $90^\degree$ 这类写法会变成双重上标，直接展开
    static const std::regex kCaretDegree(R"(\^\\degree)");
    s = std::regex_replace(s, kCaretDegree, "^{\\circ}");

    static const std::regex kTT(R"(\\tt\{)");
    static const std::regex kTTPlain(R"(\\tt\s+([A-Za-z0-9]+))");
    s = std::regex_replace(s, kTT, "\\texttt{");
    s = std::regex_replace(s, kTTPlain, "\\texttt{$1}");

    // 裸 \texttt（后面没有 {）会吞掉下一个命令当参数，补一个空花括号
    static const std::regex kTTBare(R"(\\texttt(?![{]))");
    s = std::regex_replace(s, kTTBare, "\\texttt{}");

    // \kern{...} 不接受花括号参数（TeX 原语），转成 \hspace{...}
    static const std::regex kKern(R"(\\kern\{)");
    s = std::regex_replace(s, kKern, "\\hspace{");

    // \space 后紧跟中文字符在 xelatex 报 Undefined control sequence，转成控制空格（数学/文本模式都可用）
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 6, "\\space") == 0 &&
                (p + 6 >= s.size() || !std::isalpha(static_cast<unsigned char>(s[p + 6]))))
            {
                t += "\\ ";
                p += 6;
                continue;
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // \LaTeX 是文本命令，在数学模式会触发 spacefactor 错误 → 用 \text 包裹
    static const std::regex kLaTeX(R"(\\LaTeX)");
    s = std::regex_replace(s, kLaTeX, "\\text{\\LaTeX}");

    // \text 后面直接跟中文字符（无花括号）时补空花括号，避免把中文当参数
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 5, "\\text") == 0)
            {
                const size_t after = p + 5;
                if (after >= s.size() ||
                    (!std::isalpha(static_cast<unsigned char>(s[after])) && s[after] != '{'))
                {
                    t += "\\text{}";
                    p = after;
                    continue;
                }
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // \texttt{...} 在数学模式里能正常执行命令（\textcolor 等直接保留），只需转义裸特殊字符 _ # % & ^ ~ { }
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 8, "\\texttt{") == 0)
            {
                size_t q = p + 8;
                int depth = 1;
                std::string content;
                while (q < s.size() && depth > 0)
                {
                    if (s[q] == '\\' && q + 1 < s.size() &&
                        !std::isalpha(static_cast<unsigned char>(s[q + 1])))
                    {
                        content += s[q];
                        content += s[q + 1];
                        q += 2;
                        continue;
                    }
                    if (s[q] == '\\' && q + 1 < s.size() &&
                        std::isalpha(static_cast<unsigned char>(s[q + 1])))
                    {
                        size_t w = q + 1;
                        while (w < s.size() &&
                               std::isalpha(static_cast<unsigned char>(s[w])))
                            ++w;
                        content += s.substr(q, w - q);
                        q = w;
                        continue;
                    }
                    if (s[q] == '{')
                        ++depth;
                    else if (s[q] == '}')
                    {
                        --depth;
                        if (depth == 0)
                            break;
                    }
                    content += s[q];
                    ++q;
                }
                if (depth == 0)
                {
                    // texttt（文本模式）里数学符号未定义，需包 $...$ 显示；字号命令（\small 等）在数学模式未定义，直接去掉
                    static const std::set<std::string> kMathSymbols = {
                        "sim", "times", "le", "ge", "leq", "geq", "neq", "ne",
                        "in", "notin", "pm", "mp", "cdot", "div", "oplus",
                        "ominus", "otimes", "circ", "mid", "nmid", "to",
                        "rightarrow", "leftarrow", "Rightarrow", "Leftarrow",
                        "Leftrightarrow", "mapsto", "dots", "cdots", "ldots",
                        "infty", "forall", "exists", "partial", "nabla",
                        "approx", "equiv", "propto", "subset", "subseteq",
                        "supset", "supseteq", "cup", "cap", "setminus", "sqrt",
                        "sum", "prod", "int", "max", "min", "mod", "bmod",
                        "pmod", "argmax", "argmin", "lvert", "rvert",
                        "lVert", "rVert", "angle", "bot", "top", "wedge",
                        "vee", "land", "lor", "not", "bigcup", "bigcap",
                    };
                    static const std::set<std::string> kSizeCmds = {
                        "tiny", "scriptsize", "footnotesize", "small",
                        "normalsize", "large", "Large", "LARGE", "huge",
                        "Huge",
                    };
                    std::string esc;
                    for (size_t k = 0; k < content.size(); ++k)
                    {
                        const char c = content[k];
                        if (c == '\\' && k + 1 < content.size())
                        {
                            if (!std::isalpha(static_cast<unsigned char>(content[k + 1])) &&
                                (content[k + 1] == '^' || content[k + 1] == '~'))
                            {
                                // \^ \~ 是重音命令，在 texttt 里需转成文本符号
                                esc += (content[k + 1] == '^')
                                           ? "\\textasciicircum{}"
                                           : "\\textasciitilde{}";
                                k += 1;
                                continue;
                            }
                            if (std::isalpha(static_cast<unsigned char>(content[k + 1])))
                            {
                                // 控制词（\textcolor、\textbackslash 等）连同其花括号参数原样保留，否则转义参数里的 { } 会破坏命令
                                size_t j = k + 1;
                                while (j < content.size() &&
                                       std::isalpha(static_cast<unsigned char>(content[j])))
                                    ++j;
                                while (j < content.size() && content[j] == '{')
                                {
                                    size_t d = 1;
                                    size_t m = j + 1;
                                    while (m < content.size() && d > 0)
                                    {
                                        if (content[m] == '{')
                                            ++d;
                                        else if (content[m] == '}')
                                            --d;
                                        ++m;
                                    }
                                    if (d != 0)
                                        break;
                                    j = m;
                                }
                                const std::string word =
                                    content.substr(k, j - k);
                                const size_t word_end = word.find_first_of("{ ");
                                const std::string name = word.substr(
                                    1, word_end == std::string::npos
                                           ? word.size() - 1
                                           : word_end - 1);
                                if (kSizeCmds.count(name))
                                {
                                    k = j - 1;
                                    continue;
                                }
                                if (kMathSymbols.count(name))
                                {
                                    // 数学符号连同参数包起来（如 \sqrt{2}）。用 \ensuremath 而不是 $...$：\texttt 在数学模式里并不切换到
                                    // 文本模式，此时再写 $...$ 会结束外层数学模式（\sqrt 落进文本模式直接报 Missing $ inserted）。
                                    esc += "\\ensuremath{" + word + "}";
                                    k = j - 1;
                                    continue;
                                }
                                esc += content.substr(k, j - k);
                                k = j - 1;
                            }
                            else
                            {
                                esc += c;
                                esc += content[k + 1];
                                ++k;
                            }
                            continue;
                        }
                        if (c == '\\')
                        {
                            esc += "\\textbackslash{}";
                            continue;
                        }
                        switch (c)
                        {
                        case '_': esc += "\\_"; break;
                        case '#': esc += "\\#"; break;
                        case '%': esc += "\\%"; break;
                        case '&': esc += "\\&"; break;
                        case '~': esc += "\\textasciitilde{}"; break;
                        case '^': esc += "\\textasciicircum{}"; break;
                        case '{': esc += "\\{"; break;
                        case '}': esc += "\\}"; break;
                        default: esc += c;
                        }
                    }
                    t += "\\texttt{" + esc + "}";
                    p = q + 1;
                    continue;
                }
                // 找不到闭合花括号（畸形/未闭合的 \texttt{）：剩余内容原样输出，避免对每个同类前缀重新扫描到末尾（O(n²) 停顿）
                t += s.substr(p);
                p = s.size();
                continue;
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // \operatorname{...} 参数里含 \color 时 \limits 报 Limit controls must follow a math operator，
    // 把参数整体包一层花括号
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 14, "\\operatorname{") == 0)
            {
                size_t q = p + 14;
                int depth = 1;
                while (q < s.size() && depth > 0)
                {
                    if (s[q] == '{')
                        ++depth;
                    else if (s[q] == '}')
                        --depth;
                    ++q;
                }
                if (depth == 0)
                {
                    const std::string inner =
                        s.substr(p + 14, q - p - 14 - 1);
                    if (inner.find("\\color") != std::string::npos)
                    {
                        t += "\\operatorname{{" + inner + "}}";
                        p = q;
                        continue;
                    }
                }
                else
                {
                    // 未闭合的 \operatorname{：剩余内容原样输出，避免对每个同类前缀重扫到末尾（O(n²) 停顿）
                    t += s.substr(p);
                    p = s.size();
                    continue;
                }
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // \text{...} 里的数学符号（\le、\ldots 等）在文本模式未定义，包上 $...$；\text{ 与 \texttt{ 区分开
    {
        static const std::set<std::string> kTextMathSymbols = {
            "le", "leq", "ge", "geq", "ne", "neq", "sim", "times", "div",
            "pm", "mp", "cdot", "oplus", "ominus", "otimes", "circ", "mid",
            "nmid", "to", "rightarrow", "leftarrow", "Rightarrow",
            "Leftarrow", "Leftrightarrow", "mapsto", "dots", "cdots",
            "ldots", "infty", "forall", "exists", "partial", "nabla",
            "approx", "equiv", "propto", "subset", "subseteq", "supset",
            "supseteq", "cup", "cap", "setminus", "sqrt", "sum", "prod",
            "int", "max", "min", "mod", "bmod", "pmod", "lvert", "rvert",
            "angle", "bot", "top", "wedge", "vee", "land", "lor", "not",
            "bigcup", "bigcap", "in", "notin", "ni", "lfloor", "rfloor",
            "lceil", "rceil", "vert", "Vert", "langle", "rangle",
        };
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 6, "\\text{") == 0)
            {
                size_t q = p + 6;
                int depth = 1;
                while (q < s.size() && depth > 0)
                {
                    if (s[q] == '{')
                        ++depth;
                    else if (s[q] == '}')
                        --depth;
                    ++q;
                }
                if (depth == 0)
                {
                    std::string inner = s.substr(p + 6, q - p - 6 - 1);
                    std::string esc;
                    size_t k = 0;
                    bool in_math_span = false;
                    while (k < inner.size())
                    {
                        if (inner[k] == '$')
                        {
                            esc += '$';
                            in_math_span = !in_math_span;
                            ++k;
                            continue;
                        }
                        if (inner[k] == '\\' && k + 1 < inner.size() &&
                            std::isalpha(static_cast<unsigned char>(inner[k + 1])) &&
                            !in_math_span)
                        {
                            size_t w = k + 1;
                            while (w < inner.size() &&
                                   std::isalpha(static_cast<unsigned char>(inner[w])))
                                ++w;
                            size_t end = w;
                            while (end < inner.size() && inner[end] == '{')
                            {
                                size_t d = 1;
                                size_t m = end + 1;
                                while (m < inner.size() && d > 0)
                                {
                                    if (inner[m] == '{')
                                        ++d;
                                    else if (inner[m] == '}')
                                        --d;
                                    ++m;
                                }
                                if (d != 0)
                                    break;
                                end = m;
                            }
                            const std::string name = inner.substr(k + 1, w - k - 1);
                            if (kTextMathSymbols.count(name))
                            {
                                esc += "$" + inner.substr(k, end - k) + "$";
                                k = end;
                                continue;
                            }
                        }
                        esc += inner[k];
                        ++k;
                    }
                    t += "\\text{" + esc + "}";
                    p = q;
                    continue;
                }
                else
                {
                    t += s.substr(p);
                    p = s.size();
                    continue;
                }
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // 裸 \sout（未跟 {）会吞掉后续命令作为参数（如 \sout\text{...}），直接删掉，保留正文
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 6, "\\sout{") == 0)
            {
                t += "\\sout{";
                p += 6;
                continue;
            }
            if (s.compare(p, 5, "\\sout") == 0)
            {
                p += 5;
                continue;
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // bm 包无法处理 \bm{...\color...} / \boldsymbol{...\color...}，内容含 color 时额外包一层花括号；
    // \bm 的参数里 ~ 会触发 Missing number，转成数学空格。
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            const bool is_bm = s.compare(p, 4, "\\bm{") == 0;
            const bool is_bsym = s.compare(p, 12, "\\boldsymbol{") == 0;
            if (is_bm || is_bsym)
            {
                const size_t open_len = is_bm ? 4 : 12;
                const std::string prefix = is_bm ? "\\bm" : "\\boldsymbol";
                size_t depth = 1;
                size_t q = p + open_len;
                while (q < s.size() && depth > 0)
                {
                    if (s[q] == '{')
                        ++depth;
                    else if (s[q] == '}')
                        --depth;
                    ++q;
                }
                if (depth == 0)
                {
                    std::string inner = s.substr(p + open_len, q - p - open_len - 1);
                    bool need_brace = false;
                    {
                        std::string fixed;
                        for (char ch : inner)
                        {
                            if (ch == '~')
                            {
                                fixed += "\\ ";
                                need_brace = true;
                            }
                            else
                                fixed += ch;
                        }
                        inner = std::move(fixed);
                    }
                    static const char *kColorCmds[] = {
                        "\\color", "\\textcolor", "\\red", "\\blue",
                        "\\green", "\\pink", "\\orange", "\\purple",
                        "\\brown", "\\gray", "\\cyan", "\\teal",
                        "\\magenta", "\\yellow", "\\violet",
                    };
                    for (const char *cc : kColorCmds)
                        if (inner.find(cc) != std::string::npos)
                            need_brace = true;
                    if (need_brace)
                    {
                        t += prefix + "{{" + inner + "}}";
                        p = q;
                        continue;
                    }
                }
                else
                {
                    t += s.substr(p);
                    p = s.size();
                    continue;
                }
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    s = normalize_alignment(s);

    // 顶层 & / \\：有 \begin 环境时把整段包进 aligned（洛谷常把两段矩阵用 & 直接拼接），
    // 没有环境时按普通字符转义（$&@$、样例输入换行等）。
    {
        const bool in_env = s.find("\\begin{") != std::string::npos;
        if (in_env && has_top_level_align(s))
        {
            s = "\\begin{aligned}\n" + s + "\n\\end{aligned}";
        }
        else if (!in_env)
        {
            std::string t;
            size_t k = 0;
            while (k < s.size())
            {
                const char c = s[k];
                if (c == '&' && (k == 0 || s[k - 1] != '\\'))
                {
                    t += "\\&";
                    ++k;
                }
                else if (c == '\\' && k + 1 < s.size() && s[k + 1] == '\\')
                {
                    t += "\\text{\\newline}";
                    k += 2;
                }
                else
                {
                    t += c;
                    ++k;
                }
            }
            s = std::move(t);
        }
    }

    // 洛谷公式常用 \newcommand/\renewcommand 自定义命令，与 LaTeX 已有命令同名会报 already defined，
    // 统一转成允许重复定义的 \def；须先于 %/# 转义，定义体内的 #1 才会被当成 \def 宏参数而保留。
    static const std::regex kNewCommandBraced(R"(\\(?:re)?newcommand\s*\{([^}]*)\})");
    static const std::regex kNewCommandPlain(R"(\\(?:re)?newcommand\s+([A-Za-z@]+))");
    s = std::regex_replace(s, kNewCommandBraced, "\\def$1");
    s = std::regex_replace(s, kNewCommandPlain, "\\def$1");

    // \newcommand 的 [N] 参数个数写法（\def\cases[1]{...}）对 \def 无效，转成标准的 \def\cases#1{...}
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 5, "\\def\\") == 0)
            {
                size_t w = p + 5;
                while (w < s.size() &&
                       std::isalpha(static_cast<unsigned char>(s[w])))
                    ++w;
                if (w < s.size() && s[w] == '[')
                {
                    size_t e = w + 1;
                    while (e < s.size() &&
                           std::isdigit(static_cast<unsigned char>(s[e])))
                        ++e;
                    if (e >= s.size())
                    {
                        t += s.substr(p);
                        p = s.size();
                        continue;
                    }
                    if (e < s.size() && s[e] == ']' && e > w + 1)
                    {
                        // 参数个数：安全解析并限制上限（TeX 宏参数最多 9 个）；畸形/超长数字（\def\foo[999999999999]）不再抛异常，
                        // 超过上限时保持原样输出
                        size_t n = 0;
                        if (parse_nonneg_int(s.data() + w + 1, s.data() + e, n) &&
                            n >= 1 && n <= 9)
                        {
                            std::string params;
                            for (size_t k = 1; k <= n; ++k)
                                params += "#" + std::to_string(k);
                            t += s.substr(p, w - p) + params;
                            p = e + 1;
                            continue;
                        }
                    }
                }
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // 裸 % 和 # 在 LaTeX（含数学模式）里是特殊字符，需转义；已转义的 \% / \# 先占位保护，避免二次转义
    auto escape_special = [](std::string t, char c, const std::string &escaped) {
        const std::string esc_placeholder = "\x01P\x02";
        size_t pos = 0;
        while ((pos = t.find(escaped, pos)) != std::string::npos)
        {
            t.replace(pos, 2, esc_placeholder);
            pos += esc_placeholder.size();
        }
        std::string out;
        out.reserve(t.size());
        for (size_t k = 0; k < t.size(); ++k)
        {
            const char ch = t[k];
            // 宏参数（#1、#2...）不能转义，否则 \def\c#1{...} 会被破坏：\def 的参数表与定义体内对参数的引用都要保留
            if (ch == '#' && k + 1 < t.size() &&
                std::isdigit(static_cast<unsigned char>(t[k + 1])))
            {
                bool protected_hash = false;
                size_t b = k;
                while (b > 0 && std::isalpha(static_cast<unsigned char>(t[b - 1])))
                    --b;
                if (b >= 5 && t.compare(b - 5, 5, "\\def\\") == 0)
                {
                    protected_hash = true;
                }
                // 往回找最近的 \def，若当前 # 位于其参数表或 {body} 内则保留
                if (!protected_hash)
                {
                    for (size_t d = k; d-- > 0;)
                    {
                        if (t.compare(d, 4, "\\def") == 0 &&
                            (d + 4 >= t.size() ||
                             !std::isalpha(static_cast<unsigned char>(t[d + 4]))))
                        {
                            const size_t open = t.find('{', d + 4);
                            if (open == std::string::npos)
                                break;
                            if (k < open)
                            {
                                protected_hash = true;
                            }
                            else
                            {
                                int depth = 1;
                                size_t e = open + 1;
                                while (e < t.size() && depth > 0)
                                {
                                    if (t[e] == '{')
                                        ++depth;
                                    else if (t[e] == '}')
                                        --depth;
                                    ++e;
                                }
                                if (depth == 0 && e > k)
                                    protected_hash = true;
                            }
                            break; // 最近的 \def 不包含当前 #，不再往前找
                        }
                    }
                }
                if (protected_hash)
                {
                    out += '#';
                    continue;
                }
            }
            out += (ch == c) ? ("\\" + std::string(1, c)) : std::string(1, ch);
        }
        pos = 0;
        while ((pos = out.find(esc_placeholder)) != std::string::npos)
            out.replace(pos, esc_placeholder.size(), escaped);
        return out;
    };
    s = escape_special(s, '%', "\\%");
    s = escape_special(s, '#', "\\#");

    // 洛谷常用 \def\c#1{...} 这类单字母宏，与 LaTeX 内部的重音命令（\c \t \b \s \r 等）冲突，
    // 统一改名为 \lgoX 前缀；只在单遍内处理单字母宏，避免 \def\bg 等多字母宏被误改或重复改名。
    {
        std::set<char> names;
        for (size_t q = 0; q + 6 <= s.size(); ++q)
        {
            if (s.compare(q, 5, "\\def\\") == 0 &&
                std::isalpha(static_cast<unsigned char>(s[q + 5])))
            {
                const char x = s[q + 5];
                const size_t after = q + 6;
                if (after >= s.size() || !std::isalpha(static_cast<unsigned char>(s[after])))
                    names.insert(x);
            }
        }

        std::string tmp;
        size_t p = 0;
        while (p < s.size())
        {
            bool matched = false;
            for (char x : names)
            {
                const std::string def = "\\def\\" + std::string(1, x);
                if (s.compare(p, def.size(), def) == 0)
                {
                    const size_t after = p + def.size();
                    if (after >= s.size() || !std::isalpha(static_cast<unsigned char>(s[after])))
                    {
                        tmp += "\\def\\lgo" + std::string(1, x);
                        p = after;
                        matched = true;
                        break;
                    }
                }
                // 用法 \X（后跟 { / 空格 / 标点等非小写字母；用 islower 避免误伤 \color 这类长命令）
                if (s[p] == '\\' && p + 1 < s.size() && s[p + 1] == x &&
                    (p + 2 >= s.size() ||
                     !std::islower(static_cast<unsigned char>(s[p + 2]))))
                {
                    tmp += "\\lgo" + std::string(1, x);
                    p += 2;
                    matched = true;
                    break;
                }
            }
            if (!matched)
            {
                tmp += s[p];
                ++p;
            }
        }
        s = std::move(tmp);
    }

    // 控制词后紧跟字母（含 CJK，XeTeX 里都是 catcode 11）会被并进控制词（\qquad第、\leN 都成未定义命令）：
    // 按最长已知命令前缀拆开并补空组。放在单字母宏改名之后，这样 \lgowN 这类改名产物也能被处理。
    {
        static const std::set<std::string> kKnownPrefixes = {
            // 关系符（最常被后面直接跟变量名吸收）
            "le", "leq", "ge", "geq", "ne", "neq", "sim", "simeq", "approx",
            "equiv", "propto", "lt", "gt", "times", "div", "pm", "mp",
            "cdot", "ast", "circ", "oplus", "ominus", "otimes", "oslash",
            "cup", "cap", "subset", "supset", "subseteq", "supseteq",
            "in", "notin", "ni", "mid", "nmid", "parallel", "perp", "bot",
            "top", "to", "gets", "mapsto", "rightarrow", "leftarrow",
            "Rightarrow", "Leftarrow", "Leftrightarrow", "iff", "implies",
            "uparrow", "downarrow", "Uparrow", "Downarrow", "updownarrow",
            "dots", "cdots", "ldots", "vdots", "ddots", "quad", "qquad",
            "land", "lor", "wedge", "vee", "lnot", "neg",
            "max", "min", "log", "ln", "lg", "gcd", "lcm", "mod", "bmod",
            "pmod", "sum", "prod", "int", "iint", "iiint", "oint", "lim",
            "limsup", "liminf", "sup", "inf", "det", "dim", "exp", "deg",
            "arg", "ker", "hom", "Pr", "rank", "sin", "cos", "tan", "cot",
            "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh",
            "tanh", "coth", "argmax", "argmin",
            // 常见字体/命令（长命令本身也要先放进来，完整匹配时优先）
            "mathrm", "mathbf", "mathit", "mathtt", "mathsf", "mathcal",
            "mathbb", "mathfrak", "mathscr", "boldsymbol", "bm", "text",
            "texttt", "textbf", "textit", "textrm", "textsf",
            "operatorname", "operatornamewithlimits", "textstyle",
            "displaystyle", "scriptstyle", "scriptscriptstyle", "frac",
            "dfrac", "tfrac", "binom", "dbinom", "tbinom", "sqrt",
            "overline", "underline", "overbrace", "underbrace", "widehat",
            "widetilde", "overrightarrow", "overleftarrow", "vec", "bar",
            "hat", "dot", "ddot", "tilde", "check", "acute", "grave",
            "breve", "mathring", "cancel", "bcancel", "xcancel", "sout",
            "not", "xlongequal", "xrightarrow", "xleftarrow", "xmapsto",
            "xleftrightarrow", "raisebox", "hspace", "hfill", "vspace",
            "kern", "mkern", "mskip", "limits", "nolimits",
            "newline",
            "left", "right", "big", "Big", "bigg", "Bigg", "bigl", "bigr",
            "Bigl", "Bigr", "biggl", "biggr", "Biggl", "Biggr",
            "lvert", "rvert", "lVert", "rVert", "langle", "rangle",
            "lfloor", "rfloor", "lceil", "rceil", "lbrace", "rbrace",
            "lgroup", "rgroup", "Vert", "vert", "aleph", "hbar", "ell",
            "imath", "jmath", "Re", "Im", "partial", "nabla", "forall",
            "exists", "nexists", "infty", "emptyset", "varnothing",
            // \in / \notin 开头、但本身是完整命令名的命令也要列入，否则会被「最长已知前缀」拆开（\injlim → \in{}jlim，已实测）
            "injlim", "projlim", "varinjlim", "varprojlim",
            "notindot", "notinva", "notinvb", "notinvc",
            "notniva", "notnivb", "notnivc", "notni",
            "triangle", "square", "Box", "Diamond", "clubsuit", "diamondsuit",
            "heartsuit", "spadesuit", "checkmark", "dagger", "ddagger",
            "star", "bullet", "degree", "copyright",
            "Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta",
            "Theta", "Iota", "Kappa", "Lambda", "Mu", "Nu", "Xi", "Omicron",
            "Pi", "Rho", "Sigma", "Tau", "Upsilon", "Phi", "Chi", "Psi",
            "Omega", "varTheta", "varSigma", "varPhi", "varOmega",
            "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta",
            "theta", "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron",
            "pi", "rho", "sigma", "tau", "upsilon", "phi", "chi", "psi",
            "omega", "varepsilon", "vartheta", "varrho", "varsigma",
            "varphi", "digamma", "R", "N", "Z", "Q", "C",
            // 单字母宏改名产物 \lgoX
            "lgoa", "lgob", "lgoc", "lgod", "lgof", "lgog", "lgoh", "lgol",
            "lgom", "lgon", "lgop", "lgoq", "lgos", "lgot", "lgou", "lgov",
            "lgow", "lgox", "lgoy",
        };
        // 以短命令开头、但本身是完整命令名的命令（如 \leqslant 以 \leq 开头）：整词匹配时必须原样保留，
        // 否则会被拆成 \leq{}slant（P4381 的公式曾因此编译出错）；后跟变量时（\leqslantN）仍按「命令 + 后续字符」拆开。
        // 名单取自 LaTeX 内核 / amsmath / amssymb / mathtools / unicode-math 中出现的西文命令，新增命令时在此补充。
        static const std::set<std::string> kKnownFullCommands = {
            "leq", "leqq", "leqqslant", "leqslant", "lescc", "lesdot", "lesdoto",
            "lesdotor", "lesges", "less", "lessapprox", "lessdot", "lesseqgtr",
            "lesseqqgtr", "lessgtr", "lesssim", "approxeq", "approxeqq",
            "approxident", "geq", "geqq", "geqqslant", "geqslant", "gescc", "gesdot",
            "gesdoto", "gesdotol", "gesles", "gneq", "gneqq", "gnsim", "gvertneqq",
            "gtrapprox", "gtrarr", "gtrdot", "gtreqless", "gtreqqless", "gtrless",
            "gtrsim", "gtcc", "gtcir", "gtlpar", "gtquest",
            "leadsto", "leftarrowtail", "leftharpoonaccent", "leftharpoondown",
            "leftharpoondownbar", "leftharpoonsupdown", "leftharpoonup",
            "leftharpoonupbar", "leftharpoonupdash", "leftleftarrows", "leftmoon",
            "leftouterjoin", "leftrightarrow", "leftrightarrowcircle",
            "leftrightarrows", "leftrightarrowtriangle", "leftrightharpoondowndown",
            "leftrightharpoondownup", "leftrightharpoons", "leftrightharpoonsdown",
            "leftrightharpoonsup", "leftrightharpoonupdown", "leftrightharpoonupup",
            "leftrightsquigarrow", "leftsquigarrow", "lefttail", "leftthreearrows",
            "leftthreetimes", "leftwavearrow", "leftwhitearrow", "rightarrowtail",
            "rightarrowapprox", "rightarrowbackapprox", "rightarrowbar",
            "rightarrowbsimilar", "rightarrowdiamond", "rightarrowgtr",
            "rightarrowonoplus", "rightarrowplus", "rightarrowshortleftarrow",
            "rightarrowsimilar", "rightarrowsupset", "rightarrowtriangle",
            "rightarrowx", "nearrow", "neovnwarrow", "neovsearrow", "nequiv",
            "neswarrow", "neuter", "uparrowbarred", "uparrowoncircle",
            "updownarrowbar", "updownarrows",
            "ltimes", "ltcc", "ltcir", "ltlarr", "ltquest", "ltrivb", "lneq", "lneqq",
            "lnsim", "lnapprox", "lvertneqq", "lgE", "lgblkcircle", "lgblksquare",
            "lgwhtcircle", "lgwhtsquare",
            "intercal", "interleave", "intextender", "intBar", "intbar", "intbottom",
            "intcap", "intclockwise", "intcup", "intlarhk", "intprod", "intprodr",
            "inttop", "intx", "intop", "intertext", "increment", "inversebullet",
            "inversewhitecircle", "invlazys", "invnot", "invwhitelowerhalfcircle",
            "invwhiteupperhalfcircle", "subsetneq", "subsetneqq", "subseteqq",
            "subsetapprox", "subsetcirc", "subsetdot", "subsetplus", "supsetneq",
            "supsetneqq", "supseteqq", "supsetapprox", "supsetcirc", "supsetdot",
            "supsetplus", "supsim", "supsub", "supsup", "supdsub", "supedot",
            "suphsol", "suphsub", "suplarr", "supmult", "sumbottom", "sumint",
            "sumtop", "niobar", "nis", "nisd",
            "middle", "midbarvee", "midbarwedge", "midcir", "parallelogram",
            "parallelogramblack", "perps", "notag", "notni", "models", "modtwosum",
            "cdotp", "circeq", "circlearrowleft", "circlearrowright",
            "circlebottomhalfblack", "circledS", "circledast", "circledbullet",
            "circledcirc", "circleddash", "circledequal", "circledownarrow",
            "circledparallel", "circledrightdot", "circledstar", "circledtwodots",
            "circledvert", "circledwhitebullet", "circlehbar", "circlelefthalfblack",
            "circlellquad", "circlelrquad", "circleonleftarrow", "circleonrightarrow",
            "circlerighthalfblack", "circletophalfblack", "circleulquad",
            "circleurquad", "circleurquadblack", "circlevertfill", "cupbarcap",
            "cupdot", "cupleftarrow", "cupovercap", "cupvee", "capbarcup", "capdot",
            "capovercup", "capwedge", "opluslhrim", "oplusrhrim", "otimeshat",
            "otimeslhrim", "otimesrhrim", "divideontimes", "divslash", "timesbar",
            "pmb", "asteq", "asteraccent", "astrosun",
            "dotsb", "dotsc", "dotsi", "dotsm", "dotso", "dotsim", "dotsminusdots",
            "ddotseq", "wedgebar", "wedgedot", "wedgedoublebar", "wedgemidvert",
            "wedgeodot", "wedgeonwedge", "wedgeq", "veebar", "veedot", "veedoublebar",
            "veeeq", "veemidvert", "veeodot", "veeonvee", "veeonwedge", "vertoverlay",
            "newcommand", "renewcommand", "providecommand", "infin", "circledR",
        };
        auto is_known = [&](const std::string &w) {
            return kKnownPrefixes.count(w) != 0 ||
                   kKnownFullCommands.count(w) != 0 ||
                   (w.size() > 3 && w.compare(0, 3, "lgo") == 0);
        };
        // 只有这些「短命令」允许被拆开（\leN → \le{}N）；\textcolor 这类长命令即使含已知前缀也绝不拆，避免破坏命令
        static const std::set<std::string> kSafeSplit = {
            "le", "leq", "ge", "geq", "ne", "neq", "sim", "simeq", "approx",
            "equiv", "propto", "lt", "gt", "times", "div", "pm", "mp",
            "cdot", "ast", "circ", "oplus", "ominus", "otimes", "oslash",
            "cup", "cap", "subset", "supset", "subseteq", "supseteq",
            "in", "notin", "ni", "mid", "nmid", "parallel", "perp", "bot",
            "top", "to", "gets", "mapsto", "rightarrow", "leftarrow",
            "Rightarrow", "Leftarrow", "Leftrightarrow", "iff", "implies",
            "uparrow", "downarrow", "updownarrow", "dots", "cdots", "ldots",
            "vdots", "ddots", "quad", "qquad", "land", "lor", "wedge", "vee",
            "lnot", "neg", "lfloor", "rfloor", "lceil", "rceil", "lbrace",
            "rbrace", "langle", "rangle", "lvert", "rvert", "lVert", "rVert",
            "vert", "Vert", "newline", "max", "min", "log", "ln", "lg", "gcd", "lcm",
            "mod", "bmod", "pmod", "sum", "prod", "int", "iint", "iiint",
            "oint", "lim", "limsup", "liminf", "sup", "inf", "det", "dim",
            "exp", "deg", "arg", "ker", "hom", "Pr", "rank", "sin", "cos",
            "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan",
            "sinh", "cosh", "tanh", "coth", "argmax", "argmin",
        };
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s[p] == '\\' && p + 1 < s.size() &&
                std::isalpha(static_cast<unsigned char>(s[p + 1])))
            {
                size_t w = p + 1;
                while (w < s.size() &&
                       std::isalpha(static_cast<unsigned char>(s[w])))
                    ++w;
                const std::string word = s.substr(p + 1, w - p - 1);
                // 整词不是已知命令（\leN、\qquad第）时，按最长已知前缀拆开，并在命令后补空组，避免后续字母被并入命令名；
                // 整词是已知命令（\operatornamewithlimits、\frac12、\leqslant 等）则不动。
                if (!is_known(word))
                {
                    size_t best = std::string::npos;
                    for (size_t len = 1; len < word.size(); ++len)
                        if (is_known(word.substr(0, len)))
                            best = len;
                    if (best != std::string::npos &&
                        (kSafeSplit.count(word.substr(0, best)) ||
                         kKnownFullCommands.count(word.substr(0, best))))
                    {
                        t += "\\" + word.substr(0, best) + "{}" +
                             word.substr(best);
                        p = w;
                        continue;
                    }
                }
                // CJK 等非 ASCII 字符不是 isalpha，单词扫描会停在它前面；若上面没能拆开（如 \qquad第），在整词后补空组
                if (w < s.size() &&
                    static_cast<unsigned char>(s[w]) >= 0x80)
                {
                    t += s.substr(p, w - p);
                    t += "{}";
                    p = w;
                    continue;
                }
                t += s.substr(p, w - p);
                p = w;
                continue;
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // \def\or{...} 会重定义 LaTeX 数组前导里的内部命令 \or，导致 "in array arg"；统一改名为 \lgooor
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 3, "\\or") == 0 &&
                (p + 3 >= s.size() ||
                 !std::isalpha(static_cast<unsigned char>(s[p + 3]))))
            {
                t += "\\lgooor";
                p += 3;
                continue;
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // 洛谷常见的 $^$（表示二进制异或）没有底数，编译报错：裸上/下标补空底数 ${}^1$，末尾悬空的 ^ / _ 直接输出 \wedge。
    // 放在最后处理——前面 \texttt{...}\\ 等转换会改变 ^ / _ 的相邻字符。
    {
        static const std::regex kBareCaret(R"(\$[\^_]\$)");
        s = std::regex_replace(s, kBareCaret, "$\\wedge$");

        static const std::regex kNoBaseCaret(R"((?:^|\$)[\^_])");
        s = regex_transform(s, kNoBaseCaret, [&](const std::smatch &m) {
            const std::string pre = m[0].str();
            return pre.substr(0, pre.size() - 1) + "{}" + pre.back();
        });

        // 与上面同名的 kTrailingCaret 作用域不同：这一遍要处理前面转换新产生的悬空 ^ / _，
        // 故用独立名字，避免 MSVC 的 C4456（重名遮蔽）警告
        static const std::regex kTrailingCaretFinal(R"((^|[^\\])([\^_])(?=\s*\$?\s*$))");
        s = regex_transform(s, kTrailingCaretFinal, [&](const std::smatch &m) {
            return m[1].str() + (m[2].str() == "^" ? "\\wedge" : "\\text{\\_}");
        });
    }

    // 还原 \verb 内容（此时所有转换已完成，按文本模式转义即可）
    for (size_t vi = 0; vi < verb_raws.size(); ++vi)
    {
        std::string esc;
        for (char c : verb_raws[vi])
        {
            switch (c)
            {
            case '\\': esc += "\\textbackslash{}"; break;
            case '{': esc += "\\{"; break;
            case '}': esc += "\\}"; break;
            case '_': esc += "\\_"; break;
            case '#': esc += "\\#"; break;
            case '%': esc += "\\%"; break;
            case '&': esc += "\\&"; break;
            case '~': esc += "\\textasciitilde{}"; break;
            case '^': esc += "\\textasciicircum{}"; break;
            case '$': esc += "\\$"; break;
            default: esc += c;
            }
        }
        const std::string ph = verb_placeholder(vi);
        size_t pos = 0;
        while ((pos = s.find(ph, pos)) != std::string::npos)
        {
            s.replace(pos, ph.size(), esc);
            pos += esc.size();
        }
    }
    // 最后一步：拆掉不可信内容里能读写文件/执行命令的控制词（\write18、\openout、\input、\catcode、\end{document} 等），
    // 必须在所有公式修复之后，避免修复逻辑把被拆开的控制词又重新拼回命令。
    return defuse_tex_commands(s);
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

// 转义普通文本中的 LaTeX 特殊字符（数学/代码已先用占位符保护）
std::string escape_latex(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\textbackslash{}"; break;
        case '{': out += "\\{"; break;
        case '}': out += "\\}"; break;
        case '#': out += "\\#"; break;
        case '%': out += "\\%"; break;
        case '&': out += "\\&"; break;
        case '_': out += "\\_"; break;
        case '~': out += "\\textasciitilde{}"; break;
        case '^': out += "\\textasciicircum{}"; break;
        case '$': out += "\\$"; break;
        case '<': out += "\\textless{}"; break;
        case '>': out += "\\textgreater{}"; break;
        default: out += c;
        }
    }

    // 部分 Unicode 标点在常用西文字体（Latin Modern 等）里没有字形，xeCJK 也不会把它们交给中文字体，编译时
    // 被静默丢弃（如 P1131 里的 U+2015 中文破折号整段消失，日志只报 Missing character）；这里换成等价的 LaTeX 命令。
    static const std::pair<const char *, const char *> kTextFallbacks[] = {
        {"\u2015", "\\textemdash{}"}, // ― 水平杠（中文破折号）
        {"\u2012", "\\textendash{}"}, // ‒ figure dash（数字宽的短横）
    };
    for (const auto &kv : kTextFallbacks)
    {
        const std::string ch = kv.first;
        size_t p = 0;
        while ((p = out.find(ch, p)) != std::string::npos)
        {
            out.replace(p, ch.size(), kv.second);
            p += std::strlen(kv.second);
        }
    }
    return out;
}

// 去掉 $...$ 与 $$...$$ 生成 PDF 书签用的纯文本标题：hyperref 无法把 unicode-math 的数学符号转成书签
// 字符串，含数学的题目标题须经 \texorpdfstring 提供纯文本备用串。
std::string strip_math_for_bookmark(std::string s)
{
    std::string out;
    size_t i = 0;
    while (i < s.size())
    {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '$')
        {
            const size_t close = s.find("$$", i + 2);
            i = (close == std::string::npos) ? s.size() : close + 2;
            continue;
        }
        if (s[i] == '$')
        {
            const size_t close = s.find('$', i + 1);
            i = (close == std::string::npos) ? s.size() : close + 1;
            continue;
        }
        out += s[i];
        ++i;
    }
    return out;
}

// \includegraphics / \IfFileExists 的路径归一化：\ 换成 /（TeX 不认 Windows 反斜杠；源路径已是 UTF-8）。
// 路径里的空格必须原样保留：这两个命令的文件名扫描不展开控制序列，写成 "\ "（控制符号）会被当成文件名的一部分，
// 图片永远找不到（已实测）——LaTeX 的带引号文件名机制本来就支持空格，Windows 用户目录因此能正常工作；
// 换行/制表符换成空格，否则会截断路径。
std::string escape_path(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c == '\\')
            out += '/';
        else if (c == '\n' || c == '\r' || c == '\t')
            out += ' ';
        else
            out += c;
    }
    return out;
}

// 路径能否安全写进 .tex：% 会注释掉本行剩余内容（连 } 一起吞掉）、# 是宏参数符、{ } 破坏分组，
// 且这四种字符在文件名里无法原地转义（\% \# 会排成字符而不是文件名字符）；命中时调用方跳过该图片并提示。
bool path_is_tex_safe(const std::string &s)
{
    return s.find_first_of("%#{}") == std::string::npos;
}

void warn_unsafe_image_path(const std::string &path)
{
    static std::once_flag warned;
    std::call_once(warned, [&path]() {
        std::fprintf(stderr,
                     "警告：图片路径 '%s' 含有 LaTeX 无法表示字符（%% # { }），"
                     "这些图片将被跳过；可把缓存目录改到不含这些字符的位置"
                     "（XDG_CACHE_HOME 或 HOME/临时目录）后重新导出。\n",
                     path.c_str());
    });
}

// \url{} 的参数转义：% 与 # 是 TeX 特殊字符，未转义会破坏编译或吞掉 URL 剩余部分；\ { } 还会提前闭合
// \url 的参数，让 URL 里的 \write18{...} / \input{...} 之类命令变成可执行的正文（题面、题解、文章里的链接
// 目标都是不可信远程文本）。hyperref 的 \url 会把 \\ 当字面反斜杠排版，但花括号要按 URL 规范写成百分号编码
// （{ } 本来就不允许直接出现在 URL 里），既安全又与原文等价（均已实测）。
std::string escape_url(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '%': out += "\\%"; break;
        case '#': out += "\\#"; break;
        case '\\': out += "\\\\"; break;
        case '{': out += "\\%7B"; break;
        case '}': out += "\\%7D"; break;
        default: out += c; break;
        }
    }
    return out;
}

// listings 的结束标记是字面字符串 \end{lstlisting}：它出现在代码行的任何位置（行首、行中、带前导空格）
// 都会提前结束环境，其后内容会被当成 LaTeX 正文编译。代码块与样例都来自不可信远程文本，故换成绝不会被
// listings 识别的等价写法 \lx@end{lstlisting}，再由下方 \lstset 的 literate 选项排版回原文（已实测逐字一致）。
const char *const kLstEndMarker = "\\lx@end{lstlisting}";
const char *const kLstLiterateOption =
    "    literate={\\\\lx@end\\{lstlisting\\}}{{\\textbackslash end\\{lstlisting\\}}}1,\n";

    // 把要写进 lstlisting 的文本安全化：只替换结束标记这一个字面量，其它内容逐字节保留，代码显示不受影响
std::string lst_safe(std::string s)
{
    static const std::string kEnd = "\\end{lstlisting}";
    size_t p = 0;
    while ((p = s.find(kEnd, p)) != std::string::npos)
    {
        s.replace(p, kEnd.size(), kLstEndMarker);
        p += std::strlen(kLstEndMarker);
    }
    return s;
}

// 视频链接判断：洛谷用“图片语法”插入 Bilibili 视频，或常见视频文件后缀
bool is_video_url(const std::string &url)
{
    // 洛谷的 Bilibili 视频专用语法：![](bilibili:BVxxx?page=N)
    if (url.rfind("bilibili:", 0) == 0)
        return true;
    if (url.find("bilibili.com/video/") != std::string::npos ||
        url.find("player.bilibili.com") != std::string::npos)
        return true;

    std::string path = url;
    const size_t q = path.find_first_of("?#");
    if (q != std::string::npos)
        path.resize(q);
    const std::string lower = to_lower_ascii(path);
    static const char *kExts[] = {".mp4", ".webm", ".ogv", ".m4v", ".mov", ".m3u8"};
    for (const char *e : kExts)
    {
        const size_t n = std::strlen(e);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0)
            return true;
    }
    return false;
}

// 是否看起来像真正的链接目标（防止题面里 [](一段文字) 这种写法被当成链接）
bool looks_like_url(const std::string &url)
{
    return url.rfind("http://", 0) == 0 ||
           url.rfind("https://", 0) == 0 ||
           url.rfind("mailto:", 0) == 0 ||
           url.rfind("bilibili:", 0) == 0 ||
           url.find("://") != std::string::npos;
}

// 输出用的视频链接目标：洛谷的 B 站视频伪链接（![](bilibili:BVxxx?page=1)）补全为
// https://www.bilibili.com/video/... 的完整网页 URL，其余视频（如 .mp4 直链）原样返回。
std::string video_link_target(const std::string &url)
{
    const std::string full = luogu::bilibili_video_url(url);
    return full.empty() ? url : full;
}

// data:image/...;base64,... 这类内嵌数据 URI：xelatex 无法使用，而且 base64 是一整串无空格文本，
// 会让 TeX 段落排版出问题（甚至段错误/卡死），一律跳过。
bool is_data_uri(const std::string &url)
{
    return url.rfind("data:", 0) == 0;
}

// 按文件头魔数识别缓存图片的真实格式
enum class ImageKind
{
    kPng,
    kJpeg,
    kGif,
    kWebp,
    kBmp,
    kSvg,
    kIco,
    kPdf,
    kEps,
    kUnknown,
};

ImageKind detect_image_kind(const std::filesystem::path &path)
{
    FILE *in = luogu::compat::fopen(path, "rb");
    if (!in)
        return ImageKind::kUnknown;
    unsigned char head[1024] = {0};
    const size_t n = std::fread(head, 1, sizeof(head), in);
    std::fclose(in);

    if (n >= 8 && std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0)
        return ImageKind::kPng;
    if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
        return ImageKind::kJpeg;
    if (n >= 6 && std::memcmp(head, "GIF8", 4) == 0)
        return ImageKind::kGif;
    if (n >= 12 && std::memcmp(head, "RIFF", 4) == 0 &&
        std::memcmp(head + 8, "WEBP", 4) == 0)
        return ImageKind::kWebp;
    if (n >= 2 && head[0] == 'B' && head[1] == 'M')
        return ImageKind::kBmp;
    // SVG：先跳过 UTF-8 BOM 与前导空白，再认 <svg / XML 声明 / DOCTYPE / 注释——只认文件开头正好是 "<svg"
    // 或 "<?xm" 会漏掉带 BOM、前面有空行或先写 DOCTYPE/注释的 SVG（被判成未知格式而静默丢弃）。
    {
        size_t k = 0;
        if (n >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)
            k = 3;
        while (k < n && std::isspace(head[k]))
            ++k;
        const std::string text(reinterpret_cast<const char *>(head) + k,
                               n > k ? n - k : 0);
        if (text.rfind("<svg", 0) == 0 || text.rfind("<?xml", 0) == 0)
            return ImageKind::kSvg;
        if ((text.rfind("<!DOCTYPE", 0) == 0 || text.rfind("<!--", 0) == 0) &&
            text.find("<svg") != std::string::npos)
            return ImageKind::kSvg;
    }
    if (n >= 4 && head[0] == 0x00 && head[1] == 0x00 &&
        head[2] == 0x01 && head[3] == 0x00)
        return ImageKind::kIco;
    if (n >= 5 && std::memcmp(head, "%PDF-", 5) == 0)
        return ImageKind::kPdf;
    if (n >= 2 && head[0] == '%' && head[1] == '!')
        return ImageKind::kEps;
    return ImageKind::kUnknown;
}

// 修正 JPEG 的 JFIF 像素密度：洛谷个别老图密度被写成 1 dpi，XeTeX 会把 405×256px 的图片按 405×256 英寸
// 排版，超过 TeX 的 19 英尺上限而报 Dimension too large。密度 < 72 dpi 时就地改写为 72 dpi（幂等）。
// 返回 true 表示无需修正或已修正；需要修正但缓存不可写时返回 false。
bool fix_jpeg_density(const std::filesystem::path &path)
{
    unsigned char head[24] = {0};
    {
        FILE *in = luogu::compat::fopen(path, "rb");
        if (!in)
            return false;
        const size_t n = std::fread(head, 1, sizeof(head), in);
        std::fclose(in);
        if (n < 18)
            return true;
    }
    const bool is_jfif = head[0] == 0xFF && head[1] == 0xD8 &&
                         head[2] == 0xFF && head[3] == 0xE0 &&
                         std::memcmp(head + 6, "JFIF", 4) == 0 && head[10] == 0;
    if (!is_jfif)
        return true;
    const unsigned units = head[13];
    const unsigned xd = (head[14] << 8) | head[15];
    const unsigned yd = (head[16] << 8) | head[17];
    if (units != 1 || (xd >= 72 && yd >= 72))
        return true;

    // 不原地改写缓存文件（写到一半被打断会让缓存 JPEG 永久损坏）：整份复制到同目录临时文件，只改副本的
    // 这 5 字节，校验通过后原子替换（与下载图片的写入方式一致）。
    const std::filesystem::path tmp = luogu::compat::temp_sibling_path(path);
    {
        FILE *src = luogu::compat::fopen(path, "rb");
        if (!src)
            return false;
        FILE *dst = luogu::compat::fopen(tmp, "wb");
        if (!dst)
        {
            std::fclose(src);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
        std::vector<unsigned char> buf(65536);
        bool copied = true;
        for (;;)
        {
            const size_t got = std::fread(buf.data(), 1, buf.size(), src);
            if (got > 0 && std::fwrite(buf.data(), 1, got, dst) != got)
            {
                copied = false;
                break;
            }
            if (got < buf.size())
                break;
        }
        if (std::ferror(src))
            copied = false;
        const bool closed = (std::fclose(dst) == 0);
        std::fclose(src);
        if (!copied || !closed)
        {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    // 在临时副本上写入 72dpi（JFIF 密度字段：单位 + X 密度 + Y 密度）
    {
        FILE *out = luogu::compat::fopen(tmp, "r+b");
        if (!out)
        {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
        const unsigned char k72[] = {1, 0, 72, 0, 72};
        bool ok = std::fseek(out, 13, SEEK_SET) == 0 &&
                  std::fwrite(k72, 1, sizeof(k72), out) == sizeof(k72);
        ok = (luogu::compat::flush_and_sync(out) && ok);
        if (std::fclose(out) != 0)
            ok = false;
        if (!ok)
        {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::string replace_error;
    if (!luogu::compat::atomic_replace(tmp, path, replace_error))
    {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// 让缓存图片能被 xelatex 正常加载：GIF/WebP/BMP/SVG/ICO 以及无法识别的内容返回空路径（调用方跳过该图）；
// JPEG 密度异常时先修正；PNG/JPEG/PDF/EPS 的文件名扩展名与真实内容不符（无扩展名、扩展名是 URL 的残余、
// 或扩展名与内容不符）时，在缓存目录生成带正确扩展名的副本——XeTeX 按扩展名选择解码器。
std::filesystem::path prepare_cached_image(const std::filesystem::path &cache_path)
{
    std::error_code ec;
    if (!std::filesystem::exists(cache_path, ec) || ec)
        return {};

    const ImageKind kind = detect_image_kind(cache_path);
    switch (kind)
    {
        case ImageKind::kPng:
        case ImageKind::kJpeg:
        case ImageKind::kPdf:
        case ImageKind::kEps:
            break;
        default:
            return {}; // xelatex 无法加载的格式，直接跳过
    }

    if (kind == ImageKind::kJpeg && !fix_jpeg_density(cache_path))
        return {}; // 需要修正密度但缓存不可写，跳过避免编译报错

    const char *want_ext = nullptr;
    switch (kind)
    {
        case ImageKind::kPng: want_ext = ".png"; break;
        case ImageKind::kJpeg: want_ext = ".jpg"; break;
        case ImageKind::kPdf: want_ext = ".pdf"; break;
        case ImageKind::kEps: want_ext = ".eps"; break;
        default: break;
    }

    const std::string name = to_lower_ascii(cache_path.filename().string());
    static const char *kPngExts[] = {".png"};
    static const char *kJpegExts[] = {".jpg", ".jpeg"};
    static const char *kPdfExts[] = {".pdf"};
    static const char *kEpsExts[] = {".eps"};
    const char *const *matching = nullptr;
    size_t matching_count = 0;
    switch (kind)
    {
        case ImageKind::kPng: matching = kPngExts; matching_count = 1; break;
        case ImageKind::kJpeg: matching = kJpegExts; matching_count = 2; break;
        case ImageKind::kPdf: matching = kPdfExts; matching_count = 1; break;
        case ImageKind::kEps: matching = kEpsExts; matching_count = 1; break;
        default: break;
    }
    for (size_t i = 0; i < matching_count; ++i)
    {
        const size_t m = std::strlen(matching[i]);
        if (name.size() >= m && name.compare(name.size() - m, m, matching[i]) == 0)
            return cache_path;
    }

    // 生成带正确扩展名的副本（i 递增，避免与已有缓存文件冲突）
    for (int i = 0; i < 128; ++i)
    {
        std::filesystem::path copy = cache_path;
        if (i == 0)
            copy += want_ext;
        else
            copy += "_" + std::to_string(i) + want_ext;
        if (!std::filesystem::exists(copy, ec) && !ec)
        {
            std::filesystem::copy_file(cache_path, copy,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            return ec ? std::filesystem::path() : copy;
        }
    }
    return {};
}

std::string regex_transform(const std::string &s, const std::regex &re,
                            const std::function<std::string(const std::smatch &)> &convert)
{
    std::string out;
    size_t last = 0;
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
    {
        out += s.substr(last, it->position() - last);
        out += convert(*it);
        last = it->position() + it->length();
    }
    out += s.substr(last);
    return out;
}

std::string inline_to_latex(const std::string &text);
std::string inline_to_latex_impl(const std::string &text, std::vector<std::string> &raws,
                                 std::vector<bool> &is_math, int depth = 0);
std::string inline_code_latex(const std::string &raw);

// 当前导出过程的 LaTeX 显示选项：仅 export_latex 通过 OptionsGuard 设置，行内转换（如 bilibili 视频 URL
// 是否输出为超链接）据此判断；指针为空时按默认行为处理。
const latex::Options *g_options = nullptr;

struct OptionsGuard
{
    const latex::Options *previous;
    explicit OptionsGuard(const latex::Options *opt) : previous(g_options)
    {
        g_options = opt;
    }
    ~OptionsGuard() { g_options = previous; }
    OptionsGuard(const OptionsGuard &) = delete;
    OptionsGuard &operator=(const OptionsGuard &) = delete;
};

// 占位符 \x01R<n>\x02（用字符串拼接构造，避免 \x01R 被当成十六进制转义）
std::string placeholder(size_t index)
{
    return std::string(1, '\x01') + "R" + std::to_string(index) + std::string(1, '\x02');
}

// 恢复 \x01R<n>\x02 占位符（数字安全解析：内容里的畸形/超长数字不再抛异常）
std::string restore_placeholders(const std::string &s, const std::vector<std::string> &raws)
{
    std::string out;
    size_t i = 0;
    while (i < s.size())
    {
        if (s[i] == '\x01' && i + 1 < s.size() && s[i + 1] == 'R')
        {
            size_t j = i + 2;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
                ++j;
            if (j < s.size() && s[j] == '\x02' && j > i + 2)
            {
                size_t idx = 0;
                if (parse_nonneg_int(s.data() + i + 2, s.data() + j, idx) &&
                    idx < raws.size())
                    out += raws[idx];
                i = j + 1;
                continue;
            }
        }
        out += s[i];
        ++i;
    }
    return out;
}

// 合并相邻的数学占位符：洛谷的 \$$ 等畸形写法会把一个公式拆成多段（段间夹着多余的 $ 和空白），拼回一个。
// is_math 与 raws 必须保持下标一一对应（合并产生的新条目同样追加 is_math）。
void merge_adjacent_math(std::string &s,
                         std::vector<std::string> &raws,
                         std::vector<bool> &is_math)
{
    auto parse_placeholder = [&s](size_t p, size_t &idx, size_t &end) -> bool {
        if (p + 2 >= s.size() || s[p] != '\x01' || s[p + 1] != 'R')
            return false;
        size_t j = p + 2;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
            ++j;
        if (j >= s.size() || s[j] != '\x02' || j == p + 2)
            return false;
        return parse_nonneg_int(s.data() + p + 2, s.data() + j, idx) &&
               (end = j + 1, true);
    };

    std::string out;
    size_t i = 0;
    while (i < s.size())
    {
        size_t idx = 0, end = 0;
        if (!parse_placeholder(i, idx, end) || idx >= is_math.size() || !is_math[idx])
        {
            out += s[i];
            ++i;
            continue;
        }

        if (raws[idx].rfind("$$", 0) == 0)
        {
            out += s.substr(i, end - i);
            i = end;
            continue;
        }
        // 收集相邻的数学片段：仅当间隙里含多余的 $ 才合并（纯空白间隔的两个公式是独立的；严格相邻也合并）
        std::string merged = raws[idx];
        size_t run_end = end;
        while (run_end < s.size())
        {
            size_t gap_start = run_end;
            size_t gap_end = gap_start;
            bool gap_has_dollar = false;
            while (gap_end < s.size() &&
                   (s[gap_end] == '$' || std::isspace(static_cast<unsigned char>(s[gap_end]))))
            {
                if (s[gap_end] == '$')
                    gap_has_dollar = true;
                ++gap_end;
            }
            if (!gap_has_dollar && gap_end > gap_start)
                break;
            size_t nidx = 0, nend = 0;
            if (gap_end >= s.size() || !parse_placeholder(gap_end, nidx, nend) ||
                nidx >= is_math.size() || !is_math[nidx])
                break;
            if (raws[nidx].rfind("$$", 0) == 0)
                break;
            if (!merged.empty() && merged.back() == '$')
                merged.pop_back();
            for (size_t k = gap_start; k < gap_end; ++k)
                if (s[k] != '$')
                    merged += s[k]; // 保留空白，去掉多余的 $
            if (!raws[nidx].empty() && raws[nidx].front() == '$')
                merged += raws[nidx].substr(1);
            else
                merged += raws[nidx];
            run_end = nend;
        }
        raws.push_back(std::move(merged));
        is_math.push_back(true);
        out += "\x01R" + std::to_string(raws.size() - 1) + "\x02";
        i = run_end;
    }
    s = std::move(out);
}

// 下划线强调（_斜体_ / __粗体__）按 CommonMark 的 flanking 规则判断定界符：开定界符前、收定界符后不能是
// 词字符，因此 foo_bar_baz、a_1_b 里位于词内部的下划线保持原样（最后按普通下划线转义成 \_）。

// 取 pos 处（必须是 UTF-8 字符的起始字节）的码点，len 返回其字节数；非法/截断序列按单字节处理
unsigned int utf8_codepoint_at(const std::string &s, size_t pos, size_t &len)
{
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) { len = 1; return c; }
    size_t n = 1;
    unsigned int cp = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; n = 4; }
    else { len = 1; return c; }
    if (pos + n > s.size()) { len = 1; return c; }
    for (size_t k = 1; k < n; ++k)
    {
        const unsigned char cc = static_cast<unsigned char>(s[pos + k]);
        if ((cc & 0xC0) != 0x80) { len = 1; return c; }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    len = n;
    return cp;
}

// pos 之前那个字符的起始字节位置（pos == 0 时返回 npos）
size_t utf8_prev_pos(const std::string &s, size_t pos)
{
    if (pos == 0)
        return std::string::npos;
    size_t k = pos - 1;
    while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80)
        --k;
    return k;
}

// 码点是否算「词字符」：ASCII 字母/数字/下划线、汉字、假名、全角字母数字等；
// 常见标点区（通用标点、CJK 标点、全角标点）不算，否则「（_斜体_）」会被误判成词内下划线而不转换。
bool is_word_codepoint(unsigned int cp)
{
    if (cp < 0x80)
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
               (cp >= 'a' && cp <= 'z') || cp == '_';
    if ((cp >= 0x2000 && cp <= 0x206F) ||  // 通用标点（—…“”‘’等）
        (cp >= 0x3000 && cp <= 0x303F) ||  // CJK 标点（、。《》「」等）
        (cp >= 0xFE30 && cp <= 0xFE4F) ||  // CJK 兼容形式
        (cp >= 0xFF01 && cp <= 0xFF0F) ||  // 全角标点（！＂＃…）
        (cp >= 0xFF1A && cp <= 0xFF20) ||
        (cp >= 0xFF3B && cp <= 0xFF40) ||
        (cp >= 0xFF5B && cp <= 0xFF65))
        return false;
    return true;
}

// 码点是否为空白（含全角空格 U+3000）
bool is_space_codepoint(unsigned int cp)
{
    return cp == 0x3000 ||
           (cp < 0x80 && std::isspace(static_cast<unsigned char>(cp)) != 0);
}

// 下划线强调定界符判断：s 中从 pos 开始的 run 个 '_' 能否作为开定界符（后面不能是空白、前面不能是词字符）
bool underscore_can_open(const std::string &s, size_t pos, size_t run)
{
    if (pos + run >= s.size()) // 后面没有内容（不能以定界符结尾）
        return false;
    size_t len = 0;
    if (is_space_codepoint(utf8_codepoint_at(s, pos + run, len)))
        return false;
    if (pos == 0)
        return true;
    const size_t prev = utf8_prev_pos(s, pos);
    return !is_word_codepoint(utf8_codepoint_at(s, prev, len));
}

// s 中从 pos 开始的 run 个 '_' 能否作为收定界符（前面不能是空白、后面不能是词字符）
bool underscore_can_close(const std::string &s, size_t pos, size_t run)
{
    if (pos == 0)
        return false;
    size_t len = 0;
    const size_t prev = utf8_prev_pos(s, pos);
    if (is_space_codepoint(utf8_codepoint_at(s, prev, len)))
        return false;
    if (pos + run >= s.size())
        return true;
    return !is_word_codepoint(utf8_codepoint_at(s, pos + run, len));
}

std::string inline_to_latex_impl(const std::string &text, std::vector<std::string> &raws,
                                 std::vector<bool> &is_math, int depth)
{
    // 嵌套粗体/链接等行内语法的最大递归深度：超深嵌套（恶意内容）直接按普通文本转义，避免栈溢出。
    static const int kMaxInlineDepth = 50;
    if (depth > kMaxInlineDepth)
        return escape_latex(text);

    // 超长文本先切块再过正则管道（见 kMaxRegexChunk）：不切会让 std::regex 递归耗尽栈而崩溃；切块不改变 TeX 输出。
    if (text.size() > kMaxRegexChunk)
    {
        std::string out;
        for (const auto &piece : split_for_regex(text, kMaxRegexChunk))
            out += inline_to_latex_impl(piece, raws, is_math, depth + 1);
        return out;
    }

    auto protect = [&](std::string latex) {
        raws.push_back(std::move(latex));
        is_math.push_back(false);
        return placeholder(raws.size() - 1);
    };

    std::string s = text;

    // 把 \$（转义美元符）保护起来，避免数学提取把它的 $ 当成公式分隔符
    {
        const std::string dollar_sentinel = "\x01D\x02";
        std::string t;
        size_t p = 0, last = 0;
        while ((p = s.find("\\$", p)) != std::string::npos)
        {
            // \\$ 是“行分隔符 + 公式结束符”，不是转义美元符
            if (p > 0 && s[p - 1] == '\\')
            {
                p += 2;
                continue;
            }
            t += s.substr(last, p - last) + dollar_sentinel;
            p += 2;
            last = p;
        }
        t += s.substr(last);
        s = std::move(t);
    }
    {
        static const std::regex re("(\\$\\$[^$]+\\$\\$|\\$[^$]+\\$)");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            raws.push_back(sanitize_math(m[1].str()));
            is_math.push_back(true);
            return placeholder(raws.size() - 1);
        });
    }
    // 合并相邻的数学片段（源数据畸形时一个公式可能被拆成多段）
    merge_adjacent_math(s, raws, is_math);
    // 行内代码（放在数学之后：数学里的反引号是字面量，不应被当成代码分隔符）
    {
        // 支持 1~N 个反引号包裹的代码段（CommonMark 规则），避免 ``code`` 这类写法留下多余反引号
        static const std::regex re("`+([^`]+?)`+");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            // 行内代码在任意字符之间可断行，长代码不会顶出右边距
            return protect(inline_code_latex(m[1].str()));
        });
    }
    // HTML 图片 <img src="..."> → Markdown 图片语法，交给下面的图片处理
    // （洛谷题面/题解里两种写法都有）
    {
        static const std::regex re(R"(<img[^>]*\bsrc\s*=\s*["']([^"']+)["'][^>]*>)");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            return "![](" + m[1].str() + ")";
        });
    }
    // 图片包在链接里：[![](img)](url) → \href{url}{图片}；必须先于普通链接处理，
    // 否则内层 ] 会破坏链接解析
    {
        static const std::regex re(
            R"(\[!\[[^\]]*\]\s*\(\s*([^\s)]+)(?:\s+["'][^"']*["'])?\s*\)\s*\]\s*\(\s*([^\s)]+)(?:\s+["'][^"']*["'])?\s*\))");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            const std::string img_url = m[1].str();
            const std::string link_url = m[2].str();
            if (is_data_uri(img_url) || is_data_uri(link_url))
                return std::string(); // data URI 直接丢弃
            if (!looks_like_url(link_url) || !looks_like_url(img_url))
                return m[0].str();
            if (is_video_url(link_url) || is_video_url(img_url))
            {
                // B 站视频伪链接补全为完整网页 URL；--no-bilibili-link 时视频 URL 输出为普通文本而非超链接
                const std::string target = video_link_target(link_url);
                // 识别不了的 bilibili: 伪链接（编号非法时 video_link_target 原样返回）只输出普通文本，
                // 不生成 \url{bilibili:...} 这种打不开的死链
                if ((g_options && !g_options->bilibili_links) ||
                    target.rfind("bilibili:", 0) == 0)
                    return protect(escape_latex(target));
                return protect("\\url{" + escape_url(target) + "}");
            }
            // 按缓存文件真实内容判断能否加载；扩展名与内容不符的图片先经 prepare_cached_image 归一化
            const std::filesystem::path usable =
                prepare_cached_image(crawler::image_cache_path(img_url));
            if (usable.empty())
                return std::string(); // xelatex 无法加载的格式，直接跳过
            const std::string raw_path = luogu::compat::path_to_utf8(usable);
            if (!path_is_tex_safe(raw_path))
            {
                // 路径含 % # { }：写进 .tex 必然破坏编译，跳过并提示一次
                warn_unsafe_image_path(raw_path);
                return std::string();
            }
            const std::string path = escape_path(raw_path);
            // \noindent：图片单独成段时去掉段首缩进（约 2 个中文字宽），否则宽度恰为 \linewidth 的图片会超出右边界
            return protect("\\noindent\\href{" + escape_latex(link_url) + "}{"
                           "\\IfFileExists{" + path + "}"
                           "{\\luogoincludegraphics{" + path + "}}"
                           "{\\mbox{}}}");
        });
    }
    {
        static const std::regex re("!\\[([^\\]]*)\\]\\s*\\(\\s*([^\\s)]+)(?:\\s+[\"'][^\"']*[\"'])?\\s*\\)");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            const std::string alt = m[1].str();
            const std::string url = m[2].str();
            if (is_data_uri(url))
                return std::string(); // data URI 直接丢弃
            if (!looks_like_url(url))
            {
                // 不是真正的图片地址（如题面里的 ![](一段文字)）：按 Markdown 语义，替代文字为空的图片加载失败时
                // 不显示任何内容（也不能留下空行，整段内容为空时由段落层自动跳过）；有替代文字时保留原文，
                // 交由转义阶段原样输出
                if (alt.empty())
                    return std::string();
                return m[0].str();
            }
            if (is_video_url(url))
            {
                // 视频链接：同上，补全 B 站伪链接，并按 --no-bilibili-link 决定是否输出超链接
                const std::string target = video_link_target(url);
                if ((g_options && !g_options->bilibili_links) ||
                    target.rfind("bilibili:", 0) == 0)
                    return protect(escape_latex(target));
                return protect("\\url{" + escape_url(target) + "}");
            }
            // 按缓存文件真实内容判断能否加载：GIF/WebP/SVG/BMP/ICO 等 xelatex 无法加载的格式直接跳过；
            // 扩展名与内容不符的图片先经 prepare_cached_image 归一化
            const std::filesystem::path usable =
                prepare_cached_image(crawler::image_cache_path(url));
            if (usable.empty())
                return std::string();
            const std::string raw_path = luogu::compat::path_to_utf8(usable);
            if (!path_is_tex_safe(raw_path))
            {
                warn_unsafe_image_path(raw_path);
                return std::string();
            }
            const std::string path = escape_path(raw_path);
            return protect("\\noindent\\IfFileExists{" + path + "}"
                           "{\\luogoincludegraphics{" + path + "}}"
                           "{\\mbox{}}");
        });
    }
    {
        static const std::regex re("\\[([^\\]]*)\\]\\s*\\(\\s*([^\\s)]+)(?:\\s+[\"'][^\"']*[\"'])?\\s*\\)");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            const std::string label = m[1].str();
            // 链接文字为空（如题面里的 [](一段文字)）：Markdown 渲染后是一个看不见内容的超链接；直接输出空内容，
            // 既不会打印出 `[](…)` 原文，也不会因为整段只剩空内容而多出空行（段落层对空段落直接跳过）。
            if (label.empty())
                return std::string();
            if (is_data_uri(m[2].str()))
                return label; // data URI 链接：只保留链接文字
            if (!looks_like_url(m[2].str()))
                return m[0].str(); // 不是真正的链接目标，保留原文（转义阶段处理）
            // --no-bilibili-link：链接目标是 bilibili 视频 URL 时只保留链接文字
            if (g_options && !g_options->bilibili_links && is_video_url(m[2].str()))
                return protect(inline_to_latex_impl(label, raws, is_math, depth + 1));
            return protect("\\href{" + escape_latex(video_link_target(m[2].str())) + "}{" +
                           inline_to_latex_impl(label, raws, is_math, depth + 1) + "}");
        });
    }
    {
        static const std::regex re("<((?:https?://|bilibili:)[^>]+)>");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            const std::string target = video_link_target(m[1].str());
            if (((g_options && !g_options->bilibili_links) || target.rfind("bilibili:", 0) == 0) &&
                is_video_url(target))
                return protect(escape_latex(target));
            return protect("\\url{" + escape_url(target) + "}");
        });
    }
    {
        static const std::regex re("\\*\\*([^*]+)\\*\\*");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            return protect("\\textbf{" + inline_to_latex_impl(m[1].str(), raws, is_math, depth + 1) + "}");
        });
    }
    {
        static const std::regex re("\\*([^*]+)\\*");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            return protect("\\textit{" + inline_to_latex_impl(m[1].str(), raws, is_math, depth + 1) + "}");
        });
    }
    // 下划线形式的粗体/斜体：只在成对下划线位于词边界时转换（见 underscore_can_open/close），标识符里的
    // 下划线保持原样；内层递归转换，因此 _a **b** a_、_含 $x$ 的公式_ 都能正确嵌套
    {
        std::string t;
        size_t i = 0;
        while (i < s.size())
        {
            if (s[i] != '_')
            {
                t += s[i++];
                continue;
            }
            size_t run = 1;
            while (i + run < s.size() && s[i + run] == '_')
                ++run;
            // 只处理 1~2 个下划线（___x___ 之类不常见，按普通字符保留）
            size_t close = std::string::npos;
            size_t close_run = 0;
            if (run <= 2 && underscore_can_open(s, i, run))
            {
                size_t j = i + run;
                while (j < s.size())
                {
                    if (s[j] == '_')
                    {
                        size_t m = 1;
                        while (j + m < s.size() && s[j + m] == '_')
                            ++m;
                        if (m == run && underscore_can_close(s, j, m))
                        {
                            close = j;
                            close_run = m;
                            break;
                        }
                        j += m;
                        continue;
                    }
                    ++j;
                }
            }
            if (close != std::string::npos)
            {
                const std::string inner = s.substr(i + run, close - i - run);
                const std::string cmd = (run == 2) ? "\\textbf{" : "\\textit{";
                t += protect(cmd + inline_to_latex_impl(inner, raws, is_math, depth + 1) + "}");
                i = close + close_run;
                continue;
            }
            t.append(run, '_'); // 不构成强调：保持原样，最后统一转义成 \_
            i += run;
        }
        s = std::move(t);
    }
    {
        static const std::regex re("~~([^~]+)~~");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            return protect("\\sout{" + inline_to_latex_impl(m[1].str(), raws, is_math, depth + 1) + "}");
        });
    }
    s = escape_latex(s);
    // 恢复占位符：链接/粗体等递归生成的片段内部可能还嵌着占位符，需迭代还原直到不再出现 \x01；
    // 迭代次数加上限，防止内容自指（含占位符形态的控制字符）时无限循环
    std::string result = restore_placeholders(s, raws);
    const size_t kMaxRestoreRounds = raws.size() + 2;
    for (size_t round = 0; round < kMaxRestoreRounds &&
                           result.find('\x01') != std::string::npos; ++round)
    {
        result = restore_placeholders(result, raws);
    }
    // 还原被保护的 \$（转义美元符）
    {
        const std::string dollar_sentinel = "\x01D\x02";
        size_t p = 0;
        while ((p = result.find(dollar_sentinel, p)) != std::string::npos)
        {
            result.replace(p, dollar_sentinel.size(), "\\$");
            p += 2;
        }
    }
    return result;
}

std::string inline_to_latex(const std::string &text)
{
    // 占位符池只建一次：粗体/链接等递归调用共用同一池，否则嵌套占位符在递归中无法还原会死循环
    std::vector<std::string> raws;
    std::vector<bool> is_math;
    return inline_to_latex_impl(text, raws, is_math);
}

// 行内代码（Markdown 的 `code`）→ \texttt{...}：逐 UTF-8 字符转义后在字符间插 \allowbreak。
// 行内代码里没有空格可断行，长代码会整段顶出右边距，插断点后可在行末任意两字符间断开；
// 断点不能落在 \&、\_ 或 \textbackslash{} 这类命令中间，故必须逐字符转义后再拼接
std::string inline_code_latex(const std::string &raw)
{
    std::string out = "\\texttt{";
    for (size_t i = 0; i < raw.size();)
    {
        size_t len = 1;
        utf8_codepoint_at(raw, i, len);
        out += escape_latex(raw.substr(i, len));
        // \allowbreak 后跟空组而非空格：控制词后的空格会被 TeX 全部跳过，代码里的空格会消失
        out += "\\allowbreak{}";
        i += len;
    }
    out += "}";
    return out;
}

// 是否以某种“块级”语法开头（用于结束普通段落）
bool is_block_start(const std::string &t)
{
    if (t.empty())
        return false;
    if (t[0] == '#' || t[0] == '>' || t[0] == '|')
        return true;
    if (t[0] == '`' || t[0] == '~')
        return true;
    if (t.rfind("$$", 0) == 0 || t.rfind("::", 0) == 0)
        return true;
    static const std::regex kHr(R"(^([-*_])(\s*\1){2,}\s*$)");
    // 两个分支都必须锚定在行首：t 已经 trim 过，行中如「版本 2. 0」不算列表项，否则会拆断段落
    static const std::regex kItem(R"(^[-+*]\s+|^\d+\.\s+)");
    return std::regex_match(t, kHr) || std::regex_search(t, kItem);
}

// 是否为表格分隔行（|:---|:---:| 或 :-:|:-: 等，单元格只能由 - 和 : 组成）
bool is_table_separator_row(const std::string &line)
{
    std::string r = trim(line);
    if (r.empty() || r.find('|') == std::string::npos)
        return false; // 必须有 |，避免把分隔线 --- 误判成单列表格分隔行
    if (r.front() == '|')
        r.erase(r.begin());
    if (!r.empty() && r.back() == '|')
        r.pop_back();
    if (r.empty())
        return false;

    std::string cell;
    auto cell_ok = [](const std::string &c) {
        return !c.empty() &&
               std::all_of(c.begin(), c.end(), [](char x) { return x == '-' || x == ':'; });
    };
    for (char c : r)
    {
        if (c == '|')
        {
            if (!cell_ok(trim(cell)))
                return false;
            cell.clear();
        }
        else
        {
            cell += c;
        }
    }
    return cell_ok(trim(cell));
}

bool is_table_row(const std::string &line)
{
    return trim(line).find('|') != std::string::npos;
}

// 折叠框样式：洛谷的类型名、框线与标题条底色（与洛谷网页一致）、未指定标题时的默认标题；
// color 是生成的 .tex 中 \definecolor 定义的颜色名（见 export_latex）
struct FoldStyle
{
    const char *type;
    const char *rgb;
    const char *title;
    const char *color;
};

const FoldStyle kFoldStyles[] = {
    {"info", "52,152,219", "提示", "luogofoldinfo"},
    {"success", "82,196,26", "成功", "luogofoldsuccess"},
    {"warning", "255,193,22", "警告", "luogofoldwarning"},
    {"error", "231,76,60", "错误", "luogofolderror"},
};

// 按类型名找折叠框样式（大小写不敏感）；不是折叠框类型时返回 nullptr
const FoldStyle *find_fold_style(const std::string &type)
{
    const std::string t = to_lower_ascii(type);
    for (const auto &s : kFoldStyles)
    {
        if (t == s.type)
            return &s;
    }
    return nullptr;
}

// 解析折叠框起始行：冒号不少于 3 个表示嵌套在别的折叠框内，类型大小写不敏感，可带
// [标题] 与尾部 {选项}（如 {open}，PDF 里无意义）。成功时给出样式；标题可能为空，需补默认标题
bool parse_fold_opener(const std::string &line, const FoldStyle *&style,
                       std::string &title)
{
    static const std::regex kFold(
        R"(^\s*:{3,}\s*(info|success|warning|error)\s*(?:\[([^\]]*)\])?\s*(?:\{[^}]*\})?\s*$)",
        std::regex::icase);
    std::smatch m;
    if (!std::regex_match(line, m, kFold))
        return false;
    style = find_fold_style(m[1].str());
    if (!style)
        return false;
    title = m[2].matched ? trim(m[2].str()) : std::string();
    return true;
}

// 整行只由冒号组成（至少 3 个）→ ::: 风格块的收尾行
bool is_colon_closer(const std::string &t)
{
    return t.size() >= 3 && t.find_first_not_of(':') == std::string::npos;
}

// 洛谷的 ::cute-table 指令（::cute-table{tuack} / ::cute-table[]{tuack} / 无样式）：
// 声明紧随其后的表格按 Tuack 风格渲染，指令本身不是内容；各样式名渲染一致，这里不做区分
bool is_cute_table_opener(const std::string &t)
{
    static const std::regex kCute(
        R"(^:{2,}\s*cute-table\s*(?:\[[^\]]*\])?\s*(?:\{[^}]*\})?$)",
        std::regex::icase);
    return std::regex_match(t, kCute);
}

// 是否为 ::: 风格块的起始行（至少 2 个冒号且冒号后还有其他内容）；::cute-table 这类没有
// 收尾行的指令同样算（它也会打断普通段落），是否为容器请另用 is_cute_table_opener 判断
bool is_colon_opener(const std::string &t)
{
    size_t n = 0;
    while (n < t.size() && t[n] == ':')
        ++n;
    return n >= 2 && n < t.size();
}

// 洛谷的 remark-directive 指令：:::name[label]{attrs}（冒号不少于 3 个）是有配对收尾行的
// 容器，::name（冒号恰好 2 个）是没有收尾行的叶子；未知类型的容器同样按容器处理，否则
// 指令行会原样显示在文档里、配对的收尾行还会吃掉后面的环境
struct ColonDirective
{
    size_t colons = 0;
    std::string name;
    std::string label;
    std::string attrs;
};

bool parse_colon_directive(const std::string &line, ColonDirective &out)
{
    static const std::regex kDirective(
        R"(^\s*(:{2,})\s*([A-Za-z][A-Za-z0-9_-]*)(.*)$)");
    std::smatch m;
    if (!std::regex_match(line, m, kDirective))
        return false;
    out = ColonDirective();
    out.colons = m[1].str().size();
    out.name = to_lower_ascii(m[2].str());

    // 指令名之后只允许出现 [label] 与 {attrs}（各至多一个，顺序不限）；出现别的内容说明
    // 这一行不是指令（例如正文里的「:: 注意」），保持原样
    std::string rest = trim(m[3].str());
    while (!rest.empty())
    {
        const char open = rest[0];
        if (open != '[' && open != '{')
            return false;
        const char close = (open == '[') ? ']' : '}';
        const size_t end = rest.find(close);
        if (end == std::string::npos)
            return false;
        std::string &slot = (open == '[') ? out.label : out.attrs;
        if (slot.empty())
            slot = trim(rest.substr(1, end - 1));
        rest = trim(rest.substr(end + 1));
    }
    return true;
}

// 是否为容器指令（:::name，有配对的收尾行）：叶子指令（::name）没有收尾行，不能计入嵌套层数
bool is_container_directive(const std::string &line)
{
    ColonDirective dir;
    return parse_colon_directive(line, dir) && dir.colons >= 3;
}

// 自定义块环境栈的一层：env 为空表示不输出 LaTeX 环境（未知指令、epigraph 的 list 自己给全
// 开合标签）；close_extra 在 \end{env} 之前输出（epigraph 的横线与署名行）；quote_like 表示
// 容器内的小标题按普通粗体排版（引文区只有 2/5 版心宽，套一个真正的大标题会很难看）
struct EnvFrame
{
    std::string env;
    std::string close_extra;
    bool quote_like = false;
};

// 从 open_idx 之后找到与之配对的收尾行（整行冒号行）并返回其下标：按「开块 +1 / 收尾 -1」
// 计数即可正确配对（洛谷内外层冒号数可同可不同）；代码围栏（``` / ~~~）内的 ::: 要跳过，
// ::cute-table 不是容器不能计入层数。找不到收尾行（数据残缺）时返回 lines.size()
size_t find_block_closer(const std::vector<std::string> &lines, size_t open_idx)
{
    int depth = 1;
    char fence = 0;
    size_t fence_len = 0;
    for (size_t k = open_idx + 1; k < lines.size(); ++k)
    {
        const std::string t = trim(lines[k]);
        if (fence)
        {
            if (is_fence_closer(t, fence, fence_len))
                fence = 0;
            continue;
        }
        if (t.size() >= 3 && (t[0] == '`' || t[0] == '~'))
        {
            size_t n = 0;
            while (n < t.size() && t[n] == t[0])
                ++n;
            if (n >= 3)
            {
                fence = t[0];
                fence_len = n;
                continue;
            }
        }
        if (is_cute_table_opener(t))
            continue; // 不是容器：不影响嵌套层数
        if (is_colon_closer(t))
        {
            if (--depth == 0)
                return k;
        }
        else if (is_container_directive(t))
        {
            ++depth; // 叶子指令（::name）没有收尾行，不计入层数
        }
    }
    return lines.size();
}

// 嵌套的 mdframed 不能跨页（上游文档 Known Problems）：嵌在里面的折叠框一旦超过一页就会
// 丢内容，因此把嵌套折叠框的内容按顶层块切成若干矮块、首尾相接（见 fold_piece_latex），
// 分页只发生在块与块之间。估算只用于切块、宁可偏大：每块按 8 行估算（一页正文约 35 行），
// 折叠框内图片高度上限 0.4\textheight 按 18 行计，公式行按 2 行
constexpr int kFoldBoxMaxRows = 8;

// 与 prepare_cached_image 相同的「xelatex 能否加载」判断，但不产生任何副作用（不改写 JPEG
// 密度、不复制带正确扩展名的副本）：折叠框分页的行高估算不应改动缓存文件
bool cached_image_loadable(const std::filesystem::path &cache_path)
{
    std::error_code ec;
    if (!std::filesystem::exists(cache_path, ec) || ec)
        return false;
    const ImageKind kind = detect_image_kind(cache_path);
    return kind == ImageKind::kPng || kind == ImageKind::kJpeg ||
           kind == ImageKind::kPdf || kind == ImageKind::kEps;
}

// 一行里的图片在缓存中是否可用（xelatex 无法加载的格式与未下载的图片都会渲染成空盒）
bool line_has_usable_image(const std::string &line)
{
    static const std::regex kImg("!\\[[^\\]]*\\]\\s*\\(\\s*([^\\s)]+)");
    for (std::sregex_iterator it(line.begin(), line.end(), kImg), end;
         it != end; ++it)
    {
        const std::string url = (*it)[1].str();
        if (!looks_like_url(url) || is_video_url(url) || is_data_uri(url))
            continue;
        if (cached_image_loadable(crawler::image_cache_path(url)))
            return true;
    }
    return false;
}

// 一行去掉图片语法后是否只剩空白（即这一行只有图片）
bool line_is_image_only(const std::string &line)
{
    static const std::regex kImg("!\\[[^\\]]*\\]\\s*\\([^)]*\\)");
    if (line.find("![") == std::string::npos)
        return false;
    return std::regex_replace(line, kImg, "").find_first_not_of(" \t") ==
           std::string::npos;
}

// 该行是否以图片开头（图片按自然宽度/\linewidth 排版，不能加首行缩进）
bool line_starts_with_image(const std::string &line)
{
    const size_t p = line.find("![");
    if (p == std::string::npos)
        return false;
    return line.find_first_not_of(" \t") >= p;
}

// 整个段落是否只有图片（且至少有一张确实可用），用于给独立成段的图片前后留出间距
bool paragraph_is_image_only(
    const std::vector<std::pair<std::string, bool>> &parts)
{
    bool usable = false;
    for (const auto &p : parts)
    {
        if (!line_is_image_only(p.first))
            return false;
        if (line_has_usable_image(p.first))
            usable = true;
    }
    return usable;
}

// 一行文字的估算渲染行数（空行不计；图片高度上限见 kFoldBoxMaxRows 的注释）
int estimate_text_line_rows(const std::string &line)
{
    if (trim(line).empty())
        return 0;
    // 图片语法 ![alt](url)：只在图片确实可用时才有高度
    if (line.find("![") != std::string::npos)
        return line_has_usable_image(line) ? 18 : 1;
    double units = 0.0;
    for (size_t i = 0; i < line.size();)
    {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        size_t len = 1;
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3; // CJK 等全角字符按 1 个字符宽
        else if (c >= 0xC0)
            len = 2;
        units += (len >= 3) ? 1.0 : 0.5;
        i += len;
    }
    const int rows = static_cast<int>((units + 43.0) / 44.0);
    return rows < 1 ? 1 : rows;
}

// 折叠框内容里的一个顶层块（段落 / 代码块 / 表格 / 公式 / 嵌套块），行区间 [begin, end)，块内不允许切分
struct MarkdownBlock
{
    size_t begin = 0;
    size_t end = 0;
    int rows = 1;
};

// 扫描 [begin, end) 内的顶层块并估算各块的行数
std::vector<MarkdownBlock> scan_markdown_blocks(
    const std::vector<std::string> &lines, size_t begin, size_t end);

    // 该位置是否是一个「特殊块」的起始（代码围栏 / ::: 块 / 块级公式 / 表格），用于结束普通段落
bool starts_special_block(const std::vector<std::string> &lines, size_t k,
                          size_t end)
{
    const std::string t = trim(lines[k]);
    if (t.empty())
        return false;
    if (t.size() >= 3 && (t[0] == '`' || t[0] == '~'))
    {
        size_t n = 0;
        while (n < t.size() && t[n] == t[0])
            ++n;
        if (n >= 3)
            return true;
    }
    if (is_colon_opener(t) || t.rfind("$$", 0) == 0)
        return true;
    if (is_table_row(t))
    {
        size_t j = k + 1;
        while (j < end && trim(lines[j]).empty())
            ++j;
        if (j < end && is_table_separator_row(lines[j]))
            return true;
    }
    return false;
}

std::vector<MarkdownBlock> scan_markdown_blocks(
    const std::vector<std::string> &lines, size_t begin, size_t end)
{
    std::vector<MarkdownBlock> blocks;
    size_t i = begin;
    while (i < end)
    {
        if (trim(lines[i]).empty())
        {
            ++i;
            continue;
        }
        MarkdownBlock b;
        b.begin = i;
        const std::string t = trim(lines[i]);

        // 代码围栏：整段作为一个块
        size_t fence_len = 0;
        char fence = 0;
        if (t.size() >= 3 && (t[0] == '`' || t[0] == '~'))
        {
            size_t n = 0;
            while (n < t.size() && t[n] == t[0])
                ++n;
            if (n >= 3)
            {
                fence = t[0];
                fence_len = n;
            }
        }
        if (fence)
        {
            ++i;
            int rows = 2;
            while (i < end)
            {
                const std::string l = trim(lines[i]);
                ++rows;
                const bool closes = is_fence_closer(l, fence, fence_len);
                ++i;
                if (closes)
                    break;
            }
            b.end = i;
            b.rows = rows;
            blocks.push_back(b);
            continue;
        }

        // ::cute-table 指令 + 紧随其后的表格：整段作为一个表格块（指令与表格分开会让表格
        // 丢掉 Tuack 样式）
        if (is_cute_table_opener(t))
        {
            size_t j = i + 1;
            while (j < end && trim(lines[j]).empty())
                ++j;
            size_t k = j + 1;
            while (k < end && trim(lines[k]).empty())
                ++k;
            if (j < end && is_table_row(trim(lines[j])) &&
                k < end && is_table_separator_row(lines[k]))
            {
                int rows = 2;
                i = j;
                while (i < end && !trim(lines[i]).empty() && is_table_row(lines[i]))
                {
                    rows += 2; // 每个表格行按 2 行估算（含行距）
                    ++i;
                }
                b.end = i;
                b.rows = rows;
                blocks.push_back(b);
                continue;
            }
            // 指令后面不是表格：按普通段落处理（渲染时指令会被丢弃）
        }

        // ::: 块（折叠框 / epigraph / align 等）：整段作为一个块，块内的嵌套折叠框高度一并计入
        if (is_container_directive(t))
        {
            const size_t closer = find_block_closer(lines, i);
            const size_t inner_end = closer < end ? closer : end;
            int rows = 4; // 标题条与上下间距
            for (const auto &sub : scan_markdown_blocks(lines, i + 1, inner_end))
                rows += sub.rows;
            i = closer < end ? closer + 1 : end;
            b.end = i;
            b.rows = rows;
            blocks.push_back(b);
            continue;
        }

        if (t.rfind("$$", 0) == 0)
        {
            ++i;
            int rows = 2;
            while (i < end)
            {
                const std::string l = trim(lines[i]);
                rows += 2;
                ++i;
                if (l.find("$$") != std::string::npos)
                    break;
            }
            b.end = i;
            b.rows = rows;
            blocks.push_back(b);
            continue;
        }

        // 表格：连续含 | 的行（首行后跟分隔行才算表格）
        if (is_table_row(t))
        {
            size_t j = i + 1;
            while (j < end && trim(lines[j]).empty())
                ++j;
            if (j < end && is_table_separator_row(lines[j]))
            {
                int rows = 2;
                while (i < end && !trim(lines[i]).empty() && is_table_row(lines[i]))
                {
                    rows += 2;
                    ++i;
                }
                b.end = i;
                b.rows = rows;
                blocks.push_back(b);
                continue;
            }
        }

        // 普通段落：连续的非空行，遇到空行或其他特殊块为止
        {
            int rows = 1; // 段间距
            while (i < end)
            {
                if (trim(lines[i]).empty() ||
                    (i > b.begin && starts_special_block(lines, i, end)))
                    break;
                rows += estimate_text_line_rows(lines[i]);
                ++i;
            }
            b.end = i;
            b.rows = rows;
            blocks.push_back(b);
        }
    }
    return blocks;
}

// 把块按估算行数分组：每组不超过 budget 行（单个块本身超预算时自成一组）
std::vector<std::pair<size_t, size_t>> group_blocks_into_chunks(
    const std::vector<MarkdownBlock> &blocks, int budget)
{
    std::vector<std::pair<size_t, size_t>> chunks;
    size_t i = 0;
    while (i < blocks.size())
    {
        int rows = blocks[i].rows;
        size_t j = i + 1;
        while (j < blocks.size() && rows + blocks[j].rows <= budget)
        {
            rows += blocks[j].rows;
            ++j;
        }
        chunks.emplace_back(blocks[i].begin, blocks[j - 1].end);
        i = j;
    }
    return chunks;
}

// 折叠框渲染（mdframed 环境，样式见导言区的 \mdfdefinestyle{luogofoldbox}）：第一行是标题条
// （底色为折叠框颜色、白色粗体字），下面是框内内容（白底黑字，字体与正文一致）。不用 tabular
// 模拟：LaTeX 的表格是整体不可分割的盒子，超过一页的折叠框会被截断；mdframed 可以自然跨页。
// 顶层折叠框左右外边距为 0，占满整行宽度并可以自然跨页；嵌套折叠框见 fold_piece_latex
std::string fold_box_latex(const FoldStyle &style, const std::string &title,
                           const std::string &content)
{
    std::string out;
    out += "\\begin{mdframed}[style=luogofoldbox";
    out += ", linecolor=" + std::string(style.color);
    out += ", frametitlebackgroundcolor=" + std::string(style.color);
    // frametitle 用花括号包住：标题里的逗号/等号/右方括号不会被当成键值
    out += ", frametitle={" + title + "}]\n";
    // 框内图片的高度上限收紧到 0.4\textheight（默认是 \textheight）：图片（\hbox）无法被
    // 拆开，太高时一页只能放下一张、页底留下大片空白。mdframed 是分组，设置只在本框内生效
    out += "\\setlength{\\luogoimagemaxheight}{0.4\\textheight}%\n";
    out += content;
    if (!content.empty() && content.back() != '\n')
        out += '\n';
    out += "\\end{mdframed}\n\n";
    return out;
}

// 区块引用（Markdown 的 >）的渲染：mdframed 环境，样式见导言区的 luogoquote——只在左侧画
// 一条浅灰竖条（颜色与洛谷网页一致），竖条随内容跨页延续；嵌套引用见 quote_piece_latex
std::string quote_box_latex(const std::string &content)
{
    std::string out;
    out += "\\begin{mdframed}[style=luogoquote]\n";
    out += content;
    if (!content.empty() && content.back() != '\n')
        out += '\n';
    out += "\\end{mdframed}\n\n";
    return out;
}

// 嵌套区块引用的一块（引用里的引用，或折叠框里的引用）：与嵌套折叠框同理，mdframed 的嵌套
// 盒子不能跨页，内容按顶层块切成若干矮块后首尾相接，竖条看起来仍是连续的一条
std::string quote_piece_latex(const std::string &content, bool first, bool last)
{
    std::string out;
    out += "\\begin{mdframed}[style=luogoquote";
    out += first ? ", innertopmargin=2pt" : ", innertopmargin=0pt";
    out += last ? ", innerbottommargin=2pt" : ", innerbottommargin=0pt";
    out += ", skipabove=0pt, skipbelow=0pt]\n";
    // 小块不能跨页，框内图片高度上限与切块时的估算（18 行）保持一致
    out += "\\setlength{\\luogoimagemaxheight}{0.4\\textheight}%\n";
    out += content;
    if (!content.empty() && content.back() != '\n')
        out += '\n';
    out += "\\luogofoldnoparlist\n"; // 让本块结束后不再补竖直间距
    out += "\\end{mdframed}\n";
    out += "\\luogofoldparlist\n"; // 还原开关，避免影响后面的列表/盒子
    return out;
}

// 嵌套折叠框的一块：mdframed 的嵌套盒子不能跨页，内容按块切成矮块首尾相接，视觉上仍是一个
// 完整的框——只有第一块有标题条与上框线（topline）、只有最后一块有下框线，块与块之间没有
// 任何竖直间距（块内容末尾置 \@noparlist 让 \endtrivlist 跳过它会补回的竖直间距，环境结束
// 后再还原）。frametitle 必须显式给出，否则会继承上一级的标题；小块左右各缩进 1em
std::string fold_piece_latex(const FoldStyle &style, const std::string &title,
                             const std::string &content, bool first, bool last)
{
    std::string out;
    out += "\\begin{mdframed}[style=luogofoldbox";
    out += ", linecolor=" + std::string(style.color);
    out += ", frametitlebackgroundcolor=" + std::string(style.color);
    out += ", leftmargin=1em, rightmargin=1em";
    out += first ? ", topline=true" : ", topline=false";
    out += last ? ", bottomline=true" : ", bottomline=false";
    out += ", frametitle={" + (first ? title : std::string()) + "}";
    out += first ? ", innertopmargin=4pt" : ", innertopmargin=0pt";
    out += last ? ", innerbottommargin=4pt" : ", innerbottommargin=0pt";
    out += ", skipabove=0pt, skipbelow=0pt]\n";
    // 嵌套框内图片高度上限同样是 0.4\textheight，与切块时的估算（18 行）一致
    out += "\\setlength{\\luogoimagemaxheight}{0.4\\textheight}%\n";
    out += content;
    if (!content.empty() && content.back() != '\n')
        out += '\n';
    out += "\\luogofoldnoparlist\n";
    out += "\\end{mdframed}\n";
    out += "\\luogofoldparlist\n";
    return out;
}

// 把多个行内片段拼成一个段落：硬换行用 \\\\，丢弃转换后为空的片段
// （缺失图片会变成空），\\\\ 后紧跟 [ 时补 {} 防止被当作可选参数
std::string join_inline_parts(const std::vector<std::pair<std::string, bool>> &parts)
{
    std::vector<std::pair<std::string, bool>> out;
    for (const auto &rp : parts)
    {
        const std::string c = inline_to_latex(rp.first);
        // 只剩空白的片段同样丢弃：未下载的图片会变成空串，行首的空白会在这里
        // 留下 " " 之类的内容，让后面的 \\ 落在段首（LaTeX 报
        // "There's no line here to end"，折叠框内尤其容易触发）
        if (c.find_first_not_of(" \t") != std::string::npos)
            out.emplace_back(c, rp.second);
    }
    std::string para;
    for (size_t k = 0; k < out.size(); ++k)
    {
        if (k)
        {
            // 硬换行前补 {}：前一段可能是缺失图片（编译期为空），
            // 没有 {} 的话 \\ 前无内容会报 "There's no line here to end"
            para += out[k - 1].second ? " {}\\\\ " : " ";
            if (out[k].first[0] == '[')
                para += "{}";
        }
        para += out[k].first;
    }
    return para;
}

// 一行文本是否有未闭合的括号/方括号（链接可能跨行：
// [![](img)]( 换行 url)，需要把下一行并入同一片段才能被链接正则匹配）
bool has_unclosed_paren_or_bracket(const std::string &s)
{
    int paren = 0;
    int brack = 0;
    for (char c : s)
    {
        if (c == '(')
            ++paren;
        else if (c == ')')
        {
            if (paren > 0)
                --paren;
        }
        else if (c == '[')
            ++brack;
        else if (c == ']')
        {
            if (brack > 0)
                --brack;
        }
    }
    return paren > 0 || brack > 0;
}

// 表格：rows[0] 表头，rows[1] 对齐行，其余为内容行。支持洛谷表格合并语法：单元格内容恰为
// "^" 时向上合并（行合并），恰为 "<" 时向左合并（列合并），合并标记必须是单元格内唯一的
// 纯文本内容。合并解析保证每个合并区域都是矩形（\multicolumn/\multirow 可表达），无法表达
// 的交叉 / L 形合并会安全退化为空单元格，保证输出可编译。tuack = true 时按洛谷
// 「更像 Tuack 的表格」（::cute-table{tuack}）渲染：表格整体居中、去掉最左与最右两条竖线、
// 表头不加粗、最上/最下框线加粗、表头下方框线加粗一档；tuack = false 保持默认样式
void emit_table(const std::vector<std::string> &rows, std::string &out,
                bool tuack)
{
    auto split_cells = [](const std::string &row) {
        std::string r = trim(row);
        if (!r.empty() && r.front() == '|')
            r.erase(r.begin());
        if (!r.empty() && r.back() == '|')
            r.pop_back();
        std::vector<std::string> cells;
        std::string cur;
        for (char c : r)
        {
            if (c == '|')
            {
                cells.push_back(cur);
                cur.clear();
            }
            else
            {
                cur += c;
            }
        }
        cells.push_back(cur);
        // 去掉末尾的空单元格（源数据里常见 "||" 多出的空列）
        while (!cells.empty() && trim_cell(cells.back()).empty())
            cells.pop_back();
        return cells;
    };

    const std::vector<std::string> align_row = split_cells(rows[1]);
    const size_t col_count = align_row.size();
    if (col_count == 0)
        return; // 防御：无列时不再输出（正常数据至少 1 列）

    // 列规格：默认样式带左右外框线（首尾的 '|'）与列间竖线；Tuack 样式只去掉表格最左与
    // 最右那两条竖线——开头的 '|' 即最左框线，末列的 '|' 即最右框线，列间竖线全部保留
    std::string spec;
    if (!tuack)
        spec += '|';
    std::vector<char> col_types;
    col_types.reserve(col_count);
    for (size_t c = 0; c < col_count; ++c)
    {
        const std::string t = trim_cell(align_row[c]);
        char type;
        if (t.size() >= 3 && t.front() == ':' && t.back() == ':')
            type = 'c';
        else if (!t.empty() && t.front() == ':')
            type = 'l';
        else if (!t.empty() && t.back() == ':')
            type = 'r';
        else
            type = 'l';
        spec += type;
        // 每列后面的 '|'：默认样式一律保留（末列的是最右框线）；Tuack 样式只在后面还有列时保留
        if (!tuack || c + 1 < col_count)
            spec += '|';
        col_types.push_back(type);
    }

    // 所有行（表头 + 内容行）统一归一化到 col_count 列
    std::vector<std::vector<std::string>> grid;
    grid.reserve(rows.size() - 1);
    auto add_row = [&](const std::vector<std::string> &cells) {
        std::vector<std::string> v = cells;
        if (v.size() > col_count)
            v.resize(col_count);
        while (v.size() < col_count)
            v.push_back("");
        grid.push_back(std::move(v));
    };
    add_row(split_cells(rows[0]));
    for (size_t r = 2; r < rows.size(); ++r)
        add_row(split_cells(rows[r]));
    if (grid.empty())
        return; // 防御：没有任何行时不输出
    const size_t row_count = grid.size();

    // vtop[r][c]：单元格所属纵向合并的起始行；-1 表示不属于任何纵向合并
    // vend[r][c]：纵向合并的结束行（仅起始行单元格有效）
    // hsrc[r][c]：单元格所属横向合并的起始列；-1 表示不属于任何横向合并
    // hend[r][c]：横向合并的结束列（仅起始列单元格有效）
    std::vector<std::vector<long>> vtop(row_count, std::vector<long>(col_count, -1));
    std::vector<std::vector<long>> vend(row_count, std::vector<long>(col_count, -1));
    std::vector<std::vector<long>> hsrc(row_count, std::vector<long>(col_count, -1));
    std::vector<std::vector<long>> hend(row_count, std::vector<long>(col_count, -1));

    // 1) 纵向合并（^ 向上合并）：与上方单元格合并；上方单元格本身是 ^ 时
    //    继续向上（链式）。上方是 < 或合并失败的 ^ 时无法表达，按空单元格处理
    for (size_t c = 0; c < col_count; ++c)
    {
        for (size_t r = 1; r < row_count; ++r)
        {
            if (trim_cell(grid[r][c]) != "^")
                continue;
            long top = -1;
            if (vtop[r - 1][c] >= 0) // 上方是合并成功的 ^（链式）
                top = vtop[r - 1][c];
            else if (trim_cell(grid[r - 1][c]) != "^" &&
                     trim_cell(grid[r - 1][c]) != "<")
                top = static_cast<long>(r - 1); // 上方是普通内容单元格
            if (top < 0)
                continue; // 无法合并：按空单元格处理
            vtop[r][c] = top;
            vend[top][c] = static_cast<long>(r);
        }
    }

    // 2) 横向合并（< 向左合并，链式）：左侧是 ^ 或合并失败的 < 时无法表达，按空单元格处理；
    //    左侧内容单元格带有向下延伸的纵向合并时，仅当合并区域仍是矩形才合并，否则退化为空格
    for (size_t r = 0; r < row_count; ++r)
    {
        for (size_t c = 1; c < col_count; ++c)
        {
            if (trim_cell(grid[r][c]) != "<")
                continue;
            long src = -1;
            const std::string left = trim_cell(grid[r][c - 1]);
            if (left == "<")
                src = hsrc[r][c - 1]; // 链式：接左侧 < 的起点（失败则为 -1）
            else if (left != "^")
                src = static_cast<long>(c - 1); // 左侧是内容单元格
            if (src < 0 || src >= static_cast<long>(col_count))
                continue; // 无法合并：按空单元格处理
            // 左侧内容单元格下方存在纵向合并时，检查矩形区域是否全是合并标记
            if (vend[r][static_cast<size_t>(src)] > static_cast<long>(r))
            {
                bool rect_ok = true;
                const long vbottom = vend[r][static_cast<size_t>(src)];
                for (long rr = static_cast<long>(r) + 1;
                     rr <= vbottom && rect_ok; ++rr)
                {
                    for (long cc = src; cc <= static_cast<long>(c); ++cc)
                    {
                        const std::string t =
                            trim_cell(grid[static_cast<size_t>(rr)][static_cast<size_t>(cc)]);
                        if (t != "^" && t != "<")
                        {
                            rect_ok = false;
                            break;
                        }
                    }
                }
                if (!rect_ok)
                    continue;
            }
            hsrc[r][c] = src;
            hend[r][static_cast<size_t>(src)] = static_cast<long>(c);
        }
    }

    // 3) 标记「组合矩形」（\multicolumn 与 \multirow 叠加）覆盖的单元格：它们内部的横向
    //    分隔线必须跳过，否则会穿过合并单元格；同时记录矩形左右列边界，渲染时去掉内部竖线
    std::vector<std::vector<bool>> in_rect(row_count,
                                           std::vector<bool>(col_count, false));
    std::vector<std::vector<long>> rect_left(row_count,
                                             std::vector<long>(col_count, -1));
    std::vector<std::vector<long>> rect_right(row_count,
                                              std::vector<long>(col_count, -1));
    for (size_t r0 = 0; r0 < row_count; ++r0)
    {
        for (size_t c0 = 0; c0 < col_count; ++c0)
        {
            if (vend[r0][c0] <= static_cast<long>(r0) ||
                hend[r0][c0] <= static_cast<long>(c0))
                continue;
            for (long rr = static_cast<long>(r0); rr <= vend[r0][c0]; ++rr)
            {
                for (long cc = static_cast<long>(c0); cc <= hend[r0][c0]; ++cc)
                {
                    const size_t rri = static_cast<size_t>(rr);
                    const size_t cci = static_cast<size_t>(cc);
                    in_rect[rri][cci] = true;
                    rect_left[rri][cci] = static_cast<long>(c0);
                    rect_right[rri][cci] = hend[r0][c0];
                }
            }
        }
    }

    // 行 r 之后、列 c 处的横向分隔线是否穿过合并单元格内部
    auto boundary_blocked = [&](size_t r, size_t c) -> bool {
        const std::string cell = trim_cell(grid[r][c]);
        // 纵向合并跨过该边界继续向下
        const long t = (cell == "^" && vtop[r][c] >= 0)
                           ? vtop[r][c]
                           : static_cast<long>(r);
        if (vend[static_cast<size_t>(t)][c] > static_cast<long>(r))
            return true;
        // 组合矩形：该边界两侧的行都在矩形内
        return in_rect[r][c] && r + 1 < row_count && in_rect[r + 1][c];
    };

    // Tuack 风格的表格整体居中；默认样式不加 center，保持原有排版
    if (tuack)
        out += "\\begin{center}\n";
    out += "\\begin{tabular}{" + spec + "}\n";
    // 最上框线：默认样式是普通 \hline，Tuack 样式加粗
    out += tuack ? "\\luogotuackheavyrule\n" : "\\hline\n";
    for (size_t r = 0; r < row_count; ++r)
    {
        for (size_t c = 0; c < col_count; ++c)
        {
            const std::string cell = trim_cell(grid[r][c]);
            // 横向合并的内部单元格：由起始列的 \multicolumn 占用，不输出
            if (cell == "<" && hsrc[r][c] >= 0)
                continue;
            if (c > 0)
                out += " & ";
            if (cell == "^" || cell == "<")
            {
                // 组合矩形内部（非起始单元格）：上方 \multirow 会覆盖该单元格，但 tabular
                // 的列间竖线仍会穿过合并区域；用 \multicolumn{1} 重写本格的列规格，去掉矩形
                // 内部的竖线、只保留矩形左右边界处的竖线（首列补表格左侧外框，其余列左侧竖线
                // 由左邻列右侧的竖线负责，补上会把同一条线画成双线；Tuack 无最左右框线，不补）
                if (in_rect[r][c] &&
                    !(vend[r][c] > static_cast<long>(r) &&
                      hend[r][c] > static_cast<long>(c)) &&
                    rect_left[r][c] >= 0 && rect_right[r][c] >= 0)
                {
                    const bool right_same_rect =
                        c + 1 < col_count && in_rect[r][c + 1] &&
                        rect_left[r][c + 1] == rect_left[r][c] &&
                        rect_right[r][c + 1] == rect_right[r][c];
                    std::string mspec;
                    if (!tuack && c == 0)
                        mspec += '|';
                    mspec += col_types[c];
                    // 右侧竖线：末列的是表格最右框线（Tuack 样式不画），
                    // 其余位置是合并矩形右边界处的列间竖线（保留）
                    if (tuack ? (c + 1 < col_count && !right_same_rect)
                              : (c + 1 == col_count || !right_same_rect))
                        mspec += '|';
                    out += "\\multicolumn{1}{" + mspec + "}{}";
                }
                continue; // 纵向合并内部 / 合并失败：空单元格
            }
            size_t vlen = 1;
            if (vend[r][c] >= 0)
                vlen = static_cast<size_t>(vend[r][c]) - r + 1;
            size_t hlen = 1;
            if (hend[r][c] >= 0)
                hlen = static_cast<size_t>(hend[r][c]) - c + 1;
            std::string content = inline_to_latex(cell);
            // 表头（表格第一行）加粗：\luogotablehead 同时加粗文字与公式；Tuack 样式不加粗
            if (r == 0 && !tuack && !content.empty())
                content = "\\luogotablehead{" + content + "}";
            if (vlen > 1)
                content = "\\multirow{" + std::to_string(vlen) + "}{*}{" +
                          content + "}";
            if (hlen > 1)
            {
                // \multicolumn 的对齐规格只允许一个列类型：取被合并范围内第一列的对齐方式；
                // 默认样式保留首列左侧竖线与合并区域右侧的竖线，Tuack 只在右侧还有列时保留
                std::string mspec;
                if (!tuack && c == 0)
                    mspec += '|';
                mspec += col_types[c];
                if (!tuack || c + hlen < col_count)
                    mspec += '|';
                content = "\\multicolumn{" + std::to_string(hlen) + "}{" +
                          mspec + "}{" + content + "}";
            }
            out += content;
        }

        // 行分隔线：跳过合并单元格内部（合并单元格不应被横线穿过）。默认样式保持原来的 \hline；
        // Tuack 样式的最上、最下框线加粗，表头下方那条加粗一档，其余行仍是普通 \hline；
        // 分隔线被合并单元格挡住时（如 ^ 把表头与下面的行合并）只能退化为 \cline 分段
        bool any_blocked = false;
        for (size_t c = 0; c < col_count && !any_blocked; ++c)
        {
            if (boundary_blocked(r, c))
                any_blocked = true;
        }
        if (!any_blocked)
        {
            std::string rule = "\\hline";
            if (tuack)
            {
                if (r + 1 == row_count)
                    rule = "\\luogotuackheavyrule"; // 最下框线
                else if (r == 0)
                    rule = "\\luogotuackmidrule"; // 表头下方框线
            }
            out += " \\\\\n" + rule + "\n";
        }
        else
        {
            out += " \\\\\n";
            size_t c = 0;
            while (c < col_count)
            {
                if (boundary_blocked(r, c))
                {
                    ++c;
                    continue;
                }
                size_t seg_end = c;
                while (seg_end + 1 < col_count &&
                       !boundary_blocked(r, seg_end + 1))
                    ++seg_end;
                out += "\\cline{" + std::to_string(c + 1) + "-" +
                       std::to_string(seg_end + 1) + "}";
                c = seg_end + 1;
            }
            out += "\n";
        }
    }
    out += "\\end{tabular}\n";
    if (tuack)
        out += "\\end{center}\n";
    out += "\n";
}

// 键存在但为 null 时按缺省处理（多语言字段）
std::string safe_string(const nlohmann::json &j, const char *key)
{
    if (!j.contains(key) || !j[key].is_string())
        return "";
    return j[key].get<std::string>();
}

// 代码围栏的语言标记 → listings 的语言名。
// 未知语言返回空串（不高亮），listings 内置语言有限，其余按纯文本处理
std::string fence_to_listings_lang(std::string tag)
{
    tag = to_lower_ascii(trim(tag));
    // 围栏信息串可能带洛谷的附加选项（```cpp line-numbers、```python title=...），语言标记
    // 是其中的第一个词，其余选项忽略
    const size_t space = tag.find_first_of(" \t");
    if (space != std::string::npos)
        tag = tag.substr(0, space);
    if (tag.empty() || tag == "text" || tag == "plain" || tag == "none" ||
        tag == "txt" || tag == "console" || tag == "output" ||
        tag == "input" || tag == "markdown" || tag == "md" ||
        tag == "json" || tag == "yaml" || tag == "yml" || tag == "toml" ||
        tag == "diff" || tag == "ini" || tag == "csv" || tag == "dockerfile" ||
        tag == "gitignore" || tag == "log")
        return "";
    if (tag == "c" || tag == "c11" || tag == "c17")
        return "C";
    if (tag == "cpp" || tag == "c++" || tag == "cxx" || tag == "cc" ||
        tag == "c++11" || tag == "c++14" || tag == "c++17" || tag == "c++20")
        return "C++";
    if (tag == "c#" || tag == "csharp")
        return "CSharp"; // listings 语言名不能含 #，用自定义的 CSharp
    if (tag == "python" || tag == "py" || tag == "py3" || tag == "python3")
        return "Python";
    if (tag == "java")
        return "Java";
    if (tag == "pascal" || tag == "pas")
        return "Pascal";
    if (tag == "php")
        return "PHP";
    if (tag == "ruby" || tag == "rb")
        return "Ruby";
    if (tag == "go" || tag == "golang")
        return "Go";
    if (tag == "rust" || tag == "rs")
        return "Rust";
    if (tag == "javascript" || tag == "js" || tag == "node" ||
        tag == "nodejs" || tag == "jsx")
        return "JavaScript";
    if (tag == "typescript" || tag == "ts")
        return "TypeScript";
    if (tag == "html" || tag == "htm")
        return "HTML";
    if (tag == "xml" || tag == "svg")
        return "XML";
    if (tag == "css")
        return "CSS";
    if (tag == "bash" || tag == "sh" || tag == "shell" || tag == "zsh" ||
        tag == "bashrc")
        return "bash";
    if (tag == "sql")
        return "SQL";
    if (tag == "matlab")
        return "Matlab";
    if (tag == "octave")
        return "Octave";
    if (tag == "perl" || tag == "pl")
        return "Perl";
    if (tag == "lua")
        return "Lua";
    if (tag == "haskell" || tag == "hs")
        return "Haskell";
    if (tag == "lisp" || tag == "scheme" || tag == "elisp" ||
        tag == "clisp" || tag == "racket")
        return "Lisp";
    if (tag == "fortran" || tag == "f90" || tag == "f95" || tag == "f")
        return "Fortran";
    if (tag == "vb" || tag == "vbnet" || tag == "visualbasic" ||
        tag == "basic" || tag == "vba")
        return "VBScript";
    if (tag == "r" || tag == "rscript")
        return "R";
    if (tag == "makefile" || tag == "make" || tag == "gnumake")
        return "make";
    // Objective-C 是 C 的超集，用 C 高亮即可
    if (tag == "objective-c" || tag == "objc" || tag == "objectivec" ||
        tag == "m")
        return "C";
    if (tag == "erlang" || tag == "erl")
        return "Erlang";
    // delphi 不映射到 Delphi：导言区没有定义该 listings 语言，映射过去会报
    // Couldn't load requested language
    if (tag == "prolog")
        return "Prolog";
    if (tag == "verilog" || tag == "v")
        return "Verilog";
    if (tag == "vhdl")
        return "VHDL";
    if (tag == "latex" || tag == "tex")
        return "TeX";
    if (tag == "ada")
        return "Ada";
    if (tag == "awk")
        return "Awk";
    if (tag == "tcl" || tag == "tk")
        return "tcl";
    return "";
}

// 把超过 limit 字符的行拆成多行：listings 的 breaklines 会先测量整行宽度，超长行（如几千位
// 数字）总宽会超过 TeX 的 \maxdimen（~16383pt），报 "Dimension too large"；插入空格也没用
// （测量发生在断行之前），只能物理拆行，仅影响极少数病态长行。拆行时沿 UTF-8 字符边界切断
std::string split_long_line(std::string line,
                            size_t limit = 2500,
                            size_t chunk = 1000)
{
    if (line.size() <= limit)
        return line;
    std::string out;
    out.reserve(line.size() + line.size() / chunk + 1);
    size_t i = 0;
    while (i < line.size())
    {
        if (i)
            out += '\n';
        size_t end = i + chunk;
        if (end < line.size())
        {
            // 续字节（0b10xxxxxx）属于前一个多字节字符，顺延切点
            while (end < line.size() &&
                   (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80)
                ++end;
        }
        out += line.substr(i, end - i);
        i = end;
    }
    return out;
}

// 代码块（lstlisting）：breaklines 让超长行自动换行——listings 会先测量整行
// 宽度，超长行（如几千个括号）会生成极宽的 hbox，XeTeX 会直接崩溃。
std::string code_block_latex(const std::string &fence_lang,
                             const std::vector<std::string> &code_lines)
{
    const std::string lang = fence_to_listings_lang(fence_lang);
    std::string out = "\\begin{lstlisting}";
    if (!lang.empty())
        out += "[language=" + lang + ",breaklines=true]";
    else
        out += "[breaklines=true]";
    out += "\n";
    for (const auto &cl : code_lines)
        out += split_long_line(lst_safe(cl)) + "\n";
    out += "\\end{lstlisting}\n\n";
    return out;
}

// 按 '\n' 把字符串拆成行（不保留行尾换行；结尾换行不产生多余空行）
std::vector<std::string> split_lines(const std::string &content)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < content.size())
    {
        const size_t nl = content.find('\n', start);
        lines.push_back(nl == std::string::npos
                            ? content.substr(start)
                            : content.substr(start, nl - start));
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return lines;
}

// 逐行处理多行内容（用于样例输入/输出）
std::string split_long_lines(const std::string &content)
{
    std::string out;
    bool first = true;
    for (const auto &line : split_lines(content))
    {
        if (!first)
            out += '\n';
        first = false;
        out += split_long_line(line);
    }
    return out;
}

// 块级递归的深度上限：折叠框（:::info 等）与区块引用（>）会按嵌套层递归调用
// render_markdown，而嵌套层数完全由不可信输入决定——一行 `>>>>>>…`（十万个 >）就能让递归
// 深度等于输入长度，栈耗尽即 SIGSEGV（Windows 默认 1MB 栈更早崩溃）。超限时不再递归，
// 整段按普通文本转义输出（转义后不可能执行任何命令）
const int kMaxBlockDepth = 32;

// 把一段 markdown / HTML 文本转换为 LaTeX（块级处理）。折叠框 / 区块引用需要把框内内容整体
// 放进盒子里，因此按嵌套层递归调用自身：fold_depth / quote_depth 为当前所在的折叠框、
// 区块引用嵌套层数（0 = 不在其中）；box_para_indent 表示本层内容的第一个段落是否需要手动补
// 首行缩进——mdframed 的内容从水平模式开始排，盒子里的第一段不会自动缩进，嵌套盒子被切成
// 多块时只有第一块需要补（见 quote_piece_latex / fold_piece_latex）
std::string render_markdown(const std::string &markdown, int fold_depth,
                            int quote_depth = 0, bool box_para_indent = true)
{
    if (fold_depth + quote_depth > kMaxBlockDepth)
        return escape_latex(markdown) + "\n\n";

    std::vector<std::string> lines;
    lines = split_lines(markdown);
    lines.emplace_back(); // 末尾哨兵，简化处理

    std::string out;
    std::vector<EnvFrame> env_stack; // 自定义块环境栈（center/flushright/未知容器）
    // 上一行是 ::cute-table 指令：紧随其后的表格按 Tuack 样式渲染（中间允许空行）
    bool cute_table_pending = false;

    char fence = 0;        // 当前代码围栏字符（` 或 ~），0 表示不在代码块内
    size_t fence_len = 0;
    std::string fence_lang; // 围栏语言标记（```cpp 里的 cpp）
    std::vector<std::string> code_lines;

    size_t i = 0;
    while (i < lines.size())
    {
        const std::string raw = lines[i];
        const std::string line = trim(raw);

        if (fence)
        {
            if (is_fence_closer(line, fence, fence_len))
            {
                out += code_block_latex(fence_lang, code_lines);
                fence = 0;
                fence_len = 0;
                fence_lang.clear();
                code_lines.clear();
            }
            else
            {
                code_lines.push_back(raw);
            }
            ++i;
            continue;
        }

        if (line.empty())
        {
            ++i;
            continue;
        }

        // 本行生效的表格样式：标记先取走，其他内容自然把它作废
        const bool cute_table = cute_table_pending;
        cute_table_pending = false;

        if (line.size() >= 3 && (line[0] == '`' || line[0] == '~'))
        {
            size_t n = 0;
            while (n < line.size() && line[n] == line[0])
                ++n;
            if (n >= 3)
            {
                fence = line[0];
                fence_len = n;
                fence_lang = trim(line.substr(n));
                code_lines.clear();
                ++i;
                continue;
            }
        }

        if (line.rfind("$$", 0) == 0)
        {
            std::string math = line.substr(2);
            ++i; // 消费当前行（单行公式或起始行）
            // 单行公式：行内还有 $$ 即闭合（允许后面再跟文字，如 $$...$$。）
            const size_t close = math.find("$$");
            if (close != std::string::npos)
            {
                // 闭合 $$ 之后同一行还有文字时，把剩余部分放回待处理行、交给普通行逻辑；
                // 剩余部分本身以 $$ 开头（畸形写法，如 $$a$$$$b）时不回填，否则会新开一个
                // 块级公式把后面的段落全吞进 \[...\]
                const std::string rest = math.substr(close + 2);
                math = math.substr(0, close);
                if (!trim(rest).empty() && trim(rest).rfind("$$", 0) != 0)
                {
                    lines[i - 1] = rest;
                    --i;
                }
            }
            else
            {
                while (i < lines.size())
                {
                    const std::string l = trim(lines[i]);
                    // % 开头的行是 LaTeX 注释（常用来注释掉 \def 等），直接丢弃；否则 \%
                    // 转义会让注释内容真正执行
                    if (!l.empty() && l[0] == '%')
                    {
                        ++i;
                        continue;
                    }
                    const size_t lc = l.find("$$");
                    if (lc != std::string::npos)
                    {
                        math += "\n" + l.substr(0, lc);
                        // 闭合 $$ 之后同一行还有文字时同样放回队列，下一轮按普通行处理
                        const std::string rest = l.substr(lc + 2);
                        if (!trim(rest).empty() && trim(rest).rfind("$$", 0) != 0)
                            lines[i] = rest; // 不前进：下一轮按普通行处理
                        else
                            ++i; // 消费闭合行
                        break;
                    }
                    math += "\n" + lines[i];
                    ++i;
                }
            }

            // 过滤空行，避免显示公式里出现空行触发 "Missing $ inserted"
            std::vector<std::string> ml;
            for (const auto &mline : split_lines(math))
                if (!trim(mline).empty())
                    ml.push_back(trim(mline));
            out += "\\[\n" + sanitize_math(join_strings(ml, "\n")) + "\n\\]\n\n";
            continue;
        }

        // ::cute-table{tuack}：不是内容，只声明后面的表格用 Tuack 风格
        if (is_cute_table_opener(line))
        {
            cute_table_pending = true;
            ++i;
            continue;
        }
        if (line.size() >= 2 && line[0] == ':' && line[1] == ':')
        {
            static const std::regex kCloser(R"(^\s*:+$)", std::regex::icase);

            // 折叠框：找出配对的收尾行，把框内内容整段递归转换后放进 mdframed 盒子（标题条 +
            // 白底黑字内容），嵌套的折叠框因此可以一层层套进上一级框内。除 ::info[标题] 这种
            // 标准写法外，属性写在标题前面（:::info{open}[标题]）也按折叠框处理
            ColonDirective dir;
            const bool is_directive = parse_colon_directive(line, dir);
            const FoldStyle *fold = nullptr;
            std::string fold_title;
            if (!parse_fold_opener(line, fold, fold_title) && is_directive &&
                dir.colons >= 3)
            {
                // 属性写在标题前面的写法按通用指令解析：名字命中折叠框类型时同样按折叠框渲染
                fold = find_fold_style(dir.name);
                if (fold)
                    fold_title = dir.label;
            }
            if (fold)
            {
                const size_t closer = find_block_closer(lines, i);
                const size_t inner_begin = i + 1;
                const size_t inner_end =
                    (closer < lines.size()) ? closer : lines.size();
                std::string inner;
                for (size_t k = inner_begin; k < inner_end; ++k)
                {
                    if (k > inner_begin)
                        inner += '\n';
                    inner += lines[k];
                }
                if (fold_title.empty())
                    fold_title = fold->title; // 未指定标题时用默认标题

                if (fold_depth == 0)
                {
                    // 顶层折叠框：mdframed 可以自然跨页，整段放进一个框
                    out += fold_box_latex(*fold, inline_to_latex(fold_title),
                                          render_markdown(inner, fold_depth + 1));
                }
                else
                {
                    // 嵌套折叠框不能跨页：按顶层块切成若干矮块首尾相接，避免内容被截断或留空白
                    const auto blocks =
                        scan_markdown_blocks(lines, inner_begin, inner_end);
                    const auto chunks =
                        group_blocks_into_chunks(blocks, kFoldBoxMaxRows);
                    if (chunks.empty())
                    {
                        // 空框（框内只有空行）：仍然输出标题条
                        out += fold_piece_latex(*fold, inline_to_latex(fold_title),
                                                std::string(), true, true);
                    }
                    for (size_t c = 0; c < chunks.size(); ++c)
                    {
                        const auto &chunk = chunks[c];
                        std::string piece;
                        for (size_t k = chunk.first; k < chunk.second; ++k)
                        {
                            if (k > chunk.first)
                                piece += '\n';
                            piece += lines[k];
                        }
                        // 各块首尾相接：只有第一块画标题条与上框线、最后一块画下框线，视觉上仍是
                        // 一个完整的框；首行缩进同样只有第一块的第一段要补
                        out += fold_piece_latex(
                            *fold, inline_to_latex(fold_title),
                            render_markdown(piece, fold_depth + 1,
                                            quote_depth, c == 0),
                            c == 0, c + 1 == chunks.size());
                    }
                }
                // 找不到收尾行时把剩余内容都当作框内内容（不再前进到哨兵后）
                i = (closer < lines.size()) ? closer + 1 : lines.size();
                continue;
            }

            std::smatch m;
            if (std::regex_match(line, m, kCloser) && line.size() >= 3)
            {
                if (!env_stack.empty())
                {
                    // 收尾前先输出挂在这一层上的内容（epigraph 的署名行），未知指令的层无环境
                    out += env_stack.back().close_extra;
                    if (!env_stack.back().env.empty())
                        out += "\\end{" + env_stack.back().env + "}\n\n";
                    env_stack.pop_back();
                }
                ++i;
                continue;
            }
            if (is_directive)
            {
                // 叶子指令（::name[...]{...}）：本身没有内容，也没有收尾行
                if (dir.colons < 3)
                {
                    ++i;
                    continue;
                }
                // epigraph（题记，洛谷模仿 Codeforces 的 :::epigraph[署名]）：引文区整体占页面
                // 总宽的 2/5 并整体靠右，区内左对齐；引文与署名之间有一条贯穿引文区的横线，
                // 署名在区内右对齐、正体。引文默认用正体，只有作者自己写了 _斜体_ 的片段才斜体。
                // 用 list（leftmargin = 0.6\textwidth）而不是 minipage：list 会把 \linewidth
                // 设成区宽，区内的图片、表格、代码块同样不会超出 2/5，而且内容超长时可以跨页
                if (dir.name == "epigraph")
                {
                    EnvFrame frame; // env 为空：开合标签由 open/close_extra 给全
                    frame.quote_like = true; // 区内的小标题按普通粗体排版
                    if (!dir.label.empty())
                    {
                        // 横线与署名的间距：段落之间本来会插入一个 \baselineskip 的行距，这里用
                        // 负 \vspace 抵掉大部分只留一点空隙（否则会空出将近一整行）。\par 保证
                        // 横线另起一行：正文最后一行常用硬换行结束，改用换行命令会报
                        // "There's no line here to end"
                        frame.close_extra =
                            "\\par\\vspace{-0.45\\baselineskip}\\vspace{0.15\\baselineskip}\n"
                            "\\noindent\\rule{\\linewidth}{0.4pt}\n"
                            "\\par\\vspace{-0.1\\baselineskip}\n"
                            "\\noindent\\hfill{\\upshape " +
                            inline_to_latex(dir.label) + "}\n";
                    }
                    frame.close_extra += "\\end{list}\n\\endgroup\n";
                    env_stack.push_back(frame);
                    out += "\\begingroup\n\\begin{list}{}{%\n"
                           "  \\setlength{\\leftmargin}{0.6\\textwidth}%\n"
                           "  \\setlength{\\rightmargin}{0pt}%\n"
                           "  \\setlength{\\listparindent}{0pt}%\n"
                           "  \\setlength{\\itemindent}{0pt}%\n"
                           "  \\setlength{\\topsep}{0pt}%\n"
                           "  \\setlength{\\partopsep}{0pt}%\n"
                           "  \\setlength{\\parsep}{0pt}%\n"
                           "  \\setlength{\\itemsep}{0pt}%\n"
                           "}\n\\item\\relax\n"
                            // 区内的图片按区宽缩放，高度上限收紧到 0.4 版心高，否则竖长图片会顶出页面
                           "\\setlength{\\luogoimagemaxheight}{0.4\\textheight}%\n";
                    ++i;
                    continue;
                }
                // align{center} / align{right}：整块居中或居右，可选 [标题]
                if (dir.name == "align" &&
                    (dir.attrs == "center" || dir.attrs == "right"))
                {
                    EnvFrame frame;
                    frame.env = (dir.attrs == "center") ? "center" : "flushright";
                    env_stack.push_back(frame);
                    out += "\\begin{" + frame.env + "}\n";
                    if (!dir.label.empty())
                        out += "\\textbf{" + inline_to_latex(dir.label) + "}\\\\\n";
                    ++i;
                    continue;
                }
                // 其余容器指令（洛谷将来新增的类型、其它 remark-directive 写法）：只丢掉指令行
                // 本身，内容照常渲染；入栈是为了让配对的收尾行被正确吃掉（env 为空 = 不输出环境）
                env_stack.push_back(EnvFrame());
                ++i;
                continue;
            }
        }

        if (line[0] == '#')
        {
            size_t n = 0;
            while (n < line.size() && line[n] == '#')
                ++n;
            if (n == line.size() || line[n] == ' ')
            {
                const std::string raw_title = trim(line.substr(n));
                const std::string title = inline_to_latex(raw_title);
                const bool in_quote_like =
                    (!env_stack.empty() &&
                     (env_stack.back().quote_like ||
                      env_stack.back().env == "quote" ||
                      env_stack.back().env == "center" ||
                      env_stack.back().env == "flushright"));
                static const char *kCmds[] = {"section*", "subsection*", "subsubsection*",
                                              "paragraph*", "subparagraph*"};
                if (in_quote_like || n > 5)
                {
                    // 这些 # 标题同样按「正文黑体部分」处理：中文黑体、西文与代码块同字体
                    out += "\\textbf{{\\luogomarkdownheading " + title + "}}\n\n";
                }
                else if (n >= 2 && n <= 5)
                {
                    // Markdown 的 ## / ### / #### / ##### 对应 LaTeX 的 subsection /
                    // subsubsection / paragraph / subparagraph，使用 \luogomarkdownheading
                    // （用户指定 --set-font-title-* 时优先切换为用户标题字体）。\paragraph /
                    // \subparagraph 在 LaTeX 里默认是「接排标题」，导言区用 titlesec 把它们改成
                    // 独占一行的悬挂标题，与洛谷网页的 h4 / h5 一致
                    out += "\\" + std::string(kCmds[n - 1]) +
                           "{{\\luogomarkdownheading " + title + "}}\n\n";
                }
                else
                {
                    // 一级标题（#）：--local 转写本地 Markdown 时用 \section（进目录、写页眉）；
                    // 其余场合保持 \section*（题面 / 题解 / 文章正文里的一级标题不进目录、不改页眉）
                    const bool h1_section =
                        (n == 1) && g_options && g_options->h1_as_section;
                    if (h1_section)
                    {
                        // \section 会写目录并生成 PDF 书签：标题含公式时书签必须用
                        // \texorpdfstring 提供去掉数学后的纯文本备用串，否则会报 Token not
                        // allowed in a PDF string 且书签乱码；不含公式的标题保持原样输出
                        const std::string bookmark_text =
                            strip_math_for_bookmark(raw_title);
                        if (bookmark_text != raw_title)
                            out += "\\section{\\texorpdfstring{" + title + "}{" +
                                   escape_latex(bookmark_text) + "}}\n\n";
                        else
                            out += "\\section{" + title + "}\n\n";
                    }
                    else
                    {
                        out += "\\" + std::string(kCmds[n - 1]) + "{" + title + "}\n\n";
                    }
                }
                ++i;
                continue;
            }
        }

        {
            static const std::regex kHr(R"(^([-*_])(\s*\1){2,}\s*$)");
            if (std::regex_match(line, kHr))
            {
                out += "\\bigskip\n{\\color{gray} \\hrule}\n\\medskip\n\n";
                ++i;
                continue;
            }
        }

        // 区块引用（Markdown 的 >，可嵌套：> > 表示引用里的引用）：只剥掉一层「>」标记，
        // 剩下的内容交给递归调用按普通块级语法渲染——引用里的标题、列表、代码块、表格与
        // :::info / :::align 等指令因此都能正常处理。">" 后的一个空格属于标记的一部分，其余
        // 空白原样保留：行尾的两个空格是 Markdown 的硬换行标记，不能用 trim，否则会退化
        if (line[0] == '>')
        {
            std::vector<std::string> quoted;
            while (i < lines.size())
            {
                const std::string &raw_line = lines[i];
                const size_t start = raw_line.find_first_not_of(" \t");
                if (start == std::string::npos || raw_line[start] != '>')
                    break;
                size_t pos = start + 1; // 跳过这一层的 '>'
                if (pos < raw_line.size() && raw_line[pos] == ' ')
                    ++pos;
                quoted.push_back(raw_line.substr(pos));
                ++i;
            }

            std::string inner;
            for (size_t k = 0; k < quoted.size(); ++k)
            {
                if (k)
                    inner += '\n';
                inner += quoted[k];
            }

            if (fold_depth == 0 && quote_depth == 0)
            {
                // 顶层引用：mdframed 可以自然跨页，整段放进一个框
                const std::string body =
                    render_markdown(inner, fold_depth, quote_depth + 1);
                if (!trim(body).empty())
                    out += quote_box_latex(body);
            }
            else
            {
                // 嵌套引用（引用里的引用 / 折叠框里的引用）不能跨页：与嵌套折叠框同样按顶层块
                // 切成矮块、竖条首尾相接；引用里没有内容时 blocks 为空，不输出空框
                const auto blocks =
                    scan_markdown_blocks(quoted, 0, quoted.size());
                const auto chunks =
                    group_blocks_into_chunks(blocks, kFoldBoxMaxRows);
                for (size_t c = 0; c < chunks.size(); ++c)
                {
                    const auto &chunk = chunks[c];
                    std::string piece;
                    for (size_t k = chunk.first; k < chunk.second; ++k)
                    {
                        if (k > chunk.first)
                            piece += '\n';
                        piece += quoted[k];
                    }
                    out += quote_piece_latex(
                        render_markdown(piece, fold_depth, quote_depth + 1,
                                        c == 0),
                        c == 0, c + 1 == chunks.size());
                }
            }
            continue;
        }

        {
            static const std::regex kItem(R"(^(\s*)([-+*]|\d+\.)\s+(.*)$)");
            std::smatch m;
            if (std::regex_match(raw, m, kItem))
            {
                struct Frame
                {
                    int indent;
                    bool ordered;
                };
                std::vector<Frame> stack;
                auto open_env = [&](bool ordered) {
                    out += ordered ? "\\begin{enumerate}\n" : "\\begin{itemize}\n";
                };
                auto close_env = [&]() {
                    out += stack.back().ordered ? "\\end{enumerate}\n" : "\\end{itemize}\n";
                    stack.pop_back();
                };

                while (i < lines.size())
                {
                    std::smatch im;
                    if (!std::regex_match(lines[i], im, kItem))
                        break;
                    const int indent = static_cast<int>(im[1].str().size());
                    const std::string marker = im[2].str();
                    const bool ordered = std::isdigit(static_cast<unsigned char>(marker[0]));
                    std::string content = im[3].str();

                    if (stack.empty())
                    {
                        open_env(ordered);
                        stack.push_back({indent, ordered});
                    }
                    else if (indent > stack.back().indent)
                    {
                        // LaTeX 的 itemize/enumerate 最多嵌套 4 层，更深的层级归入最内层列表
                        if (stack.size() < 4)
                        {
                            open_env(ordered);
                            stack.push_back({indent, ordered});
                        }
                    }
                    else
                    {
                        while (!stack.empty() && indent < stack.back().indent)
                            close_env();
                        if (stack.empty() || ordered != stack.back().ordered)
                        {
                            if (!stack.empty())
                                close_env();
                            open_env(ordered);
                            stack.push_back({indent, ordered});
                        }
                    }

                    // 任务列表
                    std::string prefix;
                    if (content.rfind("[ ]", 0) == 0)
                    {
                        prefix = "$\\square$ ";
                        content = content.substr(3);
                    }
                    else if (content.rfind("[x]", 0) == 0 || content.rfind("[X]", 0) == 0)
                    {
                        prefix = "$\\boxtimes$ ";
                        content = content.substr(3);
                    }

                    std::string item = prefix + inline_to_latex(content);
                    // \item 内容以 [ 开头会被当作可选参数，用花括号包住
                    if (!item.empty() && item[0] == '[')
                        item = "{" + item + "}";
                    out += "\\item " + item + "\n";
                    ++i;

                    // 松散列表（loose list）：条目之间允许有空行，CommonMark 与洛谷的渲染都把它
                    // 当作同一个列表，编号连续；这里向后跳过空行，若下一非空行仍是列表条目就继续
                    // 当前列表（不跳过则每个条目各自成一个 enumerate，全部显示 1.）。空行后面
                    // 不是条目时列表结束，空行交给外层按段落处理
                    size_t next = i;
                    while (next < lines.size() && trim(lines[next]).empty())
                        ++next;
                    if (next > i && next < lines.size() &&
                        std::regex_match(lines[next], kItem))
                        i = next;
                }
                while (!stack.empty())
                    close_env();
                out += "\n";
                continue;
            }
        }

        if (is_table_row(line))
        {
            // 下一行是分隔行才按表格处理，避免误判普通含 | 的文本
            size_t j = i + 1;
            while (j < lines.size() && trim(lines[j]).empty())
                ++j;
            if (j < lines.size() && is_table_separator_row(lines[j]))
            {
                std::vector<std::string> rows;
                while (i < lines.size())
                {
                    const std::string t = trim(lines[i]);
                    if (t.empty() || !is_table_row(t))
                        break;
                    rows.push_back(t);
                    ++i;
                }
                if (rows.size() >= 2)
                    emit_table(rows, out, cute_table);
                else
                    for (const auto &r : rows)
                        out += inline_to_latex(r) + "\n\n";
                continue;
            }
            // 不是表格，落入普通段落处理
        }

        std::vector<std::pair<std::string, bool>> parts; // (文本, 是否硬换行)
        size_t collected = 0;
        while (i < lines.size())
        {
            const std::string r = lines[i];
            const std::string t = trim(r);
            // 首行即使像「块语法」也照常当段落收集，保证外层循环总能前进，避免单反引号、
            // 无空格标题等未被块处理器识别的行造成死循环
            if (t.empty() || (collected > 0 && is_block_start(t)))
                break;
            bool hard = false;
            std::string body = r;
            if (body.size() >= 2 && body[body.size() - 1] == ' ' && body[body.size() - 2] == ' ')
            {
                hard = true;
                body = body.substr(0, body.size() - 2);
            }
            else if (!body.empty() && body.back() == ' ')
            {
                body.pop_back(); // 单个尾部空格按普通空格处理
            }
            if (!parts.empty() &&
                has_unclosed_paren_or_bracket(parts.back().first))
            {
                // 上一行链接未闭合（如 [![](img)]( 换行 url），并入同一片段
                parts.back().first += " " + body;
                parts.back().second = parts.back().second || hard;
            }
            else
            {
                parts.emplace_back(body, hard);
            }
            ++collected;
            ++i;
        }
        if (!parts.empty())
        {
            std::string para = join_inline_parts(parts);
            if (!para.empty())
            {
                if (paragraph_is_image_only(parts))
                {
                    // 独立成段的图片：前后留一点间距，否则会与相邻文字贴在一起
                    para = "\\vspace{\\medskipamount}\n" + para +
                           "\n\\vspace{\\medskipamount}";
                }
                else if ((fold_depth > 0 || quote_depth > 0) &&
                         box_para_indent && out.empty() &&
                         !line_starts_with_image(parts.front().first))
                {
                    // 折叠框 / 区块引用内的第一段的首行缩进要自己补：mdframed 的内容从水平模式
                    // 开始排，LaTeX 不会给它加首行缩进，\indent 此时也无效，只能用 \hspace。只补
                    // 这一处（其他位置的段落 LaTeX 会自己缩进，补了会缩两次）；嵌套盒子切成多块
                    // 时只有第一块补（box_para_indent）；以图片开头的段落不缩进（会超出右边界）
                    para = "\\hspace{\\parindent}" + para;
                }
                out += para + "\n\n";
            }
            continue;
        }
        // 理论上到不了这里；保险起见直接前进，避免死循环
        ++i;
    }

    // 收尾：依次闭合所有未闭合的块。题面与题解都按「一个字段一次转换」调用本函数，所以作者
    // 漏写、上游把正文截断、或内容正好在块中间结束时，未闭合的内容一律按嵌套次序从内到外
    // 自动补齐，绝不因为缺少收尾标记而丢内容。折叠框的开合标签成对生成，显示公式与代码围栏
    // 在扫描到结尾时统一输出，epigraph 先补横线与署名；env_stack 从栈顶弹出即最内层先闭合
    if (fence)
        out += code_block_latex(fence_lang, code_lines);

    while (!env_stack.empty())
    {
        out += env_stack.back().close_extra;
        if (!env_stack.back().env.empty())
            out += "\\end{" + env_stack.back().env + "}\n\n";
        env_stack.pop_back();
    }
    return out;
}

} // namespace

std::string latex::markdown_to_latex(const std::string &markdown)
{
    return render_markdown(markdown, 0);
}

std::string latex::problem_to_latex(const problem::Problem &p, const Options &opt,
                                     const std::string &first_solution_lid)
{
    const bool use_en = (opt.lang == "en");

    auto field = [&](const char *key, const std::string &zh) -> std::string {
        if (!use_en)
            return zh;
        const std::string en = safe_string(p.translations, key);
        return en.empty() ? zh : en;
    };

    std::string out;
    // 题目标题进入 PDF 书签与目录：unicode-math 的数学符号无法转成书签
    // 字符串，用 \texorpdfstring 提供去数学的纯文本备用标题
    const std::string section_title = p.pid + " " + field("title", p.name);
    const std::string section_title_latex = "\\texorpdfstring{" +
                                            inline_to_latex(section_title) + "}{" +
                                            escape_latex(strip_math_for_bookmark(section_title)) +
                                            "}";
    // 题解导出的锚点与「查看题解」按钮：锚点 sol-problem-<PID> 由 problem_anchor 白名单化生成，
    // 供「返回题目」跳转；按钮绝不能放进 \section 的参数里，否则目录条目与 PDF 书签会被按钮
    // 污染——正确写法是「锚点 + \section[短标题]{标题 + \hfill + 按钮}」，方括号里的短标题
    // 只用于目录与书签，按钮只出现在正文标题行右侧
    const bool solutions_enabled = opt.solution_export.enabled &&
                                   !opt.solution_export.articles_only;
    const bool with_button = solutions_enabled &&
                             opt.solution_export.problem_to_article_link &&
                             !first_solution_lid.empty();
    const std::string section_short =
        escape_latex(strip_math_for_bookmark(section_title));
    std::string section_button;
    if (with_button)
        section_button = "\\hfill\\luogosolutionlink{" +
                         luogu::solution_anchor(p.pid, first_solution_lid) +
                         "}{查看题解}";
    if (solutions_enabled)
        out += "\\hypertarget{" + luogu::problem_anchor(p.pid) + "}{}%\n";

    // --show-contents-difficulty-tags：目录中的题目标题按难度着色（\luogotocsection 只给写进
    // 目录的标题文字上色，正文标题、页眉、PDF 书签与目录中的引导点/页码都保持黑色）。
    // 可选参数必须写成 [{...}]：题目名里常有 ']'（如「P3953 [NOIP 2017 提高组] 逛公园」），
    // 不加花括号时 LaTeX 会在第一个 ']' 处提前结束可选参数，标题被截断、剩余文字漏进正文
    if (opt.toc_difficulty)
        out += "\\luogotocsection{" +
               std::string(luogu::difficulty_color(p.difficulty)) + "}{" +
               section_short + "}{" + section_title_latex + section_button + "}\n\n";
    else if (with_button)
        out += "\\section[{" + section_short + "}]{" + section_title_latex +
               section_button + "}\n\n";
    else
        out += "\\section{" + section_title_latex + "}\n\n";

    // 标签 / 时空限制
    out += "\\begin{center}\n\\begin{tabularx}{\\textwidth}{XX}\n";
    const auto limits = luogu::format_limits(p.time, p.memory);
    out += "时间限制: " + limits.first + " & 内存限制: " + limits.second + " \\\\\n";
    out += "\\end{tabularx}\n\\end{center}\n";
    // 难度：位于时间/内存限制之下、标签之上（--show-difficulty-tags）。\noindent 与 5.78pt
    // 缩进和上面的限制行（\tabcolsep = 6pt）对齐；下面的标签行同样用 \noindent，否则它会作为
    // 新段落额外获得首行缩进，与难度行错开
    if (opt.display.difficulty)
        out += "\\noindent\\hspace{5.78pt}难度：\\textcolor[HTML]{" +
               std::string(luogu::difficulty_color(p.difficulty)) + "}{" +
               escape_latex(luogu::difficulty_label(p.difficulty)) + "}\n\n";
    // 算法（type 2）标签默认隐藏，--show-algorithm-tags 时显示，使用蓝色背景 rgb(41,73,180)，
    // 并排在其他标签之前
    const bool show_algorithm = opt.display.algorithm_tags;
    const bool show_source = opt.display.source_tags;
    auto tagsalgo = show_algorithm ? luogu::filter_tags_by_type(p.tags, 2)
                                   : std::vector<std::string>();
    auto tagsfrom = show_source ? luogu::filter_tags_by_type(p.tags, 3)
                                : std::vector<std::string>();
    auto tagsdata = show_source ? luogu::filter_tags_by_type(p.tags, 4)
                                : std::vector<std::string>();
    auto tagsarea = show_source ? luogu::filter_tags_by_type(p.tags, 1)
                                : std::vector<std::string>();
    auto tagsspec = show_source ? luogu::filter_tags_by_type(p.tags, 5)
                                : std::vector<std::string>();
    // 算法与来源/时间/区域/特殊标签都不显示时，不输出「标签」一栏。\noindent：与难度行（以及
    // 没有难度时紧跟在上方居中表格之后的情况）保持同一缩进，避免作为新段落被额外缩进首行
    if(!tagsalgo.empty() || !tagsfrom.empty() || !tagsdata.empty() || !tagsarea.empty() || !tagsspec.empty()) out += "\\noindent\\hspace{5.78pt}标签：";
    // 标签名来自 tags.json / 缓存，可能含 LaTeX 特殊字符（如 %、#、_），必须转义后才能放进
    // \luogotag 参数，否则编译失败或注入宏。徽章统一由 \luogotag 渲染，各标签框完全等高
    auto decorate_tags = [](std::vector<std::string> &tags, const char *color) {
        for (auto &tag : tags)
            tag = "\\luogotag{" + std::string(color) + "}{" + escape_latex(tag) + "}";
    };
    decorate_tags(tagsalgo, "2949b4");
    decorate_tags(tagsfrom, "13c2c2");
    decorate_tags(tagsdata, "3498db");
    decorate_tags(tagsarea, "53c41a");
    decorate_tags(tagsspec, "f39c11");
    if(!tagsalgo.empty()) out += join_strings(tagsalgo, " \\ ") + " \\ ";
    if(!tagsfrom.empty()) out += join_strings(tagsfrom, " \\ ") + " \\ ";
    if(!tagsdata.empty()) out += join_strings(tagsdata, " \\ ") + " \\ ";
    if(!tagsarea.empty()) out += join_strings(tagsarea, " \\ ") + " \\ ";
    if(!tagsspec.empty()) out += join_strings(tagsspec, " \\ ") + " \\ ";

    const std::string background = field("background", p.background);
    const std::string description = field("description", p.description);
    const std::string formatI = field("inputFormat", p.formatI);
    const std::string formatO = field("outputFormat", p.formatO);
    const std::string hint = field("hint", p.hint);

    if (!background.empty())
        out += "\\subsection*{题目背景}\n\n" + markdown_to_latex(background) + "\n";
    if (!description.empty())
        out += "\\subsection*{题目描述}\n\n" + markdown_to_latex(description) + "\n";
    if (!formatI.empty())
        out += "\\subsection*{输入格式}\n\n" + markdown_to_latex(formatI) + "\n";
    if (!formatO.empty())
        out += "\\subsection*{输出格式}\n\n" + markdown_to_latex(formatO) + "\n";

    int sample_no = 1;
    for (const auto &s : p.samples)
    {
        out += "\\subsection*{输入输出样例 \\#" + std::to_string(sample_no) + "}\n\n";
        out += "\\subsubsection*{输入 \\#" + std::to_string(sample_no) + "}\n\n";
        out += "\\begin{lstlisting}\n" +
               lst_safe(split_long_lines(s.first)) +
               "\n\\end{lstlisting}\n\n";
        out += "\\subsubsection*{输出 \\#" + std::to_string(sample_no) + "}\n\n";
        out += "\\begin{lstlisting}\n" +
               lst_safe(split_long_lines(s.second)) +
               "\n\\end{lstlisting}\n\n";
        ++sample_no;
    }

    if (!hint.empty())
        out += "\\subsection*{说明/提示}\n\n" + markdown_to_latex(hint) + "\n";

    return out;
}

std::string latex::article_to_latex(const article::Article &a)
{
    std::string out;
    // 标题进入 PDF 书签：数学符号用 \texorpdfstring 提供纯文本备用串
    out += "\\section{\\texorpdfstring{" + inline_to_latex(a.title) + "}{" +
           escape_latex(strip_math_for_bookmark(a.title)) + "}}\n\n";
    if (!a.author_name.empty())
        out += "作者：" + escape_latex(a.author_name) + " \\\\\n\n";
    out += markdown_to_latex(a.content) + "\n";
    return out;
}

namespace
{
// 一篇题解 / 文章共用的 LaTeX 正文渲染：题解是与题目一一绑定的特殊文章，两者除「返回题目」
// 按钮与目录中注明所属题目外完全一致。\luogotocsolution 负责锚点、目录条目与正文标题；
// heading_plain 为纯文本标题（页眉用）；toc_text 为目录与 PDF 书签文字（空串表示不进目录）；
// button 为标题行右侧的跳转按钮
std::string article_body_to_latex(const std::string &heading_plain,
                                  const std::string &heading_latex,
                                  const std::string &toc_text,
                                  const std::string &anchor,
                                  const std::string &button,
                                  const luogu::SolutionView &view,
                                  const luogu::SolutionExportOptions &sol_opt)
{
    std::string out;
    out += "\\luogotocsolution{" + toc_text + "}{" + heading_latex + "}{" + anchor +
           "}{" + button + "}\n\n";
    // 页眉：每篇题解 / 文章都把自己的标题写进 \rightmark，逻辑与题目页一致（题目由 \section
    // 设置页眉），因此文章页显示的是这一篇的标题，而不是前面题面的题目名。\rightmark 是纯文本
    // 页眉：标题与题目页眉一样做「转义 + 去数学」处理；\markright 不写目录、也不生成书签
    out += "\\markright{" + escape_latex(strip_math_for_bookmark(heading_plain)) + "}\n";

    // 元信息（--no-article-meta 关闭原文链接）
    if (sol_opt.article_meta)
    {
        out += "\\noindent{\\small 来源：" + escape_latex(view.source_name) +
               "　原文：\\href{" + escape_url(view.source_url) + "}{" +
               escape_latex(view.source_url) + "}}\n\n";
    }
    if (view.upvote > 0 || view.author_name.size() > 0)
    {
        out += "\\noindent{\\small ";
        if (!view.author_name.empty())
            out += "作者：" + escape_latex(view.author_name);
        if (view.upvote > 0)
        {
            if (!view.author_name.empty())
                out += "　";
            out += "点赞数：" + std::to_string(view.upvote);
        }
        out += "}\n\n";
    }
    if (!view.content_full)
        out += "\\noindent{\\small\\textcolor{red}{注意：本篇正文不完整"
               "（因 --allow-partial 导出）。}}\n\n";

    if (!view.content.empty())
        out += render_markdown(view.content, 0) + "\n";
    return out;
}

// 把一篇题解渲染为 LaTeX：标题统一格式「题解：<题解标题>」（超过 60 字符截断，避免撑爆目录与
// 书签）；\luogotocsolution 负责锚点、目录条目（注明所属题目）与正文标题；「返回题目」按钮
// 放在标题行右侧（\hfill），不进入目录与书签
std::string solution_to_latex(const luogu::ProblemSolutionSet &set,
                              const luogu::SolutionView &view,
                              const luogu::SolutionExportOptions &sol_opt)
{
    // 标题统一格式「题解：<题解标题>」（自带前缀去掉、换行换成空格、60 字符截断），复用
    // Markdown 导出的同一个 luogu::solution_heading，避免 \n\r\t 漏进目录 / 页眉 / PDF 书签
    const std::string heading_plain = luogu::solution_heading(view.title);
    const std::string kSolutionPrefix = "题解：";
    const std::string heading_latex =
        kSolutionPrefix +
        inline_to_latex(heading_plain.rfind(kSolutionPrefix, 0) == 0
                            ? heading_plain.substr(kSolutionPrefix.size())
                            : heading_plain);

    // 目录与书签文字：注明所属题目（PID + 题目名），保持黑色（不随难度着色）
    std::string toc_text = escape_latex(heading_plain) + "（" +
                           escape_latex(set.pid + " " + set.problem_title) + "）";
    if (!sol_opt.article_toc)
        toc_text.clear();

    const std::string anchor = luogu::solution_anchor(set.pid, view.lid);

    std::string button;
    if (sol_opt.article_to_problem_link && !sol_opt.articles_only)
        button = "\\hfill\\luogosolutionlink{" +
                 luogu::problem_anchor(set.pid) + "}{返回题目}";

    return article_body_to_latex(heading_plain, heading_latex, toc_text, anchor,
                                 button, view, sol_opt);
}

// 把一篇按文章编号下载的文章（--article）渲染为 LaTeX：级别与题解正文完全相同（目录层级、
// 页眉、元信息、正文渲染），只是没有题目跳转按钮，目录条目里也不注明所属题目
std::string standalone_article_to_latex(const luogu::SolutionView &view,
                                        const luogu::SolutionExportOptions &sol_opt)
{
    // 同 solution_to_latex：复用 Markdown 导出的 luogu::article_heading，保证标题清洗两处一致
    const std::string heading_plain = luogu::article_heading(view.title);
    const std::string kArticlePrefix = "文章：";
    const std::string heading_latex =
        kArticlePrefix +
        inline_to_latex(heading_plain.rfind(kArticlePrefix, 0) == 0
                            ? heading_plain.substr(kArticlePrefix.size())
                            : heading_plain);

    std::string toc_text = escape_latex(heading_plain);
    if (!sol_opt.article_toc)
        toc_text.clear();

    return article_body_to_latex(heading_plain, heading_latex, toc_text,
                                 luogu::article_anchor(view.lid), std::string(),
                                 view, sol_opt);
}
} // namespace

namespace
{
// 写出 LaTeX 文档前言：文档类、宏包、页眉页脚、题解与文章用的宏、封面标题
// 与字体设置（\documentclass 到字体设置结束，不含 \begin{document}）。
// doc_only（--doc-only）时不输出页眉标题，只保留页码。
void write_preamble(FILE *out, const latex::Options &opt, bool doc_only)
{
    // 题解与文章用的宏只在本次导出确实包含它们时写出
    const bool export_solutions =
        opt.solutions != nullptr && opt.solution_export.enabled;
    const bool export_articles =
        opt.articles != nullptr && !opt.articles->items.empty();

    // openany：章节可在任意页开始，避免封面后的空页（book 默认章节从奇数页开始）
    std::fputs("\\documentclass[openany]{book}\n", out);
    // ctex fontset：Windows/macOS 在编译本程序时确定；Linux 在运行
    // 阶段解析 /etc/os-release，Ubuntu 系列用 ubuntu，其他发行版用 fandol。
    const std::string ctex_options = latex::ctex_package_options();
    std::fprintf(out, "\\usepackage%s{ctex}\n", ctex_options.c_str());
    std::fputs("\\usepackage{graphicx}\n", out);
    // 图片统一缩放：测量自然宽高，只在超过行宽/版心高时按比例缩小（keepaspectratio 保证宽高比
    // 不变），小图片保持原始尺寸。高度上限用 \luogoimagemaxheight 而不是 \textheight：折叠框内
    // 会把该长度改小（见 fold_box_latex），保证框装得下一页
    std::fputs("\\newlength{\\luogoimagemaxheight}\n", out);
    std::fputs("\\setlength{\\luogoimagemaxheight}{\\textheight}\n", out);
    std::fputs("\\newcommand{\\luogoincludegraphics}[1]{%\n", out);
    std::fputs("  \\setbox0=\\hbox{\\includegraphics{#1}}%\n", out);
    std::fputs("  \\ifdim\\wd0>\\linewidth\n", out);
    std::fputs("    \\includegraphics[width=\\linewidth,height=\\luogoimagemaxheight,keepaspectratio]{#1}%\n", out);
    std::fputs("  \\else\n", out);
    std::fputs("    \\ifdim\\dimexpr\\ht0+\\dp0\\relax>\\luogoimagemaxheight\n", out);
    std::fputs("      \\includegraphics[width=\\linewidth,height=\\luogoimagemaxheight,keepaspectratio]{#1}%\n", out);
    std::fputs("    \\else\n", out);
    std::fputs("      \\includegraphics{#1}%\n", out);
    std::fputs("    \\fi\n", out);
    std::fputs("  \\fi\n", out);
    std::fputs("}\n", out);
    std::fputs("\\usepackage{titlesec}\n", out);
    std::fputs("\\usepackage{fancyhdr}\n", out);
    // --no-toc-links：目录条目不带跳转到题目的超链接（默认带超链接）
    if (opt.toc_links)
        std::fputs("\\usepackage[hidelinks]{hyperref}\n", out);
    else
        std::fputs("\\usepackage[linktoc=none,hidelinks]{hyperref}\n", out);
    // bookmark 宏包在单次 xelatex 编译中也能写入 PDF 书签，确保「目录」
    // 和每个题目的书签不依赖 .out 的多遍重跑；必须在 hyperref 之后加载。
    std::fputs("\\usepackage{bookmark}\n", out);
    // 行内代码会在任意两个字符之间插入 \allowbreak；标题里出现行内代码时 hyperref 会把标题
    // 展开成 PDF 书签字符串，遇到 \allowbreak 会刷 "Token not allowed in a PDF string"
    // 警告（书签里它也没有意义），这里在 PDF 字符串中把它定义为空
    std::fputs("\\pdfstringdefDisableCommands{\\def\\allowbreak{}}\n", out);
    std::fputs("\\usepackage[normalem]{ulem}\n", out);
    std::fputs("\\usepackage{amsmath}\n", out);
    std::fputs("\\usepackage{mathtools}\n", out);
    // unicode-math：数学字体全部改为可无限缩放的 OpenType 数学字体，修复 mathrsfs（只有固定
    // 字号）导致 \mathscr 字号被替换的问题。必须加载在 amsmath / mathtools 之后；不再加载
    // amssymb、mathrsfs（符号与 \mathscr 由 unicode-math 提供）与 bm（与 unicode-math 不兼容，
    // 会报 Extended mathchar），\bm / \boldsymbol 用 unicode-math 的 \symbfit 兼容替代
    std::fputs("\\usepackage{unicode-math}\n", out);
    // 显式选择随 TeX Live / MacTeX / MiKTeX 分发的 OpenType 数学字体，避免各平台默认字体不一致
    std::fputs("\\setmathfont{Latin Modern Math}\n", out);
    // 表头加粗用的粗体数学版本：Latin Modern Math 本身没有粗体字形，用 fontspec 的 FakeBold
    // （XeTeX 的 embolden）合成粗体，使表头里的公式（如 $n\leq$）与文字一并加粗
    std::fputs("\\setmathfont[version=bold, FakeBold=2]{Latin Modern Math}\n", out);
    // 表格表头：\textbf 加粗文字，\boldmath 切换上面定义的粗体数学版本
    std::fputs("\\newcommand{\\luogotablehead}[1]{\\textbf{\\boldmath #1}}\n", out);
    // ::cute-table{tuack} 的框线：去掉表格最左、最右两条竖线（列间竖线保留），最上、最下框线
    // 加粗（\luogotuackheavyrule），表头下方那条加粗一档（\luogotuackmidrule）。直接改
    // \arrayrulewidth 会让所有框线一起变粗，故用 \noalign{\hrule height ...} 单独指定
    std::fputs("\\newcommand{\\luogotuackheavyrule}{\\noalign{\\hrule height 1.8pt}}\n", out);
    std::fputs("\\newcommand{\\luogotuackmidrule}{\\noalign{\\hrule height 1.0pt}}\n", out);
    std::fputs("\\newcommand{\\bm}{\\symbfit}\n", out);
    std::fputs("\\renewcommand{\\boldsymbol}{\\symbfit}\n", out);
    std::fputs("\\usepackage{xcolor}\n", out);
    std::fputs("\\usepackage{listings}\n", out);
    std::fputs("\\usepackage{cancel}\n", out);
    std::fputs("\\usepackage{geometry}\n", out);
    std::fputs("\\usepackage{tabularx}\n", out);
    // 表格合并（洛谷的 ^ 向上合并 / < 向左合并）需要 \multirow
    std::fputs("\\usepackage{multirow}\n", out);
    // 折叠框（洛谷 :::info 等）用 mdframed 绘制：彩色框线 + 彩色标题条 + 白底黑字。不用 tabular
    // 模拟是因为 LaTeX 表格无法跨页、内容长的折叠框会被截断，mdframed 可以自然跨页
    std::fputs("\\usepackage{mdframed}\n", out);
    // 嵌套折叠框的相连小块要求「块与块之间没有竖直间距」，否则框线会在接缝处断开。mdframed 的
    // 环境结束时 \endtrivlist 会把环境前的竖直间距重新补回来（多出一个 \topsep 左右的空隙），
    // 把 LaTeX 的 \@noparlist 开关置真即可让它跳过这段间距。开关是全局的（小块内容被收集在
    // mdframed 自己的盒子里，局部赋值传不出来），因此小块内容末尾置真、环境结束后立刻还原
    std::fputs("\\makeatletter\n", out);
    std::fputs("\\newcommand{\\luogofoldnoparlist}{\\global\\@noparlisttrue}\n", out);
    std::fputs("\\newcommand{\\luogofoldparlist}{\\global\\@noparlistfalse}\n", out);
    std::fputs("\\makeatother\n", out);
    std::fputs("\\geometry{margin=2cm}\n", out);
    // book 默认 \headheight=12pt 略小于 ctex/unicode-math 标题所需的 12.03pt；显式给到 13pt
    // 消除每页的 fancyhdr 警告，正文版心基本不变
    std::fputs("\\setlength{\\headheight}{13pt}\n", out);
    // 去掉所有章节序号（\section 等）：目录和正文都不显示数字
    std::fputs("\\setcounter{secnumdepth}{-1}\n", out);

    // 页眉：--toc-backlinks 时页码为跳回目录页的超链接（默认页码为普通文本）
    const std::string page_in_head =
        opt.toc_backlinks ? "\\hyperlink{luogotoc}{\\thepage}" : "\\thepage";
    // 标题字体（--set-font-title-zh-CN / --set-font-title-en-US）同样作用于页眉处的题目标题；
    // 未指定时保持普通正文样式
    std::string head_fonts = "\\normalfont";
    if (!opt.font_title_zh.empty())
        head_fonts += " \\luogotitlezh";
    if (!opt.font_title_en.empty())
        head_fonts += " \\luogotitleen";

    std::fputs("\\pagestyle{fancy}\n", out);
    std::fputs("\\fancyhf{}\n", out);
    // --doc-only：不输出页眉标题，只保留外侧页码
    if (doc_only)
    {
        std::fprintf(out, "\\fancyhead[LE]{%s}\n", page_in_head.c_str());
        std::fprintf(out, "\\fancyhead[RO]{%s}\n", page_in_head.c_str());
    }
    else
    {
        std::fprintf(out, "\\fancyhead[LE]{%s}\n", page_in_head.c_str());
        std::fprintf(out, "\\fancyhead[RE]{\\nouppercase{%s \\rightmark}}\n", head_fonts.c_str());
        std::fprintf(out, "\\fancyhead[LO]{\\nouppercase{%s \\rightmark}}\n", head_fonts.c_str());
        std::fprintf(out, "\\fancyhead[RO]{%s}\n", page_in_head.c_str());
    }

    // 标题字体分两套：1) 题目大标题（\section）中文跟随 --set-font-title-zh-CN，未指定时保持
    // 普通正文 CJK 字体；西文直接跟随正文主字体，因此 --set-font-body-en-US 通过 \setmainfont
    // 自动生效，不使用黑体。2) 小节标题（\subsection / \subsubsection，对应固定小节和 Markdown
    // 的 ## / ###）：中文默认 ctex 预设黑体 \heiti，西文默认代码块字体 \luogoheadinglatin；
    // 用户指定标题中西文字体时优先使用 --set-font-title-zh-CN / -en-US
    const std::string section_title_font_zh =
        opt.font_title_zh.empty() ? "" : "\\luogotitlezh";
    const std::string subsection_title_font_zh =
        opt.font_title_zh.empty() ? "\\heiti" : "\\luogotitlezh";
    const std::string subsection_title_font_en =
        opt.font_title_en.empty() ? "\\luogoheadinglatin" : "\\luogotitleen";

    std::fprintf(out, "\\titleformat{\\section}\n{%s\\Large}\n{}\n{0em}{}\n",
                 section_title_font_zh.c_str());
    std::fprintf(out, "\\titleformat{\\subsection}\n{%s%s\\large}\n{}\n{0em}{}\n",
                 subsection_title_font_zh.c_str(), subsection_title_font_en.c_str());
    std::fprintf(out, "\\titleformat{\\subsubsection}\n{%s%s\\color{gray}}\n{}\n{1em}{}\n",
                 subsection_title_font_zh.c_str(), subsection_title_font_en.c_str());
    // Markdown 的 #### / ##### 用 \paragraph / \subparagraph 渲染，而这两级在 LaTeX 里默认是
    // 「接排标题」（标题与后面的正文排在同一行），洛谷网页则把 h4 / h5 渲染成独占一行的块级
    // 标题：这里用 titlesec 改成悬挂标题（正文另起一段），字体沿用类默认值
    std::fputs("\\titleformat{\\paragraph}[hang]{\\normalfont\\normalsize\\bfseries}{}{0em}{}\n", out);
    std::fputs("\\titlespacing*{\\paragraph}{0pt}{3.25ex plus 1ex minus .2ex}{0.75ex}\n", out);
    std::fputs("\\titleformat{\\subparagraph}[hang]{\\normalfont\\normalsize\\bfseries}{}{0em}{}\n", out);
    std::fputs("\\titlespacing*{\\subparagraph}{0pt}{3.25ex plus 1ex minus .2ex}{0.5ex}\n", out);

    // --show-contents-difficulty-tags 用的 \luogotocsection{<HTML 颜色>}{<标题>}：相当于
    // \section，但把写进 .toc 的目录项文字包进 \textcolor，使目录里的题目标题按难度着色。
    // 只替换本次调用中的 \addcontentsline（hyperref 写入的目录超链接锚点照常保留），且只影响
    // 目录项中的标题文字：引导点、页码、正文标题、页眉与 PDF 书签都保持原样
    if (opt.toc_difficulty)
    {
        // #1 = HTML 颜色；#2 = 目录/书签用的纯文本短标题；#3 = 正文标题（可含 \hfill 与按钮）
        std::fputs("\\newcommand{\\luogotocsection}[3]{%\n", out);
        std::fputs("  \\begingroup\n", out);
        std::fputs("    \\let\\luogotocaddcontentsline\\addcontentsline\n", out);
        std::fputs("    \\renewcommand{\\addcontentsline}[3]{%\n", out);
        std::fputs("      \\luogotocaddcontentsline{##1}{##2}{\\textcolor[HTML]{#1}{##3}}}%\n", out);
        std::fputs("    \\section[{#2}]{#3}%\n", out);
        std::fputs("  \\endgroup}\n", out);
    }

    // \luogosolutionlink{<锚点>}{<文字>}：蓝底白字的跳转按钮，外观与 \luogotag 完全一致
    // （白字 + \colorbox[HTML]，\vphantom{涵} 与 \smash 固定高度与深度）。按钮字体统一跟随
    // 正文：\normalfont 把西文字族与 CJK 字体族一起复位到文档默认值（xeCJK 给 \normalfont 挂了
    // 钩子），而两个按钮分别嵌在 \section / \subsection* 标题里会继承标题字体，必须显式复位
    // \luogotocsolution{#1}{#2}{#3}{#4}：#1 = 目录与 PDF 书签的文字（纯文本，空串表示不进目录）；
    // #2 = 正文标题；#3 = 锚点名（题解 sol-<PID>-<lid>，文章 sol-art-<lid>）；#4 = 标题行右侧的
    // 按钮（可为空）。只用 \addcontentsline 而不额外调用 \pdfbookmark：hyperref 会为
    // \addcontentsline 的条目自动补一个同层级书签，两者同时使用会产生重复书签
    if (export_solutions || export_articles)
    {
        // --article 单独使用时没有题解，但文章与题解共用这一套宏
        std::fputs("\\newcommand{\\luogosolutionlink}[2]{%\n", out);
        std::fputs("  \\hyperlink{#1}{\\textcolor{white}{\\colorbox[HTML]{3498db}{"
                   "\\normalfont\\tagsfonts\\small\\vphantom{涵}\\smash{#2}}}}\n", out);
        std::fputs("}\n", out);
        std::fputs("\\newcommand{\\luogotocsolution}[4]{%\n", out);
        std::fputs("  \\phantomsection\n", out);
        std::fputs("  \\hypertarget{#3}{}%\n", out);
        std::fputs("  \\ifx\\relax#1\\relax\\else\\addcontentsline{toc}{luogosolution}{#1}\\fi\n", out);
        std::fputs("  \\subsection*{#2#4}%\n", out);
        std::fputs("}\n", out);
        // 目录条目类型 luogosolution：\l@luogosolution 直接取 \l@section，条目缩进与题目完全
        // 对齐；\toclevel@luogosolution 固定为 2，PDF 书签层级仍是 subsection 层级
        std::fputs("\\makeatletter\n", out);
        std::fputs("\\def\\toclevel@luogosolution{2}\n", out);
        std::fputs("\\let\\l@luogosolution\\l@section\n", out);
        std::fputs("\\makeatother\n", out);
    }

    std::fputs("\\lstset{\n", out);
    std::fputs("    breaklines=true,\n", out);
    std::fputs("    breakatwhitespace=false,\n", out);
    std::fputs("    keepspaces=true,\n", out);
    std::fputs("    showstringspaces=false,\n", out);
    std::fputs("    tabsize=4,\n", out);
    std::fputs("    keywordstyle=\\color{blue}\\bfseries,\n", out);
    std::fputs("    commentstyle=\\color{green!50!black},\n", out);
    std::fputs("    stringstyle=\\color{red!60!black},\n", out);
    std::fputs("    frame=single,\n", out);
    std::fputs("    columns=flexible,\n", out);
    std::fputs("    numbers=left,\n", out);
    std::fputs("    numberstyle=\\footnotesize\\ttfamily\\color{gray},\n", out);
    std::fputs("    basicstyle=\\small\\ttfamily,\n", out);
    std::fputs("    rulecolor=\\color{blue},\n", out);
    // 代码块/样例里的 \end{lstlisting} 已由 lst_safe 换成等价写法，这里用 literate 排版回原文
    std::fputs(kLstLiterateOption, out);
    std::fputs("}\n", out);

        // 当前 TeX Live 的 listings 没有这些语言，手动补上，否则 \begin{lstlisting}[language=Rust]
        // 会报 "Couldn't load requested language"
    std::fputs("\\lstdefinelanguage{Rust}{\n", out);
    std::fputs("    morekeywords={as,async,await,break,const,continue,crate,dyn,else,enum,", out);
    std::fputs("extern,false,fn,for,if,impl,in,let,loop,match,mod,move,mut,pub,ref,return,", out);
    std::fputs("self,Self,static,struct,super,trait,true,type,unsafe,use,where,while,yield},\n", out);
    std::fputs("  morecomment=[l]{//},\n", out);
    std::fputs("  morecomment=[s]{/*}{*/},\n", out);
    std::fputs("  morestring=[b]\",\n", out);
    std::fputs("  morestring=[b]',\n", out);
    std::fputs("}\n", out);
    std::fputs("\\lstdefinelanguage{TypeScript}{\n", out);
    std::fputs("  morekeywords={abstract,any,as,asserts,async,await,boolean,break,case,catch,", out);
    std::fputs("class,const,continue,debugger,declare,default,delete,do,else,enum,export,", out);
    std::fputs("extends,false,finally,for,from,function,get,if,implements,import,in,infer,", out);
    std::fputs("instanceof,interface,is,keyof,let,module,namespace,never,new,null,number,", out);
    std::fputs("object,of,package,private,protected,public,readonly,return,set,static,string,", out);
    std::fputs("super,switch,symbol,this,throw,true,try,type,typeof,undefined,unknown,var,", out);
    std::fputs("void,while,with,yield},\n", out);
    std::fputs("  morecomment=[l]{//},\n", out);
    std::fputs("  morecomment=[s]{/*}{*/},\n", out);
    std::fputs("  morestring=[b]\",\n", out);
    std::fputs("  morestring=[b]',\n", out);
    std::fputs("  morestring=[b]`,\n", out);
    std::fputs("}\n", out);
    std::fputs("\\lstdefinelanguage{CSharp}{\n", out);
    std::fputs("  morekeywords={abstract,as,base,bool,break,byte,case,catch,char,checked,", out);
    std::fputs("class,const,continue,decimal,default,delegate,do,double,else,enum,event,", out);
    std::fputs("explicit,extern,false,finally,fixed,float,for,foreach,goto,if,implicit,in,", out);
    std::fputs("int,interface,internal,is,lock,long,namespace,new,null,object,operator,out,", out);
    std::fputs("override,params,private,protected,public,readonly,ref,return,sbyte,sealed,", out);
    std::fputs("short,sizeof,stackalloc,static,string,struct,switch,this,throw,true,try,", out);
    std::fputs("typeof,uint,ulong,unchecked,unsafe,ushort,using,virtual,void,volatile,while},\n", out);
    std::fputs("  morecomment=[l]{//},\n", out);
    std::fputs("  morecomment=[s]{/*}{*/},\n", out);
    std::fputs("  morestring=[b]\",\n", out);
    std::fputs("  morestring=[b]',\n", out);
    std::fputs("}\n", out);
    std::fputs("\\lstdefinelanguage{CSS}{\n", out);
    std::fputs("  morekeywords={align-items,align-self,animation,background,background-color,", out);
    std::fputs("border,border-radius,bottom,box-shadow,color,content,cursor,display,flex,", out);
    std::fputs("float,font,font-family,font-size,font-weight,grid,gap,height,justify-content,", out);
    std::fputs("left,line-height,list-style,margin,max-height,max-width,min-height,min-width,", out);
    std::fputs("opacity,overflow,padding,position,right,text-align,text-decoration,top,", out);
    std::fputs("transform,transition,visibility,width,z-index},\n", out);
    std::fputs("  morecomment=[s]{/*}{*/},\n", out);
    std::fputs("  morestring=[b]\",\n", out);
    std::fputs("  morestring=[b]',\n", out);
    std::fputs("}\n", out);
    std::fputs("\\lstdefinelanguage{Lua}{\n", out);
    std::fputs("  morekeywords={and,break,do,else,elseif,end,false,for,function,goto,if,in,", out);
    std::fputs("local,nil,not,or,repeat,return,then,true,until,while},\n", out);
    std::fputs("  morecomment=[l]{--},\n", out);
    std::fputs("  morecomment=[s]{--[[}{]]},\n", out);
    std::fputs("  morestring=[b]\",\n", out);
    std::fputs("  morestring=[b]',\n", out);
    std::fputs("}\n", out);
    std::fputs("\\colorlet{Red}{red}\\colorlet{Green}{green}\\colorlet{Blue}{blue}\n", out);
    std::fputs("\\colorlet{Orange}{orange}\\colorlet{Pink}{pink}\\colorlet{Purple}{purple}\n", out);
    std::fputs("\\colorlet{Cyan}{cyan}\\colorlet{Brown}{brown}\\colorlet{Teal}{teal}\n", out);
    std::fputs("\\colorlet{Violet}{violet}\\colorlet{White}{white}\\colorlet{Black}{black}\n", out);
    std::fputs("\\colorlet{Grey}{gray}\\colorlet{grey}{gray}\\colorlet{Gray}{gray}\n", out);
    std::fputs("\\colorlet{default}{black}\n", out);
    std::fputs("\\colorlet{normal}{black}\n", out);
    std::fputs("\\colorlet{transparent}{white}\n", out);
    std::fputs("\\definecolor{Aquamarine}{RGB}{127,255,212}\n", out);
    std::fputs("\\definecolor{gold}{RGB}{255,215,0}\n", out);
    // 折叠框颜色（与洛谷网页一致）：info 蓝 / success 绿 / warning 黄 / error 红，用作框线颜色
    // 与标题条底色
    for (const auto &fold : kFoldStyles)
    {
        std::fprintf(out, "\\definecolor{%s}{RGB}{%s}\n", fold.color, fold.rgb);
    }
    // 折叠框样式：框线 1pt、白底黑字（字体与正文一致），标题条为白色粗体、底色与框线同色
    // （每次使用时用 linecolor / frametitlebackgroundcolor 指定具体颜色）。左右外边距为 0 时
    // 占满整行宽度；嵌套的折叠框在 \begin{mdframed} 处单独给出 leftmargin/rightmargin=1em。
    // 标题文字左端对齐：mdframed 生成标题盒子时会用 \mdf@par@local 把 \begin{mdframed} 时捕获的
    // \parindent（ctex 下为 2 个汉字的首行缩进）还原进去，标题会比框内正文更靠右；
    // frametitlealignment 在标题段落开始之前把 \parindent 置零即可对齐左内边距，只作用于标题盒子
    std::fputs("\\mdfdefinestyle{luogofoldbox}{%\n", out);
    std::fputs("  linewidth=1pt,\n", out);
    std::fputs("  linecolor=black,\n", out);
    std::fputs("  backgroundcolor=white,\n", out);
    std::fputs("  fontcolor=black,\n", out);
    // 四条边线必须逐条显式写出：mdframed 的框线开关会被嵌套的盒子继承，而区块引用
    // （luogoquote）只画左边线，折叠框若依赖默认值就会出现「引用里的折叠框四周没有框线」
    std::fputs("  topline=true,\n", out);
    std::fputs("  bottomline=true,\n", out);
    std::fputs("  leftline=true,\n", out);
    std::fputs("  rightline=true,\n", out);
    std::fputs("  leftmargin=0pt,\n", out);
    std::fputs("  rightmargin=0pt,\n", out);
    std::fputs("  innerleftmargin=6pt,\n", out);
    std::fputs("  innerrightmargin=6pt,\n", out);
    std::fputs("  innertopmargin=4pt,\n", out);
    std::fputs("  innerbottommargin=4pt,\n", out);
    std::fputs("  frametitlefont=\\bfseries,\n", out);
    std::fputs("  frametitlefontcolor=white,\n", out);
    std::fputs("  frametitlebackgroundcolor=black,\n", out);
    // 标题与左端对齐：抵消 mdframed 还原进来的 \parindent（见上方注释）。这里只能写 0pt 不能写
    // \z@：这段样式在 \makeatother 之后写出，此时 @ 不是字母，\z@ 会被拆成 \z 与 @ 而报错
    std::fputs("  frametitlealignment={\\setlength{\\parindent}{0pt}\\relax},\n", out);
    std::fputs("  frametitlerule=false,\n", out);
    std::fputs("  frametitleaboveskip=4pt,\n", out);
    std::fputs("  frametitlebelowskip=4pt,\n", out);
    std::fputs("  skipabove=6pt,\n", out);
    std::fputs("  skipbelow=6pt,\n", out);
    std::fputs("  nobreak=false,\n", out);
    std::fputs("}\n", out);
    // 区块引用左侧竖条的颜色（与洛谷网页一致，rgb(238,238,238)），只画左边一条线
    std::fputs("\\definecolor{luogoquotebar}{RGB}{238,238,238}\n", out);
    // 区块引用样式：左侧 4pt 浅灰竖条 + 左内边距 8pt、右内边距 0（引用文字与正文一样占满版心
    // 宽度）；顶层的引用可以自然跨页，竖条随内容延续；嵌套的引用会被切成若干矮块，每块只留
    // 一条左边线、块间不留间距，视觉上仍是连续的一条（见 quote_piece_latex）。背景不设颜色
    std::fputs("\\mdfdefinestyle{luogoquote}{%\n", out);
    std::fputs("  topline=false,\n", out);
    std::fputs("  bottomline=false,\n", out);
    std::fputs("  rightline=false,\n", out);
    std::fputs("  leftline=true,\n", out);
    std::fputs("  linewidth=4pt,\n", out);
    std::fputs("  linecolor=luogoquotebar,\n", out);
    std::fputs("  fontcolor=black,\n", out);
    std::fputs("  leftmargin=0pt,\n", out);
    std::fputs("  rightmargin=0pt,\n", out);
    std::fputs("  innerleftmargin=8pt,\n", out);
    std::fputs("  innerrightmargin=0pt,\n", out);
    std::fputs("  innertopmargin=2pt,\n", out);
    std::fputs("  innerbottommargin=2pt,\n", out);
    // 标题必须显式清空：mdframed 的选项会被嵌套的盒子继承，折叠框里的引用不写这一项就会把
    // 上一级的标题条重复画在每一小块上
    std::fputs("  frametitle={},\n", out);
    std::fputs("  skipabove=6pt,\n", out);
    std::fputs("  skipbelow=6pt,\n", out);
    std::fputs("  nobreak=false,\n", out);
    std::fputs("}\n", out);
    std::fputs("\\providecommand{\\degree}{^{\\circ}}\n", out);
    std::fputs("\\providecommand{\\exist}{\\exists}\n", out);
    std::fputs("\\providecommand{\\infin}{\\infty}\n", out);
    std::fputs("\\providecommand{\\sube}{\\subseteq}\n", out);
    std::fputs("\\providecommand{\\supe}{\\supseteq}\n", out);
    std::fputs("\\providecommand{\\lang}{\\langle}\n", out);
    std::fputs("\\providecommand{\\rang}{\\rangle}\n", out);
    std::fputs("\\providecommand{\\rarr}{\\rightarrow}\n", out);
    std::fputs("\\providecommand{\\larr}{\\leftarrow}\n", out);
    std::fputs("\\providecommand{\\uarr}{\\uparrow}\n", out);
    std::fputs("\\providecommand{\\darr}{\\downarrow}\n", out);
    std::fputs("\\providecommand{\\lrarr}{\\leftrightarrow}\n", out);
    std::fputs("\\providecommand{\\xlongequal}[2][=]{\\overset{#2}{#1}}\n", out);
    std::fputs("\\providecommand{\\argmax}{\\operatorname*{arg\\,max}}\n", out);
    std::fputs("\\providecommand{\\argmin}{\\operatorname*{arg\\,min}}\n", out);
    std::fputs("\\providecommand{\\ctg}{\\cot}\n", out);
    // 任务列表用的 \square（未勾选方框）由 amssymb 提供，而本模板不加载 amssymb；unicode-math
    // 收录的是 \mdlgwhtsquare，这里补齐别名，避免任务列表渲染成 Undefined control sequence
    std::fputs("\\providecommand{\\square}{\\mdlgwhtsquare}\n", out);
    // amssymb 的 \circledR / \circledS 未被 unicode-math 收录，而本模板不加载 amssymb；洛谷题面
    // （KaTeX 支持这两个命令）用到时补齐为等价符号，避免 Undefined control sequence
    std::fputs("\\providecommand{\\circledR}{\\text{\\textregistered}}\n", out);
    std::fputs("\\providecommand{\\circledS}{\\text{\\textcircled{S}}}\n", out);
    // 部分洛谷题面使用 \bold2 / \bold{x} 表示粗体数学字符，LaTeX 标准没有 \bold，补齐为 \mathbf
    // 别名，避免编译时 Undefined control sequence
    std::fputs("\\providecommand{\\bold}[1]{\\ifmmode\\mathbf{#1}\\else\\textbf{#1}\\fi}\n", out);
    std::fputs("\\providecommand{\\lt}{<}\n", out);
    std::fputs("\\providecommand{\\gt}{>}\n", out);
    std::fputs("\\providecommand{\\Alpha}{\\mathrm{A}}\n", out);
    std::fputs("\\providecommand{\\Beta}{\\mathrm{B}}\n", out);
    std::fputs("\\providecommand{\\Epsilon}{\\mathrm{E}}\n", out);
    std::fputs("\\providecommand{\\Zeta}{\\mathrm{Z}}\n", out);
    std::fputs("\\providecommand{\\Eta}{\\mathrm{H}}\n", out);
    std::fputs("\\providecommand{\\Iota}{\\mathrm{I}}\n", out);
    std::fputs("\\providecommand{\\Kappa}{\\mathrm{K}}\n", out);
    std::fputs("\\providecommand{\\Mu}{\\mathrm{M}}\n", out);
    std::fputs("\\providecommand{\\Nu}{\\mathrm{N}}\n", out);
    std::fputs("\\providecommand{\\Omicron}{\\mathrm{O}}\n", out);
    std::fputs("\\providecommand{\\Rho}{\\mathrm{P}}\n", out);
    std::fputs("\\providecommand{\\Tau}{\\mathrm{T}}\n", out);
    std::fputs("\\providecommand{\\Upsilon}{\\mathrm{Y}}\n", out);
    std::fputs("\\providecommand{\\Chi}{\\mathrm{X}}\n", out);
    std::fputs("\\providecommand{\\R}{\\mathbb{R}}\n", out);
    std::fputs("\\providecommand{\\N}{\\mathbb{N}}\n", out);
    std::fputs("\\providecommand{\\Z}{\\mathbb{Z}}\n", out);
    std::fputs("\\providecommand{\\Q}{\\mathbb{Q}}\n", out);
    std::fputs("\\providecommand{\\C}{\\mathbb{C}}\n", out);
    std::fputs("\\providecommand{\\red}[1]{\\textcolor{red}{#1}}\n", out);
    std::fputs("\\providecommand{\\blue}[1]{\\textcolor{blue}{#1}}\n", out);
    std::fputs("\\providecommand{\\green}[1]{\\textcolor{green}{#1}}\n", out);
    std::fputs("\\providecommand{\\pink}[1]{\\textcolor{pink}{#1}}\n", out);
    std::fputs("\\providecommand{\\orange}[1]{\\textcolor{orange}{#1}}\n", out);
    std::fputs("\\providecommand{\\purple}[1]{\\textcolor{purple}{#1}}\n", out);
    std::fputs("\\providecommand{\\brown}[1]{\\textcolor{brown}{#1}}\n", out);
    std::fputs("\\providecommand{\\gray}[1]{\\textcolor{gray}{#1}}\n", out);
    std::fputs("\\providecommand{\\cyan}[1]{\\textcolor{cyan}{#1}}\n", out);
    std::fputs("\\providecommand{\\teal}[1]{\\textcolor{teal}{#1}}\n", out);
    std::fputs("\\providecommand{\\magenta}[1]{\\textcolor{magenta}{#1}}\n", out);
    std::fputs("\\providecommand{\\yellow}[1]{\\textcolor{yellow}{#1}}\n", out);
    std::fputs("\\providecommand{\\violet}[1]{\\textcolor{violet}{#1}}\n", out);
    // JavaScript 只在这里定义一次：keywords= 会覆盖先前累积的 morekeywords，另写一份等于白写
    // （JS 保留字与模板字符串反引号的高亮都会丢失）
    std::fputs("\\lstdefinelanguage{JavaScript}{\n", out);
    std::fputs("keywords={abstract, arguments, as, async, await, boolean, break, byte, case, catch, char, class, const, continue, debugger, default, delete, do, double, else, enum, eval, export, extends, false, final, finally, float, for, from, function, goto, if, implements, import, in, instanceof, int, interface, let, long, native, new, null, of, package, private, protected, public, return, short, static, super, switch, synchronized, this, throw, throws, transient, true, try, typeof, var, void, volatile, while, with, yield},", out);
    std::fputs("keywordstyle=\\color{blue}\\bfseries,\n", out);
    std::fputs("ndkeywords={boolean, number, string, null, undefined, true, false},\n", out);
    std::fputs("ndkeywordstyle=\\color{red}\\bfseries,\n", out);
    std::fputs("identifierstyle=\\color{black},\n", out);
    std::fputs("sensitive=false,\n", out);
    std::fputs("comment=[l]{//},\n", out);
    std::fputs("morecomment=[s]{/*}{*/},", out);
    std::fputs("commentstyle=\\color{green}\\ttfamily,\n", out);
    std::fputs("stringstyle=\\color{purple}\\ttfamily,\n", out);
    std::fputs("morestring=[b]',\n", out);
    std::fputs("morestring=[b]\",\n", out);
    std::fputs("morestring=[b]`\n", out);
    std::fputs("}\n", out);
    // 封面标题：--set-cover-title 指定文字，--set-font-cover-page 指定字体（未设置字体时保持原
    // 行为：不额外指定字体族）；作者处的项目名带指向项目仓库的超链接
    {
        const std::string cover = opt.cover_title.empty() ? "luogu extract" : opt.cover_title;
        std::string cover_latex = escape_latex(cover);
        if (!opt.font_cover.empty())
            cover_latex = "{\\luogocoverfontall " + cover_latex + "}";
        std::fprintf(out,
                     "\\title{%s}\n\\author{\\href{%s}{%s}}\n\\date{\\today}\n",
                     cover_latex.c_str(),
                     LUOGU_EXTRACT_REPOSITORY_URL,
                     LUOGU_EXTRACT_PROJECT_NAME);
    }

    // 字体设置已集中到 latex_fonts.cpp：ctex fontset 预设负责默认中西文正文/标题/代码 CJK
    // 字体；本函数只负责在用户指定 --set-font-* 时覆盖，并设置代码块西文回退链
    latex::write_font_setup(out, opt);
}
} // namespace

namespace
{
// 写出目录页：章标题（\contentsname）+ \@starttoc，目录条目是否带超链接由 hyperref 的 linktoc
// 选项控制（对应 --no-toc-links）；\hypertarget{luogotoc} 是页眉页码跳回目录页的目标锚点
void write_toc(FILE *out, const latex::Options &opt)
{
    // 目录：设置标题字体时，目录页中的题目标题同样使用对应字体
    std::string toc_open, toc_close;
    if (!opt.font_title_zh.empty() || !opt.font_title_en.empty())
    {
        toc_open = "{";
        if (!opt.font_title_zh.empty())
            toc_open += "\\luogotitlezh";
        if (!opt.font_title_en.empty())
            toc_open += "\\luogotitleen";
        toc_close = "}";
    }

    // 目录统一写成「章标题 + \@starttoc」（与 \tableofcontents 等价），并在目录标题处放置：
    // - \hypertarget{luogotoc}：页眉页码（--toc-backlinks）跳回目录页的目标锚点，直接生成命名
    //   目标，不依赖 .aux 中的 label 记录；
    // - \pdfbookmark：PDF 书签中始终保留「目录」条目（book 类经 ctex 的 \tableofcontents 不会
    //   自动写目录书签），与题目的 \section 自动生成的书签并存
    std::fputs((toc_open + "\n").c_str(), out);
    std::fputs("\\chapter*{\\contentsname}\n", out);
    std::fputs("\\hypertarget{luogotoc}{}\n", out);
    std::fputs("\\pdfbookmark[0]{\\contentsname}{toc}\n", out);
    std::fputs("\\makeatletter\n", out);
    std::fputs("\\@starttoc{toc}\n", out);
    std::fputs("\\makeatother\n", out);
    std::fputs((toc_close + "\n").c_str(), out);
    std::fputs("\\newpage\n", out);
}
} // namespace

namespace
{
// 检查文档引用的图片是否都在缓存中，缺失时在终端用中文询问是否下载；同一 URL 去重，避免重复
// 下载与并发写同一缓存文件。new_download（-RD, --new-download）时全部重新下载（原子替换）
void offer_missing_images(const std::vector<std::string> &candidate_urls,
                          bool new_download, const std::string &subject)
{
    std::vector<std::string> missing;
    std::set<std::string> seen_missing;
    std::error_code ec;
    for (const auto &url : candidate_urls)
    {
        // 视频等非图片链接不算“未下载的图片”
        if (!looks_like_url(url) || is_video_url(url))
            continue;
        if (seen_missing.count(url))
            continue;
        if (new_download ||
            !std::filesystem::exists(crawler::image_cache_path(url), ec) || ec)
        {
            seen_missing.insert(url);
            missing.push_back(url);
        }
    }
    if (missing.empty())
        return;

    // 确认走 prompt::confirm（唯一的输入注入点，读不到输入时失败闭合）：直接 fgets(stdin) 会
    // 绕过注入点、也不判断 TTY，管道输入（yes |）会被当成用户同意而自动下载
    std::string prompt_text;
    if (new_download)
        prompt_text = subject + "共引用了 " + std::to_string(missing.size()) +
                      " 张图片；-RD, --new-download 将全部重新下载"
                      "（下载失败时保留原有缓存）。\n现在下载吗？[y/N] ";
    else
        prompt_text = subject + "共引用了 " + std::to_string(missing.size()) +
                      " 张尚未下载的图片。\n现在下载吗？[y/N] ";
    bool agreed = false;
    if (prompt::interactive())
    {
        agreed = prompt::confirm(prompt_text);
    }
    else
    {
        // 非交互式终端（管道/重定向）：与其它确认一致，失败闭合、不下载
        std::fputs(prompt_text.c_str(), stdout);
        std::printf("\n当前输入不是交互式终端，无法确认下载。\n");
        fflush(stdout);
    }
    if (agreed)
    {
        const crawler::derror download_result =
            crawler::download_images(missing, new_download);
        if (download_result != crawler::SUCCESS)
        {
            if (new_download)
                std::printf("部分图片重新下载失败；原有缓存保持不变，"
                            "仍然缺失的图片将在编译时被跳过（\\IfFileExists）。\n");
            else
                std::printf("部分图片下载失败；缺失的图片将在编译时被跳过"
                            "（\\IfFileExists）。\n");
        }
    }
    else
    {
        std::printf("已跳过下载；缺失的图片将在编译时被跳过"
                    "（\\IfFileExists）。\n");
    }
}
} // namespace

bool latex::export_latex(const luogu::ExportFilter &filter,
                         const std::filesystem::path &output_path,
                         std::string &error,
                         const Options &opt,
                         const luogu::ProblemSelection *preselected)
{
    error.clear();

    // 挂上本次导出的显示选项：行内转换（如 bilibili 链接）据此判断，函数返回时自动还原
    OptionsGuard options_guard(&opt);

    // 筛选（-M / -L 共用），结果已按题号排序；题解流程已筛选过一次时直接复用 preselected，
    // 避免重复解析题目列表缓存
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

    const bool export_solutions = opt.solutions != nullptr && opt.solution_export.enabled;
    // --article 的文章：与题解同文件、同级别，统一放在文档最后
    const bool export_articles = opt.articles != nullptr && !opt.articles->items.empty();
    const bool articles_only = export_solutions && opt.solution_export.articles_only;
    const bool per_problem = export_solutions && !opt.solution_export.document_end;
    if (export_solutions && articles_only &&
        (opt.solution_export.problem_to_article_link ||
         opt.solution_export.article_to_problem_link))
    {
        // --solutions-only 不导出题面，双向跳转按钮会指向不存在的锚点：自动关闭以避免死链
        std::printf("提示：--solutions-only 模式下不导出题面，"
                    "已关闭题目与题解之间的双向跳转按钮（避免死链）。\n");
    }

    // 检查图片是否都已下载到缓存，缺失时询问是否下载。题面、题解正文与文章正文中引用的图片
    // 一并处理（题解与文章的图片同样走现有的图片下载通道：洛谷图床串行 + 随机间隔）
    {
        std::vector<std::string> candidate_urls;
        for (const auto &p : problems)
            for (const auto &url : p.image_urls())
                candidate_urls.push_back(url);
        if (export_solutions)
        {
            for (const auto &item : opt.solutions->items)
                for (const auto &view : item.solutions)
                    for (const auto &url : image_util::extract_urls(view.content))
                        candidate_urls.push_back(url);
        }
        if (export_articles)
        {
            for (const auto &view : opt.articles->items)
                for (const auto &url : image_util::extract_urls(view.content))
                    candidate_urls.push_back(url);
        }
        offer_missing_images(candidate_urls, opt.new_download, "筛选出的题目");
    }
    // 逐题渲染用的显示选项：语言以筛选参数为准，其余显示开关沿用本次导出的设置
    Options opt_lang = opt;
    opt_lang.lang = filter.lang;

    // 输出采用“临时文件 + fsync + rename”的原子写：
    // 导出中途崩溃/失败不会留下半截 .tex 覆盖旧文件
    const std::filesystem::path tmp_path =
        luogu::compat::temp_sibling_path(output_path);
    FILE *out = luogu::compat::fopen(tmp_path, "w");
    if (!out)
    {
        error = "无法打开输出文件 '" + luogu::compat::path_to_utf8(output_path) + "'";
        return false;
    }

    write_preamble(out, opt, /*doc_only=*/false);

    std::fputs("\\begin{document}\n\n", out);
    std::fputs("\\maketitle\n", out);

    write_toc(out, opt);

    std::fputs("\n\n", out);

    // 写入助手：失败时统一清理临时文件（题解与题面共用）
    auto write_body = [&](const std::string &body) -> bool {
        // 内容按字节数写出：含控制字符（缓存被篡改时）也不会被 C 字符串终止符静默截断
        if (std::fwrite(body.data(), 1, body.size(), out) != body.size())
        {
            std::fclose(out);
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) +
                    "' 失败";
            return false;
        }
        return true;
    };

    // --paginate：题目与文章（题解）分页——每道题、每篇文章都从新的一页开始，即在两次写入
    // 之间插入 \newpage（正文第一个元素之前不插：目录末尾已 \newpage 换页，题解区之前也已经
    // \clearpage）。\newpage 只结束当前页，不写 \addcontentsline、也不生成书签，因此目录条目
    // 与 PDF 书签与不分页时完全一致
    bool body_started = false;
    auto page_break = [&]() {
        if (opt.paginate && body_started)
            std::fputs("\\newpage\n", out);
    };

    int cnt = 0;
    const int total = static_cast<int>(problems.size());
    for (const auto &p : problems)
    {
        const luogu::ProblemSolutionSet *set =
            export_solutions ? opt.solutions->find(p.pid) : nullptr;

        // 题目标题行右侧的「查看题解」按钮固定指向该题第一篇题解（同题题解连续排列）
        std::string first_lid;
        if (export_solutions && !articles_only &&
            opt.solution_export.problem_to_article_link && set &&
            !set->solutions.empty())
            first_lid = set->solutions.front().lid;

        if (!articles_only)
        {
            page_break(); // 上一道题（含其题解）结束后另起一页
            if (!write_body(problem_to_latex(p, opt_lang, first_lid)))
                return false;
            std::fputs("\n", out);
            body_started = true;
        }

        // --solution-placement per-problem：题解紧跟对应题目
        if (per_problem && set)
        {
            for (const auto &view : set->solutions)
            {
                page_break(); // 上一道题或上一篇文章结束后另起一页
                if (!write_body(solution_to_latex(*set, view, opt.solution_export)))
                    return false;
                body_started = true;
            }
        }

        ++cnt;
        // total == 0（筛选结果为空）时不做百分比计算，避免整数除零崩溃
        if (total > 0)
        {
            std::printf("\r正在导出：%3d %% (%d/%d)。 ", cnt * 100 / total, cnt, total);
            fflush(stdout);
        }
    }

    // --solution-placement document-end（默认）：题解统一置于文档最后，每题一组、同题连续排列
    if (export_solutions && !per_problem)
    {
        // 题解区不另起一个与目录同级的一级标题：直接从新的一页开始，每题一组，组标题与普通
        // 题目同为 \section 级（\section* 不进目录）
        std::fputs("\\clearpage\n", out);
        // \clearpage 之后已经是一张新页：分组标题不必再换一次页
        body_started = false;
        for (const auto &item : opt.solutions->items)
        {
            if (item.solutions.empty())
                continue; // 该题无可用题解：不生成小节（也就不产生死链）
            const std::string group_title = item.pid + " " + item.problem_title;
            // 分页时：上一组题解结束后另起一页；组标题与其下第一篇题解同页（组标题单独占一页
            // 没有意义），组内其余篇目各自另起一页
            page_break();
            if (!write_body("\\section*{" + escape_latex(group_title) + "}\n"))
                return false;
            // \section* 不会自动生成书签：补一个顶层（第 0 层）书签，各篇题解的书签仍以第 2
            // 层级挂在它下面
            if (!write_body("\\pdfbookmark[0]{" + escape_latex(group_title) +
                            "}{solgroup-" +
                                luogu::anchor_segment(item.pid) + "}\n"))
                return false;
            // 页眉不在这里设置：每篇题解自己用 \markright 写页眉（见 solution_to_latex），组标题
            // 只是分组用的 \section*，和题解页页眉显示的标题互不干扰
            body_started = true;
            bool first_article = true;
            for (const auto &view : item.solutions)
            {
                if (!first_article)
                    page_break(); // 上一篇文章结束后另起一页
                if (!write_body(solution_to_latex(item, view, opt.solution_export)))
                    return false;
                body_started = true;
                first_article = false;
            }
        }
        std::fputs("\n", out);
    }

    // --article：文章统一置于文档最后（题解之后）。级别与题解正文完全相同（目录层级、页眉、
    // 元信息、正文渲染），只是没有题目跳转按钮，目录条目里也不注明所属题目
    if (export_articles)
    {
        if (body_started)
            std::fputs("\\clearpage\n", out); // 前面写过题面 / 题解：另起一页
        // \clearpage 之后已经是一张新页：第一篇不必再换一次页；文档里原本什么都没有时
        // （--article 单独使用）目录末尾已经 \newpage 换过页，同样不必再插入换页
        body_started = false;
        for (const auto &view : opt.articles->items)
        {
            page_break(); // 上一篇文章（或题解区）结束后另起一页
            if (!write_body(standalone_article_to_latex(view, opt.solution_export)))
                return false;
            body_started = true;
        }
        std::fputs("\n", out);
    }

    // total == 0 时输出固定的完成提示（不计算百分比）
    if (total > 0)
        std::printf("\r正在导出：%3d %% (%d/%d)，完成。\n", cnt * 100 / total, cnt, total);
    else
        std::printf("\r正在导出：完成（共 0 道题）。\n");
    fflush(stdout);

    std::fputs("\\end{document}\n", out);
    if (std::ferror(out))
    {
        std::fclose(out);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
        return false;
    }
    // 落盘并 fsync 后原子替换目标文件；失败时清理临时文件。先 fsync 再关闭：flush 失败时也
    // 必须走 fclose，否则流一直占着临时文件（Windows 上随后删不掉 *.tex.tmp.*，留下垃圾）
    const bool flushed = luogu::compat::flush_and_sync(out);
    const bool closed = (std::fclose(out) == 0);
    if (!flushed || !closed)
    {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
        return false;
    }
    // 原子替换目标文件：MinGW-w64 的 std::filesystem::rename 在目标已存在时会失败（第二次
    // 导出同名文件必失败），必须用 compat::atomic_replace（Windows 走 MoveFileExW）
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

namespace
{
// 按一级标题（Markdown 的 #）把内容切成若干段：每段从下一个一级标题开始，供 --paginate 在
// 「每处一级标题」之前换页。代码围栏（``` / ~~~）内部的 # 不是标题（与渲染器的判定保持一致），
// 不参与切分
std::vector<std::string> split_by_h1(const std::string &markdown)
{
    std::vector<std::string> sections;
    std::string current;
    char fence = 0; // 当前代码围栏字符（` 或 ~），0 表示不在代码块内
    size_t fence_len = 0;
    size_t start = 0;
    for (;;)
    {
        const size_t nl = markdown.find('\n', start);
        const std::string line =
            (nl == std::string::npos) ? markdown.substr(start)
                                      : markdown.substr(start, nl - start);
        const std::string trimmed = trim(line);

        // 一级标题：# 后面是空格或行尾（且不在代码块内）
        bool is_h1 = false;
        if (fence == 0 && !trimmed.empty() && trimmed[0] == '#')
        {
            size_t n = 0;
            while (n < trimmed.size() && trimmed[n] == '#')
                ++n;
            is_h1 = (n == 1 && (n == trimmed.size() || trimmed[n] == ' '));
        }
        // 当前段已有内容时，一级标题另起一段；开头（或只有空行）时不切
        if (is_h1 &&
            current.find_first_not_of(" \t\r\n") != std::string::npos)
        {
            sections.push_back(current);
            current.clear();
        }
        current += line;
        if (nl != std::string::npos)
            current += '\n';

        // 代码围栏的开合（与渲染器一致：整行 trim 后以 ``` / ~~~ 开头）
        if (fence != 0)
        {
            if (is_fence_closer(trimmed, fence, fence_len))
            {
                fence = 0;
                fence_len = 0;
            }
        }
        else if (trimmed.size() >= 3 && (trimmed[0] == '`' || trimmed[0] == '~'))
        {
            size_t n = 0;
            while (n < trimmed.size() && trimmed[n] == trimmed[0])
                ++n;
            if (n >= 3)
            {
                fence = trimmed[0];
                fence_len = n;
            }
        }

        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    if (current.find_first_not_of(" \t\r\n") != std::string::npos ||
        sections.empty())
        sections.push_back(current);
    return sections;
}
} // namespace

bool latex::export_local_markdown(const std::filesystem::path &input_path,
                                  const std::filesystem::path &output_path,
                                  std::string &error, const Options &opt,
                                  bool doc_only)
{
    error.clear();

    // 本地 Markdown 的一级标题（#）就是本文档的章节标题：渲染成 \section，从而进目录、写页眉
    Options local_opt = opt;
    local_opt.h1_as_section = true;
    const Options &effective_opt = local_opt;
    OptionsGuard options_guard(&local_opt);

    // 编码自适应：BOM（UTF-8 / UTF-16 LE、BE / UTF-32 LE、BE）→ 严格 UTF-8 校验 → GB18030
    // （GBK / GB2312）转码；统一成 UTF-8 并归一化换行，生成的 .tex 才能被 xelatex + ctex 排版
    std::string markdown;
    if (!textenc::read_text_file_utf8(input_path, markdown, error))
        return false;

    // 与题面、题解、文章走同一条通道：已在缓存中的直接引用，缺失时询问是否下载
    // （-RD, --new-download 时全部重新下载）
    offer_missing_images(image_util::extract_urls(markdown), effective_opt.new_download,
                         "本地 Markdown 文件");

    const std::filesystem::path tmp_path =
        luogu::compat::temp_sibling_path(output_path);
    FILE *out = luogu::compat::fopen(tmp_path, "w");
    if (!out)
    {
        error = "无法打开输出文件 '" + luogu::compat::path_to_utf8(output_path) + "'";
        return false;
    }

    // 内容按字节数写出：含控制字符（源文件异常时）也不会被 C 字符串终止符截断
    auto write_body = [&](const std::string &body) -> bool {
        if (std::fwrite(body.data(), 1, body.size(), out) != body.size())
        {
            std::fclose(out);
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) +
                    "' 失败";
            return false;
        }
        return true;
    };

    write_preamble(out, effective_opt, doc_only);
    std::fputs("\\begin{document}\n\n", out);
    if (doc_only)
    {
        // --doc-only：没有封面与目录页；页眉页码（--toc-backlinks）改为跳回文档首页——在正文
        // 最前面放一个与目录页同名的锚点即可
        std::fputs("\\hypertarget{luogotoc}{}\n", out);
    }
    else
    {
        std::fputs("\\maketitle\n", out);
        write_toc(out, effective_opt);
    }

    // 一级标题（#）由 markdown_to_latex 渲染成 \section：进目录，同时把标题写进 \rightmark
    // （页眉）。--paginate 时每处一级标题另起一页；--doc-only 不换页，整篇连贯输出
    const bool paginate = effective_opt.paginate && !doc_only;
    const std::vector<std::string> sections =
        paginate ? split_by_h1(markdown) : std::vector<std::string>{markdown};
    bool wrote_any = false;
    for (size_t i = 0; i < sections.size(); ++i)
    {
        // 第一段之前不插换页：文档开头本来就是新一页的开始
        if (i > 0 && wrote_any)
            std::fputs("\\newpage\n", out);
        if (!write_body(markdown_to_latex(sections[i]) + "\n"))
            return false;
        if (sections[i].find_first_not_of(" \t\r\n") != std::string::npos)
            wrote_any = true;
    }

    // 缺少 \end{document} 时 LaTeX 不会正常结束文档（目录 .toc 也写不出来）
    std::fputs("\\end{document}\n", out);
    if (std::ferror(out))
    {
        std::fclose(out);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
        return false;
    }
    // 先 fsync 再关闭：flush 失败时也必须走 fclose，否则流一直占着临时文件（Windows 上删不掉）
    const bool flushed = luogu::compat::flush_and_sync(out);
    const bool closed = (std::fclose(out) == 0);
    if (!flushed || !closed)
    {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
        return false;
    }
    // MinGW-w64 的 std::filesystem::rename 在目标已存在时会失败，必须用
    // compat::atomic_replace（Windows 走 MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)）
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
