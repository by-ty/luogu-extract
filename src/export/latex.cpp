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
#include "luogu-extract/util/problem_info.h"
#include "luogu-extract/util/version.h"

namespace
{

// 解析 [first, last) 内的非负十进制数字；失败或溢出时返回 false。
// 用于处理缓存内容中可能出现的畸形/超长数字（此前 std::stoi/stoul 会抛异常）。
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
            return false; // 溢出
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

std::string to_lower_ascii(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 洛谷题面常在公式里用 \newcommand/\renewcommand 自定义命令，
// 标准 LaTeX 中若与已有命令同名会报 "already defined"；统一转成 \def（允许重复定义）
// 对齐环境行归一化：洛谷题面里 \begin{array}{c} 等常有多余/缺少的 &，
// 导致 "Extra alignment tab"；把每行统一到目标列数（array 按 spec，矩阵按最大行宽）
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
        if (name == "array" || name == "subarray")
        {
            if (body_start < s.size() && s[body_start] == '{')
            {
                const size_t spec_end = s.find('}', body_start);
                if (spec_end != std::string::npos)
                {
                    const std::string spec = s.substr(body_start + 1, spec_end - body_start - 1);
                    for (char c : spec)
                        if (std::isalpha(static_cast<unsigned char>(c)))
                            ++spec_cols;
                    body_start = spec_end + 1;
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
        // 先递归处理嵌套的对齐环境（内层 array 列规格、行内 & 等）；
        // 限制递归深度，防止恶意内容构造超深嵌套环境导致栈溢出
        std::string body = (depth >= kMaxEnvDepth)
                               ? s.substr(body_start, end_pos - body_start)
                               : normalize_alignment(
                                     s.substr(body_start, end_pos - body_start),
                                     depth + 1);

        // 去掉多余的 &&（洛谷题面常见写法，LaTeX 会报 Extra alignment tab）；
        // 只处理本层（跳过嵌套环境内部）
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

        // 按行拆分（\\ 或 \cr）：忽略花括号内和嵌套环境内部，
        // 否则内层 aligned/array 的 \\ 会被误当成外层换行
        std::vector<std::string> rows;
        std::string cur;
        int depth = 0;
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
            if (body[k] == '{') ++depth;
            else if (body[k] == '}') --depth;
            if (depth <= 0 && env_depth == 0)
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

        // 统计每行单元格数（忽略行首 \hline；嵌套环境内部不算）
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
        // 目标列数：
        // - array/subarray：以显式列规格为准（多余的 & 截掉）
        // - cases 系列：固定 2 列
        // - matrix/aligned 等：按最宽的一行
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

        // 逐行归一化：截断多余单元格、补齐缺失单元格
        std::string new_body;
        for (size_t ri = 0; ri < rows.size(); ++ri)
        {
            if (ri)
                new_body += "\\\\";

            std::string row = rows[ri];
            std::string hline;
            size_t start = 0;
            // 连续多个 \hline / \noalign{\hline} 都要作为行前缀取走，
            // 否则第二个 \hline 会变成单元格内容触发 Misplaced \noalign
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
                    new_body += " "; // gathered 等环境不接受 &
                if (!kNoAmpEnvs.count(name) || !cells[ci].empty())
                    new_body += cells[ci];
            }
        }

        // amsmath 的 matrix 环境最多 10 列，超过时改用 array
        const bool matrix_family = (name != "array" && name != "cases" &&
                                    name != "dcases" && name != "rcases");
        if (name == "array" || (matrix_family && target > 10))
        {
            // 重建列规格：取 target 列（统一用 c，保证能编译）
            out += "\\begin{array}{" + std::string(target, 'c') + "}" + new_body +
                   "\\end{array}";
        }
        else
        {
            out += "\\begin{" + name + "}" + new_body + endtag;
        }
        p = end_pos + endtag.size();
    }
    return out;
}

// 数学公式里是否有“顶层”（不在任何 \begin 环境、也不在花括号内）的 & 或 \\ / \cr。
// 洛谷题面常把两段矩阵用顶层 & 和 \\ 直接拼在一行（KaTeX 能渲染），
// 标准 LaTeX 必须包进一个对齐环境才能编译
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

std::string sanitize_math(std::string s)
{
    // \verb 内容是字面文本：先整体保护起来（占位符），
    // 等所有转换结束后再按文本模式转义还原，
    // 避免中间的 \color / px / % # 等转换污染 verb 内容
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

    // 源数据里的 \text{\\}（KaTeX 允许文本内换行）在 LaTeX 的表格/矩阵里
    // 会触发 Misplaced \cr；\newline 在文本模式任何位置都合法
    static const std::regex kTextNewline(R"(\\text\{\s*\\\\\s*\})");
    s = std::regex_replace(s, kTextNewline, "\\text{\\newline}");

    // Unicode 数学符号（∑ 等）是普通字符，\limits 要求数学算子，
    // 转成对应的 LaTeX 命令
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

    // 公式末尾悬空的 ^ / _（如“……则省略 ^”）：没有指数/下标参数，
    // 直接当成符号输出，避免 Missing { inserted（已转义的 \_ 不受影响）
    static const std::regex kTrailingCaret(R"((^|[^\\])[\^_](?=\s*\$?\s*$))");
    s = regex_transform(s, kTrailingCaret, [&](const std::smatch &m) {
        return m[1].str() + "\\wedge";
    });

    // KaTeX 兼容：\colorbox{#hex} / \textcolor{#hex} / \color{#hex}
    // → xcolor 的 HTML 颜色模型
    // 3 位十六进制色值（如 #fff）补齐成 6 位（xcolor HTML 模型要求）
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

    // 2 位十六进制（洛谷题面里的 \color{ff}）按白色处理，避免 Undefined color
    static const std::regex kColor2Hex(R"(\\color\{([0-9a-fA-F]{2})\})");
    s = std::regex_replace(s, kColor2Hex, "\\color{white}");

    // \fcolorbox{frame}{bg}{...}：任一参数是十六进制时转成 HTML 模型
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

    // \hspace 不接受 mu（数学单位），必须用 \mkern；\hspace{3mu} → \mkern3mu
    static const std::regex kHspaceMu(R"(\\hspace\{(\d+(?:\.\d+)?)mu\})");
    s = std::regex_replace(s, kHspaceMu, "\\mkern$1mu");

    // 洛谷题面常见笔误 \\end{cases} / \\\\end{cases}（多写/少写反斜杠）：
    // 行分隔符 \\ 会吃掉 \end 的反斜杠，统一还原成单个 \end{
    static const std::regex kRowEnd(R"(\\+end\{)");
    s = std::regex_replace(s, kRowEnd, "\\end{");

    // \overline\texttt{ab} 这类“重音命令直接跟另一个命令”的写法：
    // 重音命令需要花括号参数，把后面的命令连同参数一起包进 {} 
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
                        t += s.substr(p, w - p); // \overline
                        t += "{";
                        t += s.substr(w, end - w); // \texttt{ab}
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

    // 旧字体命令 \tt{...} → \texttt{...}；\tt 后跟数字/字母串也转换
    static const std::regex kTT(R"(\\tt\{)");
    static const std::regex kTTPlain(R"(\\tt\s+([A-Za-z0-9]+))");
    s = std::regex_replace(s, kTT, "\\texttt{");
    s = std::regex_replace(s, kTTPlain, "\\texttt{$1}");

    // 裸 \texttt（后面没跟 {，如 \texttt \\_）在数学模式会吞掉下一个
    // 命令当参数，补一个空花括号
    static const std::regex kTTBare(R"(\\texttt(?![{]))");
    s = std::regex_replace(s, kTTBare, "\\texttt{}");

    // \kern{...} 不接受花括号参数（TeX 原语），转成 \hspace{...}
    static const std::regex kKern(R"(\\kern\{)");
    s = std::regex_replace(s, kKern, "\\hspace{");

    // \space 后紧跟中文字符在 xelatex 会报 Undefined control sequence，
    // 转成控制空格（数学/文本模式都可用）
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

    // \texttt{...} 在数学模式里，命令（\textcolor、\textbackslash 等）直接保留
    // （LaTeX 数学模式 texttt 能正常执行）；只需转义裸特殊字符 _ # % & ^ ~ { }
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
                        content += s[q + 1]; // 转义对原样保留
                        q += 2;
                        continue;
                    }
                    if (s[q] == '\\' && q + 1 < s.size() &&
                        std::isalpha(static_cast<unsigned char>(s[q + 1])))
                    {
                        // 控制词（如 \textcolor）整体保留
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
                    // 数学符号在 texttt（文本模式）里未定义，需包 $...$ 显示；
                    // 字号命令（\small 等）在数学模式未定义，直接去掉
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
                                // 控制词（\textcolor、\textbackslash 等）连同其
                                // 花括号参数（如 \textcolor{red}、\textbackslash{}）
                                // 原样保留，否则转义参数里的 { } 会破坏命令
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
                                    // 字号命令去掉（内容保留，被循环继续处理）
                                    k = j - 1;
                                    continue;
                                }
                                if (kMathSymbols.count(name))
                                {
                                    // 数学符号连同参数包上 $...$（如 \sqrt{2}）
                                    esc += "$" + word + "$";
                                    k = j - 1;
                                    continue;
                                }
                                esc += content.substr(k, j - k);
                                k = j - 1;
                            }
                            else
                            {
                                // 转义对（\{ \} \_ 等）原样保留
                                esc += c;
                                esc += content[k + 1];
                                ++k;
                            }
                            continue;
                        }
                        if (c == '\\') // 结尾悬空的 \ → \textbackslash{}
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
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // \operatorname{...} 参数里含 \color 时，\limits 会报
    // "Limit controls must follow a math operator"，把参数整体包一层花括号
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
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // \text{...} 里的数学符号（\le、\ldots 等）在文本模式未定义，
    // 包上 $...$；\text{ 与 \texttt{ 区分开（\texttt 已单独处理）
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
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // 裸 \sout（未跟 {）会吞掉后续命令作为参数（如 \sout\text{...}），
    // 直接删掉，保留正文
    {
        std::string t;
        size_t p = 0;
        while (p < s.size())
        {
            if (s.compare(p, 7, "\\sout{") == 0)
            {
                t += "\\sout{";
                p += 7;
                continue;
            }
            if (s.compare(p, 5, "\\sout") == 0)
            {
                p += 5; // 裸 \sout 删掉
                continue;
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }


    // bm 包无法处理 \bm{...\color...} / \boldsymbol{...\color...}，
    // 内容含 color 时额外包一层花括号；\bm 的参数里 ~ 会触发
    // Missing number，转成数学空格
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
            }
            t += s[p];
            ++p;
        }
        s = std::move(t);
    }

    // 对齐环境行归一化（多余/缺少 &、数组列规格不匹配）
    s = normalize_alignment(s);

    // 顶层 & / \\：有 \begin 环境时把整段包进 aligned（如两段矩阵用 & 直接拼接），
    // 没有环境时按普通字符转义（$&@$、样例输入换行等）
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
                    // \text{\\} 在表格/矩阵里会触发 Misplaced \cr，
                    // \newline 在文本模式里任何位置都合法
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

    // \newcommand → \def（先于 %/# 转义，这样定义体内的 #1 参数引用
    // 在转义阶段已被识别为 \def 宏参数而保留）
    static const std::regex kNewCommandBraced(R"(\\(?:re)?newcommand\s*\{([^}]*)\})");
    static const std::regex kNewCommandPlain(R"(\\(?:re)?newcommand\s+([A-Za-z@]+))");
    s = std::regex_replace(s, kNewCommandBraced, "\\def$1");
    s = std::regex_replace(s, kNewCommandPlain, "\\def$1");

    // \newcommand 的 [N] 参数个数写法（\def\cases[1]{...}）对 \def 无效，
    // 转成标准的参数形式 \def\cases#1{...}
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
                    if (e < s.size() && s[e] == ']' && e > w + 1)
                    {
                        // 参数个数：安全解析并限制上限（TeX 宏参数最多 9 个）。
                        // 畸形/超长数字（如 \def\foo[999999999999]）不再抛异常，
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

    // 裸 % 和 # 在 LaTeX（含数学模式）里是特殊字符，需转义；
    // 已转义的 \% / \# 先保护起来，避免二次转义
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
            // 宏参数（#1、#2...）不能转义，否则 \def\c#1{...} 会被破坏；
            // \def\<名字>#1 的参数表，以及 \def 定义体内对参数的引用都要保留
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
                // \def 的参数表（\def\foo#1#2{...}）和定义体内的参数引用：
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
                                // 位于 \def\<名字> 与 body 之间的参数表
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

    // 洛谷题面常用 \def\c#1{...} 这类单字母自定义宏，与 LaTeX 内部命令
    // （\c \t \b \s \r 等重音命令）冲突；统一重命名为 \lgoX 前缀。
    // 只在单遍内处理单字母宏，避免 \def\bg 等多字母宏被误改或重复改名
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
                    names.insert(x); // 单字母宏
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
                // 用法 \X（后跟 { / 空格 / 标点 等非小写字母，避免误伤 \color 这类长命令）
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

    // 控制词后紧跟字母（含 CJK，XeTeX 里都是 catcode 11）时，会被并进
    // 控制词（\qquad第 → 未定义命令 \qquad第；\leN → 未定义命令 \leN）；
    // 按最长已知命令前缀拆开并补空组。放在单字母宏改名之后，
    // 这样 \lgowN 这类改名产物也能被处理
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
            // 常用算子/函数
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
            "triangle", "square", "Box", "Diamond", "clubsuit", "diamondsuit",
            "heartsuit", "spadesuit", "checkmark", "dagger", "ddagger",
            "star", "bullet", "degree", "copyright",
            // 希腊字母
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
        // 以 kSafeSplit 中某个短命令开头、但本身是完整命令名的命令
        // （如 \leqslant 以 \leq 开头）。整词匹配时必须原样保留，否则会
        // 被拆成 \leq{}slant（P4381 的公式曾因此编译出错）；后跟变量时
        // （如 \leqslantN）仍按“命令 + 后续字符”拆开。
        // 名单取自 LaTeX 内核 / amsmath / amssymb / mathtools / unicode-math
        // 中被引用的西文命令，新增命令时在此补充。
        static const std::set<std::string> kKnownFullCommands = {
            // le / ge：小于等于、大于等于、大小关系
            "leq", "leqq", "leqqslant", "leqslant", "lescc", "lesdot", "lesdoto",
            "lesdotor", "lesges", "less", "lessapprox", "lessdot", "lesseqgtr",
            "lesseqqgtr", "lessgtr", "lesssim", "approxeq", "approxeqq",
            "approxident", "geq", "geqq", "geqqslant", "geqslant", "gescc", "gesdot",
            "gesdoto", "gesdotol", "gesles", "gneq", "gneqq", "gnsim", "gvertneqq",
            "gtrapprox", "gtrarr", "gtrdot", "gtreqless", "gtreqqless", "gtrless",
            "gtrsim", "gtcc", "gtcir", "gtlpar", "gtquest",
            // le / ge：箭头与左向符号
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
            // lt / ln / lg、gt：大小关系与对数
            "ltimes", "ltcc", "ltcir", "ltlarr", "ltquest", "ltrivb", "lneq", "lneqq",
            "lnsim", "lnapprox", "lvertneqq", "lgE", "lgblkcircle", "lgblksquare",
            "lgwhtcircle", "lgwhtsquare",
            // in / int / sup / sub / sum：积分与上下限
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
            // mid / parallel / not / mod
            "middle", "midbarvee", "midbarwedge", "midcir", "parallelogram",
            "parallelogramblack", "perps", "notag", "notni", "models", "modtwosum",
            // cdot / circ / cup / cap / oplus 等算子
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
            // dots / wedge / vee 等
            "dotsb", "dotsc", "dotsi", "dotsm", "dotso", "dotsim", "dotsminusdots",
            "ddotseq", "wedgebar", "wedgedot", "wedgedoublebar", "wedgemidvert",
            "wedgeodot", "wedgeonwedge", "wedgeq", "veebar", "veedot", "veedoublebar",
            "veeeq", "veemidvert", "veeodot", "veeonvee", "veeonwedge", "vertoverlay",
            // 文档命令与本项目预置的兼容命令（\providecommand）
            "newcommand", "renewcommand", "providecommand", "infin", "circledR",
        };
        auto is_known = [&](const std::string &w) {
            return kKnownPrefixes.count(w) != 0 ||
                   kKnownFullCommands.count(w) != 0 ||
                   (w.size() > 3 && w.compare(0, 3, "lgo") == 0);
        };
        // 只有这些“短命令”允许被拆开（\leN → \le{}N）。
        // \textcolor 这类长命令即使含已知前缀也绝不拆，避免破坏命令
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
                // 整词不是已知命令时（\leN、\qquad第），按最长已知前缀拆开，
                // 在命令后补空组，避免后续字母被并入命令名；
                // 整词是已知命令（\operatornamewithlimits、\frac12、
                // \leqslant 等）则不动
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
                // CJK 等非 ASCII 字符不是 isalpha，单词扫描会停在它前面；
                // 若上面没能拆开（如 \qquad第），在整词后补空组
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

    // \def\or{...} 会重定义 LaTeX 数组前导里的内部命令 \or，
    // 导致 "in array arg"；统一改名为 \lgooor
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

    // 洛谷题面常见的 $^$（表示“二进制异或”）没有底数，LaTeX 编译报错；
    // 裸上/下标（$^1$、$^*$ 等脚注标记）补空底数 ${}^1$；
    // 末尾悬空的 ^ / _（如“……则省略 ^”）直接输出 \wedge。
    // 放在最后处理：前面 \texttt{...}\\ 等转换会改变 ^ / _ 的相邻字符
    {
        static const std::regex kBareCaret(R"(\$[\^_]\$)");
        s = std::regex_replace(s, kBareCaret, "$\\wedge$");

        static const std::regex kNoBaseCaret(R"((?:^|\$)[\^_])");
        s = regex_transform(s, kNoBaseCaret, [&](const std::smatch &m) {
            const std::string pre = m[0].str();
            return pre.substr(0, pre.size() - 1) + "{}" + pre.back();
        });

        static const std::regex kTrailingCaret(R"((^|[^\\])[\^_](?=\s*\$?\s*$))");
        s = regex_transform(s, kTrailingCaret, [&](const std::smatch &m) {
            return m[1].str() + "\\wedge";
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
    return s;
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

    // 部分 Unicode 标点在常用西文字体（Latin Modern 等）中没有字形，
    // 而 xeCJK 也不会把它们交给中文字体，编译时会被静默丢弃：例如洛谷
    // 题面里的中文破折号“――”用的是 U+2015 HORIZONTAL BAR，P1131 中
    // 会整段消失（日志只报 "Missing character"）。这里在文本模式把这些
    // 字符换成等价的 LaTeX 命令，任何字体方案下都能正常排版。
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

// 去掉 $...$ 与 $$...$$ 数学片段，生成适合 PDF 书签的纯文本标题：
// hyperref 无法把 unicode-math 的数学符号（\Umathchar 定义）转成书签
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

// \includegraphics 的路径：转义空格，并把 '\' 归一化为 '/'（TeX 不认识
// Windows 反斜杠路径；源路径已用 path_to_utf8 转成 UTF-8）
std::string escape_path(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c == ' ')
            out += "\\ ";
        else if (c == '\\')
            out += '/';
        else
            out += c;
    }
    return out;
}

// \url{} / \href{} 中的原始 URL 做最小转义：% 与 # 在 TeX 中是特殊字符，
// 未转义会破坏编译或吞掉 URL 剩余部分
std::string escape_url(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c == '%')
            out += "\\%";
        else if (c == '#')
            out += "\\#";
        else
            out += c;
    }
    return out;
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

// data:image/...;base64,... 这类内嵌数据 URI：xelatex 无法使用，
// 而且 base64 内容是一整串无空格文本，会让 TeX 段落排版出问题
// （甚至段错误/卡死），一律跳过
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
    unsigned char head[16] = {0};
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
    if (n >= 4 && (std::memcmp(head, "<svg", 4) == 0 ||
                   std::memcmp(head, "<?xm", 4) == 0))
        return ImageKind::kSvg;
    if (n >= 4 && head[0] == 0x00 && head[1] == 0x00 &&
        head[2] == 0x01 && head[3] == 0x00)
        return ImageKind::kIco;
    if (n >= 5 && std::memcmp(head, "%PDF-", 5) == 0)
        return ImageKind::kPdf;
    if (n >= 2 && head[0] == '%' && head[1] == '!')
        return ImageKind::kEps;
    return ImageKind::kUnknown;
}

// 修正 JPEG 的 JFIF 像素密度。洛谷个别老图密度被写成 1 dpi，
// XeTeX 会把 405×256px 的图片按 405×256 英寸排版，超过 TeX 的
// 19 英尺上限而报 "Dimension too large"。密度明显异常（< 72 dpi）
// 时就地改写为 72 dpi（幂等，可重复执行）。
// 返回 true 表示无需修正或已修正；若需要修正但缓存不可写则返回 false。
bool fix_jpeg_density(const std::filesystem::path &path)
{
    FILE *in = luogu::compat::fopen(path, "r+b");
    if (!in)
        return false;

    unsigned char head[24] = {0};
    const size_t n = std::fread(head, 1, sizeof(head), in);
    const bool is_jfif = n >= 18 && head[0] == 0xFF && head[1] == 0xD8 &&
                         head[2] == 0xFF && head[3] == 0xE0 &&
                         std::memcmp(head + 6, "JFIF", 4) == 0 && head[10] == 0;
    if (is_jfif)
    {
        const unsigned units = head[13];
        const unsigned xd = (head[14] << 8) | head[15];
        const unsigned yd = (head[16] << 8) | head[17];
        if (units == 1 && (xd < 72 || yd < 72))
        {
            const unsigned char k72[] = {1, 0, 72, 0, 72};
            // 检查 fseek 返回值：失败时不能继续写，否则会写在当前文件
            // 位置（读取 24 字节后），可能直接损坏 JPEG
            if (std::fseek(in, 13, SEEK_SET) != 0)
            {
                std::fclose(in);
                return false;
            }
            const bool wrote = std::fwrite(k72, 1, sizeof(k72), in) == sizeof(k72);
            const int close_status = std::fclose(in);
            return wrote && close_status == 0;
        }
    }
    std::fclose(in);
    return true;
}

// 让缓存图片能被 xelatex 正常加载：
// - GIF/WebP/BMP/SVG/ICO 以及无法识别的内容返回空路径（调用方跳过该图）；
// - JPEG 密度异常时先就地修正；
// - PNG/JPEG/PDF/EPS 内容若文件名没有与真实内容相符的扩展名（如无扩展名、
//   扩展名是 URL 的残余、或扩展名与内容不符，如 .png 里存的是 JPEG），
//   在缓存目录生成带正确扩展名的副本——XeTeX 按扩展名选择解码器。
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

    // 扩展名必须与真实内容相符才直接使用；不符时与无扩展名同样处理，
    // 复制一份带正确扩展名的副本（避免 xelatex 按错误扩展名解码失败）
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

    // 生成带正确扩展名的副本，避免与已有缓存文件冲突
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

// 用正则逐个替换，convert(match) 返回替换文本
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

// 当前导出过程的 LaTeX 显示选项。仅 export_latex 通过 OptionsGuard 设置；
// 行内转换（如 bilibili 视频 URL 是否输出为超链接）据此判断。
// 指针为空时按默认行为处理（与未传入任何新参数时一致）。
const latex::Options *g_options = nullptr;

// RAII 守卫：进入 export_latex 时挂上选项，离开（含提前返回）时自动还原
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

// 占位符：\x01R<n>\x02（注意用字符串拼接构造，避免 \x01R 被当作十六进制转义）
std::string placeholder(size_t index)
{
    return std::string(1, '\x01') + "R" + std::to_string(index) + std::string(1, '\x02');
}

// 恢复 \x01R<n>\x02 占位符（数字解析安全化：内容里的畸形/超长数字不再抛异常）
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

// 合并相邻的数学占位符：洛谷题面里 \$$ 等畸形写法会把一个公式拆成多段
// （段间可能夹着多余的 $ 和空白），这里把它们拼成一个，避免输出残缺公式
void merge_adjacent_math(std::string &s,
                         std::vector<std::string> &raws,
                         const std::vector<bool> &is_math)
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

        // 块级公式 $$...$$ 不参与合并
        if (raws[idx].rfind("$$", 0) == 0)
        {
            out += s.substr(i, end - i);
            i = end;
            continue;
        }
        // 收集相邻的数学片段并合并：仅当间隙里含多余的 $ 才合并
        // （纯空白间隔的两个公式是独立的；如 $$...$$ 后跟 $...$ 不应合并）
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
            // 严格相邻（空间隙）也合并；只有非空且无 $ 的间隙（如 "$$...$$ 后跟 $...$"）才断开
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
        out += "\x01R" + std::to_string(raws.size() - 1) + "\x02";
        i = run_end;
    }
    s = std::move(out);
}

std::string inline_to_latex_impl(const std::string &text, std::vector<std::string> &raws,
                                 std::vector<bool> &is_math, int depth)
{
    // 嵌套粗体/链接等行内语法的最大递归深度：超深嵌套（恶意内容）直接
    // 按普通文本转义，不再深入，避免栈溢出
    static const int kMaxInlineDepth = 50;
    if (depth > kMaxInlineDepth)
        return escape_latex(text);

    auto protect = [&](std::string latex) {
        raws.push_back(std::move(latex));
        is_math.push_back(false);
        return placeholder(raws.size() - 1);
    };

    std::string s = text;

    // 1. 把 \$（转义美元符）保护起来，避免数学提取时把它的 $ 当成公式分隔符
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
    // 2. 数学公式（原样保留）
    {
        static const std::regex re("(\\$\\$[^$]+\\$\\$|\\$[^$]+\\$)");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            raws.push_back(sanitize_math(m[1].str()));
            is_math.push_back(true);
            return placeholder(raws.size() - 1);
        });
    }
    // 2.5 合并相邻的数学片段（源数据畸形时一个公式可能被拆成多段）
    merge_adjacent_math(s, raws, is_math);
    // 3. 行内代码（放在数学之后：数学里的反引号是字面量，不应被当成代码分隔符）
    {
        // 支持 1~N 个反引号包裹的代码段（CommonMark 规则），
        // 避免 ``code`` 这类写法留下多余反引号
        static const std::regex re("`+([^`]+?)`+");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            return protect("\\texttt{" + escape_latex(m[1].str()) + "}");
        });
    }
    // 4. 图片包在链接里：[![](img)](url) → \href{url}{图片}；
    //     必须先于普通链接处理，否则内层 ] 会破坏链接解析
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
                // --no-bilibili-link：视频 URL 输出为普通文本而非超链接
                if (g_options && !g_options->bilibili_links)
                    return protect(escape_latex(link_url));
                return protect("\\url{" + escape_url(link_url) + "}");
            }
            // 按缓存文件真实内容判断能否加载，扩展名与内容不符的图片
            // 先经 prepare_cached_image 归一化（修正密度/补扩展名）
            const std::filesystem::path usable =
                prepare_cached_image(crawler::image_cache_path(img_url));
            if (usable.empty())
                return std::string(); // xelatex 无法加载的格式，直接跳过
            const std::string path = escape_path(luogu::compat::path_to_utf8(usable));
            // \noindent：图片单独成段时去掉段首缩进（约 2 个中文字宽），
            // 否则宽度恰为 \linewidth 的图片会连同缩进一起超出右边界
            return protect("\\noindent\\href{" + escape_latex(link_url) + "}{"
                           "\\IfFileExists{" + path + "}"
                           "{\\luogoincludegraphics{" + path + "}}"
                           "{\\mbox{}}}");
        });
    }
    // 4.5 图片 → 缓存文件；视频 → 仅链接
    {
        static const std::regex re("!\\[[^\\]]*\\]\\s*\\(\\s*([^\\s)]+)(?:\\s+[\"'][^\"']*[\"'])?\\s*\\)");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            const std::string url = m[1].str();
            if (is_data_uri(url))
                return std::string(); // data URI 直接丢弃
            if (!looks_like_url(url))
                return m[0].str(); // 不是真正的图片链接，保留原文（转义阶段处理）
            if (is_video_url(url))
            {
                // --no-bilibili-link：视频 URL 输出为普通文本而非超链接
                if (g_options && !g_options->bilibili_links)
                    return protect(escape_latex(url));
                return protect("\\url{" + escape_url(url) + "}");
            }
            // 按缓存文件真实内容判断能否加载：GIF/WebP/SVG/BMP/ICO 等
            // xelatex 无法加载的格式直接跳过；扩展名与内容不符的图片
            // 先经 prepare_cached_image 归一化（修正密度/补扩展名）
            const std::filesystem::path usable =
                prepare_cached_image(crawler::image_cache_path(url));
            if (usable.empty())
                return std::string();
            const std::string path = escape_path(luogu::compat::path_to_utf8(usable));
            // \noindent：图片单独成段时去掉段首缩进（约 2 个中文字宽），
            // 否则宽度恰为 \linewidth 的图片会连同缩进一起超出右边界
            return protect("\\noindent\\IfFileExists{" + path + "}"
                           "{\\luogoincludegraphics{" + path + "}}"
                           "{\\mbox{}}");
        });
    }
    // 5. 链接
    {
        static const std::regex re("\\[([^\\]]*)\\]\\s*\\(\\s*([^\\s)]+)(?:\\s+[\"'][^\"']*[\"'])?\\s*\\)");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            if (is_data_uri(m[2].str()))
                return m[1].str(); // data URI 链接：只保留链接文字
            if (!looks_like_url(m[2].str()))
                return m[0].str(); // 不是真正的链接目标，保留原文（转义阶段处理）
            // --no-bilibili-link：链接目标是 bilibili 视频 URL 时只保留链接文字
            if (g_options && !g_options->bilibili_links && is_video_url(m[2].str()))
                return protect(inline_to_latex_impl(m[1].str(), raws, is_math, depth + 1));
            return protect("\\href{" + escape_latex(m[2].str()) + "}{" +
                           inline_to_latex_impl(m[1].str(), raws, is_math, depth + 1) + "}");
        });
    }
    // 6. 自动链接 <https://...>
    {
        static const std::regex re("<(https?://[^>]+)>");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            // --no-bilibili-link：视频 URL 输出为普通文本而非超链接
            if (g_options && !g_options->bilibili_links && is_video_url(m[1].str()))
                return protect(escape_latex(m[1].str()));
            return protect("\\url{" + escape_url(m[1].str()) + "}");
        });
    }
    // 7. 粗体
    {
        static const std::regex re("\\*\\*([^*]+)\\*\\*");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            return protect("\\textbf{" + inline_to_latex_impl(m[1].str(), raws, is_math, depth + 1) + "}");
        });
    }
    // 8. 斜体（下划线形式的斜体不转换，避免误伤标识符中的 _）
    {
        static const std::regex re("\\*([^*]+)\\*");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            return protect("\\textit{" + inline_to_latex_impl(m[1].str(), raws, is_math, depth + 1) + "}");
        });
    }
    // 9. 删除线
    {
        static const std::regex re("~~([^~]+)~~");
        s = regex_transform(s, re, [&](const std::smatch &m) {
            return protect("\\sout{" + inline_to_latex_impl(m[1].str(), raws, is_math, depth + 1) + "}");
        });
    }
    // 10. 转义剩余特殊字符
    s = escape_latex(s);
    // 11. 恢复占位符。链接/粗体等递归生成的片段内部可能还嵌着占位符，
    //     需要迭代还原直到不再出现 \x01。
    //     迭代次数加上限：若还原结果仍含 \x01（例如内容本身含占位符形态的
    //     控制字符形成自指），不再无限循环，剩余控制字符会被视为普通内容
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
    // 占位符池只建一次；粗体/链接等递归调用共用同一池，
    // 否则嵌套占位符在递归中无法还原会死循环
    std::vector<std::string> raws;
    std::vector<bool> is_math;
    return inline_to_latex_impl(text, raws, is_math);
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
    static const std::regex kItem(R"(^[-+*]\s+|\d+\.\s+)");
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

// 一行是否可能是表格行（包含 |）
bool is_table_row(const std::string &line)
{
    return trim(line).find('|') != std::string::npos;
}

// 把多个行内片段拼成一个段落：硬换行用 \\\\，丢弃转换后为空的片段
// （缺失图片会变成空），\\\\ 后紧跟 [ 时补 {} 防止被当作可选参数
std::string join_inline_parts(const std::vector<std::pair<std::string, bool>> &parts)
{
    std::vector<std::pair<std::string, bool>> out;
    for (const auto &rp : parts)
    {
        const std::string c = inline_to_latex(rp.first);
        if (!c.empty())
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

// 表格：rows[0] 表头，rows[1] 对齐行，其余为内容行。
// 支持洛谷表格合并语法：单元格内容恰为 "^" 时向上合并单元格（行合并），
// 恰为 "<" 时向左合并单元格（列合并）；合并标记必须是单元格内唯一的纯文本内容。
// 合并解析保证每个合并区域都是矩形（\multicolumn/\multirow 可表达），
// 无法表达的交叉/ L 形合并会安全退化为空单元格，保证输出可编译。
void emit_table(const std::vector<std::string> &rows, std::string &out)
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
        while (!cells.empty() && trim(cells.back()).empty())
            cells.pop_back();
        return cells;
    };

    const std::vector<std::string> align_row = split_cells(rows[1]);
    const size_t col_count = align_row.size();
    if (col_count == 0)
        return; // 防御：无列时不再输出（正常数据至少 1 列）

    std::string spec = "|";
    std::vector<char> col_types;
    col_types.reserve(col_count);
    for (const auto &a : align_row)
    {
        const std::string t = trim(a);
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

    // ---- 合并解析 ----
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
            if (trim(grid[r][c]) != "^")
                continue;
            long top = -1;
            if (vtop[r - 1][c] >= 0) // 上方是合并成功的 ^（链式）
                top = vtop[r - 1][c];
            else if (trim(grid[r - 1][c]) != "^" && trim(grid[r - 1][c]) != "<")
                top = static_cast<long>(r - 1); // 上方是普通内容单元格
            if (top < 0)
                continue; // 无法合并：按空单元格处理
            vtop[r][c] = top;
            vend[top][c] = static_cast<long>(r);
        }
    }

    // 2) 横向合并（< 向左合并）：与左侧单元格合并；左侧是 < 时继续向左（链式）。
    //    左侧是 ^ 或合并失败的 < 时无法表达，按空单元格处理；
    //    左侧内容单元格带有向下延伸的纵向合并时，仅当合并区域仍是矩形
    //    （下方对应位置全是 ^ / < 标记）才合并，否则退化为空单元格
    for (size_t r = 0; r < row_count; ++r)
    {
        for (size_t c = 1; c < col_count; ++c)
        {
            if (trim(grid[r][c]) != "<")
                continue;
            long src = -1;
            const std::string left = trim(grid[r][c - 1]);
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
                            trim(grid[static_cast<size_t>(rr)][static_cast<size_t>(cc)]);
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

    // 3) 标记“组合矩形”（\multicolumn 与 \multirow 叠加）覆盖的单元格：
    //    其内部的横向分隔线也必须跳过，否则会穿过合并单元格；
    //    同时记录每个单元格所属矩形的左右列边界（渲染时用于去掉矩形
    //    内部的列间竖线，只保留矩形外边界处的竖线）
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
        const std::string cell = trim(grid[r][c]);
        // 纵向合并跨过该边界继续向下
        const long t = (cell == "^" && vtop[r][c] >= 0)
                           ? vtop[r][c]
                           : static_cast<long>(r);
        if (vend[static_cast<size_t>(t)][c] > static_cast<long>(r))
            return true;
        // 组合矩形：该边界两侧的行都在矩形内
        return in_rect[r][c] && r + 1 < row_count && in_rect[r + 1][c];
    };

    // ---- 渲染 ----
    out += "\\begin{tabular}{" + spec + "}\n\\hline\n";
    for (size_t r = 0; r < row_count; ++r)
    {
        for (size_t c = 0; c < col_count; ++c)
        {
            const std::string cell = trim(grid[r][c]);
            // 横向合并的内部单元格：由起始列的 \multicolumn 占用，不输出
            if (cell == "<" && hsrc[r][c] >= 0)
                continue;
            if (c > 0)
                out += " & ";
            if (cell == "^" || cell == "<")
            {
                // 组合矩形内部（非起始单元格）：上方 \multirow 会覆盖该单元格，
                // 但 tabular 的列间竖线仍会穿过合并区域；用 \multicolumn{1}
                // 重写本格的列规格，去掉矩形内部的竖线，只保留矩形左右
                // 边界处的竖线，避免竖线出现在合并单元格中间。
                // 注意 \multicolumn{1} 会删除本格列规格自带的竖线：首列删除
                // 表格左侧外框，其余列删除右侧列间竖线。左侧竖线只由表格
                // 第一列（无左邻列）自己补上；其余位置左侧竖线由左邻列右侧
                // 的竖线负责绘制，补上会把同一条线画成双线。右侧竖线只在
                // 右邻列不属于同一合并矩形（或本列已是末列）时补上。
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
                    if (c == 0)
                        mspec += '|';
                    mspec += col_types[c];
                    if (c + 1 == col_count || !right_same_rect)
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
            // 表头（表格第一行）加粗：\luogotablehead 同时加粗文字与公式
            if (r == 0 && !content.empty())
                content = "\\luogotablehead{" + content + "}";
            if (vlen > 1)
                content = "\\multirow{" + std::to_string(vlen) + "}{*}{" +
                          content + "}";
            if (hlen > 1)
            {
                // \multicolumn 的对齐规格只允许一个列类型：取被合并范围内
                // 第一列的对齐方式；保留首列左侧竖线（表格首列时）与合并区域
                // 右侧的竖线，列间竖线按 LaTeX 惯例省略
                std::string mspec;
                if (c == 0)
                    mspec += '|';
                mspec += col_types[c];
                mspec += '|';
                content = "\\multicolumn{" + std::to_string(hlen) + "}{" +
                          mspec + "}{" + content + "}";
            }
            out += content;
        }

        // 行分隔线：跳过合并单元格内部（合并单元格不应被横线穿过）。
        // 无合并的表格保持原来的 \hline 输出
        bool any_blocked = false;
        for (size_t c = 0; c < col_count && !any_blocked; ++c)
        {
            if (boundary_blocked(r, c))
                any_blocked = true;
        }
        if (!any_blocked)
        {
            out += " \\\\\n\\hline\n";
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
    out += "\\end{tabular}\n\n";
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
        tag == "bashrc" || tag == "console")
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
    if (tag == "delphi" || tag == "pascal")
        return "Delphi";
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

// 把超过 limit 字符的行拆成多行：listings 的 breaklines 会先测量整行宽度，
// 超长行（如几千位数字）总宽会超过 TeX 的 \maxdimen（~16383pt），
// 报 "Dimension too large"；插入空格也没用（测量发生在断行之前），
// 只能物理拆行。仅影响极少数病态长行，正常代码/样例不受影响。
// 拆行时沿 UTF-8 字符边界切断：若切点落在多字节字符的续字节上则向后顺延，
// 避免生成非法 UTF-8 字节序列导致 xelatex 编译报错
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

} // namespace

std::string latex::markdown_to_latex(const std::string &markdown)
{
    std::vector<std::string> lines;
    lines = split_lines(markdown);
    lines.emplace_back(); // 末尾哨兵，简化处理

    std::string out;
    std::vector<std::string> env_stack; // 自定义块环境栈（quote/center/flushright）

    char fence = 0;        // 当前代码围栏字符（` 或 ~），0 表示不在代码块内
    size_t fence_len = 0;
    std::string fence_lang; // 围栏语言标记（```cpp 里的 cpp）
    std::vector<std::string> code_lines;

    size_t i = 0;
    while (i < lines.size())
    {
        const std::string raw = lines[i];
        const std::string line = trim(raw);

        // ---- 代码块内部 ----
        if (fence)
        {
            if (line.size() >= fence_len &&
                std::string(line.begin(), line.begin() + fence_len) == std::string(fence_len, fence))
            {
                // lstlisting + breaklines：超长行（如几千个括号）会生成
                // 极宽的 hbox，XeTeX 会直接崩溃；开启自动换行后不再超宽
                const std::string lang = fence_to_listings_lang(fence_lang);
                out += "\\begin{lstlisting}";
                if (!lang.empty())
                    out += "[language=" + lang + ",breaklines=true]";
                else
                    out += "[breaklines=true]";
                out += "\n";
                for (const auto &cl : code_lines)
                    out += split_long_line(cl) + "\n";
                out += "\\end{lstlisting}\n\n";
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

        // ---- 打开代码块（``` 或 ~~~）----
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

        // ---- 块级数学 $$...$$ ----
        if (line.rfind("$$", 0) == 0)
        {
            std::string math = line.substr(2);
            ++i; // 消费当前行（单行公式或起始行）
            // 单行公式：行内还有 $$ 即闭合（允许后面再跟文字，如 $$...$$。）
            const size_t close = math.find("$$");
            if (close != std::string::npos)
            {
                math = math.substr(0, close);
            }
            else
            {
                while (i < lines.size())
                {
                    const std::string l = trim(lines[i]);
                    // % 开头的行是 LaTeX 注释（常用来注释掉 \def 等），直接丢弃；
                    // 否则 \% 转义会让注释内容真正执行
                    if (!l.empty() && l[0] == '%')
                    {
                        ++i;
                        continue;
                    }
                    const size_t lc = l.find("$$");
                    if (lc != std::string::npos)
                    {
                        math += "\n" + l.substr(0, lc);
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

        // ---- Luogu 扩展块：cute-table / align / epigraph / info 等 ----
        if (line.rfind("::cute-table", 0) == 0)
        {
            ++i;
            continue;
        }
        if (line.size() >= 2 && line[0] == ':' && line[1] == ':')
        {
            static const std::regex kCloser(R"(^\s*:+$)", std::regex::icase);
            static const std::regex kCustom(
                R"(^\s*:+\s*(align\{(center|right)\}|epigraph(?:\[[^\]]*\])?|(?:info|success|warning|error)(?:\[[^\]]*\])?(?:\{[^}]*\})?)\s*$)",
                std::regex::icase);
            static const std::regex kTitle(R"(\[([^\]]*)\])");

            std::smatch m;
            if (std::regex_match(line, m, kCloser) && line.size() >= 3)
            {
                if (!env_stack.empty())
                {
                    out += "\\end{" + env_stack.back() + "}\n\n";
                    env_stack.pop_back();
                }
                ++i;
                continue;
            }
            if (std::regex_match(line, m, kCustom))
            {
                const std::string spec = m[1].str();
                std::string title;
                std::smatch tm;
                if (std::regex_search(spec, tm, kTitle))
                    title = tm[1].str();

                std::string env = "quote";
                if (spec.rfind("align{center}", 0) == 0)
                    env = "center";
                else if (spec.rfind("align{right}", 0) == 0)
                    env = "flushright";
                env_stack.push_back(env);
                out += "\\begin{" + env + "}\n";
                if (!title.empty())
                    out += "\\textbf{" + inline_to_latex(title) + "}\\\\\n";
                ++i;
                continue;
            }
        }

        // ---- 标题 ----
        if (line[0] == '#')
        {
            size_t n = 0;
            while (n < line.size() && line[n] == '#')
                ++n;
            if (n == line.size() || line[n] == ' ')
            {
                const std::string title = inline_to_latex(trim(line.substr(n)));
                const bool in_quote_like = (!env_stack.empty() &&
                                            (env_stack.back() == "quote" ||
                                             env_stack.back() == "center" ||
                                             env_stack.back() == "flushright"));
                static const char *kCmds[] = {"section*", "subsection*", "subsubsection*",
                                              "paragraph*", "subparagraph*"};
                if (in_quote_like || n > 5)
                {
                    // 这些 # 标题同样按“正文黑体部分”处理：中文黑体、西文
                    // 与代码块同字体（\luogomarkdownheading）
                    out += "\\textbf{{\\luogomarkdownheading " + title + "}}\n\n";
                }
                else if (n >= 2 && n <= 4)
                {
                    // Markdown 的 ## / ### / #### 对应 LaTeX 的 subsection /
                    // subsubsection / paragraph：默认使用 ctex fontset 预设的
                    // 黑体；\luogomarkdownheading 在用户指定 --set-font-title-*
                    // 时优先切换为用户标题字体，保证该系列参数优先级最高。
                    out += "\\" + std::string(kCmds[n - 1]) +
                           "{{\\luogomarkdownheading " + title + "}}\n\n";
                }
                else
                {
                    out += "\\" + std::string(kCmds[n - 1]) + "{" + title + "}\n\n";
                }
                ++i;
                continue;
            }
        }

        // ---- 分隔线 ----
        {
            static const std::regex kHr(R"(^([-*_])(\s*\1){2,}\s*$)");
            if (std::regex_match(line, kHr))
            {
                out += "\\bigskip\n{\\color{gray} \\hrule}\n\\medskip\n\n";
                ++i;
                continue;
            }
        }

        // ---- 区块引用 ----
        if (line[0] == '>')
        {
            using Part = std::pair<std::string, bool>;
            std::vector<std::vector<Part>> groups; // 空行分段
            std::vector<Part> cur;
            while (i < lines.size())
            {
                const std::string l = trim(lines[i]);
                if (l.empty() || l[0] != '>')
                    break;
                size_t pos = 0;
                while (pos < l.size() && l[pos] == '>')
                    ++pos;
                if (pos < l.size() && l[pos] == ' ')
                    ++pos;
                std::string body = l.substr(pos);
                bool hard = false;
                if (body.size() >= 2 && body.back() == ' ' && body[body.size() - 2] == ' ')
                {
                    hard = true;
                    body = body.substr(0, body.size() - 2);
                }
                else if (!body.empty() && body.back() == ' ')
                {
                    body.pop_back();
                }

                if (trim(body).empty())
                {
                    if (!cur.empty())
                    {
                        groups.push_back(cur);
                        cur.clear();
                    }
                }
                else if (body[0] == '#')
                {
                    if (!cur.empty())
                    {
                        groups.push_back(cur);
                        cur.clear();
                    }
                    size_t n = 0;
                    while (n < body.size() && body[n] == '#')
                        ++n;
                    // 引用块内的 # 标题：中文黑体、西文与代码块同字体
                    groups.push_back({{"\\textbf{{\\luogomarkdownheading " +
                                        inline_to_latex(trim(body.substr(n))) + "}}",
                                        false}});
                }
                else
                {
                    if (!cur.empty() &&
                        has_unclosed_paren_or_bracket(cur.back().first))
                    {
                        // 上一行链接未闭合（如 [![](img)]( 换行 url），并入同一片段
                        cur.back().first += " " + body;
                        cur.back().second = cur.back().second || hard;
                    }
                    else
                    {
                        cur.emplace_back(body, hard);
                    }
                }
                ++i;
            }
            if (!cur.empty())
                groups.push_back(cur);

            out += "\\begin{quote}\n";
            for (const auto &g : groups)
                out += join_inline_parts(g) + "\n\n";
            out += "\\end{quote}\n\n";
            continue;
        }

        // ---- 列表 ----
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
                        // LaTeX 的 itemize/enumerate 最多嵌套 4 层，
                        // 更深的层级归入最内层列表，避免 "Too deeply nested"
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
                }
                while (!stack.empty())
                    close_env();
                out += "\n";
                continue;
            }
        }

        // ---- 表格（支持有无首尾竖线两种写法）----
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
                    emit_table(rows, out);
                else
                    for (const auto &r : rows)
                        out += inline_to_latex(r) + "\n\n";
                continue;
            }
            // 不是表格，落入普通段落处理
        }

        // ---- 普通段落 ----
        std::vector<std::pair<std::string, bool>> parts; // (文本, 是否硬换行)
        size_t collected = 0;
        while (i < lines.size())
        {
            const std::string r = lines[i];
            const std::string t = trim(r);
            // 首行即使像“块语法”也照常当段落收集，保证外层循环总能前进，
            // 避免单反引号、无空格标题等未被块处理器识别的行造成死循环
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
            const std::string para = join_inline_parts(parts);
            if (!para.empty())
                out += para + "\n\n";
            continue;
        }
        // 理论上到不了这里；保险起见直接前进，避免死循环
        ++i;
    }

    // 收尾：关闭未闭合的块环境
    while (!env_stack.empty())
    {
        out += "\\end{" + env_stack.back() + "}\n\n";
        env_stack.pop_back();
    }
    return out;
}

std::string latex::problem_to_latex(const problem::Problem &p, const Options &opt)
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
    // --show-contents-difficulty-tags：目录中的题目标题按难度着色
    // （\luogotocsection 只给写进目录的标题文字上色，正文标题、页眉、
    //   PDF 书签与目录中的引导点/页码都保持黑色）
    if (opt.toc_difficulty)
        out += "\\luogotocsection{" +
               std::string(luogu::difficulty_color(p.difficulty)) + "}{" +
               section_title_latex + "}\n\n";
    else
        out += "\\section{" + section_title_latex + "}\n\n";

    // 标签 / 时空限制
    out += "\\begin{center}\n\\begin{tabularx}{\\textwidth}{XX}\n";
    const auto limits = luogu::format_limits(p.time, p.memory);
    out += "时间限制: " + limits.first + " & 内存限制: " + limits.second + " \\\\\n";
    out += "\\end{tabularx}\n\\end{center}\n";
    // 难度：位于时间/内存限制之下、标签之上（--show-difficulty-tags）。
    // 难度文字的颜色与洛谷网页一致
    if (opt.difficulty)
        out += "\\hspace{5.78pt}难度：\\textcolor[HTML]{" +
               std::string(luogu::difficulty_color(p.difficulty)) + "}{" +
               escape_latex(luogu::difficulty_label(p.difficulty)) + "}\n\n";
    // 算法（type 2）标签默认隐藏，--show-algorithm-tags 时显示，使用蓝色背景
    // rgb(41,73,180)，并排在其他标签之前
    auto tagsalgo = opt.algorithm_tags ? luogu::filter_tags_by_type(p.tags, 2)
                                       : std::vector<std::string>();
    auto tagsfrom = opt.source_tags ? luogu::filter_tags_by_type(p.tags, 3)
                                    : std::vector<std::string>();
    auto tagsdata = opt.source_tags ? luogu::filter_tags_by_type(p.tags, 4)
                                    : std::vector<std::string>();
    auto tagsarea = opt.source_tags ? luogu::filter_tags_by_type(p.tags, 1)
                                    : std::vector<std::string>();
    auto tagsspec = opt.source_tags ? luogu::filter_tags_by_type(p.tags, 5)
                                    : std::vector<std::string>();
    // 算法与来源/时间/区域/特殊标签都不显示时，不输出「标签」一栏
    if(!tagsalgo.empty() || !tagsfrom.empty() || !tagsdata.empty() || !tagsarea.empty() || !tagsspec.empty()) out += "\\hspace{5.78pt}标签：";
    // 标签名来自 tags.json / 缓存，可能含 LaTeX 特殊字符（如 %、#、_），
    // 必须转义后才能放进 \textcolor/\colorbox 参数，否则编译失败或注入宏
    for(auto &tag : tagsalgo) tag = "\\textcolor{white}{\\colorbox[HTML]{2949b4}{\\tagsfonts\\small\\vphantom{涵}" + escape_latex(tag) + "}}";
    for(auto &tag : tagsfrom) tag = "\\textcolor{white}{\\colorbox[HTML]{13c2c2}{\\tagsfonts\\small\\vphantom{涵}" + escape_latex(tag) + "}}";
    for(auto &tag : tagsdata) tag = "\\textcolor{white}{\\colorbox[HTML]{3498db}{\\tagsfonts\\small\\vphantom{涵}" + escape_latex(tag) + "}}";
    for(auto &tag : tagsarea) tag = "\\textcolor{white}{\\colorbox[HTML]{53c41a}{\\tagsfonts\\small\\vphantom{涵}" + escape_latex(tag) + "}}";
    for(auto &tag : tagsspec) tag = "\\textcolor{white}{\\colorbox[HTML]{f39c11}{\\tagsfonts\\small\\vphantom{涵}" + escape_latex(tag) + "}}";
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
               split_long_lines(s.first) +
               "\n\\end{lstlisting}\n\n";
        out += "\\subsubsection*{输出 \\#" + std::to_string(sample_no) + "}\n\n";
        out += "\\begin{lstlisting}\n" +
               split_long_lines(s.second) +
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

bool latex::export_latex(const luogu::ExportFilter &filter,
                         const std::filesystem::path &output_path,
                         std::string &error,
                         const Options &opt)
{
    error.clear();

    // 挂上本次导出的显示选项：行内转换（如 bilibili 链接）据此判断，
    // 函数返回（含提前返回）时自动还原
    OptionsGuard options_guard(&opt);

    // 筛选（-M / -L 共用），结果已按题号排序
    std::vector<problem::Problem> problems;
    std::vector<std::string> resolved_tags;
    if (!luogu::select_problems(filter, problems, &resolved_tags, error))
        return false;

    // 检查图片是否都已下载到缓存；缺失时在终端用中文询问是否下载。
    // 同一 URL 跨题目去重，避免重复下载与并发写同一缓存文件
    std::vector<std::string> missing;
    {
        std::set<std::string> seen_missing;
        std::error_code ec;
        for (const auto &p : problems)
        {
            for (const auto &url : p.image_urls())
            {
                // 视频等非图片链接不算“未下载的图片”
                if (!looks_like_url(url) || is_video_url(url))
                    continue;
                if (seen_missing.count(url))
                    continue;
                if (!std::filesystem::exists(crawler::image_cache_path(url), ec) || ec)
                {
                    seen_missing.insert(url);
                    missing.push_back(url);
                }
            }
        }
    }
    if (!missing.empty())
    {
        std::printf("筛选出的题目共引用了 %zu 张尚未下载的图片。\n现在下载吗？[y/N] ", missing.size());
        fflush(stdout);
        char answer_buf[16];
        if (!fgets(answer_buf, sizeof(answer_buf), stdin))
            answer_buf[0] = '\0';
        std::string answer(answer_buf);
        if (!answer.empty() && (answer[0] == 'y' || answer[0] == 'Y'))
        {
            const crawler::derror download_result = crawler::download_images(missing);
            if (download_result != crawler::SUCCESS)
            {
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

    // 逐题渲染用的显示选项：语言以筛选参数为准，其余（标签/难度/目录着色
    // 等显示开关）沿用本次导出的设置
    Options opt_lang = opt;
    opt_lang.lang = filter.lang;
    // opt_lang.show 保持默认 "00"：-L 不再支持 --show，默认不显示难度和标签

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

    // openany：章节可在任意页开始，避免封面后的空页（book 默认章节
    // 从奇数页开始，\maketitle 之后紧跟 \chapter* 会留出一张空白页）
    std::fputs("\\documentclass[openany]{book}\n", out);
    // ctex fontset：Windows/macOS 在编译本程序时确定；Linux 在运行
    // 阶段解析 /etc/os-release，Ubuntu 系列用 ubuntu，其他发行版用 fandol。
    const std::string ctex_options = latex::ctex_package_options();
    std::fprintf(out, "\\usepackage%s{ctex}\n", ctex_options.c_str());
    std::fputs("\\usepackage{graphicx}\n", out);
    // 图片统一缩放：测量自然宽高，只在超过行宽/版心高时按比例缩小；
    // 小图片保持原始尺寸，不放大。max width 与 max height 同时给出并由
    // keepaspectratio 保证宽高比不变，避免超高或超宽图片溢出页面。
    std::fputs("\\newcommand{\\luogoincludegraphics}[1]{%\n", out);
    std::fputs("  \\setbox0=\\hbox{\\includegraphics{#1}}%\n", out);
    std::fputs("  \\ifdim\\wd0>\\linewidth\n", out);
    std::fputs("    \\includegraphics[width=\\linewidth,height=\\textheight,keepaspectratio]{#1}%\n", out);
    std::fputs("  \\else\n", out);
    std::fputs("    \\ifdim\\dimexpr\\ht0+\\dp0\\relax>\\textheight\n", out);
    std::fputs("      \\includegraphics[width=\\linewidth,height=\\textheight,keepaspectratio]{#1}%\n", out);
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
    std::fputs("\\usepackage[normalem]{ulem}\n", out);
    std::fputs("\\usepackage{amsmath}\n", out);
    std::fputs("\\usepackage{mathtools}\n", out);
    // unicode-math：数学字体全部改为可无限缩放的 OpenType 数学字体，
    // 修复 mathrsfs（RSFS 字体只有固定字号）导致 \mathscr 字号被替换的问题。
    // 兼容性：必须加载在 amsmath / mathtools 之后；不再加载 amssymb、
    // mathrsfs（其符号与 \mathscr 由 unicode-math 提供）与 bm（bm 与
    // unicode-math 不兼容，会报 Extended mathchar）；\bm / \boldsymbol
    // 用 unicode-math 的粗斜体数学字母表 \symbfit 兼容替代。
    std::fputs("\\usepackage{unicode-math}\n", out);
    // 显式选择随 TeX Live / MacTeX / MiKTeX 分发的 OpenType 数学字体，
    // 避免不同平台上 unicode-math 默认数学字体不一致。
    std::fputs("\\setmathfont{Latin Modern Math}\n", out);
    // 表头加粗用的粗体数学版本：Latin Modern Math 本身没有粗体字形，
    // 用 fontspec 的 FakeBold（XeTeX 的 embolden）合成粗体，
    // 使表头里的公式（如 $n\\leq$）与文字一并加粗。
    std::fputs("\\setmathfont[version=bold, FakeBold=2]{Latin Modern Math}\n", out);
    // 表格表头：\textbf 加粗文字，\boldmath 切换上面定义的粗体数学版本
    std::fputs("\\newcommand{\\luogotablehead}[1]{\\textbf{\\boldmath #1}}\n", out);
    std::fputs("\\newcommand{\\bm}{\\symbfit}\n", out);
    std::fputs("\\renewcommand{\\boldsymbol}{\\symbfit}\n", out);
    std::fputs("\\usepackage{xcolor}\n", out);
    std::fputs("\\usepackage{listings}\n", out);
    std::fputs("\\usepackage{cancel}\n", out);
    std::fputs("\\usepackage{geometry}\n", out);
    std::fputs("\\usepackage{tabularx}\n", out);
    // 表格合并（洛谷的 ^ 向上合并 / < 向左合并）需要 \multirow
    std::fputs("\\usepackage{multirow}\n", out);
    std::fputs("\\geometry{margin=2cm}\n", out);
    // book 默认 \headheight=12pt 略小于 ctex/unicode-math 标题所需的
    // 12.03pt；显式给到 13pt，消除每一页的 fancyhdr 警告，正文版心基本不变。
    std::fputs("\\setlength{\\headheight}{13pt}\n", out);
    // 去掉所有章节序号（\section 等）：目录和正文都不显示数字
    std::fputs("\\setcounter{secnumdepth}{-1}\n", out);

    // 页眉：--toc-backlinks 时页码为跳回目录页的超链接（默认页码为普通文本）
    const std::string page_in_head =
        opt.toc_backlinks ? "\\hyperlink{luogotoc}{\\thepage}" : "\\thepage";
    // 标题字体（--set-font-title-zh-CN / --set-font-title-en-US）同样作用于
    // 页眉处的题目标题；未指定时保持普通正文样式。
    std::string head_fonts = "\\normalfont";
    if (!opt.font_title_zh.empty())
        head_fonts += " \\luogotitlezh";
    if (!opt.font_title_en.empty())
        head_fonts += " \\luogotitleen";

    std::fputs("\\pagestyle{fancy}\n", out);
    std::fputs("\\fancyhf{}\n", out);
    std::fprintf(out, "\\fancyhead[LE]{%s}\n", page_in_head.c_str());
    std::fprintf(out, "\\fancyhead[RE]{\\nouppercase{%s \\rightmark}}\n", head_fonts.c_str());
    std::fprintf(out, "\\fancyhead[LO]{\\nouppercase{%s \\rightmark}}\n", head_fonts.c_str());
    std::fprintf(out, "\\fancyhead[RO]{%s}\n", page_in_head.c_str());

    // 标题字体分两套：
    // 1) 题目大标题（\section）：中文跟随 --set-font-title-zh-CN；未指定时
    //    保持普通正文 CJK 字体。西文直接跟随正文主字体，因此
    //    --set-font-body-en-US 通过 \setmainfont 自动生效；不使用黑体。
    // 2) 小节标题（\subsection / \subsubsection，对应固定小节和 Markdown
    //    的 ## / ###）：中文默认使用 ctex 预设黑体 \heiti；西文默认使用
    //    代码块字体 \luogoheadinglatin（--set-font-body-codes 或
    //    Consolas -> Menlo -> DejaVu Sans Mono 回退链）。用户指定标题
    //    中西文字体时优先使用 --set-font-title-zh-CN / -en-US。
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

    // --show-contents-difficulty-tags 用的 \luogotocsection{<HTML 颜色>}{<标题>}：
    // 相当于 \section，但把写进 .toc 的目录项文字包进 \textcolor，使目录里的
    // 题目标题按难度着色。只替换本次调用中的 \addcontentsline（hyperref 写入
    // 的目录超链接锚点因此照常保留），且只影响目录项中的标题文字：引导点、页码、
    // 正文标题、页眉与 PDF 书签都保持原样。未启用该参数时不写入这段定义，
    // 生成的文档与启用前完全一致。
    if (opt.toc_difficulty)
    {
        std::fputs("\\newcommand{\\luogotocsection}[2]{%\n", out);
        std::fputs("  \\begingroup\n", out);
        std::fputs("    \\let\\luogotocaddcontentsline\\addcontentsline\n", out);
        std::fputs("    \\renewcommand{\\addcontentsline}[3]{%\n", out);
        std::fputs("      \\luogotocaddcontentsline{##1}{##2}{\\textcolor[HTML]{#1}{##3}}}%\n", out);
        std::fputs("    \\section{#2}%\n", out);
        std::fputs("  \\endgroup}\n", out);
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
    std::fputs("}\n", out);

        // 当前 TeX Live 的 listings 没有这些语言，手动补上，否则
        // \begin{lstlisting}[language=Rust] 会报 "Couldn't load requested language"
    std::fputs("\\lstdefinelanguage{Rust}{\n", out);
    std::fputs("    morekeywords={as,async,await,break,const,continue,crate,dyn,else,enum,", out);
    std::fputs("extern,false,fn,for,if,impl,in,let,loop,match,mod,move,mut,pub,ref,return,", out);
    std::fputs("self,Self,static,struct,super,trait,true,type,unsafe,use,where,while,yield},\n", out);
    std::fputs("  morecomment=[l]{//},\n", out);
    std::fputs("  morecomment=[s]{/*}{*/},\n", out);
    std::fputs("  morestring=[b]\",\n", out);
    std::fputs("  morestring=[b]',\n", out);
    std::fputs("}\n", out);
    std::fputs("\\lstdefinelanguage{JavaScript}{\n", out);
    std::fputs("  morekeywords={abstract,arguments,await,boolean,break,byte,case,catch,char,", out);
    std::fputs("class,const,continue,debugger,default,delete,do,double,else,enum,eval,export,", out);
    std::fputs("extends,false,final,finally,float,for,function,goto,if,implements,import,in,", out);
    std::fputs("instanceof,int,interface,let,long,native,new,null,package,private,protected,", out);
    std::fputs("public,return,short,static,super,switch,synchronized,this,throw,throws,", out);
    std::fputs("transient,true,try,typeof,var,void,volatile,while,with,yield},\n", out);
    std::fputs("  morecomment=[l]{//},\n", out);
    std::fputs("  morecomment=[s]{/*}{*/},\n", out);
    std::fputs("  morestring=[b]\",\n", out);
    std::fputs("  morestring=[b]',\n", out);
    std::fputs("  morestring=[b]`,\n", out);
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
    // amssymb 的 \circledR / \circledS 未被 unicode-math 收录，而本模板不加载
    // amssymb；洛谷题面（KaTeX 支持这两个命令）用到时补齐为等价符号，
    // 避免 Undefined control sequence。
    std::fputs("\\providecommand{\\circledR}{\\text{\\textregistered}}\n", out);
    std::fputs("\\providecommand{\\circledS}{\\text{\\textcircled{S}}}\n", out);
    // 部分洛谷题面使用 \bold2 / \bold{x} 表示粗体数学字符；LaTeX 标准
    // 没有 \bold，补齐为 \mathbf 别名，避免编译时 Undefined control sequence。
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
    std::fputs("\\lstdefinelanguage{JavaScript}{\n", out);
    std::fputs("keywords={break, case, catch, class, const, continue, debugger, default, delete, do, else, export, extends, finally, for, function, if, import, in, instanceof, let, new, return, super, switch, this, throw, try, typeof, var, void, while, with, yield, await, async, of, from, as},", out);
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
    std::fputs("morestring=[b]\"\n", out);
    std::fputs("}\n", out);
    // 封面标题：--set-cover-title 指定文字，--set-font-cover-page 指定字体
    // （未设置字体时保持原代码行为：不额外指定字体族）；
    // 作者处的项目名带指向项目仓库的超链接（hyperref 已在上方加载）
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

    // 字体设置已集中到 latex_fonts.cpp：ctex fontset 预设负责默认中西文
    // 正文/标题/代码 CJK 字体；本函数只负责在用户指定 --set-font-* 时覆盖，
    // 以及为代码块西文设置 Consolas -> Menlo -> DejaVu Sans Mono 回退链。
    latex::write_font_setup(out, opt);

    std::fputs("\\begin{document}\n\n", out);
    std::fputs("\\maketitle\n", out);

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

    // 目录统一写成“章标题 + \@starttoc”（与 \tableofcontents 等价，
    // 目录条目是否带超链接由 hyperref 的 linktoc 选项控制，对应
    // --no-toc-links），并在目录标题处放置：
    // - \hypertarget{luogotoc}：页眉页码（--toc-backlinks）跳回目录页的
    //   目标锚点。\hypertarget 直接生成命名目标，不依赖 .aux 中的 label
    //   记录，点击即可跳到目录页顶端；
    // - \pdfbookmark：PDF 书签中始终保留“目录”条目（book 类经 ctex 的
    //   \tableofcontents 不会自动写目录书签），与题目的 \section 自动生成
    //   的书签并存，两种导出模式（有无 --toc-backlinks）行为一致。
    std::fputs((toc_open + "\n").c_str(), out);
    std::fputs("\\chapter*{\\contentsname}\n", out);
    std::fputs("\\hypertarget{luogotoc}{}\n", out);
    std::fputs("\\pdfbookmark[0]{\\contentsname}{toc}\n", out);
    std::fputs("\\makeatletter\n", out);
    std::fputs("\\@starttoc{toc}\n", out);
    std::fputs("\\makeatother\n", out);
    std::fputs((toc_close + "\n").c_str(), out);
    std::fputs("\\newpage\n", out);

    std::fputs("\n\n", out);
    int cnt = 0;
    const int total = static_cast<int>(problems.size());
    for (const auto &p : problems)
    {
        // 题目内容按字节数写出：内容含控制字符（缓存被篡改时）也不会被
        // C 字符串终止符静默截断
        const std::string body = problem_to_latex(p, opt_lang);
        if (std::fwrite(body.data(), 1, body.size(), out) != body.size())
        {
            std::fclose(out);
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
            return false;
        }
        std::fputs("\n", out);
        ++cnt;
        // total == 0（筛选结果为空）时不做百分比计算，避免整数除零崩溃
        if (total > 0)
        {
            std::printf("\r正在导出：%3d %% (%d/%d)。 ", cnt * 100 / total, cnt, total);
            fflush(stdout);
        }
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
    // 落盘并 fsync 后原子替换目标文件；失败时清理临时文件
    if (!luogu::compat::flush_and_sync(out) || std::fclose(out) != 0)
    {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        error = "写入输出文件 '" + luogu::compat::path_to_utf8(output_path) + "' 失败";
        return false;
    }
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
