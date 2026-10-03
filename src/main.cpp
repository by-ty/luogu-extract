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
// Windows（MSVC / MinGW-w64）不保证提供 POSIX getopt_long，全平台统一使用自带实现
// （语义同 GNU getopt）；LUOGU_FORCE_COMPAT_GETOPT 可在非 Windows 平台强制测试该实现。
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
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "luogu-extract/app/app.h"
#include "luogu-extract/contents/solution_task.h"
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/crawler/request_gate.h"
#include "luogu-extract/export/common.h"
#include "luogu-extract/export/latex.h"
#include "luogu-extract/export/markdown.h"
#include "luogu-extract/interactive/interactive.h"
#include "luogu-extract/util/cli_parse.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/prompt.h"
#include "luogu-extract/util/solution_cache.h"
#include "luogu-extract/util/tag_cache.h"
#include "luogu-extract/util/version.h"

namespace
{

// 程序运行参数（新增参数在此添加字段）
struct Options
{
    bool update = false;    // -U, --update
    bool markdown = false;  // -M, --markdown
    bool latex = false;     // -L, --latex
    bool list_tags = false; // --tags
    bool help = false;      // -h, --help
    bool version = false;   // -V, --version
    std::string output;     // --output（空则按模式取默认 problems.md / problems.tex）

    // 缓存清除参数：只能彼此组合使用，不能与其它参数同用
    bool clean_all = false;      // -C, --clean-all
    bool clean_images = false;   // -CIMG, --clean-images
    bool clean_problems = false; // -CP, --clean-problems
    bool clean_fonts = false;    // -CF, --clean-fonts

    // -RD, --new-download：图片不用已有缓存而是重新下载（仅 -L；新图原子替换）
    bool new_download = false;

    // --local：把本地 Markdown 转写为 LaTeX（仅 -L，不读写缓存）；空串 = 未使用
    std::string local_file;
    // --doc-only：不输出封面/目录/页眉，整篇连贯输出（仅与 --local 同用）
    bool doc_only = false;

    // --compile：-L 导出成功后自动执行 latexmk --xelatex
    bool compile_latex = false;
    bool no_toc_links = false;      // --no-toc-links：目录条目不带超链接（仅 -L）
    bool toc_backlinks = false;     // --toc-backlinks：页码为跳回目录的超链接（仅 -L）
    bool no_bilibili_link = false;  // --no-bilibili-link：bilibili URL 输出为普通文本（仅 -L）
    // 题目信息显示开关（-M / -L 均有效）
    bool no_show_source_tags = false;   // --no-show-source-tags：不显示来源/时间/区域/特殊标签
    bool show_algorithm_tags = false;   // --show-algorithm-tags：显示算法标签
    bool show_difficulty_tags = false;  // --show-difficulty-tags：显示题目难度
    // --show-contents-difficulty-tags：目录标题按难度着色（仅 -L）
    bool show_contents_difficulty_tags = false;
    // --paginate：题目与文章之间分页（仅 -L）
    bool paginate = false;
    std::string font_cover;         // --set-font-cover-page（仅 -L）
    std::string font_body_zh;       // --set-font-body-zh-CN（仅 -L）
    std::string font_body_en;       // --set-font-body-en-US（仅 -L）
    std::string font_body_codes;    // --set-font-body-codes（仅 -L）
    std::string font_title_zh;      // --set-font-title-zh-CN（仅 -L）
    std::string font_title_en;      // --set-font-title-en-US（仅 -L）
    std::string cover_title;        // --set-cover-title（-L / -M 均支持）

    luogu::ExportFilter filter; // -M / -L 共用的筛选条件

    // 题解与文章抓取与导出（--with-solutions / --article 等）
    bool with_solutions = false;          // --with-solutions
    bool solutions_only = false;          // --solutions-only
    bool articles_only_download = false; // --articles-only-download
    // --article：按编号单独下载（已转小写去重），与题解共用来源/TTL 等选项
    std::vector<std::string> articles;
    std::string cookie_file;              // --cookie
    std::string cookie_string;            // --cookie-string
    std::string article_source;          // --article-source（空 = auto）
    int max_solutions = 1;               // --max-solutions（-1 表示 all）
    std::string request_delay;           // --request-delay（原始文本）
    bool no_delay_auto_scale = false;     // --no-delay-auto-scale
    // --solution-ttl：题解**列表**有效期（天）；-1 表示无限（默认）
    int solution_ttl = -1;
    // --article-ttl：题解**正文**有效期（天）；-1 表示无限（默认）
    int article_ttl = -1;
    int rate_limit_wait = 120;            // --rate-limit-wait
    bool allow_partial = false;           // --allow-partial
    bool refresh_solutions = false;       // --refresh-solutions
    bool refresh_articles = false;        // --refresh-articles
    bool clean_solutions = false;         // -CS, --clean-solutions（题解列表缓存）
    bool clean_articles = false;          // -CA, --clean-articles（文章缓存）
    std::string solution_placement = "document-end"; // --solution-placement
    bool no_problem_to_solution_link = false; // --no-problem-to-solution-link
    bool no_solution_to_problem_link = false; // --no-solution-to-problem-link
    bool no_solution_toc = false;         // --no-solution-toc
    bool no_article_meta = false;        // --no-article-meta
    bool yes = false;                     // --yes

    // 题解/文章相关参数是否被显式给出（「缺少 --with-solutions」校验用；--article 不算）
    bool solution_option_used = false;
};

enum
{
    OPT_TAG = 1000,
    OPT_DIFFICULTY,
    OPT_OUTPUT,
    OPT_TYPE,
    OPT_LANG,
    OPT_TAGS,
    OPT_NO_TOC_LINKS,
    OPT_TOC_BACKLINKS,
    OPT_NO_SHOW_SOURCE_TAGS,
    OPT_SHOW_ALGORITHM_TAGS,
    OPT_SHOW_DIFFICULTY_TAGS,
    OPT_SHOW_CONTENTS_DIFFICULTY_TAGS,
    OPT_PAGINATE,
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
    OPT_CLEAN_ALL,
    OPT_CLEAN_IMAGES,
    OPT_CLEAN_PROBLEMS,
    OPT_CLEAN_FONTS,
    OPT_NEW_DOWNLOAD,
    OPT_COMPILE,
    OPT_COOKIE,
    OPT_COOKIE_STRING,
    OPT_WITH_SOLUTIONS,
    OPT_ARTICLE,
    OPT_ARTICLE_SOURCE,
    OPT_MAX_SOLUTIONS,
    OPT_REQUEST_DELAY,
    OPT_NO_DELAY_AUTO_SCALE,
    OPT_SOLUTION_TTL,
    OPT_ARTICLE_TTL,
    OPT_RATE_LIMIT_WAIT,
    OPT_ALLOW_PARTIAL,
    OPT_REFRESH_SOLUTIONS,
    OPT_REFRESH_ARTICLES,
    OPT_CLEAN_SOLUTIONS,
    OPT_CLEAN_ARTICLES,
    OPT_SOLUTIONS_ONLY,
    OPT_ARTICLES_ONLY_DOWNLOAD,
    OPT_SOLUTION_PLACEMENT,
    OPT_NO_PROBLEM_TO_SOLUTION_LINK,
    OPT_NO_SOLUTION_TO_PROBLEM_LINK,
    OPT_NO_SOLUTION_TOC,
    OPT_NO_ARTICLE_META,
    OPT_LOCAL,
    OPT_DOC_ONLY,
};

inline const char *option_name_for(int code)
{
    switch (code)
    {
    case OPT_TAG: return "--tag";
    case OPT_DIFFICULTY: return "--difficulty";
    case OPT_OUTPUT: return "--output";
    case OPT_TYPE: return "--type";
    case OPT_LANG: return "--lang";
    case OPT_FONT_COVER: return "--set-font-cover-page";
    case OPT_FONT_BODY_ZH: return "--set-font-body-zh-CN";
    case OPT_FONT_BODY_EN: return "--set-font-body-en-US";
    case OPT_FONT_BODY_CODES: return "--set-font-body-codes";
    case OPT_FONT_TITLE_ZH: return "--set-font-title-zh-CN";
    case OPT_FONT_TITLE_EN: return "--set-font-title-en-US";
    case OPT_COVER_TITLE: return "--set-cover-title";
    case OPT_ARTICLE: return "--article";
    case OPT_LOCAL: return "--local";
    case OPT_DOC_ONLY: return "--doc-only";
    case OPT_COOKIE: return "--cookie";
    case OPT_COOKIE_STRING: return "--cookie-string";
    case OPT_ARTICLE_SOURCE: return "--article-source";
    case OPT_MAX_SOLUTIONS: return "--max-solutions";
    case OPT_REQUEST_DELAY: return "--request-delay";
    case OPT_SOLUTION_TTL: return "--solution-ttl";
    case OPT_RATE_LIMIT_WAIT: return "--rate-limit-wait";
    case OPT_SOLUTION_PLACEMENT: return "--solution-placement";
    default: return "";
    }
}

inline std::string option_argument_hint(const std::string &token)
{
    if (token == "--tag") return "标签名称或数字 ID";
    if (token == "--difficulty") return "难度（0-8，或区间 1-4）";
    if (token == "--output") return "输出文件路径";
    if (token == "--type") return "题目类型（B 或 P）";
    if (token == "--lang") return "题面语言（zh-CN 或 en）";
    if (token == "--set-font-cover-page") return "字体名称或字体文件地址";
    if (token == "--set-font-body-zh-CN") return "字体名称或字体文件地址";
    if (token == "--set-font-body-en-US") return "字体名称或字体文件地址";
    if (token == "--set-font-body-codes") return "字体名称或字体文件地址";
    if (token == "--set-font-title-zh-CN") return "字体名称或字体文件地址";
    if (token == "--set-font-title-en-US") return "字体名称或字体文件地址";
    if (token == "--set-cover-title") return "封面标题";
    if (token == "--pid") return "题号（如 P1001，可多个，空格分隔或重复 --pid）";
    if (token == "--pid-range") return "题号范围（如 P1001-P1010，可多组，空格分隔或重复 --pid-range）";
    if (token == "--article") return "文章编号（如 p7fsb45w，可多个，空格分隔或重复 --article）";
    if (token == "--local") return "本地 Markdown 文件路径";
    if (token == "--cookie") return "Netscape 格式的 cookies.txt 路径";
    if (token == "--cookie-string") return "Cookie 串（形如 \"k=v; k2=v2\"）";
    if (token == "--article-source") return "文章正文来源（auto / official / save）";
    if (token == "--max-solutions") return "每题抓取的题解篇数（正整数或 all）";
    if (token == "--request-delay") return "请求间隔秒数（如 5 或 8-15）";
    if (token == "--solution-ttl") return "题解列表缓存有效期天数（0 表示只用 ETag）";
    if (token == "--article-ttl") return "文章正文缓存有效期天数（0 表示只用 ETag）";
    if (token == "--rate-limit-wait") return "限流等待秒数（0 表示检测到限流直接停止）";
    if (token == "--solution-placement") return "题解位置（document-end 或 per-problem）";
    return "";
}

const char *kUsage =
    "用法：luogu-extract [选项]\n"
    "      不带任何参数运行（./luogu-extract）时进入简易命令行交互程序：\n"
    "      一步步询问更新缓存、导出类型、导出内容与各项设置，确认后按等价的\n"
    "      参数执行；过程中输入 q 可随时结束程序\n"
    "\n"
    "选项：\n"
    "  -U, --update          更新题目列表缓存（latest.ndjson）与标签缓存（tags.json）。\n"
    "                        可与 -M / -L 一同使用：同时给出时先更新缓存再下载题目；\n"
    "                        更新失败时不继续执行后续操作\n"
    "  -M, --markdown        筛选并导出 Markdown（默认输出 problems.md）\n"
    "  -L, --latex           筛选并导出 LaTeX（默认输出 problems.tex）\n"
    "                        （-M 与 -L 不能同时使用；需要两种格式时请分两次执行）\n"
    "  -RD, --new-download   仅 -L 有效：下载题目时不使用之前缓存的图片，而是重新下载图片\n"
    "      --compile         仅 -L 有效：LaTeX 文档导出成功后自动在输出文件所在目录执行\n"
    "                        latexmk --xelatex <输出文件名>.tex，等编译结束后再执行\n"
    "                        latexmk -c <输出文件名>.tex 清理中间文件（PDF 保留）；\n"
    "                        编译失败时提示退出码并以非零状态码结束\n"
    "  -C, --clean-all       清空 luogu-extract 缓存文件夹（含题目列表、标签、图片与字体缓存）\n"
    "  -CIMG, --clean-images\n"
    "                        清除 <缓存目录>/images/ 下的图片缓存\n"
    "  -CP, --clean-problems\n"
    "                        清除题面缓存（latest.ndjson 与 latest.ndjson.gz）\n"
    "  -CF, --clean-fonts\n"
    "                        清除字体缓存（<缓存目录>/fonts/，即 --set-font-* 传入\n"
    "                        无扩展名字体文件时复制的副本）\n"
    "  -CS, --clean-solutions\n"
    "                        清除题解列表缓存（<缓存目录>/solutions.ndjson）\n"
    "  -CA, --clean-articles\n"
    "                        清除文章缓存（<缓存目录>/articles/）\n"
    "  （以上清除缓存的参数只能彼此组合使用，不能与其他参数同时使用）\n"
    "      --tags            按官方分类打印标签 ID 对照表（可与 -h 组合）\n"
    "      --tag <name|ID>...\n"
    "                        按标签筛选；多个值可用空格分隔或重复 --tag，题目须包含全部标签；\n"
    "                        引号整体恰好等于已知标签名（如 \"NOIP 普及组\"）时按一个标签处理\n"
    "      --difficulty <spec>\n"
    "                        按难度（0~8）筛选；支持区间写法（如 1-4），多组值可用空格\n"
    "                        分隔或重复 --difficulty\n"
    "      --type <B|P>      按题目类型筛选（可重复，空表示全部类型）\n"
    "      --pid <pid>...    按题号精确筛选；多个值可用空格分隔或重复 --pid。每个题号必须\n"
    "                        存在于题目列表缓存中。可与其它筛选参数（--tag、--difficulty、\n"
    "                        --type、--pid-range）同时使用：命中的题目会追加到其它条件筛选\n"
    "                        出的题目之外（并集），命中的题目不要求满足其它条件（已经符合\n"
    "                        筛选条件时不会重复导出）\n"
    "      --pid-range <a>-<b>\n"
    "                        按题号闭区间筛选；多组值可用空格分隔或重复 --pid-range。一组\n"
    "                        范围两端必须为同一题库（如都为 P 题库或都为 B 题库，多组范围间\n"
    "                        可不为同一题库）。可与 --tag、--difficulty、--type、--pid 同时使用\n"
    "      --lang <zh-CN|en> 题面语言（默认 zh-CN；en 缺失时回退中文）\n"
    "      --output <file>   输出文件路径（默认 problems.md / problems.tex）\n"
    "\n"
    "题目信息显示选项（-M / -L 均有效）：\n"
    "      --no-show-source-tags\n"
    "                        不显示来源、时间、区域、特殊题目标签（默认显示）\n"
    "      --show-algorithm-tags\n"
    "                        显示算法标签（默认不显示）\n"
    "      --show-difficulty-tags\n"
    "                        显示难度（默认不显示）\n"
    "\n"
    "LaTeX 排版选项（仅 -L 有效）：\n"
    "      --no-toc-links    目录条目不带跳转到对应题目页的超链接（默认带超链接）\n"
    "      --toc-backlinks   每页页眉处的页码为跳回目录页的超链接（默认无超链接）\n"
    "      --show-contents-difficulty-tags\n"
    "                        目录中的题目标题按难度着色\n"
    "      --paginate        题目与题解（文章）之间分页，每道题、每篇文章都从新的一页开始\n"
    "                        （默认连续排版）\n"
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
    "                        题面中的 B 站视频补全为完整网址后输出为普通文本而非超链接\n"
    "                        （默认超链接）\n"
    "      --set-cover-title <title>\n"
    "                        设置封面标题（-L，默认 luogu extract）或 Markdown 一级标题\n"
    "                        （-M，默认 洛谷题目导出）\n"
    "\n"
    "本地 Markdown 转写选项（仅 -L 有效，不能与下载题目/题解/文章同用）：\n"
    "      --local <文件地址>\n"
    "                        把本地 Markdown 文本文件转写为 LaTeX；指定的文件不写入缓存，\n"
    "                        也不能与下载题目、题解、文章的功能同时使用\n"
    "      --doc-only        仅与 --local 同用：不输出封面、目录与页眉标题\n"
    "                        （不能与 --paginate 同用）\n"
    "\n"
    "题解与文章下载选项：\n"
    "      --with-solutions  启用题解抓取与导出；需与 -M 或 -L 同用，\n"
    "                        题解与题面导出到同一个文件\n"
    "      --article <文章编号>...\n"
    "                        按文章编号下载指定的文章（编号为 6~32 位小写字母或数字，取自\n"
    "                        文章页地址 /article/<编号>，如 p7fsb45w；大写会自动转小写）；\n"
    "                        多个值可用空格分隔或重复 --article，重复编号只下载一次；导出的\n"
    "                        文章统一放在文档最后。需与 -M 或 -L 同用；未给出任何题目筛选\n"
    "                        参数且未启用题解功能时，文档只含这些文章\n"
    "      --cookie <file>   需提供 Netscape 格式的 cookies.txt（含登录态）。题解列表接口\n"
    "                        需要登录态，该参数是启用题解功能的前提\n"
    "      --cookie-string <k=v; ...>\n"
    "                        直接传入 Cookie 串（与 --cookie 二选一）\n"
    "      --article-source <auto|official|save>\n"
    "                        文章正文来源，默认 auto：缓存优先（两个来源都有时优先原站），\n"
    "                        未命中的在洛谷原站与洛谷保存站之间轮流分配、并行抓取；\n"
    "                        official 只用原站；save 只用保存站。题解列表恒取洛谷原站\n"
    "      --max-solutions <n|all>\n"
    "                        每题抓取篇数，默认 1，按列表顺序取最靠前的 n 篇；\n"
    "                        all 表示该题全部题解\n"
    "      --request-delay <mean|min-max>\n"
    "                        请求的平均间隔秒数，默认 5（实际为均值 ±30% 均匀抖动，\n"
    "                        即 3.5~6.5 秒）；也支持显式区间（如 8-15）与小数（如 2.5）；\n"
    "                        单次间隔上限 300 秒\n"
    "      --no-delay-auto-scale\n"
    "                        关闭「随抓取量自动递增延时」与限流后的额外放大\n"
    "      --solution-ttl <days>\n"
    "                        题解列表缓存有效期天数：默认无限；0 表示每次都发 ETag 条件\n"
    "                        请求（304 时只刷新时间戳）\n"
    "      --article-ttl <days>\n"
    "                        文章正文缓存有效期天数：默认无限；0 表示每次都发 ETag 条件\n"
    "                        请求（304 时只刷新时间戳）\n"
    "      --rate-limit-wait <seconds>\n"
    "                        检测到限流后的等待时长，默认 120；0 表示检测到限流直接停止。\n"
    "                        等待期间可按 S 立即停止、按 C 确认后立即继续\n"
    "      --allow-partial   允许导出正文不完整的文章（默认跳过并在结束时汇总）\n"
    "      --refresh-solutions\n"
    "                        强制重新获取题解列表（忽略有效期与 ETag）\n"
    "      --refresh-articles\n"
    "                        强制重新获取文章正文（忽略有效期与 ETag）\n"
    "      --solutions-only  只导出题解，不导出题面（需与 -M 或 -L 同用；\n"
    "                        --article 的文章仍会导出在文档最后）\n"
    "      --articles-only-download\n"
    "                        只抓取并缓存文章，不导出任何文件（不需要 -M / -L）\n"
    "      --solution-placement <document-end|per-problem>\n"
    "                        题解在文档中的位置，默认 document-end（统一置于文档最后）；\n"
    "                        per-problem 表示紧跟对应题目之后\n"
    "      --no-problem-to-solution-link\n"
    "                        关闭题目到题解的跳转（-L 为题目标题右侧的「查看题解」按钮，\n"
    "                        -M 为 Markdown 中的跳转链接）\n"
    "      --no-solution-to-problem-link\n"
    "                        关闭题解到题目的跳转（-L 为题解标题右侧的「返回题目」按钮，\n"
    "                        -M 为 Markdown 中的跳转链接）\n"
    "      --no-solution-toc 仅 -L 有效：文章的标题不进目录（默认进目录，题解条目注明\n"
    "                        所属题目，普通文章条目只有标题）\n"
    "      --no-article-meta 不显示文章的来源与原文链接\n"
    "  -y, --yes             把爬取风险的确认次数减少 1 次（减到 0 为止）；\n"
    "                        不能把第 5 档变为无需确认\n"
    "  -h, --help            显示帮助\n"
    "  -V, --version         显示项目简介、版本号、版权声明与项目仓库链接\n"
    "                        （不能与其他参数同时使用）\n"
    "\n"
    "声明：\n"
    "  文章（含题解）著作权归原作者，导出物仅供个人离线阅读，请勿再分发或用于\n"
    "  商业用途；抓取频率与请求总量由你自行判断，后果自负；保存站 luogu.me 为\n"
    "  第三方站点；请遵守洛谷用户协议及相关法律法规。\n";

void printUsage()
{
    std::printf("%s", kUsage);
}

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

// 颜色只在输出流连接终端时启用（重定向时不写 ANSI 转义序列）
inline void printError(const std::string &message)
{
    if (luogu::compat::stderr_is_tty())
        std::fprintf(stderr, "\033[1;31m错误：\033[0m%s\n", message.c_str());
    else
        std::fprintf(stderr, "错误：%s\n", message.c_str());
}

inline void printSuccess(const std::string &message)
{
    if (luogu::compat::stdout_is_tty())
        std::printf("\033[1;32m%s\033[0m\n", message.c_str());
    else
        std::printf("%s\n", message.c_str());
}

// 参数值解析与校验统一放在 util/cli_parse（与交互程序共用同一份实现）
using cliparse::default_local_output;
using cliparse::parse_difficulty_spec;
using cliparse::parse_pid_range_arg;
using cliparse::parse_positive_int;
using cliparse::split_whitespace;
using cliparse::to_lower_ascii;
using cliparse::to_upper_ascii;
using cliparse::validate_font_option;

// 参数报错时定位：短选项簇（"-local" 会被拆成 -l -o -c -a -l）出错时 optind 可能仍指向
// 该簇、也可能已指向下一个参数，取 argv[optind - 1] 会误判，故用 optopt 配合两个位置定位。

inline bool is_short_option_cluster(const std::string &token)
{
    return token.size() >= 2 && token[0] == '-' && token[1] != '-';
}

inline std::string long_option_name_of(const std::string &token)
{
    if (token.size() < 2 || token.compare(0, 2, "--") != 0)
        return "";
    std::string name = token.substr(2);
    const size_t eq = name.find('=');
    if (eq != std::string::npos)
        name = name.substr(0, eq);
    return name;
}

// 「-local」这类笔误：首字符恰为出错字符时返回其本应的长选项名
inline std::string single_dash_long_name(const std::string &token, int bad_char)
{
    if (!is_short_option_cluster(token) ||
        static_cast<unsigned char>(token[1]) != static_cast<unsigned char>(bad_char))
        return "";
    return long_option_name_of("--" + token.substr(1));
}

// 在已知长选项中找相近的名字（互为前缀且至少 3 个字符相同）
inline std::string suggest_long_option(const std::vector<std::string> &names,
                                       const std::string &name)
{
    if (name.size() < 3)
        return "";
    std::string best;
    for (const auto &candidate : names)
    {
        size_t common = 0;
        while (common < candidate.size() && common < name.size() &&
               candidate[common] == name[common])
            ++common;
        if (common < 3)
            continue;
        if (candidate.size() != common && name.size() != common)
            continue; // 必须一个是另一个的前缀
        if (best.empty() || candidate.size() < best.size())
            best = candidate;
    }
    return best;
}

// 末尾是否为中文句末标点（UTF-8 下均为 3 字节）
inline bool ends_with_cjk_sentence_end(const std::string &text)
{
    if (text.size() < 3)
        return false;
    const std::string tail = text.substr(text.size() - 3);
    return tail == "？" || tail == "。" || tail == "！";
}

// 未知参数报错的补充建议（无建议时返回空串）。bad_opt 为 getopt 的 optopt 值：参数名
// 不加 opt 前缀，避免与 getopt_compat.h 的全局 optopt 同名（MSVC C4459）
inline std::string bad_option_advice(const std::string &token, int bad_opt,
                                     const std::vector<std::string> &long_names)
{
    if (bad_opt > 0 && bad_opt < 128)
    {
    // 短选项：只有确实像长选项（- 后跟着一个词）时才提示，避免无意义建议
        const std::string name = single_dash_long_name(token, bad_opt);
        if (name.empty())
            return "";
        const std::string suggestion = suggest_long_option(long_names, name);
        if (!suggestion.empty())
            return "；长选项需要两个连字符，是否想输入 '--" + suggestion + "'？";
        if (name.size() >= 3)
            return "；若本意是长选项，需要两个连字符（'--" + name + "'）";
        return "";
    }

    const std::string name = long_option_name_of(token);
    if (name.empty())
        return "";
    std::vector<std::string> prefix_matches;
    for (const auto &candidate : long_names)
        if (candidate.size() > name.size() &&
            candidate.compare(0, name.size(), name) == 0)
            prefix_matches.push_back(candidate);
    if (prefix_matches.size() >= 2)
    {
        std::string list;
        for (size_t i = 0; i < prefix_matches.size() && i < 4; ++i)
            list += std::string(i ? "、" : "") + "--" + prefix_matches[i];
        if (prefix_matches.size() > 4)
            list += " 等";
        return "；该缩写有歧义（可能是 " + list + "），请写出完整的长选项名。";
    }
    const std::string suggestion = suggest_long_option(long_names, name);
    if (!suggestion.empty())
        return "；是否想输入 '--" + suggestion + "'？";
    return "";
}

// 定位出错参数原文：bad_opt 为出错的短选项字符（0 表示长选项），即 getopt 的 optopt；
// scan_index 为 getopt 的 optind。两者都按值传入，参数名不加 opt 前缀以免遮蔽
// getopt_compat.h 的全局变量（MSVC C4459）。
// 短选项簇里未知字符不是最后一个时 optind 仍指向该簇，是最后一个时已指向下一个参数，
// 故两个位置都要看
inline std::string locate_bad_option(char **argv, int argc, int scan_index, int bad_opt)
{
    const auto token_at = [&](int idx) -> std::string {
        return (idx >= 0 && idx < argc) ? std::string(argv[idx]) : std::string();
    };

    if (bad_opt > 0 && bad_opt < 128)
    {
        for (int idx : {scan_index, scan_index - 1})
        {
            const std::string candidate = token_at(idx);
            if (!single_dash_long_name(candidate, bad_opt).empty())
                return candidate;
        }
        return std::string("-") + static_cast<char>(bad_opt);
    }
    return token_at(scan_index - 1);
}

// 长选项表：新增参数在此加一项并在 app::run 的 switch 中处理（文件作用域：展开多字符
// 短选项时要知道哪些长选项带取值）
const struct option kLongOptions[] = {
    {"update",     no_argument,       nullptr, 'U'},
    {"markdown",   no_argument,       nullptr, 'M'},
    {"latex",      no_argument,       nullptr, 'L'},
    {"tag",        required_argument, nullptr, OPT_TAG},
    {"difficulty", required_argument, nullptr, OPT_DIFFICULTY},
    {"output",     required_argument, nullptr, OPT_OUTPUT},
    {"type",       required_argument, nullptr, OPT_TYPE},
    {"lang",       required_argument, nullptr, OPT_LANG},
    {"tags",       no_argument,       nullptr, OPT_TAGS},
    {"no-toc-links",         no_argument,       nullptr, OPT_NO_TOC_LINKS},
    {"toc-backlinks",        no_argument,       nullptr, OPT_TOC_BACKLINKS},
    {"no-show-source-tags",  no_argument,       nullptr, OPT_NO_SHOW_SOURCE_TAGS},
    {"show-algorithm-tags",  no_argument,       nullptr, OPT_SHOW_ALGORITHM_TAGS},
    {"show-difficulty-tags", no_argument,       nullptr, OPT_SHOW_DIFFICULTY_TAGS},
    {"show-contents-difficulty-tags", no_argument, nullptr, OPT_SHOW_CONTENTS_DIFFICULTY_TAGS},
    {"paginate",             no_argument,       nullptr, OPT_PAGINATE},
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
    {"clean-all",            no_argument,       nullptr, OPT_CLEAN_ALL},
    {"clean-images",         no_argument,       nullptr, OPT_CLEAN_IMAGES},
    {"clean-problems",       no_argument,       nullptr, OPT_CLEAN_PROBLEMS},
    {"clean-fonts",          no_argument,       nullptr, OPT_CLEAN_FONTS},
    {"new-download",         no_argument,       nullptr, OPT_NEW_DOWNLOAD},
    {"compile",              no_argument,       nullptr, OPT_COMPILE},
    {"cookie",               required_argument, nullptr, OPT_COOKIE},
    {"cookie-string",        required_argument, nullptr, OPT_COOKIE_STRING},
    {"with-solutions",       no_argument,       nullptr, OPT_WITH_SOLUTIONS},
    {"article",              required_argument, nullptr, OPT_ARTICLE},
    {"local",                required_argument, nullptr, OPT_LOCAL},
    {"doc-only",             no_argument,       nullptr, OPT_DOC_ONLY},
    {"article-source",      required_argument, nullptr, OPT_ARTICLE_SOURCE},
    {"max-solutions",       required_argument, nullptr, OPT_MAX_SOLUTIONS},
    {"request-delay",        required_argument, nullptr, OPT_REQUEST_DELAY},
    {"no-delay-auto-scale",  no_argument,       nullptr, OPT_NO_DELAY_AUTO_SCALE},
    {"solution-ttl",         required_argument, nullptr, OPT_SOLUTION_TTL},
    {"article-ttl",          required_argument, nullptr, OPT_ARTICLE_TTL},
    {"rate-limit-wait",      required_argument, nullptr, OPT_RATE_LIMIT_WAIT},
    {"allow-partial",        no_argument,       nullptr, OPT_ALLOW_PARTIAL},
    {"refresh-solutions",    no_argument,       nullptr, OPT_REFRESH_SOLUTIONS},
    {"refresh-articles",     no_argument,       nullptr, OPT_REFRESH_ARTICLES},
    {"clean-solutions",      no_argument,       nullptr, OPT_CLEAN_SOLUTIONS},
    {"clean-articles",       no_argument,       nullptr, OPT_CLEAN_ARTICLES},
    {"solutions-only",       no_argument,       nullptr, OPT_SOLUTIONS_ONLY},
    {"articles-only-download", no_argument,    nullptr, OPT_ARTICLES_ONLY_DOWNLOAD},
    {"solution-placement",  required_argument, nullptr, OPT_SOLUTION_PLACEMENT},
    {"no-problem-to-solution-link", no_argument, nullptr, OPT_NO_PROBLEM_TO_SOLUTION_LINK},
    {"no-solution-to-problem-link", no_argument, nullptr, OPT_NO_SOLUTION_TO_PROBLEM_LINK},
    {"no-solution-toc",     no_argument,       nullptr, OPT_NO_SOLUTION_TOC},
    {"no-article-meta",     no_argument,       nullptr, OPT_NO_ARTICLE_META},
    {"yes",                  no_argument,       nullptr, 'y'},
    {"help",       no_argument,       nullptr, 'h'},
    {"version",    no_argument,       nullptr, 'V'},
    {nullptr,      0,                 nullptr, 0},
};

// 是否为需要取值的长选项（含无歧义前缀缩写）；--output=xxx 形式不单独占参数，按不需取值处理
inline bool long_option_takes_value(const std::string &token)
{
    if (token.size() < 3 || token.compare(0, 2, "--") != 0 ||
        token.find('=') != std::string::npos)
        return false;
    const std::string name = token.substr(2);
    // 完全匹配优先（与 getopt_long 一致）
    for (const struct option *o = kLongOptions; o->name != nullptr; ++o)
        if (name == o->name)
            return o->has_arg == required_argument;
    // 前缀缩写：所有候选都要取值时才算
    bool found = false;
    for (const struct option *o = kLongOptions; o->name != nullptr; ++o)
    {
        if (std::string(o->name).compare(0, name.size(), name) != 0)
            continue;
        found = true;
        if (o->has_arg != required_argument)
            return false;
    }
    return found;
}

// 多字符短选项（-CIMG / -CP / -RD）不是 getopt 支持的写法（会被当成 -C -I -M -G 的选项簇），
// 故先替换成等价长选项；"--" 之后的位置参数与带值长选项的取值都不替换，否则
// `--output -RD` 会把输出文件名变成 `--new-download`
inline void expand_multichar_short_options(std::vector<std::string> &args_utf8)
{
    static const struct
    {
        const char *short_form;
        const char *long_form;
    } kAliases[] = {
        {"-CIMG", "--clean-images"},
        {"-CS", "--clean-solutions"},
        {"-CA", "--clean-articles"},
        {"-CF", "--clean-fonts"},
        {"-CP", "--clean-problems"},
        {"-RD", "--new-download"},
    };

    for (size_t i = 0; i < args_utf8.size(); ++i)
    {
        std::string &arg = args_utf8[i];
        if (arg == "--")
            break; // "--" 之后全部是位置参数
        if (i > 0 && long_option_takes_value(args_utf8[i - 1]))
            continue; // 本参数是上一个选项的取值
        for (const auto &alias : kAliases)
        {
            if (arg == alias.short_form)
            {
                arg = alias.long_form;
                break;
            }
        }
    }
}

// 题解 / 文章抓取流程：计划（只读缓存）→ 延时与系数 → 风险分级确认 → 逐题串行抓取
// （--article 与题解共用同一套流程）；返回值即退出码，proceed 为 false 表示不再导出
struct SolutionRun
{
    int exit_code = 0;
    bool proceed = true;
};

SolutionRun run_solutions(const Options &options,
                          const luogu::ProblemSelection &selection,
                          luogu::SolutionBundle &bundle,
                          luogu::ArticleBundle &articles,
                          solution::CrawlStats &stats,
                          bool need_cookie)
{
    SolutionRun run;
    // 只下载文章（--article，没有题解功能参与）：文案与登录态要求都按文章处理
    const bool articles_only_run = !options.articles.empty() &&
                                   !options.with_solutions && !options.solutions_only;
    const char *subject = articles_only_run ? "文章" : "题解";

    // Cookie 只在洛谷原站请求上使用，保存站等第三方域名一律不带
    std::string cookie_error;
    std::string cookie_warnings;
    if (!options.cookie_string.empty())
    {
        if (!crawler::gate_set_cookie_string(options.cookie_string, cookie_error))
        {
            printError(cookie_error);
            run.exit_code = 1;
            run.proceed = false;
            return run;
        }
    }
    else if (!options.cookie_file.empty())
    {
        if (!crawler::gate_load_cookies(
                luogu::compat::path_from_utf8(options.cookie_file), cookie_error,
                &cookie_warnings))
        {
            printError(cookie_error);
            run.exit_code = 1;
            run.proceed = false;
            return run;
        }
    }
    else
    {
        // 两种凭据都没给：清掉上一轮 app::run 留在内存与 libcurl Cookie 引擎里的凭据，
        // 否则交互模式（同进程多次调用）下「清空 Cookie」不生效
        crawler::gate_clear_cookies();
    }
    if (!cookie_warnings.empty())
        std::fputs(cookie_warnings.c_str(), stdout);
    if (!crawler::gate_has_cookies())
    {
        if (need_cookie)
        {
            printError("题解列表接口需要登录态；请登录洛谷后导出 cookies.txt，"
                       "并用 --cookie <file> 指定（或使用 --cookie-string）");
            run.exit_code = 1;
            run.proceed = false;
            return run;
        }
        // 只下载文章：Cookie 可选
        std::printf("未提供 Cookie：文章接口通常无需登录态；"
                    "若某篇文章不可访问（需要权限），可用 --cookie 指定登录态\n");
    }
    else
    {
        std::printf("已载入 %zu 条 Cookie（来源：%s；Cookie 不会用于保存站等第三方域名）\n",
                    crawler::gate_cookie_count(),
                    crawler::gate_cookie_file_hint().c_str());
    }

    solution::TaskOptions task;
    // 默认 auto：缓存优先 + 两个站点轮流分配、并行抓取
    if (options.article_source == "save")
        task.source = solution::Source::Save;
    else if (options.article_source == "official")
        task.source = solution::Source::Official;
    else
        task.source = solution::Source::Auto;
    task.article_lids = options.articles; // --article：按文章编号下载
    task.max_articles = options.max_solutions;
    task.list_ttl_days = options.solution_ttl;
    task.article_ttl_days = options.article_ttl;
    task.refresh_solutions = options.refresh_solutions;
    task.refresh_articles = options.refresh_articles;
    task.allow_partial = options.allow_partial;

    if (task.source == solution::Source::Save)
        std::printf("%s正文来源：洛谷保存站（第三方镜像，内容可能滞后或缺失；"
                    "该来源不发送任何 Cookie）\n", subject);
    else if (task.source == solution::Source::Official)
        std::printf("%s正文来源：洛谷原站\n", subject);
    else
        std::printf("%s正文来源：auto（缓存优先；未命中的在原站与保存站之间轮流分配、并行抓取）\n",
                    subject);

    crawler::GateConfig gate_config;
    if (!options.request_delay.empty() &&
        !crawler::parse_delay_spec(options.request_delay, gate_config.delay,
                                   cookie_error))
    {
        printError(cookie_error);
        run.exit_code = 1;
        run.proceed = false;
        return run;
    }
    gate_config.auto_scale = !options.no_delay_auto_scale;
    gate_config.rate_limit_wait_sec = options.rate_limit_wait;
    // 计划阶段的列表请求走原站通道，保留交互式等待与重试
    gate_config.interactive_retry = true;

    // 计划阶段（第一遍只读缓存）先给闸门一个初步延时系数，保证「按需获取题解列表」也受
    // --request-delay 控制；随后把缺失/过期的列表抓回来，风险确认看到的篇数才是精确值
    solution::Plan plan;
    solution::PlanResult plan_result =
        solution::make_plan(selection.problems, task, false, plan);
    crawler::gate_configure_channel(crawler::Channel::Official, gate_config,
                                    plan.total_requests);
    crawler::gate_configure_channel(crawler::Channel::Save, gate_config,
                                    plan.total_requests);

    // 列表请求本就要发（属于 N + P 中的 P），抓回来才能得到精确篇数
    plan_result = solution::make_plan(selection.problems, task, true, plan);
    if (plan_result.status == solution::PlanStatus::Stopped)
    {
        std::printf("已按你的选择取消；本次未抓取任何题解正文。\n");
        run.exit_code = 0;
        run.proceed = false;
        return run;
    }
    if (plan_result.status == solution::PlanStatus::RateLimited)
    {
        printError("获取题解列表时被限流：" + plan_result.error);
        printError("已停止本次抓取；缓存已保留，可稍后重跑续传");
        run.exit_code = 3;
        run.proceed = false;
        return run;
    }
    if (plan_result.status != solution::PlanStatus::Ok)
    {
        printError(plan_result.error);
        run.exit_code = 1;
        run.proceed = false;
        return run;
    }
    // auto 模式下正文由调度器处理限流（临时转给另一站点），故进入并行抓取前把两条通道切到
    // 「命中限流立即返回」；计划阶段的列表请求只能走原站、无备用站点，仍保留闸门内部的重试
    if (task.source == solution::Source::Auto)
    {
        crawler::GateConfig parallel_config = gate_config;
        parallel_config.interactive_retry = false;
        crawler::gate_configure_channel(crawler::Channel::Official, parallel_config,
                                        plan.total_requests);
        crawler::gate_configure_channel(crawler::Channel::Save, parallel_config,
                                        plan.total_requests);
    }

    // 用精确篇数重算延时系数（只会更保守）
    crawler::gate_set_planned_requests(crawler::Channel::Official,
                                       plan.total_requests);
    crawler::gate_set_planned_requests(crawler::Channel::Save,
                                       plan.total_requests);
    // 预计 0 次请求（全部命中缓存）时不打印延时与规模提示
    if (plan.total_requests > 0)
        crawler::gate_print_delay_notice(plan.total_requests);

    prompt::RiskInfo risk = prompt::plan_risk(plan.total_requests, options.yes);
    risk.problems = plan.problems;
    risk.per_problem = options.max_solutions < 0 ? 0 : options.max_solutions;
    risk.per_problem_all = options.max_solutions < 0;
    risk.articles = plan.articles_to_fetch;
    risk.cached_articles = plan.cached_articles;
    risk.solution_to_fetch = plan.solution_to_fetch;
    risk.cached_solutions = plan.cached_solutions;
    risk.standalone_articles =
        static_cast<long long>(plan.standalone_articles.size());
    risk.standalone_to_fetch = plan.standalone_to_fetch;
    risk.cached_standalone = plan.cached_standalone;
    risk.list_requests = plan.list_requests;
    risk.total_requests = plan.total_requests;
    risk.seconds_per_request = crawler::gate_effective_delay_seconds();
    risk.eta_seconds = static_cast<long long>(
        risk.seconds_per_request * static_cast<double>(plan.total_requests));

    const prompt::ConfirmResult confirm = prompt::confirm_risk(risk);
    if (confirm == prompt::ConfirmResult::Cancelled)
    {
        run.exit_code = 0;
        run.proceed = false;
        return run;
    }
    if (confirm == prompt::ConfirmResult::CannotConfirm)
    {
        run.exit_code = 1;
        run.proceed = false;
        return run;
    }

    std::string crawl_error;
    if (!solution::crawl(plan, task, bundle, articles, stats, crawl_error))
    {
        printError(crawl_error);
        run.exit_code = 1;
        run.proceed = false;
        return run;
    }

    printSuccess(std::string(subject) + "抓取完成：" +
                 solution::describe_crawl_stats(stats));
    const std::uintmax_t used = solcache::cache_size();
    if (articles_only_run)
        std::printf("文章缓存占用：%.2f MB（%s）\n",
                    static_cast<double>(used) / (1024.0 * 1024.0),
                    luogu::compat::path_to_utf8(solcache::articles_dir()).c_str());
    else
        std::printf("题解缓存占用：%.2f MB（%s 与 %s）\n",
                    static_cast<double>(used) / (1024.0 * 1024.0),
                    luogu::compat::path_to_utf8(solcache::articles_dir()).c_str(),
                    luogu::compat::path_to_utf8(solcache::solutions_index_path()).c_str());

    if (stats.stopped_by_rate_limit)
    {
        if (!stats.stop_reason.empty())
            printError("因限流中止：" + stats.stop_reason);
        printError("已停止本次抓取；缓存已保留，可稍后重跑续传");
        run.exit_code = 3; // 与用户主动取消（0）区分，便于脚本判断
        run.proceed = false;
        return run;
    }
    if (stats.stopped_by_user)
    {
        std::printf("已按你的选择取消；已抓取的缓存全部保留，可稍后重跑续传。\n");
        run.exit_code = 0;
        run.proceed = false;
        return run;
    }
    return run;
}

// 作用域内把工作目录切到 dir，析构时恢复：保证所有返回路径（含提前 return）都恢复原目录
class ScopedCurrentPath
{
public:
    explicit ScopedCurrentPath(const std::filesystem::path &dir)
    {
        std::error_code ec;
        old_ = std::filesystem::current_path(ec);
        if (ec)
        {
            // 读不到当前目录就宁可不切换（由调用方报错）
            error_ = ec;
            return;
        }
        ec.clear();
        std::filesystem::current_path(dir, ec);
        error_ = ec;
        changed_ = !ec;
    }
    ~ScopedCurrentPath()
    {
        if (changed_)
        {
            std::error_code restore_ec;
            std::filesystem::current_path(old_, restore_ec); // 尽力而为
        }
    }
    ScopedCurrentPath(const ScopedCurrentPath &) = delete;
    ScopedCurrentPath &operator=(const ScopedCurrentPath &) = delete;

    // 切换是否成功；失败时工作目录未被改变
    bool ok() const { return !error_; }
    const std::error_code &error() const { return error_; }

private:
    std::filesystem::path old_;
    std::error_code error_;
    bool changed_ = false;
};

// --compile：-L 导出成功后依次执行 `latexmk --xelatex <输出文件名>.tex` 与 `latexmk -c`
// （只删可再生的中间文件，保留 .pdf/.tex），工作目录切到 .tex 所在目录。命令按参数数组经
// compat::run_command_utf8 直接执行、不过 shell，--output 的文件名含 shell 元字符也只当文件名。
// 返回退出码（清理失败只提示，不改退出码）。
int compile_latex_document(const std::filesystem::path &tex_path)
{
    const std::filesystem::path dir =
        tex_path.has_parent_path() ? tex_path.parent_path()
                                   : std::filesystem::path(".");
    ScopedCurrentPath cwd(dir);
    if (!cwd.ok())
    {
        printError("参数 --compile：无法切换到输出文件所在目录 '" +
                   luogu::compat::path_to_utf8(dir) + "'：" + cwd.error().message());
        return 1;
    }

    const std::string raw_name = luogu::compat::path_to_utf8(tex_path.filename());
    // 以 '-' 开头的文件名会被 latexmk 当成选项（选项注入），加 "./" 前缀规避
    const std::string file_name =
        (!raw_name.empty() && raw_name[0] == '-') ? "./" + raw_name : raw_name;
    const std::vector<std::string> compile_argv = {"latexmk", "--xelatex", file_name};
    // 仅用于打印，不执行（引号只为便于手动复制）
    const std::string command = "latexmk --xelatex \"" + file_name + "\"";
    std::printf("正在编译 LaTeX 文档：%s\n", command.c_str());
    std::fflush(stdout);
    std::string run_error;
    const int status = luogu::compat::run_command_utf8(compile_argv, run_error);

    // latexmk 启动不了：不必再清理（目录由 guard 恢复）
    if (status < 0)
    {
        printError("无法执行 latexmk（" + run_error +
                   "）；请确认已安装 LaTeX 与 latexmk");
        return 1;
    }

    // 无论编译成败都执行 latexmk -c 清理中间文件
    const std::vector<std::string> clean_argv = {"latexmk", "-c", file_name};
    const std::string clean_command = "latexmk -c \"" + file_name + "\"";
    std::printf("清理中间文件：%s\n", clean_command.c_str());
    std::fflush(stdout);
    std::string clean_run_error;
    const int clean_status = luogu::compat::run_command_utf8(clean_argv, clean_run_error);

    if (status != 0)
    {
        printError("latexmk 编译失败" +
                   (status > 0 ? "（退出码 " + std::to_string(status) + "）" : "") +
                   "；请确认已安装 LaTeX 与 latexmk；中间文件已按 --compile 的约定"
                   "用 latexmk -c 清理，如需保留编译日志请手动执行：" + command);
        return 1;
    }

    if (clean_status != 0)
    {
        // 编译已成功、PDF 已生成：清理失败只提示，不改退出码
        printError("latexmk -c 清理中间文件失败" +
                   (clean_status > 0 ? "（退出码 " + std::to_string(clean_status) + "）" : "") +
                   "；可手动执行：" + clean_command);
    }

    std::filesystem::path pdf_path = tex_path;
    pdf_path.replace_extension(".pdf");
    std::error_code pdf_ec;
    if (std::filesystem::exists(pdf_path, pdf_ec) && !pdf_ec)
        printSuccess("已生成 PDF：" + luogu::compat::path_to_utf8(pdf_path));
    else
        printSuccess("latexmk 编译完成");
    return 0;
}

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

// 同一进程内多次执行 app::run 前复位 getopt 全局解析状态（optind = 0 让系统 getopt
// 重新初始化；自带兼容实现另需复位参数重排状态，见 getopt_reset 的说明）。
static void reset_getopt_state()
{
#if defined(LUOGU_FORCE_COMPAT_GETOPT) || defined(_WIN32)
    getopt_reset();
#else
    optind = 0;
    optarg = nullptr;
    optopt = '?';
#endif
}

// libcurl 全局初始化只做一次且不配对 cleanup：request_gate 的静态 easy handle 只创建、从不
// 释放，若每次 app::run 都 cleanup，它们会被再次使用（libcurl 禁止的 UB）；资源随进程退出回收。
bool ensure_curl_global_init()
{
    static std::mutex init_mutex;
    static bool initialized = false;
    std::lock_guard<std::mutex> lock(init_mutex);
    if (initialized)
        return true;
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
        return false; // 失败不缓存，下次调用可重试
    initialized = true;
    return true;
}

int app::run(const std::vector<std::string> &args_input)
{
    reset_getopt_state();
    // 复位上一次留下的进程级静态抓取状态（通道延时、限流等待、「已停止」标记），
    // 否则会把「已停止 / 已被限流」带进本次运行
    crawler::gate_reset_state();

    // Windows 下 CRT 的 main(char**) 参数按 ANSI 代码页转换，统一转成 UTF-8 后构造参数表
    std::vector<std::string> args_utf8 = args_input;
    expand_multichar_short_options(args_utf8);
    std::vector<char *> args;
    args.reserve(args_utf8.size());
    for (auto &a : args_utf8)
        args.push_back(const_cast<char *>(a.c_str()));
    const int arg_count = static_cast<int>(args.size());
    char **const arg_vector = args.data();

    std::vector<std::string> long_option_names;
    for (const struct option *o = kLongOptions; o->name != nullptr; ++o)
        long_option_names.push_back(o->name);

    Options options;
    int opt;
    // 参数互斥校验用的计数：-V 必须单独使用，清除类参数只能彼此组合
    int option_count = 0;
    int non_clean_option_count = 0;
    // 最后给出的「多值选项」：1 = --pid，2 = --pid-range，3 = --article；空格分隔的
    // 多余裸参数按最后给出的那个选项处理，这样 --pid 与 --article 不会互抢后续值
    int last_multi_option = 0;
    // 短选项串以 ':' 开头：getopt 不打印英文提示，由 '?' / ':' 分支输出中文错误
    while ((opt = getopt_long(arg_count, arg_vector, ":UMLhVCy", kLongOptions, nullptr)) != -1)
    {
        ++option_count;
        if (opt != 'C' && opt != OPT_CLEAN_ALL && opt != OPT_CLEAN_IMAGES &&
            opt != OPT_CLEAN_PROBLEMS && opt != OPT_CLEAN_FONTS &&
            opt != OPT_CLEAN_SOLUTIONS && opt != OPT_CLEAN_ARTICLES)
            ++non_clean_option_count;
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
        case 'C':
            options.clean_all = true;
            break;
        case OPT_TAG:
            // 整体保留，是单个标签名还是按空格拆成多个由 select_problems 判断
            if (optarg == nullptr || optarg[0] == '\0')
            {
                printError("参数 '--tag' 后缺少标签名称或数字 ID；正确用法：--tag <标签名称或数字 ID>");
                return 1;
            }
            options.filter.tags.push_back(optarg);
            break;
        case OPT_DIFFICULTY:
        {
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
        case OPT_TAGS:
            options.list_tags = true;
            break;
        case OPT_CLEAN_ALL:
            options.clean_all = true;
            break;
        case OPT_CLEAN_IMAGES:
            options.clean_images = true;
            break;
        case OPT_CLEAN_PROBLEMS:
            options.clean_problems = true;
            break;
        case OPT_CLEAN_FONTS:
            options.clean_fonts = true;
            break;
        case OPT_NEW_DOWNLOAD:
            options.new_download = true;
            break;
        case OPT_COMPILE:
            options.compile_latex = true;
            break;
        case OPT_COOKIE:
            if (optarg == nullptr || optarg[0] == '\0')
            {
                printError("参数 '--cookie' 后缺少 Cookie 文件路径；正确用法：--cookie <cookies.txt>");
                return 1;
            }
            options.cookie_file = optarg;
            options.solution_option_used = true;
            break;
        case OPT_COOKIE_STRING:
            if (optarg == nullptr || optarg[0] == '\0')
            {
                printError("参数 '--cookie-string' 后缺少 Cookie 串；"
                           "正确用法：--cookie-string \"k=v; k2=v2\"");
                return 1;
            }
            options.cookie_string = optarg;
            options.solution_option_used = true;
            break;
        case OPT_WITH_SOLUTIONS:
            options.with_solutions = true;
            break;
        case OPT_ARTICLE:
        {
            // 支持空格分隔的多个文章编号；空值视为参数缺失
            const std::vector<std::string> tokens =
                split_whitespace(optarg ? optarg : "");
            if (tokens.empty() || (optarg && optarg[0] == '-'))
            {
                printError("参数 '--article' 后缺少文章编号；"
                           "正确用法：--article <文章编号>（如 p7fsb45w，可多个，"
                           "空格分隔或重复 --article）");
                return 1;
            }
            for (const auto &tok : tokens)
            {
                const std::string lid = to_lower_ascii(tok);
                if (!solution::valid_lid(lid))
                {
                    printError("参数 '--article' 的值 '" + tok +
                               "' 不是合法的文章编号；文章编号为 6~32 位小写字母或"
                               "数字（如 p7fsb45w），可在文章页地址 /article/<编号> "
                               "中找到");
                    return 1;
                }
                if (std::find(options.articles.begin(), options.articles.end(),
                              lid) == options.articles.end())
                    options.articles.push_back(lid); // 重复编号只下载一次
            }
            last_multi_option = 3;
            break;
        }
        case OPT_LOCAL:
        {
            if (optarg == nullptr || optarg[0] == '\0' || optarg[0] == '-')
            {
                printError("参数 '--local' 后缺少本地文件路径；"
                           "正确用法：--local <文件地址>（如 --local 笔记.md）");
                return 1;
            }
            if (!options.local_file.empty())
            {
                printError("参数 '--local' 只能指定一个本地文件（已指定 '" +
                           options.local_file + "'）；如需转写多个文件，请分多次执行");
                return 1;
            }
            options.local_file = optarg;
            break;
        }
        case OPT_DOC_ONLY:
            options.doc_only = true;
            break;
        case OPT_SOLUTIONS_ONLY:
            options.solutions_only = true;
            break;
        case OPT_ARTICLES_ONLY_DOWNLOAD:
            options.articles_only_download = true;
            break;
        case OPT_ARTICLE_SOURCE:
        {
            const std::string value = optarg ? optarg : "";
            if (value != "official" && value != "save" && value != "auto")
            {
                printError("参数 '--article-source' 的值 '" + value +
                           "' 不是合法的来源；正确用法："
                           "--article-source <auto|official|save>"
                           "（auto 为默认：缓存优先，未命中的题解在原站与保存站"
                           "之间轮流分配并并行抓取；official 只用洛谷原站；"
                           "save 只用第三方镜像保存站。题解列表恒取洛谷原站）");
                return 1;
            }
            options.article_source = value;
            options.solution_option_used = true;
            break;
        }
        case OPT_MAX_SOLUTIONS:
        {
            const std::string value = to_lower_ascii(optarg ? optarg : "");
            if (value == "all")
            {
                options.max_solutions = -1;
            }
            else
            {
                long parsed = 0;
                if (!parse_positive_int(value, 1, 100000, parsed))
                {
                    printError("参数 '--max-solutions' 的值 '" + std::string(optarg ? optarg : "") +
                               "' 不是合法的篇数；正确用法：--max-solutions <n|all>"
                               "（n 为正整数，all 表示该题全部题解）");
                    return 1;
                }
                options.max_solutions = static_cast<int>(parsed);
            }
            options.solution_option_used = true;
            break;
        }
        case OPT_REQUEST_DELAY:
        {
            crawler::DelaySpec spec;
            std::string err;
            if (!crawler::parse_delay_spec(optarg ? optarg : "", spec, err))
            {
                printError(err);
                return 1;
            }
            options.request_delay = optarg ? optarg : "";
            options.solution_option_used = true;
            break;
        }
        case OPT_NO_DELAY_AUTO_SCALE:
            options.no_delay_auto_scale = true;
            options.solution_option_used = true;
            break;
        case OPT_SOLUTION_TTL:
        case OPT_ARTICLE_TTL:
        {
            const std::string name =
                (opt == OPT_SOLUTION_TTL) ? "--solution-ttl" : "--article-ttl";
            long parsed = 0;
            if (!parse_positive_int(optarg ? optarg : "", 0, 36500, parsed))
            {
                printError("参数 '" + name + "' 的值 '" +
                           std::string(optarg ? optarg : "") +
                           "' 不是合法的天数；正确用法：" + name + " <days>"
                           "（不小于 0 的整数；不指定时为无限，即一律优先使用缓存；"
                           "0 表示每次都发 ETag 条件请求）");
                return 1;
            }
            if (opt == OPT_SOLUTION_TTL)
                options.solution_ttl = static_cast<int>(parsed);
            else
                options.article_ttl = static_cast<int>(parsed);
            options.solution_option_used = true;
            break;
        }
        case OPT_RATE_LIMIT_WAIT:
        {
            long parsed = 0;
            if (!parse_positive_int(optarg ? optarg : "", 0, 86400, parsed))
            {
                printError("参数 '--rate-limit-wait' 的值 '" +
                           std::string(optarg ? optarg : "") +
                           "' 不是合法的秒数；正确用法：--rate-limit-wait <seconds>"
                           "（不小于 0 的整数，默认 120；0 表示检测到限流直接停止）");
                return 1;
            }
            options.rate_limit_wait = static_cast<int>(parsed);
            options.solution_option_used = true;
            break;
        }
        case OPT_ALLOW_PARTIAL:
            options.allow_partial = true;
            options.solution_option_used = true;
            break;
        case OPT_REFRESH_SOLUTIONS:
            options.refresh_solutions = true;
            options.solution_option_used = true;
            break;
        case OPT_REFRESH_ARTICLES:
            options.refresh_articles = true;
            options.solution_option_used = true;
            break;
        case OPT_CLEAN_SOLUTIONS:
            options.clean_solutions = true;
            break;
        case OPT_CLEAN_ARTICLES:
            options.clean_articles = true;
            break;
        case OPT_SOLUTION_PLACEMENT:
        {
            const std::string value = optarg ? optarg : "";
            if (value != "document-end" && value != "per-problem")
            {
                printError("参数 '--solution-placement' 的值 '" + value +
                           "' 不是合法位置；正确用法："
                           "--solution-placement <document-end|per-problem>"
                           "（默认 document-end，即题解统一置于文档最后）");
                return 1;
            }
            options.solution_placement = value;
            options.solution_option_used = true;
            break;
        }
        case OPT_NO_PROBLEM_TO_SOLUTION_LINK:
            options.no_problem_to_solution_link = true;
            options.solution_option_used = true;
            break;
        case OPT_NO_SOLUTION_TO_PROBLEM_LINK:
            options.no_solution_to_problem_link = true;
            options.solution_option_used = true;
            break;
        case OPT_NO_SOLUTION_TOC:
            options.no_solution_toc = true;
            options.solution_option_used = true;
            break;
        case OPT_NO_ARTICLE_META:
            options.no_article_meta = true;
            options.solution_option_used = true;
            break;
        case 'y':
            options.yes = true;
            options.solution_option_used = true;
            break;
        case OPT_NO_TOC_LINKS:
            options.no_toc_links = true;
            break;
        case OPT_TOC_BACKLINKS:
            options.toc_backlinks = true;
            break;
        case OPT_NO_SHOW_SOURCE_TAGS:
            options.no_show_source_tags = true;
            break;
        case OPT_SHOW_ALGORITHM_TAGS:
            options.show_algorithm_tags = true;
            break;
        case OPT_SHOW_DIFFICULTY_TAGS:
            options.show_difficulty_tags = true;
            break;
        case OPT_SHOW_CONTENTS_DIFFICULTY_TAGS:
            options.show_contents_difficulty_tags = true;
            break;
        case OPT_PAGINATE:
            options.paginate = true;
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
            last_multi_option = 1;
            break;
        }
        case OPT_PID_RANGE:
        {
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
            last_multi_option = 2;
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
            // 未知参数：用 locate_bad_option 定位出错参数的原文（见该函数的说明）
            const std::string token =
                locate_bad_option(arg_vector, arg_count, optind, optopt);
            const std::string advice =
                bad_option_advice(token, optopt, long_option_names);
            printError("未知参数 '" + token + "'（程序没有此参数）" +
                       (advice.empty() ? "，" : advice +
                                                  (ends_with_cjk_sentence_end(advice)
                                                       ? ""
                                                       : "，")) +
                       "请使用 -h, --help 查看帮助信息");
            return 1;
        }
        case ':':
        {
            // 选项后缺少参数值（本程序只有长选项需要取值）
            const std::string token =
                locate_bad_option(arg_vector, arg_count, optind, optopt);
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

    if (options.version)
    {
        if (option_count > 1 || optind < arg_count)
        {
            printError("参数 -V, --version 不能与其他参数同时使用；"
                       "正确用法：luogu-extract -V（或 luogu-extract --version），"
                       "单独执行该参数即可查看项目简介、版本号、版权声明与项目仓库链接");
            return 1;
        }
        printVersion();
        return 0;
    }

    // 清除类参数只能彼此组合（与非清除类参数或位置参数同用即拒绝）
    {
        std::vector<std::string> used_clean;
        if (options.clean_all) used_clean.push_back("-C, --clean-all");
        if (options.clean_images) used_clean.push_back("-CIMG, --clean-images");
        if (options.clean_problems) used_clean.push_back("-CP, --clean-problems");
        if (options.clean_fonts) used_clean.push_back("-CF, --clean-fonts");
        if (options.clean_solutions) used_clean.push_back("-CS, --clean-solutions");
        if (options.clean_articles) used_clean.push_back("-CA, --clean-articles");
        if (!used_clean.empty() &&
            (non_clean_option_count > 0 || optind < arg_count))
        {
            std::string joined;
            for (size_t i = 0; i < used_clean.size(); ++i)
                joined += (i ? "、" : "") + used_clean[i];
            printError("清除缓存的参数（" + joined +
                       "）只能与其他清除缓存的参数（-C、-CIMG、-CP、-CF、-CS、-CA）"
                       "同时使用，不能与其它参数或多余的位置参数一起使用；"
                       "正确用法：luogu-extract -CIMG -CP -CF -CS -CA"
                       "（互相组合，一次清除多类缓存），或单独执行其中一个");
            return 1;
        }
    }

    if (options.help)
    {
        printUsage();
        return options.list_tags ? (print_tag_list() ? 0 : 1) : 0;
    }

    if (options.list_tags)
        return print_tag_list() ? 0 : 1;

    // 清除类参数只操作本地缓存，不访问网络也不需要 libcurl；-C 删掉整个缓存目录，
    // 其余动作按「目录不存在 = 已清空」返回
    if (options.clean_all || options.clean_images || options.clean_problems ||
        options.clean_fonts || options.clean_solutions || options.clean_articles)
    {
        crawler::derror clean_result = crawler::SUCCESS;
        auto run_clean = [&clean_result](bool used, crawler::derror (*action)()) {
            if (!used)
                return;
            const crawler::derror result = action();
            if (result != crawler::SUCCESS && clean_result == crawler::SUCCESS)
                clean_result = result;
        };
        run_clean(options.clean_all, crawler::clean_all);
        run_clean(options.clean_images, crawler::clean_images);
        run_clean(options.clean_problems, crawler::clean_problems);
        run_clean(options.clean_fonts, crawler::clean_fonts);
        run_clean(options.clean_solutions, solcache::clean_solutions);
        run_clean(options.clean_articles, solcache::clean_articles);
        return clean_result == crawler::SUCCESS ? 0 : 1;
    }

    if (options.markdown && options.latex)
    {
        printError("参数 -M 与 -L 不能同时使用；请分两次导出（-M 导出 Markdown，-L 导出 LaTeX）");
        return 1;
    }

    const bool solutions_enabled = options.with_solutions || options.solutions_only ||
                                   options.articles_only_download;
    const bool articles_enabled = !options.articles.empty();
    const bool articles_only_run = articles_enabled && !options.with_solutions &&
                                   !options.solutions_only;
    const bool download_enabled = solutions_enabled || articles_enabled;
    if (options.solutions_only && options.articles_only_download)
    {
        printError("参数 --solutions-only 与 --articles-only-download 不能同时使用；"
                   "--solutions-only 只导出题解（不导出题面），"
                   "--articles-only-download 只抓取并缓存、不导出任何文件");
        return 1;
    }
    if (!download_enabled && options.solution_option_used)
    {
        printError("题解与文章相关参数（--cookie、--cookie-string、--solution-*、"
                   "--article-*、--max-solutions、--refresh-*、--no-solution-*、"
                   "-y/--yes 等）需要与 --with-solutions（或 --solutions-only / "
                   "--articles-only-download / --article）一起使用；正确用法："
                   "luogu-extract -L --cookie cookies.txt --with-solutions ..."
                   "（未启用题解与文章功能时程序完全不碰它们）");
        return 1;
    }
    if (solutions_enabled)
    {
        // --with-solutions / --solutions-only 需与 -M 或 -L 同用（--articles-only-download 例外）
        if (!options.articles_only_download && !options.markdown && !options.latex)
        {
            printError("参数 --with-solutions / --solutions-only 需要与 -M（导出 "
                       "Markdown）或 -L（导出 LaTeX）一起使用；若只想抓取并缓存题解，"
                       "请改用 --articles-only-download");
            return 1;
        }
    }
    if (articles_enabled && !options.articles_only_download &&
        !options.markdown && !options.latex &&
        !(options.with_solutions || options.solutions_only))
    {
        printError("参数 --article 需要与 -M（导出 Markdown）或 -L（导出 LaTeX）"
                   "一起使用；若只想抓取并缓存文章，请改用 --articles-only-download");
        return 1;
    }
    // 题解列表接口需要登录态；只下载文章时 Cookie 可选
    const bool need_cookie = solutions_enabled && !articles_only_run;
    if (need_cookie && options.cookie_file.empty() && options.cookie_string.empty())
    {
        printError("题解列表接口需要登录态；请登录洛谷后导出 cookies.txt，"
                   "并用 --cookie <file> 指定（或使用 --cookie-string）。"
                   "题解功能必须带登录态，未提供 Cookie 时无法启用");
        return 1;
    }
    if (!options.cookie_file.empty() && !options.cookie_string.empty())
    {
        printError("参数 --cookie 与 --cookie-string 不能同时使用；"
                   "请只保留其中一个（--cookie 指定 Netscape 格式的 cookies.txt，"
                   "--cookie-string 直接给出 Cookie 串）");
        return 1;
    }

    const bool local_mode = !options.local_file.empty();
    if (options.doc_only && !local_mode)
    {
        printError("参数 --doc-only 仅与 --local（转写本地 Markdown 文件）一起使用；"
                   "正确用法：luogu-extract -L --local <文件地址> --doc-only");
        return 1;
    }
    if (local_mode)
    {
        if (!options.latex)
        {
            printError("参数 --local 仅在使用 -L（导出 LaTeX）时可用；"
                       "本地 Markdown 只能转写为 LaTeX（-M 导出 Markdown 不涉及转写）");
            return 1;
        }
        std::vector<std::string> local_conflicts;
        if (options.update) local_conflicts.push_back("-U, --update");
        if (solutions_enabled)
            local_conflicts.push_back("--with-solutions / --solutions-only / "
                                      "--articles-only-download");
        if (articles_enabled) local_conflicts.push_back("--article");
        if (!options.filter.tags.empty()) local_conflicts.push_back("--tag");
        if (!options.filter.difficulties.empty()) local_conflicts.push_back("--difficulty");
        if (!options.filter.types.empty()) local_conflicts.push_back("--type");
        if (!options.filter.pids.empty()) local_conflicts.push_back("--pid");
        if (!options.filter.pid_ranges.empty()) local_conflicts.push_back("--pid-range");
        if (!local_conflicts.empty())
        {
            std::string joined;
            for (size_t i = 0; i < local_conflicts.size(); ++i)
                joined += (i ? "、" : "") + local_conflicts[i];
            printError("参数 --local 不能与下载题目、题解、文章的功能（" + joined +
                       "）同时使用；--local 只把指定的本地文件转写为 LaTeX，"
                       "不下载题目、题解与文章，也不读写题目与文章缓存");
            return 1;
        }
        if (options.doc_only && options.paginate)
        {
            printError("参数 --doc-only 不能与 --paginate 同时使用；"
                       "--doc-only 不输出封面与目录、整篇连贯输出，没有可另起一页的章节");
            return 1;
        }
        // 必须存在且是普通文件（读取时还会再校验一次编码与可读性）
        std::error_code file_ec;
        const std::filesystem::path local_path =
            luogu::compat::path_from_utf8(options.local_file);
        if (!std::filesystem::exists(local_path, file_ec) || file_ec)
        {
            printError("参数 --local 指定的文件 '" + options.local_file +
                       "' 不存在；请检查文件路径");
            return 1;
        }
        if (std::filesystem::is_directory(local_path, file_ec) && !file_ec)
        {
            printError("参数 --local 指定的 '" + options.local_file +
                       "' 是目录；请指定一个 Markdown 文本文件");
            return 1;
        }
    }

    // 仅 -L 支持的参数与 -M 同用属于参数错误，拒绝执行（三个 --show/--no-show-*-tags 对 -M 有效）
    if (options.markdown && !options.latex)
    {
        std::vector<std::string> latex_only;
        if (options.no_toc_links) latex_only.push_back("--no-toc-links");
        if (options.toc_backlinks) latex_only.push_back("--toc-backlinks");
        if (options.show_contents_difficulty_tags) latex_only.push_back("--show-contents-difficulty-tags");
        if (options.paginate) latex_only.push_back("--paginate");
        if (!options.font_cover.empty()) latex_only.push_back("--set-font-cover-page");
        if (!options.font_body_zh.empty()) latex_only.push_back("--set-font-body-zh-CN");
        if (!options.font_body_en.empty()) latex_only.push_back("--set-font-body-en-US");
        if (!options.font_body_codes.empty()) latex_only.push_back("--set-font-body-codes");
        if (!options.font_title_zh.empty()) latex_only.push_back("--set-font-title-zh-CN");
        if (!options.font_title_en.empty()) latex_only.push_back("--set-font-title-en-US");
        if (options.no_bilibili_link) latex_only.push_back("--no-bilibili-link");
        // 两个跳转开关对 -M 也有效，但 --no-solution-toc 仅 -L
        if (options.no_solution_toc) latex_only.push_back("--no-solution-toc");
        if (options.new_download) latex_only.push_back("--new-download");
        if (options.compile_latex) latex_only.push_back("--compile");
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

    // --compile 与 -M、-U、--articles-only-download 等组合时拒绝执行
    if (options.compile_latex && !options.latex)
    {
        printError("参数 --compile 仅在 -L（导出 LaTeX）时有效；"
                   "请与 -L 一起使用（导出完成后会自动执行 "
                   "latexmk --xelatex <输出文件名>.tex，随后执行 latexmk -c）");
        return 1;
    }

    // --pid / --pid-range / --tag / --difficulty 支持「空格分隔多个值」：后面的裸参数按
    // 这些选项的后续值处理；--local 只转写指定文件，多余的参数按错误处理
    const bool bare_args_allowed = !local_mode &&
                                   (options.markdown || options.latex ||
                                    options.with_solutions || options.solutions_only ||
                                    options.articles_only_download ||
                                    !options.articles.empty());
    if (optind < arg_count)
    {
        if (!bare_args_allowed)
        {
            printError("多余的参数 '" + std::string(arg_vector[optind]) +
                       "'：该参数不是任何选项的值；请检查命令行，"
                       "或使用 -h, --help 查看帮助信息");
            printUsage();
            return 1;
        }

        // 剩余裸参数按「最后给出的多值选项」归属：--article → 文章编号（与 --pid 同用不会
        // 互抢）；用过 --pid → 题号；用过 --pid-range → 题号范围；否则按难度解析，失败则当 --tag 值
        if (last_multi_option == 3 && !options.articles.empty())
        {
            for (int i = optind; i < arg_count; ++i)
            {
                const std::string lid = to_lower_ascii(arg_vector[i]);
                if (!solution::valid_lid(lid))
                {
                    printError("参数 '--article' 的值 '" +
                               std::string(arg_vector[i]) +
                               "' 不是合法的文章编号；文章编号为 6~32 位小写字母或"
                               "数字（如 p7fsb45w），可在文章页地址 /article/<编号> "
                               "中找到");
                    return 1;
                }
                if (std::find(options.articles.begin(), options.articles.end(),
                              lid) == options.articles.end())
                    options.articles.push_back(lid);
            }
        }
        else if (!options.filter.pids.empty())
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

    if (options.new_download && !options.latex)
    {
        printError("参数 -RD, --new-download 仅在下载题面图片时有效；"
                   "请在使用 -L（导出 LaTeX）的同时使用该参数"
                   "（-M 导出 Markdown 不下载图片，-U 只更新题目列表与标签缓存）");
        return 1;
    }

    if (!options.update && !options.markdown && !options.latex &&
        !options.articles_only_download)
    {
        printError("未指定任何操作；请至少使用 -U（更新缓存）、-M（导出 Markdown）、"
                   "-L（导出 LaTeX）、-C（清空缓存）、--tags（查看标签对照表）或 "
                   "--articles-only-download（只抓取题解与文章缓存）之一，"
                   "并可用 -h, --help 查看帮助信息");
        return 1;
    }

    if (!ensure_curl_global_init())
    {
        printError("初始化 libcurl 失败；请检查网络相关运行库是否安装完整");
        return 1;
    }

    int result = 0;
    // -M / -L 共用的题目信息显示开关（默认显示来源等标签，隐藏算法与难度）
    luogu::DisplayOptions display;
    display.source_tags = !options.no_show_source_tags;
    display.algorithm_tags = options.show_algorithm_tags;
    display.difficulty = options.show_difficulty_tags;

    // -U 可与 -M / -L 同用：只要出现 -U 就先更新缓存再下载题目（与命令行顺序无关）；
    // 更新失败时不继续
    if (options.update)
    {
        result = crawler::update();
        if (result != crawler::SUCCESS)
        {
            printError("缓存更新失败，已停止后续操作");
            return result;
        }
    }

    // 题解与文章抓取（--with-solutions / --article 等）：先选中题目（与后面导出共用结果），
    // 再走「计划 → 风险确认 → 抓取」，最后与题面一起导出；--article 未给筛选参数时只导出文章
    const bool no_problem_filters = options.filter.tags.empty() &&
                                    options.filter.difficulties.empty() &&
                                    options.filter.types.empty() &&
                                    options.filter.pids.empty() &&
                                    options.filter.pid_ranges.empty();
    const bool article_only_document = articles_enabled && no_problem_filters &&
                                       !options.with_solutions && !options.solutions_only;

    luogu::ProblemSelection selection;
    luogu::SolutionBundle solution_bundle;
    luogu::ArticleBundle article_bundle;
    solution::CrawlStats solution_stats;
    luogu::SolutionExportOptions solution_export;
    if (solutions_enabled || articles_enabled)
    {
        if (!article_only_document)
        {
            std::string select_error;
            if (!luogu::select_problems(options.filter, selection.problems,
                                        &selection.resolved_tags, select_error))
            {
                printError(select_error);
                return 1;
            }
        }

        const SolutionRun run = run_solutions(options, selection, solution_bundle,
                                              article_bundle, solution_stats,
                                              need_cookie);
        if (!run.proceed)
            return run.exit_code;

        solution_export.enabled = solutions_enabled;
        solution_export.document_end = (options.solution_placement != "per-problem");
        solution_export.articles_only = options.solutions_only;
        solution_export.export_problems = !article_only_document;
        solution_export.problem_to_article_link = !options.no_problem_to_solution_link;
        solution_export.article_to_problem_link = !options.no_solution_to_problem_link;
        solution_export.article_toc = !options.no_solution_toc;
        solution_export.article_meta = !options.no_article_meta;

        if (options.articles_only_download)
        {
            printSuccess("已按 --articles-only-download 完成抓取，未导出任何文件");
            return 0;
        }
    }
    // 无文章时为 nullptr，生成的文档与不含 --article 时相同
    const luogu::ArticleBundle *articles_ptr =
        articles_enabled ? &article_bundle : nullptr;

    if (options.markdown)
    {
        const std::filesystem::path out_path = luogu::compat::path_from_utf8(
            options.output.empty() ? "problems.md" : options.output);
        std::string error;
        if (markdown::export_markdown(options.filter, out_path, error,
                                      options.cover_title, display,
                                      solutions_enabled ? &solution_bundle : nullptr,
                                      solution_export,
                                      (solutions_enabled || articles_enabled)
                                          ? &selection
                                          : nullptr,
                                      articles_ptr))
            printSuccess(std::string("已把") +
                         (article_only_document
                              ? "下载的文章"
                              : (articles_enabled ? "筛选出的题目与文章"
                                                  : "筛选出的题目")) +
                         "导出到 '" + luogu::compat::path_to_utf8(out_path) + "'");
        else
        {
            printError(error);
            result = 1;
        }
    }
    else if (options.latex)
    {
        // 输出路径按 UTF-8 构造（Windows 中文路径可用）；--local 默认输出同名 .tex
        const std::filesystem::path out_path = luogu::compat::path_from_utf8(
            options.output.empty()
                ? (local_mode ? default_local_output(options.local_file)
                              : std::string("problems.tex"))
                : options.output);

        latex::Options latex_opt;
        latex_opt.lang = options.filter.lang;
        latex_opt.toc_links = !options.no_toc_links;
        latex_opt.toc_backlinks = options.toc_backlinks;
        latex_opt.bilibili_links = !options.no_bilibili_link;
        latex_opt.display = display;
        latex_opt.toc_difficulty = options.show_contents_difficulty_tags;
        latex_opt.paginate = options.paginate;
        latex_opt.new_download = options.new_download;
        latex_opt.font_cover = options.font_cover;
        latex_opt.font_body_zh = options.font_body_zh;
        latex_opt.font_body_en = options.font_body_en;
        latex_opt.font_code = options.font_body_codes;
        latex_opt.font_title_zh = options.font_title_zh;
        latex_opt.font_title_en = options.font_title_en;
        latex_opt.cover_title = options.cover_title;
        // --local：封面标题默认取文件名，--set-cover-title 优先
        if (local_mode && latex_opt.cover_title.empty())
            latex_opt.cover_title = luogu::compat::path_to_utf8(
                luogu::compat::path_from_utf8(options.local_file).stem());

        latex_opt.solutions = solutions_enabled ? &solution_bundle : nullptr;
        latex_opt.articles = articles_ptr;
        latex_opt.solution_export = solution_export;

        std::string error;
        if (local_mode)
        {
            const std::filesystem::path in_path =
                luogu::compat::path_from_utf8(options.local_file);
            std::printf("正在转写本地 Markdown：%s\n",
                        luogu::compat::path_to_utf8(in_path).c_str());
            std::fflush(stdout);
            if (!latex::export_local_markdown(in_path, out_path, error, latex_opt,
                                              options.doc_only))
            {
                printError(error);
                result = 1;
            }
            else if (options.compile_latex)
            {
                result = compile_latex_document(out_path);
            }
            else
            {
                printSuccess("已把 '" + luogu::compat::path_to_utf8(in_path) +
                             "' 转写为 '" + luogu::compat::path_to_utf8(out_path) +
                             "'");
            }
        }
        else if (!latex::export_latex(options.filter, out_path, error, latex_opt,
                                      (solutions_enabled || articles_enabled)
                                          ? &selection
                                          : nullptr))
        {
            printError(error);
            result = 1;
        }
        else if (options.compile_latex)
        {
            result = compile_latex_document(out_path);
        }
    }

    // 不调用 curl_global_cleanup：全局资源随进程退出回收
    return result;
}

int main(int argc, char *argv[])
{
    // Windows 传统控制台启用 ANSI 转义解析与 UTF-8 代码页
    luogu::compat::init_console();

    // 不带参数运行时进入简易交互程序：它把用户设置翻译成等价参数后交给 app::run
    if (argc <= 1)
        return interactive::run();

    // Windows 下 CRT 的 main(char**) 参数按 ANSI 代码页转换，统一转成 UTF-8 后交给 app::run
    return app::run(luogu::compat::get_argv_utf8(argc, argv));
}
