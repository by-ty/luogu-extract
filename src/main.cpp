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

// src/main.cpp
// Windows（MSVC / MinGW-w64）不保证提供 POSIX getopt/getopt_long，
// 全平台统一使用自带实现（语义与 GNU getopt 一致：长选项、无歧义前缀缩写、
// ':' 前缀 optstring、参数重排等，与 glibc 的行为做过逐用例比对）。
// LUOGU_FORCE_COMPAT_GETOPT 用于在非 Windows 平台强制测试该实现。
#if defined(LUOGU_FORCE_COMPAT_GETOPT) || defined(_WIN32)
#include "luogu-extract/util/getopt_compat.h"
#else
#include <getopt.h>
#endif
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/export/common.h"
#include "luogu-extract/export/latex.h"
#include "luogu-extract/export/markdown.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/tag_cache.h"
#include "luogu-extract/util/version.h"

namespace
{

// 程序运行参数。以后新增参数时在这里添加字段。
struct Options
{
    bool update = false;    // -U, --update
    bool markdown = false;  // -M, --markdown
    bool latex = false;     // -L, --latex
    bool list_tags = false; // --tags
    bool help = false;      // -h, --help
    bool version = false;   // -V, --version
    bool show_explicit = false; // 是否显式给了 --show
    std::string output;     // --output（空则按模式取默认 problems.md / problems.tex）

    // ---- 导出设置参数 ----
    bool no_toc_links = false;      // --no-toc-links：目录条目不带跳转超链接（仅 -L）
    bool toc_backlinks = false;     // --toc-backlinks：页码为跳回目录的超链接（仅 -L）
    bool no_bilibili_link = false;  // --no-bilibili-link：bilibili URL 输出为普通文本（仅 -L）
    std::string font_cover;         // --set-font-cover-page（仅 -L）
    std::string font_body_zh;       // --set-font-body-zh-CN（仅 -L）
    std::string font_body_en;       // --set-font-body-en-US（仅 -L）
    std::string font_body_codes;    // --set-font-body-codes（仅 -L）
    std::string font_title_zh;      // --set-font-title-zh-CN（仅 -L）
    std::string font_title_en;      // --set-font-title-en-US（仅 -L）
    std::string cover_title;        // --set-cover-title（-L / -M 均支持）

    luogu::ExportFilter filter; // -M / -L 共用的筛选条件
};

// 只作为长选项使用的选项码（getopt_long 返回值）
enum
{
    OPT_TAG = 1000,
    OPT_DIFFICULTY,
    OPT_OUTPUT,
    OPT_TYPE,
    OPT_LANG,
    OPT_SHOW,
    OPT_TAGS,
    OPT_NO_TOC_LINKS,
    OPT_TOC_BACKLINKS,
    OPT_FONT_COVER,
    OPT_FONT_BODY_ZH,
    OPT_FONT_BODY_EN,
    OPT_FONT_BODY_CODES,
    OPT_FONT_TITLE_ZH,
    OPT_FONT_TITLE_EN,
    OPT_NO_BILIBILI_LINK,
    OPT_COVER_TITLE,
    OPT_PID,
    OPT_PID_RANGE,
};

// 选项码 → 长选项名（用于报错信息）
inline const char *option_name_for(int code)
{
    switch (code)
    {
    case OPT_TAG: return "--tag";
    case OPT_DIFFICULTY: return "--difficulty";
    case OPT_OUTPUT: return "--output";
    case OPT_TYPE: return "--type";
    case OPT_LANG: return "--lang";
    case OPT_SHOW: return "--show";
    case OPT_FONT_COVER: return "--set-font-cover-page";
    case OPT_FONT_BODY_ZH: return "--set-font-body-zh-CN";
    case OPT_FONT_BODY_EN: return "--set-font-body-en-US";
    case OPT_FONT_BODY_CODES: return "--set-font-body-codes";
    case OPT_FONT_TITLE_ZH: return "--set-font-title-zh-CN";
    case OPT_FONT_TITLE_EN: return "--set-font-title-en-US";
    case OPT_COVER_TITLE: return "--set-cover-title";
    default: return "";
    }
}

// 选项缺失参数值时，给出“需要什么参数”的说明（用于中文报错）
inline std::string option_argument_hint(const std::string &token)
{
    if (token == "--tag") return "标签名称或数字 ID";
    if (token == "--difficulty") return "难度（0-8，或区间 1-4）";
    if (token == "--output") return "输出文件路径";
    if (token == "--type") return "题目类型（B 或 P）";
    if (token == "--lang") return "题面语言（zh-CN 或 en）";
    if (token == "--show") return "两位显示开关（如 11）";
    if (token == "--set-font-cover-page") return "字体名称或字体文件地址";
    if (token == "--set-font-body-zh-CN") return "字体名称或字体文件地址";
    if (token == "--set-font-body-en-US") return "字体名称或字体文件地址";
    if (token == "--set-font-body-codes") return "字体名称或字体文件地址";
    if (token == "--set-font-title-zh-CN") return "字体名称或字体文件地址";
    if (token == "--set-font-title-en-US") return "字体名称或字体文件地址";
    if (token == "--set-cover-title") return "封面标题";
    if (token == "--pid") return "题号（如 P1001，可多个，空格分隔或重复 --pid）";
    if (token == "--pid-range") return "题号范围（如 P1001-P1010，可多组，空格分隔或重复 --pid-range）";
    return "";
}

// -h, --help 的帮助信息。文本与 README.md「参数说明」表格保持一致，
// 新增参数时请同步更新 README.md。
const char *kUsage =
    "用法：luogu-extract [选项]\n"
    "\n"
    "选项：\n"
    "  -U, --update          更新题目列表缓存（latest.ndjson）与标签缓存（tags.json）\n"
    "  -M, --markdown        筛选并导出 Markdown（默认输出 problems.md）\n"
    "  -L, --latex           筛选并导出 LaTeX（默认输出 problems.tex）\n"
    "                        （-M 与 -L 不能同时使用）\n"
    "      --tags            按官方分类打印标签 ID 对照表（可与 -h 组合使用）\n"
    "      --tag <name|ID>...\n"
    "                        按标签筛选；多个值可用空格分隔或重复 --tag，题目须包含全部标签；\n"
    "                        引号整体恰好等于已知标签名（如 \"NOIP 普及组\"）时按一个标签处理\n"
    "      --difficulty <spec>\n"
    "                        按难度（0~8）筛选；支持区间写法（如 1-4），多组值可用空格\n"
    "                        分隔或重复 --difficulty\n"
    "      --type <B|P>      按题目类型筛选（可重复，空表示全部类型）\n"
    "      --pid <pid>...    按题号精确筛选；多个值可用空格分隔或重复 --pid。\n"
    "                        不能与 --tag、--difficulty、--type 同时使用，\n"
    "                        且每个题号必须存在于题目列表缓存中\n"
    "      --pid-range <a>-<b>\n"
    "                        按题号闭区间筛选；多组值可用空格分隔或重复 --pid-range。\n"
    "                        一组范围两端必须为同一题库（如都为 P 题库或都为 B 题库，\n"
    "                        多组范围间可不为同一题库），且两端点均须存在于缓存中。\n"
    "                        可与 --tag、--difficulty、--type 同时使用\n"
    "      --lang <zh-CN|en> 题面语言（默认 zh-CN；en 缺失时回退中文）\n"
    "      --show <NN>       仅 -M 有效：第 1 位=是否显示难度，第 2 位=是否显示标签\n"
    "                        （默认 11）；隐藏标签仅隐藏「算法」类标签，其他类型始终显示\n"
    "      --output <file>   输出文件路径（默认 problems.md / problems.tex）\n"
    "\n"
    "LaTeX 排版选项（仅在使用 -L 时有效）：\n"
    "      --no-toc-links    目录条目不带跳转到对应题目页的超链接（默认带超链接）\n"
    "      --toc-backlinks   每页页眉处的页码为跳回目录页的超链接（默认无超链接）\n"
    "      --set-font-cover-page <font>\n"
    "                        设置封面标题字体；<font> 为系统已安装的字体名称或字体文件地址\n"
    "      --set-font-body-zh-CN <font>\n"
    "                        设置题面正文中文字符的字体（名称或字体文件地址）\n"
    "      --set-font-body-en-US <font>\n"
    "                        设置题面正文及题目大标题中的西文字符字体\n"
    "                        （名称或字体文件地址；不作用于公式）\n"
    "      --set-font-body-codes <font>\n"
    "                        设置代码块西文，以及正文黑体部分西文的字体（名称或字体文件地址；\n"
    "                        默认按 Consolas → Menlo → DejaVu Sans Mono 回退）\n"
    "      --set-font-title-zh-CN <font>\n"
    "                        设置题目大标题、小节标题、目录页标题与每页页眉标题中的中文字体\n"
    "                        （名称或字体文件地址）\n"
    "      --set-font-title-en-US <font>\n"
    "                        设置小节标题、目录页标题与每页页眉标题中的西文字体；题目大标题\n"
    "                        西文跟随 --set-font-body-en-US（名称或字体文件地址）\n"
    "      --no-bilibili-link\n"
    "                        bilibili 视频 URL 输出为普通文本而非超链接（默认超链接）\n"
    "      --set-cover-title <title>\n"
    "                        设置封面标题（-L，默认 luogu extract）或 Markdown 一级标题\n"
    "                        （-M，默认 洛谷题目导出）\n"
    "  -h, --help            显示帮助\n"
    "  -V, --version         显示项目简介、版本号、版权声明与项目仓库链接\n"
    "                        （不能与其他参数同时使用）\n";

void printUsage()
{
    std::printf("%s", kUsage);
}

// -V, --version：项目简介 + 版本号 + 版权声明 + 项目仓库链接
void printVersion()
{
    std::printf("%s %s\n", LUOGU_EXTRACT_PROJECT_NAME, LUOGU_EXTRACT_VERSION);
    std::printf("抓取洛谷题目列表与标签，按标签、难度、类型、题号筛选后，\n"
                "导出为 Markdown 或 LaTeX 文档的命令行工具。\n\n");
    std::printf("%s\n", LUOGU_EXTRACT_COPYRIGHT);
    std::printf("以 %s 许可证发布，本程序不提供任何担保。\n",
                LUOGU_EXTRACT_LICENSE_NAME);
    std::printf("项目仓库：%s\n", LUOGU_EXTRACT_REPOSITORY_URL);
}

inline void printError(const std::string &message)
{
    std::fprintf(stderr, "\033[1;31m错误：\033[0m%s\n", message.c_str());
}

inline void printSuccess(const std::string &message)
{
    std::printf("\033[1;32m%s\033[0m\n", message.c_str());
}

inline std::string to_lower_ascii(const std::string &s)
{
    std::string out = s;
    for (auto &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline std::string to_upper_ascii(const std::string &s)
{
    std::string out = s;
    for (auto &c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// 按空白把字符串拆成多个 token（用于 --tag 模拟 贪心 这类写法）
inline std::vector<std::string> split_whitespace(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        const size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

// 校验字体参数（--set-font-*）的值，并区分两种写法：
// - 系统已安装的字体名称：直接透传给 fontspec；
// - 字体文件地址：含路径分隔符、以常见字体扩展名结尾，或当前目录下存在
//   同名文件时按文件处理。文件地址必须真实存在，否则报错拒绝执行；
//   存在时转成绝对路径并把 '\' 归一化为 '/'，便于写入 LaTeX 代码。
// 返回错误信息（空串表示合法）；合法时把规范化结果写入 font_out。
// 识别字体文件的实际格式（按文件头魔数），返回应使用的扩展名（含点号）；
// 无法识别时返回空串。用于给无扩展名的字体文件补全扩展名。
inline std::string detect_font_extension(const std::filesystem::path &path)
{
    FILE *f = luogu::compat::fopen(path, "rb");
    if (!f)
        return "";
    unsigned char head[4] = {0};
    const size_t n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    if (n < 4)
        return "";
    // TrueType：00 01 00 00 / 'true' / 'typ1'
    if ((head[0] == 0x00 && head[1] == 0x01 && head[2] == 0x00 && head[3] == 0x00) ||
        std::memcmp(head, "true", 4) == 0 ||
        std::memcmp(head, "typ1", 4) == 0)
        return ".ttf";
    // OpenType（CFF）：'OTTO'
    if (std::memcmp(head, "OTTO", 4) == 0)
        return ".otf";
    // TrueType Collection：'ttcf'
    if (std::memcmp(head, "ttcf", 4) == 0)
        return ".ttc";
    return "";
}

inline std::string validate_font_option(const std::string &option_name,
                                        const std::string &value,
                                        std::string &font_out)
{
    if (value.empty() || value[0] == '-')
        return "参数 '" + option_name + "' 后缺少字体名称或字体文件地址；正确用法：" +
               option_name + " <字体名称或字体文件地址>";

    const bool has_separator = value.find('/') != std::string::npos ||
                               value.find('\\') != std::string::npos;
    const std::string lower = to_lower_ascii(value);
    static const char *kFontExts[] = {".ttf", ".otf", ".ttc", ".dfont",
                                      ".pfb", ".woff", ".woff2"};
    bool has_ext = false;
    for (const char *e : kFontExts)
    {
        const size_t n = std::strlen(e);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0)
        {
            has_ext = true;
            break;
        }
    }

    std::error_code ec;
    const bool file_exists_now =
        std::filesystem::exists(luogu::compat::path_from_utf8(value), ec) && !ec;

    // 不含路径特征且不是现存文件 → 按系统已安装的字体名称处理
    if (!has_separator && !has_ext && !file_exists_now)
    {
        font_out = value;
        return "";
    }

    // 按字体文件地址处理：文件必须存在，否则拒绝执行
    std::filesystem::path p =
        std::filesystem::absolute(luogu::compat::path_from_utf8(value), ec);
    if (ec || !std::filesystem::exists(p, ec))
        return "参数 '" + option_name + "' 指定的字体文件 '" + value +
               "' 不存在；请检查文件路径，或改用系统已安装的字体名称";

    // 无扩展名的字体文件：fontspec 无法加载，按文件头识别格式后复制到
    // 缓存目录并补上扩展名，生成的 LaTeX 引用副本
    if (!p.has_extension())
    {
        const std::string ext = detect_font_extension(p);
        if (ext.empty())
            return "参数 '" + option_name + "' 指定的字体文件 '" + value +
                   "' 无法识别其字体格式；请使用 .ttf / .otf / .ttc 格式的字体文件，"
                   "或改用系统已安装的字体名称";
        const std::filesystem::path cache_fonts = crawler::get_cache_dir() / "fonts";
        std::error_code ec2;
        if (!std::filesystem::exists(cache_fonts, ec2) &&
            !std::filesystem::create_directories(cache_fonts, ec2))
        {
            return "无法创建字体缓存目录 '" + luogu::compat::path_to_utf8(cache_fonts) + "': " + ec2.message();
        }
        std::filesystem::path target = cache_fonts / p.filename();
        target += ext;
        for (int i = 1; std::filesystem::exists(target, ec2); ++i)
        {
            target = cache_fonts / p.filename();
            target += "_" + std::to_string(i) + ext;
        }
        std::filesystem::copy_file(p, target, std::filesystem::copy_options::none, ec2);
        if (ec2)
            return "无法把字体文件复制到缓存目录: " + ec2.message();
        p = std::filesystem::absolute(target, ec2);
    }

    // 输出 UTF-8 路径（Windows 下 string() 按 ANSI 代码页解释，会乱码），
    // 并把 '\' 归一化为 '/'，便于写入 LaTeX 代码
    std::string spec = luogu::compat::path_to_utf8(p);
    std::replace(spec.begin(), spec.end(), '\\', '/');
    font_out = std::move(spec);
    return "";
}

// 解析难度规格："N"（单个数字）或 "A-B"（闭区间），展开后追加到 difficulties
inline bool parse_difficulty_spec(const std::string &spec, std::vector<int> &difficulties)
{
    auto parse_num = [](const std::string &s, long &out) -> bool {
        if (s.empty())
            return false;
        char *end = nullptr;
        out = std::strtol(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || out < 0 || out > 8)
            return false;
        return true;
    };

    const size_t dash = spec.find('-');
    if (dash == std::string::npos)
    {
        long v = 0;
        if (!parse_num(spec, v))
            return false;
        difficulties.push_back(static_cast<int>(v));
        return true;
    }

    // 区间 A-B（不允许再出现第二个 '-'，如 "1-2-3"）
    const std::string a = spec.substr(0, dash);
    const std::string b = spec.substr(dash + 1);
    if (b.find('-') != std::string::npos)
        return false;

    long lo = 0, hi = 0;
    if (!parse_num(a, lo) || !parse_num(b, hi))
        return false;
    if (lo > hi)
        return false;
    for (long v = lo; v <= hi; ++v)
        difficulties.push_back(static_cast<int>(v));
    return true;
}

// 解析一组 --pid-range 规格 "<题号>-<题号>"（两端点均包含）。
// 成功时把规范化（大写）的两端点写入 out 并返回 true；
// 失败时返回 false，并把中文错误信息（含相关要求）写入 err。
inline bool parse_pid_range_arg(const std::string &spec,
                                std::pair<std::string, std::string> &out,
                                std::string &err)
{
    const size_t dash = spec.find('-');
    if (dash == std::string::npos)
    {
        err = "参数错误：'--pid-range' 的值 '" + spec +
              "' 缺少 '-'；正确用法：--pid-range <题号>-<题号>"
              "（如 P1001-P1010，两端点均包含）";
        return false;
    }
    const std::string a = spec.substr(0, dash);
    const std::string b = spec.substr(dash + 1);
    if (a.empty() || b.empty() || b.find('-') != std::string::npos)
    {
        err = "参数错误：'--pid-range' 的值 '" + spec +
              "' 不是合法的题号范围；正确用法：--pid-range <题号>-<题号>"
              "（如 P1001-P1010，两端点均包含，且只含一个 '-'）";
        return false;
    }

    std::string a_prefix, a_suffix, b_prefix, b_suffix;
    unsigned long long a_num = 0, b_num = 0;
    if (!luogu::parse_pid_parts(a, a_prefix, a_num, a_suffix))
    {
        err = "参数错误：'--pid-range' 的端点 '" + a +
              "' 不是合法题号（应为 1 个或多个字母 + 数字，如 P1001）";
        return false;
    }
    if (!luogu::parse_pid_parts(b, b_prefix, b_num, b_suffix))
    {
        err = "参数错误：'--pid-range' 的端点 '" + b +
              "' 不是合法题号（应为 1 个或多个字母 + 数字，如 P1010）";
        return false;
    }
    if (a_prefix != b_prefix)
    {
        err = "参数错误：'--pid-range' 的一组范围两端必须为同一题库的题目"
              "（例如 P1001-P1010 或 B2000-B2010）；'" + spec +
              "' 跨越了不同题库（" + a_prefix + " 题库与 " + b_prefix +
              " 题库），不同题库请分成多组范围分别传入";
        return false;
    }
    if (luogu::compare_pid_parts(a_num, a_suffix, b_num, b_suffix) > 0)
    {
        err = "参数错误：'--pid-range' 的范围左端点不能大于右端点（'" + spec +
              "'）；正确用法：--pid-range <题号>-<题号>，两端点均包含且"
              "左端点不超过右端点";
        return false;
    }
    out = {to_upper_ascii(a), to_upper_ascii(b)};
    return true;
}

// --tags：按官方分类（type）打印标签 ID 对照表
inline bool print_tag_list()
{
    if (!tagcache::shared_cache_loaded())
    {
        printError("找不到标签缓存 tags.json，请先运行 -U 更新缓存");
        return false;
    }
    const tagcache::Cache &cache = tagcache::shared_cache();

    static const char *kTypeLabels[] = {
        "未知",         // 0
        "地区/赛区",    // 1
        "算法与技巧",   // 2
        "竞赛来源",     // 3
        "年份",         // 4
        "特殊题目属性", // 5
        "旧版标签",     // 6
    };
    const int kKnownTypes = 7;

    std::vector<std::vector<int>> groups(kKnownTypes + 1);
    for (const auto &kv : cache.id_to_name)
    {
        int type = 0;
        auto it = cache.name_to_type.find(kv.second);
        if (it != cache.name_to_type.end())
            type = it->second;
        if (type < 0 || type >= kKnownTypes)
            type = kKnownTypes;
        groups[type].push_back(kv.first);
    }

    std::printf("洛谷标签 ID 对照表（共 %zu 个）\n\n", cache.id_to_name.size());
    for (int g = 0; g <= kKnownTypes; ++g)
    {
        if (groups[g].empty())
            continue;
        std::sort(groups[g].begin(), groups[g].end());
        const char *label = (g == kKnownTypes) ? "其他" : kTypeLabels[g];
        std::printf("【%s】（分类 %d）%zu 个\n", label, g, groups[g].size());
        for (int id : groups[g])
            std::printf("  %-6d %s\n", id, cache.id_to_name.at(id).c_str());
        std::printf("\n");
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    // Windows 传统控制台启用 ANSI 转义解析与 UTF-8 代码页
    // （彩色/进度输出不乱码）；其他平台为空操作
    luogu::compat::init_console();

    // Windows 下 CRT 的 main(char**) 参数按 ANSI 代码页转换而非 UTF-8，
    // 统一转换为 UTF-8 后构造 getopt 可用的参数表（中文参数不乱码）；
    // 其他平台等价于原 argv 的副本。
    std::vector<std::string> args_utf8 = luogu::compat::get_argv_utf8(argc, argv);
    std::vector<char *> args;
    args.reserve(args_utf8.size());
    for (auto &a : args_utf8)
        args.push_back(const_cast<char *>(a.c_str()));
    const int arg_count = static_cast<int>(args.size());
    char **const arg_vector = args.data();

    // 长选项表：以后新增参数时在这里加一项，并在下方 switch 中处理。
    static const struct option kLongOptions[] = {
        {"update",     no_argument,       nullptr, 'U'},
        {"markdown",   no_argument,       nullptr, 'M'},
        {"latex",      no_argument,       nullptr, 'L'},
        {"tag",        required_argument, nullptr, OPT_TAG},
        {"difficulty", required_argument, nullptr, OPT_DIFFICULTY},
        {"output",     required_argument, nullptr, OPT_OUTPUT},
        {"type",       required_argument, nullptr, OPT_TYPE},
        {"lang",       required_argument, nullptr, OPT_LANG},
        {"show",       required_argument, nullptr, OPT_SHOW},
        {"tags",       no_argument,       nullptr, OPT_TAGS},
        {"no-toc-links",         no_argument,       nullptr, OPT_NO_TOC_LINKS},
        {"toc-backlinks",        no_argument,       nullptr, OPT_TOC_BACKLINKS},
        {"set-font-cover-page",  required_argument, nullptr, OPT_FONT_COVER},
        {"set-font-body-zh-CN",  required_argument, nullptr, OPT_FONT_BODY_ZH},
        {"set-font-body-en-US",  required_argument, nullptr, OPT_FONT_BODY_EN},
        {"set-font-body-codes",  required_argument, nullptr, OPT_FONT_BODY_CODES},
        {"set-font-title-zh-CN", required_argument, nullptr, OPT_FONT_TITLE_ZH},
        {"set-font-title-en-US", required_argument, nullptr, OPT_FONT_TITLE_EN},
        {"no-bilibili-link",     no_argument,       nullptr, OPT_NO_BILIBILI_LINK},
        {"set-cover-title",      required_argument, nullptr, OPT_COVER_TITLE},
        {"pid",                  required_argument, nullptr, OPT_PID},
        {"pid-range",            required_argument, nullptr, OPT_PID_RANGE},
        {"help",       no_argument,       nullptr, 'h'},
        {"version",    no_argument,       nullptr, 'V'},
        {nullptr,      0,                 nullptr, 0},
    };

    Options options;
    int opt;
    // -V, --version 必须单独使用：统计除 -V 之外出现的选项个数，
    // 与位置参数（optind）一起判断是否属于参数使用错误
    int other_option_count = 0;
    // 短选项串以 ':' 开头：getopt 出错时不打印英文提示，
    // 由下面的 '?' / ':' 分支输出统一的中文错误信息
    while ((opt = getopt_long(arg_count, arg_vector, ":UMLhV", kLongOptions, nullptr)) != -1)
    {
        if (opt != 'V')
            ++other_option_count;
        switch (opt)
        {
        case 'U':
            options.update = true;
            break;
        case 'M':
            options.markdown = true;
            break;
        case 'L':
            options.latex = true;
            break;
        case OPT_TAG:
            // 先整体保留，具体按一个标签还是按空格拆分，交给 select_problems
            // 结合 tags.json 判断：整体是已知标签名（如 "NOIP 普及组"）就按一个，
            // 否则按空格拆成多个（如 --tag "模拟 贪心"）
            if (optarg == nullptr || optarg[0] == '\0')
            {
                printError("参数 '--tag' 后缺少标签名称或数字 ID；正确用法：--tag <标签名称或数字 ID>");
                return 1;
            }
            options.filter.tags.push_back(optarg);
            break;
        case OPT_DIFFICULTY:
        {
            // 支持空格分隔的多个难度（如 --difficulty 1 3-5）；空值视为参数缺失
            const std::vector<std::string> specs = split_whitespace(optarg ? optarg : "");
            if (specs.empty())
            {
                printError("参数 '--difficulty' 后缺少难度值；正确用法：--difficulty <难度（0-8，或区间 1-4）>");
                return 1;
            }
            for (const auto &spec : specs)
            {
                if (!parse_difficulty_spec(spec, options.filter.difficulties))
                {
                    printError("参数 '--difficulty' 的值 '" + spec +
                               "' 不是合法的难度；正确用法："
                               "--difficulty <spec>，spec 为 0~8 的数字或闭区间（如 1-4）");
                    return 1;
                }
            }
            break;
        }
        case OPT_OUTPUT:
            if (optarg == nullptr || optarg[0] == '\0')
            {
                printError("参数 '--output' 后缺少输出文件路径；正确用法：--output <输出文件路径>");
                return 1;
            }
            options.output = optarg;
            break;
        case OPT_TYPE:
        {
            std::string t = optarg;
            for (auto &c : t)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (t != "B" && t != "P")
            {
                printError("参数 '--type' 的值 '" + std::string(optarg) +
                           "' 不是合法的题目类型；正确用法：--type <B|P>"
                           "（B 表示基础题，P 表示普通题）");
                return 1;
            }
            options.filter.types.push_back(t);
            break;
        }
        case OPT_LANG:
        {
            std::string lang = optarg;
            if (lang != "zh-CN" && lang != "zh" && lang != "en")
            {
                printError("参数 '--lang' 的值 '" + std::string(optarg) +
                           "' 不是合法的题面语言；正确用法：--lang <zh-CN|en>");
                return 1;
            }
            options.filter.lang = lang;
            break;
        }
        case OPT_SHOW:
        {
            const std::string s = optarg;
            options.show_explicit = true;
            if (s.size() != 2 ||
                (s[0] != '0' && s[0] != '1') ||
                (s[1] != '0' && s[1] != '1'))
            {
                printError("参数 '--show' 的值 '" + std::string(optarg) +
                           "' 不是合法的显示开关；正确用法：--show <NN>，"
                           "NN 为两位 0/1（如 11 / 10 / 01 / 00），"
                           "第 1 位控制是否显示难度，第 2 位控制是否显示标签");
                return 1;
            }
            options.filter.show = s;
            break;
        }
        case OPT_TAGS:
            options.list_tags = true;
            break;
        case OPT_NO_TOC_LINKS:
            options.no_toc_links = true;
            break;
        case OPT_TOC_BACKLINKS:
            options.toc_backlinks = true;
            break;
        case OPT_NO_BILIBILI_LINK:
            options.no_bilibili_link = true;
            break;
        case OPT_FONT_COVER:
        case OPT_FONT_BODY_ZH:
        case OPT_FONT_BODY_EN:
        case OPT_FONT_BODY_CODES:
        case OPT_FONT_TITLE_ZH:
        case OPT_FONT_TITLE_EN:
        {
            // 校验字体参数（区分系统字体名称与字体文件地址），出错时拒绝执行
            std::string spec;
            const std::string err = validate_font_option(option_name_for(opt), optarg, spec);
            if (!err.empty())
            {
                printError(err);
                return 1;
            }
            switch (opt)
            {
            case OPT_FONT_COVER: options.font_cover = std::move(spec); break;
            case OPT_FONT_BODY_ZH: options.font_body_zh = std::move(spec); break;
            case OPT_FONT_BODY_EN: options.font_body_en = std::move(spec); break;
            case OPT_FONT_BODY_CODES: options.font_body_codes = std::move(spec); break;
            case OPT_FONT_TITLE_ZH: options.font_title_zh = std::move(spec); break;
            case OPT_FONT_TITLE_EN: options.font_title_en = std::move(spec); break;
            }
            break;
        }
        case OPT_COVER_TITLE:
        {
            const std::string t = optarg;
            if (t.empty() || t[0] == '-')
            {
                printError("参数 '--set-cover-title' 后缺少标题文字；"
                           "正确用法：--set-cover-title <封面标题>");
                return 1;
            }
            options.cover_title = t;
            break;
        }
        case OPT_PID:
        {
            // 支持空格分隔的多个题号（如 --pid P1001 P1002）；空值视为参数缺失。
            // getopt 只取一个参数，后续裸参数在下方统一并入 --pid
            const std::vector<std::string> tokens =
                split_whitespace(optarg ? optarg : "");
            if (tokens.empty() || (optarg && optarg[0] == '-'))
            {
                printError("参数 '--pid' 后缺少题号；"
                           "正确用法：--pid <题号>（如 P1001，可多个，"
                           "空格分隔或重复 --pid）");
                return 1;
            }
            for (const auto &tok : tokens)
                options.filter.pids.push_back(tok);
            break;
        }
        case OPT_PID_RANGE:
        {
            // 支持空格分隔的多组范围；空值视为参数缺失
            const std::vector<std::string> tokens =
                split_whitespace(optarg ? optarg : "");
            if (tokens.empty() || (optarg && optarg[0] == '-'))
            {
                printError("参数 '--pid-range' 后缺少题号范围；"
                           "正确用法：--pid-range <题号>-<题号>"
                           "（如 P1001-P1010，可多组，空格分隔或重复 --pid-range）");
                return 1;
            }
            for (const auto &tok : tokens)
            {
                std::pair<std::string, std::string> range;
                std::string err;
                if (!parse_pid_range_arg(tok, range, err))
                {
                    printError(err);
                    return 1;
                }
                options.filter.pid_ranges.push_back(std::move(range));
            }
            break;
        }
        case 'h':
            options.help = true;
            break;
        case 'V':
            options.version = true;
            break;
        case '?':
        {
            // 未知参数：提示程序没有此参数，并提示使用 -h, --help 查看帮助
            const char *token = (optind > 0 && optind <= arg_count) ? arg_vector[optind - 1] : "";
            printError("未知参数 '" + std::string(token) + "'（程序没有此参数），"
                       "请使用 -h, --help 查看帮助信息");
            return 1;
        }
        case ':':
        {
            // 选项后缺少必要的参数值
            const std::string token = (optind > 0 && optind <= arg_count) ? arg_vector[optind - 1] : "";
            const std::string hint = option_argument_hint(token);
            if (!hint.empty())
                printError("参数 '" + token + "' 后缺少必要的参数值；正确用法：" +
                           token + " <" + hint + ">");
            else
                printError("参数 '" + token + "' 后缺少必要的参数值；"
                           "请使用 -h, --help 查看帮助信息");
            return 1;
        }
        default:
            printUsage();
            return 1;
        }
    }

    // -V / --version 属于「单独使用」的信息类参数：与其他任何参数
    // （含 -h、--tags、-M、-L 等）或多余的位置参数同时出现即为参数使用错误
    if (options.version)
    {
        if (other_option_count > 0 || optind < arg_count)
        {
            printError("参数 -V, --version 不能与其他参数同时使用；"
                       "正确用法：luogu-extract -V（或 luogu-extract --version），"
                       "单独执行该参数即可查看项目简介、版本号、版权声明与项目仓库链接");
            return 1;
        }
        printVersion();
        return 0;
    }

    if (options.help)
    {
        printUsage();
        return options.list_tags ? (print_tag_list() ? 0 : 1) : 0;
    }

    if (options.list_tags)
        return print_tag_list() ? 0 : 1;

    if (options.latex && options.show_explicit)
    {
        printError("参数 --show 仅在使用 -M（导出 Markdown）时有效；"
                   "-L（导出 LaTeX）始终不显示难度，且仅隐藏「算法」类标签。"
                   "请移除 --show，或改用 -M 导出");
        return 1;
    }

    // -M 与 -L 同时给出：此前 -L 会被静默忽略。明确拒绝，避免用户误以为
    // 两种格式都已导出；需要两种格式时请分两次执行
    if (options.markdown && options.latex)
    {
        printError("参数 -M 与 -L 不能同时使用；请分两次导出（-M 导出 Markdown，-L 导出 LaTeX）");
        return 1;
    }

    // 仅 -L 支持的参数与 -M 一起使用属于参数填用错误：拒绝执行并提示正确用法
    if (options.markdown && !options.latex)
    {
        std::vector<std::string> latex_only;
        if (options.no_toc_links) latex_only.push_back("--no-toc-links");
        if (options.toc_backlinks) latex_only.push_back("--toc-backlinks");
        if (!options.font_cover.empty()) latex_only.push_back("--set-font-cover-page");
        if (!options.font_body_zh.empty()) latex_only.push_back("--set-font-body-zh-CN");
        if (!options.font_body_en.empty()) latex_only.push_back("--set-font-body-en-US");
        if (!options.font_body_codes.empty()) latex_only.push_back("--set-font-body-codes");
        if (!options.font_title_zh.empty()) latex_only.push_back("--set-font-title-zh-CN");
        if (!options.font_title_en.empty()) latex_only.push_back("--set-font-title-en-US");
        if (options.no_bilibili_link) latex_only.push_back("--no-bilibili-link");
        if (!latex_only.empty())
        {
            std::string joined;
            for (size_t i = 0; i < latex_only.size(); ++i)
                joined += (i ? "、" : "") + latex_only[i];
            printError("参数 " + joined + " 仅在使用 -L（导出 LaTeX）时支持；"
                       "请移除上述参数，或在命令行中加入 -L 导出 LaTeX");
            return 1;
        }
    }

    if (optind < arg_count)
    {
        if (!options.markdown && !options.latex)
        {
            printError("多余的参数 '" + std::string(arg_vector[optind]) +
                       "'：该参数不是任何选项的值；请检查命令行，"
                       "或使用 -h, --help 查看帮助信息");
            printUsage();
            return 1;
        }

        // -M/-L 模式下剩余裸参数的处理：
        // - 使用过 --pid 时按题号并入 --pid（支持 "--pid P1001 P1002"）；
        // - 使用过 --pid-range 时按题号范围并入 --pid-range
        //   （支持 "--pid-range P1001-P1010 P2000-P2010"）；
        // - 否则保持原行为：按难度解析，解析不了则当作 --tag 的后续值
        //   （支持 "--difficulty 1 2 3" 与 "--tag 模拟 贪心" 两种写法）
        if (!options.filter.pids.empty())
        {
            for (int i = optind; i < arg_count; ++i)
                options.filter.pids.push_back(arg_vector[i]);
        }
        else if (!options.filter.pid_ranges.empty())
        {
            for (int i = optind; i < arg_count; ++i)
            {
                std::pair<std::string, std::string> range;
                std::string err;
                if (!parse_pid_range_arg(arg_vector[i], range, err))
                {
                    printError(err);
                    return 1;
                }
                options.filter.pid_ranges.push_back(std::move(range));
            }
        }
        else
        {
            for (int i = optind; i < arg_count; ++i)
            {
                std::vector<int> tmp = options.filter.difficulties;
                if (parse_difficulty_spec(arg_vector[i], tmp))
                    options.filter.difficulties = std::move(tmp);
                else
                    options.filter.tags.push_back(arg_vector[i]);
            }
        }
    }

    // --pid 不能与 --tag、--difficulty、--type 同时使用（参数填用错误）
    if (!options.filter.pids.empty() &&
        (!options.filter.tags.empty() ||
         !options.filter.difficulties.empty() ||
         !options.filter.types.empty()))
    {
        printError("参数错误：--pid 不能与 --tag、--difficulty、--type 同时使用；"
                   "若需按题号与其他条件组合筛选，请改用 --pid-range"
                   "（--pid-range 支持与上述参数同时使用）");
        return 1;
    }

    if (!options.update && !options.markdown && !options.latex)
    {
        printError("未指定任何操作；请至少使用 -U（更新缓存）、-M（导出 Markdown）、"
                   "-L（导出 LaTeX）或 --tags（查看标签对照表）之一，"
                   "并可用 -h, --help 查看帮助信息");
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
    {
        printError("初始化 libcurl 失败；请检查网络相关运行库是否安装完整");
        return 1;
    }

    int result = 0;
    if (options.update)
    {
        result = crawler::update();
        if (result != crawler::SUCCESS)
        {
            // 缓存更新失败时不继续用旧缓存导出：可能掩盖更新失败
            printError("缓存更新失败，已停止后续操作");
            curl_global_cleanup();
            return result;
        }
    }

    if (options.markdown)
    {
        // 输出路径按 UTF-8 构造 filesystem::path（Windows 下中文路径可用）
        const std::filesystem::path out_path = luogu::compat::path_from_utf8(
            options.output.empty() ? "problems.md" : options.output);
        std::string error;
        if (markdown::export_markdown(options.filter, out_path, error, options.cover_title))
            printSuccess("已把筛选出的题目导出到 '" +
                         luogu::compat::path_to_utf8(out_path) + "'");
        else
        {
            printError(error);
            result = 1;
        }
    }
    else if (options.latex)
    {
        // 输出路径按 UTF-8 构造 filesystem::path（Windows 下中文路径可用）
        const std::filesystem::path out_path = luogu::compat::path_from_utf8(
            options.output.empty() ? "problems.tex" : options.output);

        // 组装 LaTeX 显示选项
        latex::Options latex_opt;
        latex_opt.lang = options.filter.lang;
        latex_opt.toc_links = !options.no_toc_links;
        latex_opt.toc_backlinks = options.toc_backlinks;
        latex_opt.bilibili_links = !options.no_bilibili_link;
        latex_opt.font_cover = options.font_cover;
        latex_opt.font_body_zh = options.font_body_zh;
        latex_opt.font_body_en = options.font_body_en;
        latex_opt.font_code = options.font_body_codes;
        latex_opt.font_title_zh = options.font_title_zh;
        latex_opt.font_title_en = options.font_title_en;
        latex_opt.cover_title = options.cover_title;

        std::string error;
        if (!latex::export_latex(options.filter, out_path, error, latex_opt))
        {
            printError(error);
            result = 1;
        }
    }

    curl_global_cleanup();
    return result;
}
