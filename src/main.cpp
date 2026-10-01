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
#include "luogu-extract/contents/solution_task.h"
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/crawler/request_gate.h"
#include "luogu-extract/export/common.h"
#include "luogu-extract/export/latex.h"
#include "luogu-extract/export/markdown.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/prompt.h"
#include "luogu-extract/util/solution_cache.h"
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
    std::string output;     // --output（空则按模式取默认 problems.md / problems.tex）

    // ---- 缓存清除参数（三者都只能单独使用）----
    bool clean_all = false;      // -C, --clean-all：清空整个缓存文件夹
    bool clean_images = false;   // -CIMG, --clean-images：清空图片缓存
    bool clean_problems = false; // -CP, --clean-problems：清空题目列表缓存

    // ---- 下载设置参数 ----
    // -RD, --new-download：下载题面图片时不使用之前缓存的图片，而是重新下载
    // （新图片原子替换缓存中的同名图片，下载失败不影响原有缓存；仅 -L）
    bool new_download = false;

    // ---- 导出设置参数 ----
    bool no_toc_links = false;      // --no-toc-links：目录条目不带跳转超链接（仅 -L）
    bool toc_backlinks = false;     // --toc-backlinks：页码为跳回目录的超链接（仅 -L）
    bool no_bilibili_link = false;  // --no-bilibili-link：bilibili URL 输出为普通文本（仅 -L）
    // ---- 题目信息显示开关（-M / -L 均有效）----
    bool no_show_source_tags = false;   // --no-show-source-tags：不显示来源/时间/区域/特殊标签
    bool show_algorithm_tags = false;   // --show-algorithm-tags：显示算法标签
    bool show_difficulty_tags = false;  // --show-difficulty-tags：显示题目难度
    // --show-contents-difficulty-tags：目录中的题目标题按难度着色（仅 -L）
    bool show_contents_difficulty_tags = false;
    // --paginate：题目之间、文章（题解）之间分页（仅 -L）
    bool paginate = false;
    std::string font_cover;         // --set-font-cover-page（仅 -L）
    std::string font_body_zh;       // --set-font-body-zh-CN（仅 -L）
    std::string font_body_en;       // --set-font-body-en-US（仅 -L）
    std::string font_body_codes;    // --set-font-body-codes（仅 -L）
    std::string font_title_zh;      // --set-font-title-zh-CN（仅 -L）
    std::string font_title_en;      // --set-font-title-en-US（仅 -L）
    std::string cover_title;        // --set-cover-title（-L / -M 均支持）

    luogu::ExportFilter filter; // -M / -L 共用的筛选条件

    // ---- 题解抓取与导出（--with-solutions 等，设计 §十二）----
    bool with_solutions = false;          // --with-solutions
    bool solutions_only = false;          // --solutions-only
    bool solutions_only_download = false; // --solutions-only-download
    std::string cookie_file;              // --cookie
    std::string cookie_string;            // --cookie-string
    std::string solution_source;          // --solution-source（空 = auto）
    int max_solutions = 1;                // --max-solutions（-1 表示 all）
    std::string solution_delay;           // --solution-delay（原始文本）
    bool no_delay_auto_scale = false;     // --no-delay-auto-scale
    // --solution-ttl：题解**列表**有效期（天）；-1 表示无限（默认）
    int solution_ttl = -1;
    // --article-ttl：题解**正文**有效期（天）；-1 表示无限（默认）
    int article_ttl = -1;
    int rate_limit_wait = 120;            // --rate-limit-wait
    bool allow_partial = false;           // --allow-partial
    bool refresh_solutions = false;       // --refresh-solutions
    bool refresh_articles = false;        // --refresh-articles
    bool clean_solutions = false;         // -CS, --clean-solutions
    std::string solution_placement = "document-end"; // --solution-placement
    bool no_problem_to_solution_link = false; // --no-problem-to-solution-link
    bool no_solution_to_problem_link = false; // --no-solution-to-problem-link
    bool no_solution_toc = false;         // --no-solution-toc
    bool no_solution_meta = false;        // --no-solution-meta
    bool yes = false;                     // --yes

    // 题解相关参数是否被显式给出（用于「缺少 --with-solutions」校验）
    bool solution_option_used = false;
};

// 只作为长选项使用的选项码（getopt_long 返回值）
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
    OPT_NEW_DOWNLOAD,
    OPT_COOKIE,
    OPT_COOKIE_STRING,
    OPT_WITH_SOLUTIONS,
    OPT_SOLUTION_SOURCE,
    OPT_MAX_SOLUTIONS,
    OPT_SOLUTION_DELAY,
    OPT_NO_DELAY_AUTO_SCALE,
    OPT_SOLUTION_TTL,
    OPT_ARTICLE_TTL,
    OPT_RATE_LIMIT_WAIT,
    OPT_ALLOW_PARTIAL,
    OPT_REFRESH_SOLUTIONS,
    OPT_REFRESH_ARTICLES,
    OPT_CLEAN_SOLUTIONS,
    OPT_SOLUTIONS_ONLY,
    OPT_SOLUTIONS_ONLY_DOWNLOAD,
    OPT_SOLUTION_PLACEMENT,
    OPT_NO_PROBLEM_TO_SOLUTION_LINK,
    OPT_NO_SOLUTION_TO_PROBLEM_LINK,
    OPT_NO_SOLUTION_TOC,
    OPT_NO_SOLUTION_META,
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
    case OPT_FONT_COVER: return "--set-font-cover-page";
    case OPT_FONT_BODY_ZH: return "--set-font-body-zh-CN";
    case OPT_FONT_BODY_EN: return "--set-font-body-en-US";
    case OPT_FONT_BODY_CODES: return "--set-font-body-codes";
    case OPT_FONT_TITLE_ZH: return "--set-font-title-zh-CN";
    case OPT_FONT_TITLE_EN: return "--set-font-title-en-US";
    case OPT_COVER_TITLE: return "--set-cover-title";
    case OPT_COOKIE: return "--cookie";
    case OPT_COOKIE_STRING: return "--cookie-string";
    case OPT_SOLUTION_SOURCE: return "--solution-source";
    case OPT_MAX_SOLUTIONS: return "--max-solutions";
    case OPT_SOLUTION_DELAY: return "--solution-delay";
    case OPT_SOLUTION_TTL: return "--solution-ttl";
    case OPT_RATE_LIMIT_WAIT: return "--rate-limit-wait";
    case OPT_SOLUTION_PLACEMENT: return "--solution-placement";
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
    if (token == "--set-font-cover-page") return "字体名称或字体文件地址";
    if (token == "--set-font-body-zh-CN") return "字体名称或字体文件地址";
    if (token == "--set-font-body-en-US") return "字体名称或字体文件地址";
    if (token == "--set-font-body-codes") return "字体名称或字体文件地址";
    if (token == "--set-font-title-zh-CN") return "字体名称或字体文件地址";
    if (token == "--set-font-title-en-US") return "字体名称或字体文件地址";
    if (token == "--set-cover-title") return "封面标题";
    if (token == "--pid") return "题号（如 P1001，可多个，空格分隔或重复 --pid）";
    if (token == "--pid-range") return "题号范围（如 P1001-P1010，可多组，空格分隔或重复 --pid-range）";
    if (token == "--cookie") return "Netscape 格式的 cookies.txt 路径";
    if (token == "--cookie-string") return "Cookie 串（形如 \"k=v; k2=v2\"）";
    if (token == "--solution-source") return "题解正文来源（auto / official / save）";
    if (token == "--max-solutions") return "每题抓取的题解篇数（正整数或 all）";
    if (token == "--solution-delay") return "请求间隔秒数（如 5 或 8-15）";
    if (token == "--solution-ttl") return "题解列表缓存有效期天数（0 表示只用 ETag）";
    if (token == "--article-ttl") return "题解正文缓存有效期天数（0 表示只用 ETag）";
    if (token == "--rate-limit-wait") return "限流等待秒数（0 表示检测到限流直接停止）";
    if (token == "--solution-placement") return "题解位置（document-end 或 per-problem）";
    return "";
}

// -h, --help 的帮助信息
const char *kUsage =
    "用法：luogu-extract [选项]\n"
    "\n"
    "选项：\n"
    "  -U, --update          更新题目列表缓存（latest.ndjson）与标签缓存（tags.json）；\n"
    "                        可与 -M / -L 一同使用，此时先更新缓存再下载题目\n"
    "  -M, --markdown        筛选并导出 Markdown（默认输出 problems.md）\n"
    "  -L, --latex           筛选并导出 LaTeX（默认输出 problems.tex）\n"
    "                        （-M 与 -L 不能同时使用）\n"
    "  -RD, --new-download   下载题目时不使用之前缓存的图片，而是重新下载图片\n"
    "                        （仅在使用 -L 时有效）\n"
    "  -C, --clean-all       清空 luogu-extract 缓存文件夹（含题目列表、标签、图片\n"
    "                        与字体缓存）\n"
    "  -CIMG, --clean-images\n"
    "                        清除 luogu-extract/images/ 下的图片缓存\n"
    "  -CP, --clean-problems\n"
    "                        清除题面缓存（latest.ndjson 与 latest.ndjson.gz）\n"
    "  -CS, --clean-solutions\n"
    "                        清除题解缓存（<缓存目录>/solutions/）\n"
    "  （以上清除缓存的参数只能彼此组合使用，不能与其他参数同时使用）\n"
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
    "      --output <file>   输出文件路径（默认 problems.md / problems.tex）\n"
    "\n"
    "题目信息显示选项（-M / -L 均有效）：\n"
    "      --no-show-source-tags\n"
    "                        不显示来源、时间、区域、特殊题目标签（默认显示）\n"
    "      --show-algorithm-tags\n"
    "                        显示算法标签（默认不显示）\n"
    "      --show-difficulty-tags\n"
    "                        显示题目难度（默认不显示）\n"
    "\n"
    "LaTeX 排版选项（仅在使用 -L 时有效）：\n"
    "      --no-toc-links    目录条目不带跳转到对应题目页的超链接（默认带超链接）\n"
    "      --toc-backlinks   每页页眉处的页码为跳回目录页的超链接（默认无超链接）\n"
    "      --show-contents-difficulty-tags\n"
    "                        目录中的题目标题按题目难度着色\n"
    "      --paginate        题目与题解各自从新的一页开始（默认连续排版；\n"
    "                        不影响目录与 PDF 书签）\n"
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
    "题解下载选项（需先登录洛谷并导出 cookies.txt）：\n"
    "      --with-solutions  启用题解抓取与导出；\n"
    "                        需与 -M 或 -L 同用，题解与题面导出到同一个文件\n"
    "      --cookie <file>   Netscape 格式的 cookies.txt（含登录态）；题解列表\n"
    "                        接口需要登录态，该参数是启用题解功能的前提\n"
    "      --cookie-string <k=v; ...>\n"
    "                        直接传入 Cookie 串（与 --cookie 二选一）\n"
    "      --solution-source <auto|official|save>\n"
    "                        题解正文来源，默认 auto：缓存里已有的题解一律优先\n"
    "                        使用（两个来源都有时优先原站），未命中的在原站与\n"
    "                        保存站之间轮流分配并**并行**抓取（两个站点各自计算\n"
    "                        延时；某一站点被限流时任务暂时转给另一站点，连续\n"
    "                        被限流 3 次则放弃该站点，两个站点都放弃则终止）。\n"
    "                        official 只用洛谷原站，save 只用第三方镜像洛谷保存站\n"
    "                        题解列表恒取洛谷原站\n"
    "      --max-solutions <n|all>\n"
    "                        每题抓取篇数，默认 1，按列表顺序取最靠前的 n 篇；\n"
    "                        all 表示该题全部题解\n"
    "      --solution-delay <mean|min-max>\n"
    "                        题解请求的平均间隔秒数，默认 5（实际为均值 ±30% 均匀抖动，\n"
    "                        即 3.5~6.5 秒）；也支持显式区间（如 8-15）与小数；\n"
    "                        单次间隔上限 300 秒。图片下载不受该参数影响\n"
    "      --no-delay-auto-scale\n"
    "                        关闭「随抓取量自动递增延时」与限流后的额外放大\n"
    "      --solution-ttl <days>\n"
    "                        题解列表缓存有效期天数；默认无限；0 表示每次都发\n"
    "                        ETag 条件请求（304 时只刷新时间戳）\n"
    "      --article-ttl <days>\n"
    "                        题解正文缓存有效期天数，语义同上，默认无限\n"
    "      --rate-limit-wait <seconds>\n"
    "                        检测到限流后的等待时长，默认 120；0 表示检测到限流\n"
    "                        直接停止（等待期间可按 S 立即停止、按 C 确认后继续）\n"
    "      --allow-partial   允许导出正文不完整的题解（默认跳过并汇总）\n"
    "      --refresh-solutions\n"
    "                        强制重新获取题解列表（忽略 TTL 与 ETag）\n"
    "      --refresh-articles\n"
    "                        强制重新获取题解正文（忽略 TTL 与 ETag）\n"

    "      --solutions-only  只导出题解，不导出题面（需与 -M 或 -L 同用）\n"
    "      --solutions-only-download\n"
    "                        只抓取并缓存题解，不导出任何文件（不需要 -M / -L）\n"
    "      --solution-placement <document-end|per-problem>\n"
    "                        题解在文档中的位置，默认 document-end（统一置于文档\n"
    "                        最后）；per-problem 表示紧跟对应题目之后\n"
    "      --no-problem-to-solution-link\n"
    "                        关闭题目到题解的跳转（-L 为题目标题右侧的「查看题解」\n"
    "                        按钮，-M 为 Markdown 中的跳转链接）\n"
    "      --no-solution-to-problem-link\n"
    "                        关闭题解到题目的跳转（-L 为题解标题右侧的「返回题目」\n"
    "                        按钮，-M 为 Markdown 中的跳转链接）\n"
    "      --no-solution-toc 题解标题不进目录（仅 -L；默认进目录并注明所属题目）\n"
    "      --no-solution-meta\n"
    "                        不显示题解的来源与原文链接\n"
    "  -y, --yes             把爬取风险的确认次数减少 1 次（减到 0 为止）；\n"
    "                        不能把第 5 档变为无需确认\n"
    "  -h, --help            显示帮助\n"
    "  -V, --version         显示项目简介、版本号、版权声明与项目仓库链接\n"
    "                        （不能与其他参数同时使用）\n"
    "\n"
    "声明：\n"
    "  题解著作权归原作者，导出物仅供个人离线阅读，请勿再分发或用于商业用途；\n"
    "  抓取频率与请求总量由你自行判断，后果自负；保存站 luogu.me 为第三方站点；\n"
    "  请遵守洛谷用户协议及相关法律法规。\n";

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

// 多字符短选项（-CIMG / -CP / -RD）不是 getopt 支持的写法：getopt 只认单字符
// 短选项，会把 "-CIMG" 当成 -C -I -M -G 这样的选项簇。因此在这些参数交给
// getopt 之前，先把它们逐个替换成等价的长选项（--clean-images 等）；
// "--" 之后的内容按惯例是位置参数，不做替换。
// （-C 是单字符短选项，直接由 optstring 处理，无需在此展开。）
inline void expand_multichar_short_options(std::vector<std::string> &args_utf8)
{
    static const struct
    {
        const char *short_form;
        const char *long_form;
    } kAliases[] = {
        {"-CIMG", "--clean-images"},
        {"-CS", "--clean-solutions"},
        {"-CP", "--clean-problems"},
        {"-RD", "--new-download"},
    };

    for (auto &arg : args_utf8)
    {
        if (arg == "--")
            break; // "--" 之后全部是位置参数
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

// 解析非负/正整数参数：允许首尾空白，超出 [min,max] 或含非数字字符时返回 false
inline bool parse_positive_int(const std::string &value, long min_value,
                               long max_value, long &out)
{
    const std::string text = value;
    if (text.empty())
        return false;
    char *end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || (end && *end != '\0'))
        return false;
    if (parsed < min_value || parsed > max_value)
        return false;
    out = parsed;
    return true;
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

// 题解抓取的整体流程（设计 §四 / §五 / §六 / §八）：
// 计划（只读缓存）→ 延时与系数 → 风险分级确认 → 逐题串行抓取。
// 返回值即进程退出码；proceed 为 false 表示不要再导出（取消或中止）。
struct SolutionRun
{
    int exit_code = 0;
    bool proceed = true;
};

SolutionRun run_solutions(const Options &options,
                          const luogu::ProblemSelection &selection,
                          luogu::SolutionBundle &bundle,
                          solution::CrawlStats &stats)
{
    SolutionRun run;

    // ---- 凭据通道：Cookie 只在洛谷原站请求上使用（保存站一律不带）----
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
    if (!cookie_warnings.empty())
        std::fputs(cookie_warnings.c_str(), stdout);
    if (!crawler::gate_has_cookies())
    {
        printError("题解列表接口需要登录态；请登录洛谷后导出 cookies.txt，"
                   "并用 --cookie <file> 指定（或使用 --cookie-string）");
        run.exit_code = 1;
        run.proceed = false;
        return run;
    }
    std::printf("已载入 %zu 条 Cookie（来源：%s；Cookie 不会用于保存站等第三方域名）\n",
                crawler::gate_cookie_count(),
                crawler::gate_cookie_file_hint().c_str());

    // ---- 抓取参数 ----
    solution::TaskOptions task;
    // 默认 auto：缓存优先 + 两个站点轮流分配、并行抓取
    if (options.solution_source == "save")
        task.source = solution::Source::Save;
    else if (options.solution_source == "official")
        task.source = solution::Source::Official;
    else
        task.source = solution::Source::Auto;
    task.max_solutions = options.max_solutions;
    task.list_ttl_days = options.solution_ttl;
    task.article_ttl_days = options.article_ttl;
    task.refresh_solutions = options.refresh_solutions;
    task.refresh_articles = options.refresh_articles;
    task.allow_partial = options.allow_partial;

    if (task.source == solution::Source::Save)
        std::printf("题解正文来源：洛谷保存站（第三方镜像，内容可能滞后或缺失；"
                    "该来源不发送任何 Cookie）\n");
    else if (task.source == solution::Source::Official)
        std::printf("题解正文来源：洛谷原站\n");
    else
        std::printf("题解正文来源：auto（缓存优先；未命中的在原站与保存站之间"
                    "轮流分配、并行抓取；保存站为第三方镜像，内容可能滞后或缺失）\n");

    crawler::GateConfig gate_config;
    if (!options.solution_delay.empty() &&
        !crawler::parse_delay_spec(options.solution_delay, gate_config.delay,
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

    // ---- 计划阶段（第一遍：只读缓存）----
    // 先给请求闸门一个初步的延时系数，保证「按需获取题解列表」这一步本身
    // 也受 --solution-delay 控制
    solution::Plan plan;
    solution::PlanResult plan_result =
        solution::make_plan(selection.problems, task, false, plan);
    crawler::gate_configure_channel(crawler::Channel::Official, gate_config,
                                    plan.total_requests);
    crawler::gate_configure_channel(crawler::Channel::Save, gate_config,
                                    plan.total_requests);

    // 计划阶段把缓存缺失/过期的题解列表抓回来（列表请求本来就要发，属于
    // N + P 中的 P），这样风险确认看到的篇数是精确值
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
    // auto 模式下，正文由调度器处理限流（临时转给另一站点），因此进入
    // 并行抓取阶段前把两条通道切到「命中限流立即返回」；计划阶段的列表请求
    // 只能走原站、没有备用站点，仍保留闸门内部的交互式等待与重试
    if (task.source == solution::Source::Auto)
    {
        crawler::GateConfig parallel_config = gate_config;
        parallel_config.interactive_retry = false;
        crawler::gate_configure_channel(crawler::Channel::Official, parallel_config,
                                        plan.total_requests);
        crawler::gate_configure_channel(crawler::Channel::Save, parallel_config,
                                        plan.total_requests);
    }

    // 用精确的正文篇数重新计算延时系数（只会更保守）
    crawler::gate_set_planned_requests(crawler::Channel::Official,
                                       plan.total_requests);
    crawler::gate_set_planned_requests(crawler::Channel::Save,
                                       plan.total_requests);
    // 预计 0 次请求（全部命中缓存）：不打印延时与规模提示，
    // 由 confirm_risk 给出一行精简说明
    if (plan.total_requests > 0)
        crawler::gate_print_delay_notice(plan.total_requests);

    // ---- 风险分级与确认（设计 §五）----
    prompt::RiskInfo risk = prompt::plan_risk(plan.total_requests, options.yes);
    risk.problems = plan.problems;
    risk.per_problem = options.max_solutions < 0 ? 0 : options.max_solutions;
    risk.per_problem_all = options.max_solutions < 0;
    risk.articles = plan.articles_to_fetch;
    risk.cached_articles = plan.cached_articles;
    risk.list_requests = plan.list_requests;
    risk.total_requests = plan.total_requests;
    risk.seconds_per_request = crawler::gate_effective_delay_seconds();
    risk.eta_seconds = static_cast<long long>(
        risk.seconds_per_request * static_cast<double>(plan.total_requests));

    const prompt::ConfirmResult confirm = prompt::confirm_risk(risk);
    if (confirm == prompt::ConfirmResult::Cancelled)
    {
        // 用户拒绝确认：立即退出，退出码 0
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

    // ---- 抓取阶段 ----
    std::string crawl_error;
    if (!solution::crawl(plan, task, bundle, stats, crawl_error))
    {
        printError(crawl_error);
        run.exit_code = 1;
        run.proceed = false;
        return run;
    }

    printSuccess("题解抓取完成：" + solution::describe_crawl_stats(stats));
    const std::uintmax_t used = solcache::cache_size();
    std::printf("题解缓存占用：%.2f MB（%s）\n",
                static_cast<double>(used) / (1024.0 * 1024.0),
                luogu::compat::path_to_utf8(solcache::solutions_dir()).c_str());

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
    // "-CIMG" / "-CP" / "-RD" 这类多字符短选项先展开为等价的长选项
    expand_multichar_short_options(args_utf8);
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
        {"new-download",         no_argument,       nullptr, OPT_NEW_DOWNLOAD},
        {"cookie",               required_argument, nullptr, OPT_COOKIE},
        {"cookie-string",        required_argument, nullptr, OPT_COOKIE_STRING},
        {"with-solutions",       no_argument,       nullptr, OPT_WITH_SOLUTIONS},
        {"solution-source",      required_argument, nullptr, OPT_SOLUTION_SOURCE},
        {"max-solutions",        required_argument, nullptr, OPT_MAX_SOLUTIONS},
        {"solution-delay",       required_argument, nullptr, OPT_SOLUTION_DELAY},
        {"no-delay-auto-scale",  no_argument,       nullptr, OPT_NO_DELAY_AUTO_SCALE},
        {"solution-ttl",         required_argument, nullptr, OPT_SOLUTION_TTL},
        {"article-ttl",          required_argument, nullptr, OPT_ARTICLE_TTL},
        {"rate-limit-wait",      required_argument, nullptr, OPT_RATE_LIMIT_WAIT},
        {"allow-partial",        no_argument,       nullptr, OPT_ALLOW_PARTIAL},
        {"refresh-solutions",    no_argument,       nullptr, OPT_REFRESH_SOLUTIONS},
        {"refresh-articles",     no_argument,       nullptr, OPT_REFRESH_ARTICLES},
        {"clean-solutions",      no_argument,       nullptr, OPT_CLEAN_SOLUTIONS},
        {"solutions-only",       no_argument,       nullptr, OPT_SOLUTIONS_ONLY},
        {"solutions-only-download", no_argument,    nullptr, OPT_SOLUTIONS_ONLY_DOWNLOAD},
        {"solution-placement",   required_argument, nullptr, OPT_SOLUTION_PLACEMENT},
        {"no-problem-to-solution-link", no_argument, nullptr, OPT_NO_PROBLEM_TO_SOLUTION_LINK},
        {"no-solution-to-problem-link", no_argument, nullptr, OPT_NO_SOLUTION_TO_PROBLEM_LINK},
        {"no-solution-toc",      no_argument,       nullptr, OPT_NO_SOLUTION_TOC},
        {"no-solution-meta",     no_argument,       nullptr, OPT_NO_SOLUTION_META},
        {"yes",                  no_argument,       nullptr, 'y'},
        {"help",       no_argument,       nullptr, 'h'},
        {"version",    no_argument,       nullptr, 'V'},
        {nullptr,      0,                 nullptr, 0},
    };

    Options options;
    int opt;
    // -V, --version 必须单独使用；清除类参数（-C / -CIMG / -CP / -CS）
    // 只能彼此组合使用：分别统计总选项数与非清除类选项数，
    // 与位置参数（optind）一起判断是否属于参数使用错误
    int option_count = 0;
    int non_clean_option_count = 0;
    // 短选项串以 ':' 开头：getopt 出错时不打印英文提示，
    // 由下面的 '?' / ':' 分支输出统一的中文错误信息
    while ((opt = getopt_long(arg_count, arg_vector, ":UMLhVCy", kLongOptions, nullptr)) != -1)
    {
        ++option_count;
        if (opt != 'C' && opt != OPT_CLEAN_ALL && opt != OPT_CLEAN_IMAGES &&
            opt != OPT_CLEAN_PROBLEMS && opt != OPT_CLEAN_SOLUTIONS)
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
        case OPT_NEW_DOWNLOAD:
            options.new_download = true;
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
        case OPT_SOLUTIONS_ONLY:
            options.solutions_only = true;
            break;
        case OPT_SOLUTIONS_ONLY_DOWNLOAD:
            options.solutions_only_download = true;
            break;
        case OPT_SOLUTION_SOURCE:
        {
            const std::string value = optarg ? optarg : "";
            if (value != "official" && value != "save" && value != "auto")
            {
                printError("参数 '--solution-source' 的值 '" + value +
                           "' 不是合法的来源；正确用法："
                           "--solution-source <auto|official|save>"
                           "（auto 为默认：缓存优先，未命中的题解在原站与保存站"
                           "之间轮流分配并并行抓取；official 只用洛谷原站；"
                           "save 只用第三方镜像保存站。题解列表恒取洛谷原站）");
                return 1;
            }
            options.solution_source = value;
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
        case OPT_SOLUTION_DELAY:
        {
            crawler::DelaySpec spec;
            std::string err;
            if (!crawler::parse_delay_spec(optarg ? optarg : "", spec, err))
            {
                printError(err);
                return 1;
            }
            options.solution_delay = optarg ? optarg : "";
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
        case OPT_NO_SOLUTION_META:
            options.no_solution_meta = true;
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

    // 清除类参数（-C / -CIMG / -CP / -CS）只能彼此组合使用：
    // 与非清除类参数（含 -h、--tags、-U、-M、-L 等）或位置参数同时出现即拒绝执行
    {
        std::vector<std::string> used_clean;
        if (options.clean_all) used_clean.push_back("-C, --clean-all");
        if (options.clean_images) used_clean.push_back("-CIMG, --clean-images");
        if (options.clean_problems) used_clean.push_back("-CP, --clean-problems");
        if (options.clean_solutions) used_clean.push_back("-CS, --clean-solutions");
        if (!used_clean.empty() &&
            (non_clean_option_count > 0 || optind < arg_count))
        {
            std::string joined;
            for (size_t i = 0; i < used_clean.size(); ++i)
                joined += (i ? "、" : "") + used_clean[i];
            printError("清除缓存的参数（" + joined +
                       "）只能与其他清除缓存的参数（-C、-CIMG、-CP、-CS）"
                       "同时使用，不能与其它参数或多余的位置参数一起使用；"
                       "正确用法：luogu-extract -CIMG -CP -CS"
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

    // 清除类参数只操作本地缓存，不访问网络、也不需要 libcurl：直接执行后退出
    if (options.clean_all || options.clean_images || options.clean_problems ||
        options.clean_solutions)
    {
        // 依次执行所有被指定的清除动作（可组合）：-C 会删掉整个缓存目录，
        // 其余动作随后按「目录不存在 = 已清空」正常返回
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
        run_clean(options.clean_solutions, solcache::clean_solutions);
        return clean_result == crawler::SUCCESS ? 0 : 1;
    }

    // -M 与 -L 同时给出：此前 -L 会被静默忽略。明确拒绝，避免用户误以为
    // 两种格式都已导出；需要两种格式时请分两次执行
    if (options.markdown && options.latex)
    {
        printError("参数 -M 与 -L 不能同时使用；请分两次导出（-M 导出 Markdown，-L 导出 LaTeX）");
        return 1;
    }

    // ---- 题解功能：参数使用校验（设计 §十二）----
    const bool solutions_enabled = options.with_solutions || options.solutions_only ||
                                   options.solutions_only_download;
    if (options.solutions_only && options.solutions_only_download)
    {
        printError("参数 --solutions-only 与 --solutions-only-download 不能同时使用；"
                   "--solutions-only 只导出题解（不导出题面），"
                   "--solutions-only-download 只抓取并缓存、不导出任何文件");
        return 1;
    }
    if (!solutions_enabled && options.solution_option_used)
    {
        printError("题解相关参数（--cookie、--cookie-string、--solution-*、"
                   "--max-solutions、--refresh-*、--no-solution-*、-y/--yes 等）"
                   "需要与 --with-solutions（或 --solutions-only / "
                   "--solutions-only-download）一起使用；正确用法："
                   "luogu-extract -L --cookie cookies.txt --with-solutions ..."
                   "（未启用题解功能时程序完全不碰题解）");
        return 1;
    }
    if (solutions_enabled)
    {
        // --with-solutions / --solutions-only 需与 -M 或 -L 同用；
        // --solutions-only-download 例外（只抓缓存，不需要导出模式）
        if (!options.solutions_only_download && !options.markdown && !options.latex)
        {
            printError("参数 --with-solutions / --solutions-only 需要与 -M（导出 "
                       "Markdown）或 -L（导出 LaTeX）一起使用；若只想抓取并缓存题解，"
                       "请改用 --solutions-only-download");
            return 1;
        }
        if (options.cookie_file.empty() && options.cookie_string.empty())
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
    }

    // 仅 -L 支持的参数与 -M 一起使用属于参数填用错误：拒绝执行并提示正确用法
    // （--no-show-source-tags / --show-algorithm-tags / --show-difficulty-tags
    //   对 -M 同样有效，不在此列）
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
        // 双向跳转按钮与目录条目只对 LaTeX 有意义（Markdown 侧两个跳转开关
        // 同样有效，但 --no-solution-toc 仅 -L）
        if (options.no_solution_toc) latex_only.push_back("--no-solution-toc");
        // -RD 只在 -L 导出下载题面图片时才有意义（-M 不下载图片）
        if (options.new_download) latex_only.push_back("--new-download");
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

    // --pid / --pid-range / --tag / --difficulty 支持「空格分隔多个值」的写法，
    // 后面跟的裸参数按这些选项的后续值处理；只有导出模式与题解抓取模式
    // （--solutions-only-download 只抓缓存、不需要 -M/-L）才允许这样写
    const bool bare_args_allowed = options.markdown || options.latex ||
                                   options.with_solutions || options.solutions_only ||
                                   options.solutions_only_download;
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

    // -RD, --new-download 只在 -L 导出下载题面图片时才有意义：
    // 单独使用或只与 -U 一起使用时不会下载任何图片，按参数填用错误拒绝
    if (options.new_download && !options.latex)
    {
        printError("参数 -RD, --new-download 仅在下载题面图片时有效；"
                   "请在使用 -L（导出 LaTeX）的同时使用该参数"
                   "（-M 导出 Markdown 不下载图片，-U 只更新题目列表与标签缓存）");
        return 1;
    }

    if (!options.update && !options.markdown && !options.latex &&
        !options.solutions_only_download)
    {
        printError("未指定任何操作；请至少使用 -U（更新缓存）、-M（导出 Markdown）、"
                   "-L（导出 LaTeX）、-C（清空缓存）、--tags（查看标签对照表）或 "
                   "--solutions-only-download（只抓取题解缓存）之一，"
                   "并可用 -h, --help 查看帮助信息");
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
    {
        printError("初始化 libcurl 失败；请检查网络相关运行库是否安装完整");
        return 1;
    }

    int result = 0;
    // -M / -L 共用的题目信息显示开关（默认与 -L 一致：显示来源等标签，
    // 隐藏算法标签与难度）
    luogu::DisplayOptions display;
    display.source_tags = !options.no_show_source_tags;
    display.algorithm_tags = options.show_algorithm_tags;
    display.difficulty = options.show_difficulty_tags;

    // -U, --update 可与下载题目的参数（-M / -L，含 -RD）一同使用：
    // 只要命令行中出现 -U 就先更新缓存，再下载题目；与各参数在命令行中的
    // 先后顺序无关（更新失败时不继续，避免用旧缓存掩盖更新失败）
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

    // ---- 题解抓取（--with-solutions / --solutions-only*）----
    // 先按筛选条件选中题目（与后面导出共用同一份结果，避免重复解析题目缓存），
    // 再走「计划 → 风险确认 → 抓取」，最后与题面一起导出到同一份文件
    luogu::ProblemSelection selection;
    luogu::SolutionBundle solution_bundle;
    solution::CrawlStats solution_stats;
    luogu::SolutionExportOptions solution_export;
    if (solutions_enabled)
    {
        std::string select_error;
        if (!luogu::select_problems(options.filter, selection.problems,
                                    &selection.resolved_tags, select_error))
        {
            printError(select_error);
            curl_global_cleanup();
            return 1;
        }

        const SolutionRun run =
            run_solutions(options, selection, solution_bundle, solution_stats);
        if (!run.proceed)
        {
            curl_global_cleanup();
            return run.exit_code;
        }

        solution_export.enabled = true;
        solution_export.document_end = (options.solution_placement != "per-problem");
        solution_export.solutions_only = options.solutions_only;
        solution_export.problem_to_solution_link = !options.no_problem_to_solution_link;
        solution_export.solution_to_problem_link = !options.no_solution_to_problem_link;
        solution_export.solution_toc = !options.no_solution_toc;
        solution_export.solution_meta = !options.no_solution_meta;

        if (options.solutions_only_download)
        {
            // 只抓取并缓存，不导出文件
            printSuccess("已按 --solutions-only-download 完成抓取，未导出任何文件");
            curl_global_cleanup();
            return 0;
        }
    }

    if (options.markdown)
    {
        // 输出路径按 UTF-8 构造 filesystem::path（Windows 下中文路径可用）
        const std::filesystem::path out_path = luogu::compat::path_from_utf8(
            options.output.empty() ? "problems.md" : options.output);
        std::string error;
        if (markdown::export_markdown(options.filter, out_path, error,
                                      options.cover_title, display,
                                      solutions_enabled ? &solution_bundle : nullptr,
                                      solution_export,
                                      solutions_enabled ? &selection : nullptr))
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

        // 组装 LaTeX 显示选项（题目信息显示开关与 -M 共用同一组参数）
        latex::Options latex_opt;
        latex_opt.lang = options.filter.lang;
        latex_opt.toc_links = !options.no_toc_links;
        latex_opt.toc_backlinks = options.toc_backlinks;
        latex_opt.bilibili_links = !options.no_bilibili_link;
        latex_opt.display = display;
        latex_opt.toc_difficulty = options.show_contents_difficulty_tags;
        // --paginate：题目之间、文章（题解）之间分页
        latex_opt.paginate = options.paginate;
        // -RD, --new-download：下载题面图片时忽略已有缓存，全部重新下载
        // （新图片原子替换缓存中的同名图片，下载失败时保留原有缓存）
        latex_opt.new_download = options.new_download;
        latex_opt.font_cover = options.font_cover;
        latex_opt.font_body_zh = options.font_body_zh;
        latex_opt.font_body_en = options.font_body_en;
        latex_opt.font_code = options.font_body_codes;
        latex_opt.font_title_zh = options.font_title_zh;
        latex_opt.font_title_en = options.font_title_en;
        latex_opt.cover_title = options.cover_title;

        latex_opt.solutions = solutions_enabled ? &solution_bundle : nullptr;
        latex_opt.solution_export = solution_export;

        std::string error;
        if (!latex::export_latex(options.filter, out_path, error, latex_opt,
                                 solutions_enabled ? &selection : nullptr))
        {
            printError(error);
            result = 1;
        }
    }

    curl_global_cleanup();
    return result;
}
