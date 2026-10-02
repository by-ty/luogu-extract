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

// src/interactive/interactive.cpp
// 简易命令行交互程序：不带任何参数运行可执行文件时进入（./luogu-extract）。
//
// 设计要点：
// - 本文件只负责「问」与「把答案翻译成命令行参数」，真正的更新、清理、
//   打印标签与导出全部交给 app::run 执行，因此下载过程中的提示、警告、
//   风险确认与退出码与非交互模式完全一致，也不会出现两套逻辑不一致；
// - 任何一步输入 q 并回车即结束程序；直接回车一律表示使用默认值
//   （默认值与非交互模式下不传该参数时完全相同）；
// - 输入不符合要求时给出中文提示并重新询问；
// - 可配置的项目随前面的选择变化（导出 Markdown 时不出现仅 LaTeX 的排版
//   选项，未启用题解/文章时也不出现对应的抓取选项）。
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include "luogu-extract/app/app.h"
#include "luogu-extract/contents/solution.h"
#include "luogu-extract/crawler/request_gate.h"
#include "luogu-extract/export/common.h"
#include "luogu-extract/interactive/interactive.h"
#include "luogu-extract/util/cli_parse.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/tag_cache.h"
#include "luogu-extract/util/version.h"

namespace
{

// ---- 输出 ----

const char *kReset = "\033[0m";
const char *kTitle = "\033[1;36m";
const char *kGreen = "\033[1;32m";
const char *kRed = "\033[1;31m";
const char *kYellow = "\033[1;33m";

// 选项状态的配色（便于一眼区分「已启用/已填值」与「未启用/使用默认值」）：
// 已启用用绿色、已填写的值用黄色，未启用与「默认（…）/未设置」用灰色
const char *kStateOn = "\033[1;32m";
const char *kStateValue = "\033[1;33m";
const char *kStateOff = "\033[90m";

// 二元参数的当前状态
std::string toggle_state_text(bool enabled)
{
    return enabled ? std::string(kStateOn) + "已启用" + kReset
                   : std::string(kStateOff) + "未启用" + kReset;
}

// 值参数的当前状态：is_default 为 true 表示未设置 / 使用默认值
std::string value_state_text(const std::string &text, bool is_default)
{
    return (is_default ? std::string(kStateOff) : std::string(kStateValue)) +
           text + kReset;
}

void print_line(const std::string &text = std::string())
{
    std::printf("%s\n", text.c_str());
}

void print_title(const std::string &text)
{
    std::printf("\n%s%s%s\n", kTitle, text.c_str(), kReset);
}

void print_notice(const std::string &text)
{
    std::fflush(stdout);
    std::printf("%s%s%s\n", kYellow, text.c_str(), kReset);
}

void print_error(const std::string &text)
{
    std::fflush(stdout);
    std::fprintf(stderr, "%s错误：%s%s\n", kRed, text.c_str(), kReset);
}

void print_success(const std::string &text)
{
    std::fflush(stdout);
    std::printf("%s%s%s\n", kGreen, text.c_str(), kReset);
}

// ---- 输入 ----

// 去掉首尾空白（含 Windows 换行残留的 '\r'）；路径、标题等值内部的空格保留
std::string trim(const std::string &s)
{
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(begin, end - begin);
}

// 交互会话：把「读一行输入」与「用户要求结束程序」封装在一起。
// 任何一步（含值输入）输入 q 并回车、或标准输入结束，都立即结束整个程序。
struct Session
{
    bool ended = false;

    // 读取一行（去掉首尾空白）；返回 false 表示会话已结束，调用方应立即返回
    bool ask(const std::string &prompt, std::string &out)
    {
        std::fputs(prompt.c_str(), stdout);
        std::fflush(stdout);
        std::string raw;
        if (luogu::compat::read_line(stdin, raw) < 0)
        {
            out.clear();
            print_line();
            print_line("输入已结束，程序退出。");
            ended = true;
            return false;
        }
        const std::string line = trim(raw);
        if (cliparse::to_lower_ascii(line) == "q")
        {
            out.clear();
            print_line();
            print_line("已按你的选择退出程序。");
            ended = true;
            return false;
        }
        out = line;
        return true;
    }
};

// ---- 设置的数据模型 ----

// 第 3 步：要执行的操作
enum class Action
{
    Markdown,     // -M
    Latex,        // -L
    ArticlesOnly, // --articles-only-download
    Clean,        // -C / -CIMG / -CP / -CF / -CS / -CA
    Tags,         // --tags
};

// 值参数的取值方式（决定如何解析与校验用户输入）
enum class ValueKind
{
    Text,              // 整行文本
    OutputFile,        // 输出文件路径（整行）
    CoverTitle,        // 标题（整行）
    Tags,              // 标签（一行可多个）
    Difficulties,      // 难度（一行可多个）
    Types,             // 题目类型（一行可多个）
    Pids,              // 题号（一行可多个）
    PidRanges,         // 题号范围（一行可多个）
    Lang,              // 题面语言
    Font,              // 字体名称或字体文件地址（整行）
    LocalFile,         // 本地 Markdown 文件地址（整行）
    ArticleIds,        // 文章编号（一行可多个）
    CookieFile,        // cookies.txt 路径（整行）
    CookieString,      // Cookie 串（整行）
    ArticleSource,     // 文章正文来源
    MaxSolutions,      // 每题抓取篇数
    RequestDelay,      // 请求间隔
    TtlDays,           // 缓存有效期天数
    RateLimitWait,     // 限流等待秒数
    SolutionPlacement, // 题解在文档中的位置
};

// 一项可修改的设置：二元参数（flag）或值参数（value / values）。
// 二元参数一律使用「积极称呼」显示（如 --no-show-source-tags 显示为
// 「显示来源、时间、区域、特殊题目标签」，默认启用）。
struct Setting
{
    std::string label;
    bool *flag = nullptr;                        // 二元参数
    std::string *value = nullptr;                // 单值参数
    std::vector<std::string> *values = nullptr;  // 多值参数
    ValueKind kind = ValueKind::Text;
    std::string empty_text; // 留空（即不传该参数）时的显示文本
    std::string hint;       // 值输入提示
};

// 交互过程中收集到的全部设置
struct State
{
    // ---- 第 2 步：是否先更新缓存 ----
    bool update_cache = true;

    // ---- 第 3 步：操作类型 ----
    Action action = Action::Markdown;

    // ---- 第 4 步：导出内容 ----
    bool c_problems = true;
    bool c_solutions = false;
    bool c_articles = false;
    bool c_local = false;

    // ---- 题目筛选 ----
    std::vector<std::string> tags;
    std::vector<std::string> difficulties;
    std::vector<std::string> types;
    std::vector<std::string> pids;
    std::vector<std::string> pid_ranges;
    std::string lang;
    // 「下载所有题目」：不传任何筛选参数（= 全部题目）。与筛选条件互斥，
    // 只是为了让用户显式确认「确实要下载全部题目」。
    bool all_problems = false;

    // ---- 题目信息显示（-M / -L 均有效）----
    bool show_source_tags = true;
    bool show_algorithm_tags = false;
    bool show_difficulty_tags = false;

    // ---- 输出 ----
    std::string output;
    std::string cover_title;

    // ---- LaTeX 排版（仅 -L）----
    bool toc_links = true;
    bool toc_backlinks = false;
    bool toc_difficulty = false;
    bool paginate = false;
    bool bilibili_links = true;
    bool new_download = false;
    std::string font_cover;
    std::string font_body_zh;
    std::string font_body_en;
    std::string font_body_codes;
    std::string font_title_zh;
    std::string font_title_en;

    // ---- 本地 Markdown 转写（仅 -L）----
    std::string local_file;
    bool doc_only = false;

    // ---- 文章 ----
    std::vector<std::string> articles;

    // ---- 题解 / 文章抓取与导出 ----
    std::string cookie_file;
    std::string cookie_string;
    std::string article_source;
    std::string max_solutions;
    std::string request_delay;
    bool delay_auto_scale = true;
    std::string solution_ttl;
    std::string article_ttl;
    std::string rate_limit_wait;
    bool allow_partial = false;
    bool refresh_solutions = false;
    bool refresh_articles = false;
    std::string solution_placement;
    bool problem_to_solution_link = true;
    bool solution_to_problem_link = true;
    bool solution_toc = true;
    bool article_meta = true;

    // ---- 第 6 步：LaTeX 导出后自动编译----
    bool compile_latex = false;

    bool exporting() const
    {
        return action == Action::Markdown || action == Action::Latex;
    }
    bool latex() const { return action == Action::Latex; }
    bool downloads_solutions() const { return c_solutions; }
    bool downloads_articles() const { return c_articles; }
    bool downloads_any() const { return c_solutions || c_articles; }
    // 需要题目集合：导出题面，或按题目抓题解
    bool needs_problems() const { return c_problems || c_solutions; }
};

// ---- 设置的构造与显示 ----

std::string join(const std::vector<std::string> &items, const std::string &sep)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i)
            out += sep;
        out += items[i];
    }
    return out;
}

void add_unique(std::vector<std::string> &out, const std::string &value)
{
    if (std::find(out.begin(), out.end(), value) == out.end())
        out.push_back(value);
}

Setting toggle_item(const std::string &label, bool *flag)
{
    Setting s;
    s.label = label;
    s.flag = flag;
    return s;
}

Setting value_item(const std::string &label, std::string *value, ValueKind kind,
                   const std::string &empty_text, const std::string &hint = std::string())
{
    Setting s;
    s.label = label;
    s.value = value;
    s.kind = kind;
    s.empty_text = empty_text;
    s.hint = hint;
    return s;
}

Setting multi_item(const std::string &label, std::vector<std::string> *values,
                   ValueKind kind, const std::string &empty_text,
                   const std::string &hint = std::string())
{
    Setting s;
    s.label = label;
    s.values = values;
    s.kind = kind;
    s.empty_text = empty_text;
    s.hint = hint;
    return s;
}

// 一项设置当前取值的显示文本（带状态配色）
std::string current_text(const Setting &s)
{
    if (s.flag)
        return toggle_state_text(*s.flag);
    if (s.values)
        return s.values->empty() ? value_state_text(s.empty_text, true)
                                 : value_state_text(join(*s.values, "、"), false);
    return s.value->empty() ? value_state_text(s.empty_text, true)
                            : value_state_text(*s.value, false);
}

void print_settings(const std::vector<Setting> &settings, const std::string &header,
                    bool numbered)
{
    print_title(header);
    for (size_t i = 0; i < settings.size(); ++i)
    {
        if (numbered)
            std::printf("  %2zu) %s：%s\n", i + 1, settings[i].label.c_str(),
                        current_text(settings[i]).c_str());
        else
            std::printf("  %s：%s\n", settings[i].label.c_str(),
                        current_text(settings[i]).c_str());
    }
}

// 默认输出文件：随模式变化（与非交互模式的默认值一致）
std::string default_output_text(const State &st)
{
    if (st.c_local)
        return "默认（<原文件名>.tex，与输入文件同目录）";
    if (st.latex())
        return "默认（problems.tex）";
    return "默认（problems.md）";
}

std::string default_cover_text(const State &st)
{
    if (st.c_local)
        return "默认（取本地文件名）";
    if (st.latex())
        return "默认（luogu extract）";
    return "默认（洛谷题目导出）";
}

// 根据当前选择组装可修改的设置列表（第 5 步）：只列出与本次操作有关的项目
std::vector<Setting> build_settings(State &st)
{
    std::vector<Setting> list;

    // ---- 题目筛选（导出题面或按题目抓题解时才有意义）----
    if (st.needs_problems())
    {
        list.push_back(multi_item("标签筛选", &st.tags, ValueKind::Tags, "未设置",
                                  "多个标签用空格分隔（如：模拟 贪心）；含空格的标签名"
                                  "（如 NOIP 普及组）可整体输入，多个之间也可用「、」分隔"));
        list.push_back(multi_item("难度筛选", &st.difficulties,
                                  ValueKind::Difficulties, "未设置",
                                  "0~8 的数字或闭区间（如 1-4），多个用空格分隔"));
        list.push_back(multi_item("题目类型筛选", &st.types, ValueKind::Types,
                                  "未设置",
                                  "B（基础题）或 P（普通题），多个用空格分隔"));
        list.push_back(multi_item("题号筛选", &st.pids, ValueKind::Pids, "未设置",
                                  "如 P1001，多个用空格分隔；与其它筛选条件同时使用时"
                                  "命中的题目追加为并集"));
        list.push_back(multi_item("题号范围筛选", &st.pid_ranges,
                                  ValueKind::PidRanges, "未设置",
                                  "如 P1001-P1010，多组用空格分隔；一组范围的两端"
                                  "必须属于同一题库"));
        list.push_back(value_item("题面语言", &st.lang, ValueKind::Lang,
                                  "默认（zh-CN）", "zh-CN 或 en（英文缺失时回退中文）"));
        list.push_back(toggle_item("下载所有题目（不设置任何筛选条件，导出全部题目）",
                                   &st.all_problems));
    }

    // ---- 导出设置 ----
    if (st.exporting())
    {
        list.push_back(value_item("输出文件", &st.output, ValueKind::OutputFile,
                                  default_output_text(st),
                                  "整行都会作为文件路径（可以含空格）"));
        list.push_back(value_item("封面标题 / Markdown 一级标题",
                                  &st.cover_title, ValueKind::CoverTitle,
                                  default_cover_text(st), "整行都会作为标题文字（可以含空格）"));
        // 本地 Markdown 转写不涉及题面显示设置
        if (!st.c_local)
        {
            list.push_back(toggle_item("显示来源、时间、区域、特殊题目标签",
                                       &st.show_source_tags));
            list.push_back(toggle_item("显示算法标签", &st.show_algorithm_tags));
            list.push_back(toggle_item("显示难度", &st.show_difficulty_tags));
        }
    }

    // ---- LaTeX 排版（仅 -L）----
    if (st.latex())
    {
        list.push_back(toggle_item("目录条目带跳转到对应题目页的超链接", &st.toc_links));
        list.push_back(toggle_item("页眉页码跳回目录页", &st.toc_backlinks));
        list.push_back(toggle_item("目录中的题目标题按难度着色", &st.toc_difficulty));
        list.push_back(toggle_item("题目与题解（文章）之间分页", &st.paginate));
        list.push_back(toggle_item("重新下载题面图片，不使用图片缓存", &st.new_download));
        list.push_back(toggle_item("题面中的 B 站视频输出为超链接", &st.bilibili_links));
        const std::string font_empty = "默认（跟随系统字体方案）";
        const std::string font_hint =
            "整行都会作为字体值：可填系统已安装的字体名称（如 SimSun），"
            "也可填字体文件地址（.ttf / .otf / .ttc）";
        list.push_back(value_item("封面标题字体", &st.font_cover,
                                  ValueKind::Font, font_empty, font_hint));
        list.push_back(value_item("正文中文字体", &st.font_body_zh,
                                  ValueKind::Font, font_empty, font_hint));
        list.push_back(value_item("正文西文字体", &st.font_body_en,
                                  ValueKind::Font, font_empty, font_hint));
        list.push_back(value_item("代码与黑体西文字体",
                                  &st.font_body_codes, ValueKind::Font, font_empty, font_hint));
        list.push_back(value_item("标题中文字体", &st.font_title_zh,
                                  ValueKind::Font, font_empty, font_hint));
        list.push_back(value_item("标题西文字体", &st.font_title_en,
                                  ValueKind::Font, font_empty, font_hint));
    }

    // ---- 本地 Markdown 转写 ----
    if (st.c_local)
    {
        list.push_back(value_item("本地 Markdown 文件", &st.local_file,
                                  ValueKind::LocalFile, "未设置",
                                  "整行都会作为文件地址（可以含空格）"));
        list.push_back(toggle_item("不输出封面、目录与页眉标题", &st.doc_only));
    }

    // ---- 文章编号 ----
    if (st.c_articles)
    {
        list.push_back(multi_item("文章编号", &st.articles, ValueKind::ArticleIds,
                                  "未设置",
                                  "6~32 位小写字母或数字（如 p7fsb45w），多个用空格分隔"));
    }

    // ---- 题解 / 文章抓取设置 ----
    if (st.downloads_any())
    {
        list.push_back(value_item("Cookie 文件", &st.cookie_file,
                                  ValueKind::CookieFile, "未设置（与 Cookie 串二选一）",
                                  "Netscape 格式的 cookies.txt 路径（整行作为路径）"));
        list.push_back(value_item("Cookie 串", &st.cookie_string,
                                  ValueKind::CookieString, "未设置（与 Cookie 文件二选一）",
                                  "形如 \"k=v; k2=v2\"；整行都会作为 Cookie 串"));
        list.push_back(value_item("文章正文来源", &st.article_source,
                                  ValueKind::ArticleSource,
                                  "默认（auto：缓存优先，未命中的在原站与保存站之间轮流抓取）",
                                  "auto / official（只用洛谷原站）/ save（只用洛谷保存站）"));
        list.push_back(value_item("请求的平均间隔", &st.request_delay,
                                  ValueKind::RequestDelay, "默认（5 秒，实际为均值 ±30% 抖动）",
                                  "秒数（如 5）或显式区间（如 8-15），支持小数"));
        list.push_back(toggle_item("随抓取量自动递增延时", &st.delay_auto_scale));
        list.push_back(value_item("限流后的等待时长", &st.rate_limit_wait,
                                  ValueKind::RateLimitWait, "默认（120 秒）",
                                  "不小于 0 的整数秒数；0 表示检测到限流直接停止"));
        list.push_back(value_item("文章正文缓存有效期", &st.article_ttl,
                                  ValueKind::TtlDays, "默认（无限，命中缓存即直接用）",
                                  "不小于 0 的整数天数；0 表示每次都发 ETag 条件请求"));
        list.push_back(toggle_item("强制重新获取文章正文",
                                   &st.refresh_articles));
        list.push_back(toggle_item("允许导出正文不完整的文章",
                                   &st.allow_partial));
        if (st.downloads_solutions())
        {
            list.push_back(value_item("每题抓取题解篇数", &st.max_solutions,
                                      ValueKind::MaxSolutions, "默认（1 篇）",
                                      "正整数或 all（该题全部题解）"));
            list.push_back(value_item("题解列表缓存有效期", &st.solution_ttl,
                                      ValueKind::TtlDays, "默认（无限，命中缓存即直接用）",
                                      "不小于 0 的整数天数；0 表示每次都发 ETag 条件请求"));
            list.push_back(toggle_item("强制重新获取题解列表",
                                       &st.refresh_solutions));
        }
    }

    // ---- 题解 / 文章导出（文档内）的设置 ----
    if (st.exporting() && st.downloads_any())
    {
        if (st.downloads_solutions())
        {
            list.push_back(value_item("题解在文档中的位置",
                                      &st.solution_placement, ValueKind::SolutionPlacement,
                                      "默认（document-end：统一置于文档最后）",
                                      "document-end 或 per-problem（紧跟对应题目之后）"));
            if (st.c_problems)
            {
                list.push_back(toggle_item("题目到题解的跳转", &st.problem_to_solution_link));
                list.push_back(toggle_item("题解到题目的跳转", &st.solution_to_problem_link));
            }
        }
        list.push_back(toggle_item("显示文章的来源与原文链接", &st.article_meta));
        if (st.latex())
            list.push_back(toggle_item("文章的标题进入目录", &st.solution_toc));
    }

    return list;
}

// ---- 取值（解析与校验）----

// 标签是否能在标签缓存中找到（名称或数字 ID 均可）
bool tag_known(const std::string &token, const tagcache::Cache &cache)
{
    if (cache.name_to_id.find(token) != cache.name_to_id.end())
        return true;
    const bool numeric = !token.empty() &&
                         std::all_of(token.begin(), token.end(), [](char c) {
                             return std::isdigit(static_cast<unsigned char>(c));
                         });
    if (!numeric)
        return false;
    try
    {
        return cache.id_to_name.find(std::stoi(token)) != cache.id_to_name.end();
    }
    catch (...)
    {
        return false;
    }
}

// 标签输入：先按「、」「；」「;」分组，每组若整体恰好是已知标签名（如
// 「NOIP 普及组」）就按一个标签，否则按空白拆成多个——与 --tag 的处理一致，
// 同时让「含空格的标签名 + 其它标签」也能在一行里写完。
std::vector<std::string> split_tag_input(const std::string &line)
{
    const bool has_map = tagcache::shared_cache_loaded();
    const tagcache::Cache &cache = tagcache::shared_cache();

    std::vector<std::string> out;
    std::string group;
    const auto flush_group = [&]() {
        const std::string item = trim(group);
        group.clear();
        if (item.empty())
            return;
        if (item.find_first_of(" \t") != std::string::npos && has_map &&
            cache.name_to_id.find(item) != cache.name_to_id.end())
        {
            out.push_back(item); // 整体就是一个含空格的标签名
            return;
        }
        for (const auto &tok : cliparse::split_whitespace(item))
            out.push_back(tok);
    };

    for (size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];
        // 分隔符：';'、「；」(EF BC 9B)、「、」(E3 80 81)
        const bool cjk_semicolon = c == '\xEF' && i + 2 < line.size() &&
                                   line[i + 1] == '\xBC' && line[i + 2] == '\x9B';
        const bool cjk_comma = c == '\xE3' && i + 2 < line.size() &&
                               line[i + 1] == '\x80' && line[i + 2] == '\x81';
        if (c == ';' || cjk_semicolon || cjk_comma)
        {
            flush_group();
            if (cjk_semicolon || cjk_comma)
                i += 2;
            continue;
        }
        group += c;
    }
    flush_group();
    return out;
}

// 把命令行报错文案里的「参数错误：」前缀去掉（交互式提示里已经有「错误：」）
std::string strip_error_prefix(std::string text)
{
    static const std::string prefix = "参数错误：";
    if (text.compare(0, prefix.size(), prefix) == 0)
        text = text.substr(prefix.size());
    return text;
}

// 解析并写入一项设置的新值；返回空串表示成功。
// 空输入一律表示「使用默认值」：即不传该参数，与命令行留空的默认值完全一致。
// 所有校验都通过后才提交（先解析到临时变量，最后一次性赋值）：输错一个标签或
// 路径时不会把之前设置好的内容清空（调用方会重新询问）。
std::string apply_value(State &st, const Setting &s, const std::string &line)
{
    // 本次解析的结果；校验失败时直接返回错误，目标设置保持原值
    std::vector<std::string> new_values;
    std::string new_value;

    if (line.empty())
    {
        // 空输入 = 使用默认值：清空该设置（与非交互模式下不传该参数一致）
        if (s.values)
            s.values->clear();
        if (s.value)
            s.value->clear();
        return "";
    }

    switch (s.kind)
    {
    case ValueKind::Text:
    case ValueKind::OutputFile:
        new_value = line;
        break;

    case ValueKind::CoverTitle:
        if (line[0] == '-')
            return "标题不能以 '-' 开头（会被当成命令行参数）；请换一个标题";
        new_value = line;
        break;

    case ValueKind::Tags:
    {
        const std::vector<std::string> tokens = split_tag_input(line);
        if (tokens.empty())
            return "请输入至少一个标签";
        if (tagcache::shared_cache_loaded())
        {
            const tagcache::Cache &cache = tagcache::shared_cache();
            for (const auto &tok : tokens)
                if (!tag_known(tok, cache))
                    return "标签 '" + tok + "' 不在标签缓存中；请检查拼写，"
                           "或先在「导出类型」中选择「打印标签 ID 对照表」查看全部标签";
        }
        new_values = tokens;
        break;
    }

    case ValueKind::Difficulties:
    {
        const std::vector<std::string> tokens = cliparse::split_whitespace(line);
        if (tokens.empty())
            return "请输入至少一个难度";
        for (const auto &tok : tokens)
        {
            std::vector<int> tmp;
            if (!cliparse::parse_difficulty_spec(tok, tmp))
                return "难度 '" + tok + "' 不合法；应为 0~8 的数字或闭区间（如 1-4）";
        }
        for (const auto &tok : tokens)
            add_unique(new_values, tok);
        break;
    }

    case ValueKind::Types:
    {
        const std::vector<std::string> tokens = cliparse::split_whitespace(line);
        if (tokens.empty())
            return "请输入至少一个题目类型";
        for (const auto &tok : tokens)
        {
            const std::string type = cliparse::to_upper_ascii(tok);
            if (type != "B" && type != "P")
                return "题目类型 '" + tok + "' 不合法；应为 B（基础题）或 P（普通题）";
            add_unique(new_values, type);
        }
        break;
    }

    case ValueKind::Pids:
    {
        const std::vector<std::string> tokens = cliparse::split_whitespace(line);
        if (tokens.empty())
            return "请输入至少一个题号";
        for (const auto &tok : tokens)
        {
            std::string prefix, suffix;
            unsigned long long num = 0;
            if (!luogu::parse_pid_parts(tok, prefix, num, suffix))
                return "题号 '" + tok + "' 不合法；应为字母 + 数字（如 P1001）";
            add_unique(new_values, cliparse::to_upper_ascii(tok));
        }
        break;
    }

    case ValueKind::PidRanges:
    {
        const std::vector<std::string> tokens = cliparse::split_whitespace(line);
        if (tokens.empty())
            return "请输入至少一组题号范围";
        for (const auto &tok : tokens)
        {
            std::pair<std::string, std::string> range;
            std::string err;
            if (!cliparse::parse_pid_range_arg(tok, range, err))
                return strip_error_prefix(err);
            add_unique(new_values, range.first + "-" + range.second);
        }
        break;
    }

    case ValueKind::Lang:
    {
        const std::string value = cliparse::to_lower_ascii(line);
        if (value == "zh-cn" || value == "zh" || value == "zh_cn")
        {
            new_value = "zh-CN";
            break;
        }
        if (value == "en")
        {
            new_value = "en";
            break;
        }
        return "题面语言 '" + line + "' 不合法；应为 zh-CN 或 en";
    }

    case ValueKind::Font:
    {
        if (line[0] == '-')
            return "字体名称或字体文件地址不能以 '-' 开头";
        if (cliparse::font_value_is_file_address(line))
        {
            std::error_code ec;
            const std::filesystem::path path = luogu::compat::path_from_utf8(line);
            if (!std::filesystem::exists(path, ec) || ec)
                return "字体文件 '" + line + "' 不存在；请检查路径，"
                       "或直接填写系统已安装的字体名称（如 SimSun）";
        }
        new_value = line;
        break;
    }

    case ValueKind::LocalFile:
    {
        if (line[0] == '-')
            return "文件地址不能以 '-' 开头（会被当成命令行参数）";
        std::error_code ec;
        const std::filesystem::path path = luogu::compat::path_from_utf8(line);
        if (!std::filesystem::exists(path, ec) || ec)
            return "文件 '" + line + "' 不存在；请检查路径";
        if (std::filesystem::is_directory(path, ec) && !ec)
            return "'" + line + "' 是目录；请指定一个 Markdown 文本文件";
        new_value = line;
        break;
    }

    case ValueKind::ArticleIds:
    {
        const std::vector<std::string> tokens = cliparse::split_whitespace(line);
        if (tokens.empty())
            return "请输入至少一个文章编号";
        for (const auto &tok : tokens)
        {
            const std::string lid = cliparse::to_lower_ascii(tok);
            if (!solution::valid_lid(lid))
                return "文章编号 '" + tok + "' 不合法；应为 6~32 位小写字母或数字"
                       "（如 p7fsb45w，可在文章页地址 /article/<编号> 中找到）";
            add_unique(new_values, lid);
        }
        break;
    }

    case ValueKind::CookieFile:
    {
        std::error_code ec;
        const std::filesystem::path path = luogu::compat::path_from_utf8(line);
        if (!std::filesystem::exists(path, ec) || ec)
            return "Cookie 文件 '" + line + "' 不存在；请先用浏览器扩展导出"
                   "Netscape 格式的 cookies.txt";
        if (std::filesystem::is_directory(path, ec) && !ec)
            return "'" + line + "' 是目录；请指定 cookies.txt 文件";
        new_value = line;
        st.cookie_string.clear(); // 与 Cookie 串二选一
        break;
    }

    case ValueKind::CookieString:
    {
        if (line.find('=') == std::string::npos)
            return "Cookie 串应形如 \"k=v; k2=v2\"；请检查是否漏掉了 '='";
        new_value = line;
        st.cookie_file.clear(); // 与 Cookie 文件二选一
        break;
    }

    case ValueKind::ArticleSource:
    {
        const std::string value = cliparse::to_lower_ascii(line);
        if (value != "auto" && value != "official" && value != "save")
            return "正文来源 '" + line + "' 不合法；应为 auto（缓存优先 + 两站轮流抓取）、"
                   "official（只用洛谷原站）或 save（只用洛谷保存站）";
        new_value = value;
        break;
    }

    case ValueKind::MaxSolutions:
    {
        const std::string value = cliparse::to_lower_ascii(line);
        if (value == "all")
        {
            new_value = value;
            break;
        }
        long parsed = 0;
        if (!cliparse::parse_positive_int(value, 1, 100000, parsed))
            return "篇数 '" + line + "' 不合法；应为正整数或 all（该题全部题解）";
        new_value = std::to_string(parsed);
        break;
    }

    case ValueKind::RequestDelay:
    {
        crawler::DelaySpec spec;
        std::string err;
        if (!crawler::parse_delay_spec(line, spec, err))
            return err.empty() ? ("请求间隔 '" + line + "' 不合法") : err;
        new_value = line;
        break;
    }

    case ValueKind::TtlDays:
    {
        long parsed = 0;
        if (!cliparse::parse_positive_int(line, 0, 36500, parsed))
            return "天数 '" + line + "' 不合法；应为不小于 0 的整数"
                   "（0 表示每次都发 ETag 条件请求）";
        new_value = std::to_string(parsed);
        break;
    }

    case ValueKind::RateLimitWait:
    {
        long parsed = 0;
        if (!cliparse::parse_positive_int(line, 0, 86400, parsed))
            return "秒数 '" + line + "' 不合法；应为不小于 0 的整数"
                   "（0 表示检测到限流直接停止）";
        new_value = std::to_string(parsed);
        break;
    }

    case ValueKind::SolutionPlacement:
    {
        const std::string value = cliparse::to_lower_ascii(line);
        if (value != "document-end" && value != "per-problem")
            return "题解位置 '" + line + "' 不合法；应为 document-end（统一置于文档最后）"
                   "或 per-problem（紧跟对应题目之后）";
        new_value = value;
        break;
    }
    }

    // 全部校验通过：一次性提交（与之前「先清空再写入」的成功路径等价）
    if (s.values)
        *s.values = new_values;
    if (s.value)
        *s.value = new_value;
    return "";
}

// 用户表示「本轮修改完毕」时检查设置是否齐全；返回空串表示可以继续
std::string check_requirements(const State &st)
{
    if (st.needs_problems())
    {
        const bool has_filter = !st.tags.empty() || !st.difficulties.empty() ||
                                !st.types.empty() || !st.pids.empty() ||
                                !st.pid_ranges.empty();
        if (!has_filter && !st.all_problems)
            return "尚未设置题目范围：请设置标签 / 难度 / 题目类型 / 题号 / 题号范围之一，"
                   "或选择「下载所有题目」（不设置任何筛选条件，导出全部题目）";
    }
    if (st.downloads_solutions() && st.cookie_file.empty() && st.cookie_string.empty())
        return "题解列表接口需要登录态：请设置「Cookie 文件」"
               "或「Cookie 串」";
    if (st.c_articles && st.articles.empty())
        return "尚未设置文章编号：请设置「文章编号」（如 p7fsb45w）";
    if (st.c_local)
    {
        if (st.local_file.empty())
            return "尚未设置本地 Markdown 文件：请设置「本地 Markdown 文件」";
        if (st.doc_only && st.paginate)
            return "「不输出封面、目录与页眉标题」不能与"
                   "「题目与题解（文章）之间分页」同时启用；请修改其中一项";
    }
    return "";
}

// ---- 询问序号 / 是否 ----

// 解析用户输入的序号（多个用空格分隔，去重并保持输入顺序）
bool parse_selection(const std::string &line, size_t count,
                     std::vector<size_t> &out, std::string &err)
{
    out.clear();
    const std::vector<std::string> tokens = cliparse::split_whitespace(line);
    if (tokens.empty())
    {
        err = "没有读到序号；请输入 1~" + std::to_string(count) + " 之间的序号";
        return false;
    }
    for (const auto &tok : tokens)
    {
        long value = 0;
        if (!cliparse::parse_positive_int(tok, 1, static_cast<long>(count), value))
        {
            err = "序号 '" + tok + "' 无效；请输入 1~" + std::to_string(count) +
                  " 之间的序号（多个序号用空格分隔）";
            return false;
        }
        const size_t index = static_cast<size_t>(value) - 1;
        if (std::find(out.begin(), out.end(), index) == out.end())
            out.push_back(index);
    }
    return true;
}

// [Y/n] / [y/N] 询问：返回 1（是）/ 0（否）/ -1（结束程序）
int ask_yes_no(Session &session, const std::string &question, bool default_value)
{
    for (;;)
    {
        std::string line;
        if (!session.ask(question + (default_value ? " [Y/n] " : " [y/N] "), line))
            return -1;
        if (line.empty())
            return default_value ? 1 : 0;
        const std::string value = cliparse::to_lower_ascii(line);
        if (value == "y" || value == "yes")
            return 1;
        if (value == "n" || value == "no")
            return 0;
        print_error("请输入 Y（是）或 n（否）");
    }
}

// 询问一项值参数的新值（校验不通过时重新询问；空输入 = 使用默认值）
bool ask_value(Session &session, State &st, const Setting &setting)
{
    print_line();
    print_line("修改「" + setting.label + "」");
    if (!setting.hint.empty())
        print_line("  " + setting.hint);
    print_line("  直接回车 = " + setting.empty_text);
    for (;;)
    {
        std::string line;
        if (!session.ask("新值（输入 q 结束程序）：", line))
            return false;
        const std::string err = apply_value(st, setting, line);
        if (err.empty())
            return true;
        print_error(err);
    }
}

// ---- 各步骤 ----

// 第 2 步：是否更新题目列表缓存与标签缓存（直接回车 = Y）
bool ask_update(Session &session, State &st)
{
    const int answer = ask_yes_no(
        session, "是否更新题目列表缓存与标签缓存？（需要联网）", true);
    if (answer < 0)
        return false;
    st.update_cache = (answer == 1);
    if (!st.update_cache)
        return true;

    const int code = app::run({"luogu-extract", "-U"});
    tagcache::reset_shared_cache(); // 更新后重新读盘，避免用到旧的标签表
    if (code != 0)
        print_notice("缓存更新失败（退出码 " + std::to_string(code) +
                     "）；将继续使用已有缓存（若缓存缺失，后续操作会提示先更新缓存）。");
    return true;
}

// 第 3 步：选择导出类型（只能选一个）
bool ask_action(Session &session, State &st)
{
    print_title("请选择要执行的操作（只可选一个）");
    print_line("  1) 导出 Markdown 文档");
    print_line("  2) 导出 LaTeX 文档");
    print_line("  3) 只抓取并缓存文章，不导出任何文件");
    print_line("  4) 清理缓存");
    print_line("  5) 打印标签 ID 对照表");
    for (;;)
    {
        std::string line;
        if (!session.ask("请输入序号（输入 q 结束程序）：", line))
            return false;
        std::vector<size_t> picks;
        std::string err;
        if (!parse_selection(line, 5, picks, err))
        {
            print_error(err);
            continue;
        }
        if (picks.size() != 1)
        {
            print_error("这里只能选择一个序号；请重新输入");
            continue;
        }
        switch (picks[0])
        {
        case 0: st.action = Action::Markdown; break;
        case 1: st.action = Action::Latex; break;
        case 2: st.action = Action::ArticlesOnly; break;
        case 3: st.action = Action::Clean; break;
        default: st.action = Action::Tags; break;
        }
        return true;
    }
}

// 第 3 步之「清理缓存」：选择要清理的内容（可多选），清理完回到第 3 步
bool ask_clean(Session &session)
{
    static const char *kCleanArgs[] = {"-C",    "-CIMG", "-CP",
                                       "-CF",   "-CS",   "-CA"};
    print_title("请选择要清理的缓存内容（可多选）");
    print_line("  1) 全部缓存（题目列表、标签、图片、字体、题解列表与文章）");
    print_line("  2) 图片缓存（images/ 目录）");
    print_line("  3) 题面缓存（latest.ndjson 与 latest.ndjson.gz）");
    print_line("  4) 字体缓存（fonts/ 目录）");
    print_line("  5) 题解列表缓存（solutions.ndjson）");
    print_line("  6) 文章缓存（articles/ 目录）");
    for (;;)
    {
        std::string line;
        if (!session.ask("请输入序号（多个序号用空格分隔，输入 q 结束程序）：", line))
            return false;
        if (line.empty())
        {
            print_error("请至少选择一项要清理的内容");
            continue;
        }
        std::vector<size_t> picks;
        std::string err;
        if (!parse_selection(line, 6, picks, err))
        {
            print_error(err);
            continue;
        }
        std::vector<std::string> args{"luogu-extract"};
        for (size_t index : picks)
            args.push_back(kCleanArgs[index]);
        const int code = app::run(args);
        tagcache::reset_shared_cache(); // 标签缓存可能已被删除，重新读盘
        if (code == 0)
            print_success("缓存清理完成。");
        else
            print_error("清理缓存失败（退出码 " + std::to_string(code) + "）。");
        return true;
    }
}

// 第 4 步：选择导出内容（可多选；直接回车 = 只导出题目）
bool ask_content(Session &session, State &st)
{
    // 本地 Markdown 只能转写为 LaTeX：导出 Markdown 时第 4 步不提供该选项
    const bool local_available = st.latex();
    const size_t option_count = local_available ? 4 : 3;

    print_title("请选择导出内容（可多选，直接回车 = 只导出题目）");
    print_line("  1) 题目（导出题面）");
    print_line("  2) 题解（需要 Cookie；题解与题面导出到同一个文件）");
    print_line("  3) 文章（按文章编号下载，导出的文章统一放在文档最后）");
    if (local_available)
        print_line("  4) 本地 Markdown 文件转写（不能与其它内容同时选择）");
    for (;;)
    {
        std::string line;
        if (!session.ask("请输入序号（多个序号用空格分隔，输入 q 结束程序）：", line))
            return false;
        if (line.empty())
        {
            st.c_problems = true;
            st.c_solutions = false;
            st.c_articles = false;
            st.c_local = false;
            return true;
        }
        std::vector<size_t> picks;
        std::string err;
        if (!parse_selection(line, option_count, picks, err))
        {
            print_error(err);
            continue;
        }
        const auto picked = [&picks](size_t index) {
            return std::find(picks.begin(), picks.end(), index) != picks.end();
        };
        const bool problems = picked(0);
        const bool solutions = picked(1);
        const bool articles = picked(2);
        const bool local = local_available && picked(3);
        if (local && picks.size() > 1)
        {
            print_error("「本地 Markdown 文件转写」不能与题目、题解、文章同时选择；"
                        "请重新输入");
            continue;
        }
        st.c_problems = problems;
        st.c_solutions = solutions;
        st.c_articles = articles;
        st.c_local = local;
        return true;
    }
}

// 选择题解后立即询问 Cookie（题解列表接口需要登录态）
bool ask_cookie(Session &session, State &st)
{
    print_title("题解需要登录态");
    print_line("  题解列表接口需要登录态，请提供浏览器导出的 Netscape 格式 cookies.txt；");
    print_line("  也可以稍后在设置里改填「Cookie 串」。");
    std::error_code ec;
    const bool has_default =
        std::filesystem::exists(luogu::compat::path_from_utf8("cookies.txt"), ec) && !ec;
    if (has_default)
        print_line("  当前目录下检测到 cookies.txt，直接回车即使用它。");

    const Setting setting = value_item("Cookie 文件", &st.cookie_file,
                                       ValueKind::CookieFile, "未设置");
    for (;;)
    {
        std::string line;
        if (!session.ask("请输入 cookies.txt 路径（输入 q 结束程序）：", line))
            return false;
        if (line.empty())
        {
            if (!has_default)
            {
                print_notice("未提供 Cookie：请在设置中填写「Cookie 文件」或"
                             "「Cookie 串」，否则无法启用题解功能。");
                return true;
            }
            line = "cookies.txt";
        }
        const std::string err = apply_value(st, setting, line);
        if (err.empty())
            return true;
        print_error(err);
    }
}

// 选择文章后立即询问文章编号
bool ask_articles(Session &session, State &st)
{
    print_title("请输入要下载的文章编号");
    print_line("  编号为 6~32 位小写字母或数字（如 p7fsb45w，取自文章页地址"
               " /article/<编号>）；");
    print_line("  多个编号用空格分隔，重复的编号只下载一次。");
    const Setting setting = multi_item("文章编号", &st.articles,
                                       ValueKind::ArticleIds, "未设置");
    for (;;)
    {
        std::string line;
        if (!session.ask("请输入文章编号（输入 q 结束程序）：", line))
            return false;
        if (line.empty())
        {
            print_error("必须至少提供一个文章编号；例如 p7fsb45w");
            continue;
        }
        const std::string err = apply_value(st, setting, line);
        if (err.empty())
            return true;
        print_error(err);
    }
}

// 选择本地转写后立即询问文件地址
bool ask_local_file(Session &session, State &st)
{
    print_title("请输入要转写的本地 Markdown 文件");
    print_line("  整行都会作为文件地址（可以含空格）；后缀名不限，编码自动识别。");
    const Setting setting = value_item("本地 Markdown 文件", &st.local_file,
                                       ValueKind::LocalFile, "未设置");
    for (;;)
    {
        std::string line;
        if (!session.ask("请输入文件地址（输入 q 结束程序）：", line))
            return false;
        if (line.empty())
        {
            print_error("必须提供一个 Markdown 文件地址");
            continue;
        }
        const std::string err = apply_value(st, setting, line);
        if (err.empty())
            return true;
        print_error(err);
    }
}

// 第 5 步：显示设置 → 按序号修改 → 再次显示……直到用户输入 n / 直接回车
bool settings_loop(Session &session, State &st)
{
    for (;;)
    {
        const std::vector<Setting> settings = build_settings(st);
        print_settings(settings, "当前设置（输入序号可修改，可多选）", true);
        std::string line;
        if (!session.ask("请输入要修改的设置序号（多个用空格分隔；"
                         "直接回车或输入 n 继续下一步）：",
                         line))
            return false;
        if (line.empty() || cliparse::to_lower_ascii(line) == "n")
        {
            const std::string problem = check_requirements(st);
            if (problem.empty())
                return true;
            print_error(problem);
            continue; // 设置不完整：重新显示设置让用户补齐
        }

        std::vector<size_t> picks;
        std::string err;
        if (!parse_selection(line, settings.size(), picks, err))
        {
            print_error(err);
            continue;
        }
        for (size_t index : picks)
        {
            const Setting &setting = settings[index];
            if (setting.flag)
            {
                *setting.flag = !*setting.flag;
                print_line("已将「" + setting.label + "」设为：" +
                           toggle_state_text(*setting.flag));
                // 「下载所有题目」与筛选条件互斥：启用时清空已有筛选条件
                if (setting.flag == &st.all_problems && st.all_problems)
                {
                    st.tags.clear();
                    st.difficulties.clear();
                    st.types.clear();
                    st.pids.clear();
                    st.pid_ranges.clear();
                    print_notice("已清空题目筛选条件（「下载所有题目」不使用筛选条件）。");
                }
                continue;
            }
            if (!ask_value(session, st, setting))
                return false;
            // 设置了筛选条件就不再是「下载所有题目」
            if (setting.values && !setting.values->empty() &&
                (setting.kind == ValueKind::Tags ||
                 setting.kind == ValueKind::Difficulties ||
                 setting.kind == ValueKind::Types ||
                 setting.kind == ValueKind::Pids ||
                 setting.kind == ValueKind::PidRanges))
            {
                st.all_problems = false;
            }
        }
        print_line();
        print_line("已应用修改，下面是新的设置：");
    }
}

// ---- 把设置翻译成命令行参数 ----

// 「下载所有题目」默认用「不传任何筛选参数」表达；但 --article 单独使用
// （没有任何题目筛选参数、也没有题解功能）时程序只导出文章、不导出题面，
// 因此「题目 + 文章 + 下载所有题目」这一种组合需要给一个恒真的筛选条件，
// 才能让题面与文章一起导出。洛谷题库的题目类型只有 B（基础题）与 P（普通题），
// 传入 --type B --type P 即「全部类型」，与不筛选等价。
bool needs_forced_all_filter(const State &st)
{
    return st.all_problems && st.c_problems && st.c_articles && !st.c_solutions;
}

std::vector<std::string> build_args(const State &st)
{
    std::vector<std::string> args{"luogu-extract"};

    // ---- 导出类型 ----
    if (st.action == Action::Markdown)
        args.push_back("-M");
    else if (st.action == Action::Latex)
        args.push_back("-L");
    else if (st.action == Action::ArticlesOnly)
        args.push_back("--articles-only-download");

    // ---- 导出内容 ----
    if (st.c_problems && st.c_solutions)
        args.push_back("--with-solutions");
    else if (st.c_solutions)
    {
        args.push_back("--with-solutions");
        args.push_back("--solutions-only"); // 只导出题解，不导出题面
    }

    // ---- 题目筛选 ----
    for (const auto &tag : st.tags)
        args.push_back("--tag=" + tag);
    for (const auto &difficulty : st.difficulties)
        args.push_back("--difficulty=" + difficulty);
    for (const auto &type : st.types)
        args.push_back("--type=" + type);
    for (const auto &pid : st.pids)
        args.push_back("--pid=" + pid);
    for (const auto &range : st.pid_ranges)
        args.push_back("--pid-range=" + range);
    if (needs_forced_all_filter(st))
    {
        args.push_back("--type=B");
        args.push_back("--type=P");
    }
    if (!st.lang.empty())
        args.push_back("--lang=" + st.lang);

    // ---- 输出与显示 ----
    if (st.exporting())
    {
        if (!st.output.empty())
            args.push_back("--output=" + st.output);
        if (!st.cover_title.empty())
            args.push_back("--set-cover-title=" + st.cover_title);
        if (!st.c_local)
        {
            if (!st.show_source_tags)
                args.push_back("--no-show-source-tags");
            if (st.show_algorithm_tags)
                args.push_back("--show-algorithm-tags");
            if (st.show_difficulty_tags)
                args.push_back("--show-difficulty-tags");
        }
    }

    // ---- LaTeX 排版 ----
    if (st.latex())
    {
        if (!st.toc_links)
            args.push_back("--no-toc-links");
        if (st.toc_backlinks)
            args.push_back("--toc-backlinks");
        if (st.toc_difficulty)
            args.push_back("--show-contents-difficulty-tags");
        if (st.paginate)
            args.push_back("--paginate");
        if (st.new_download)
            args.push_back("--new-download");
        if (!st.bilibili_links)
            args.push_back("--no-bilibili-link");
        if (!st.font_cover.empty())
            args.push_back("--set-font-cover-page=" + st.font_cover);
        if (!st.font_body_zh.empty())
            args.push_back("--set-font-body-zh-CN=" + st.font_body_zh);
        if (!st.font_body_en.empty())
            args.push_back("--set-font-body-en-US=" + st.font_body_en);
        if (!st.font_body_codes.empty())
            args.push_back("--set-font-body-codes=" + st.font_body_codes);
        if (!st.font_title_zh.empty())
            args.push_back("--set-font-title-zh-CN=" + st.font_title_zh);
        if (!st.font_title_en.empty())
            args.push_back("--set-font-title-en-US=" + st.font_title_en);
        if (st.compile_latex)
            args.push_back("--compile");
    }

    // ---- 本地 Markdown 转写 ----
    if (st.c_local)
    {
        args.push_back("--local=" + st.local_file);
        if (st.doc_only)
            args.push_back("--doc-only");
    }

    // ---- 文章 ----
    for (const auto &lid : st.articles)
        args.push_back("--article=" + lid);

    // ---- 题解 / 文章抓取 ----
    if (st.downloads_any())
    {
        if (!st.cookie_file.empty())
            args.push_back("--cookie=" + st.cookie_file);
        if (!st.cookie_string.empty())
            args.push_back("--cookie-string=" + st.cookie_string);
        if (!st.article_source.empty())
            args.push_back("--article-source=" + st.article_source);
        if (!st.request_delay.empty())
            args.push_back("--request-delay=" + st.request_delay);
        if (!st.delay_auto_scale)
            args.push_back("--no-delay-auto-scale");
        if (!st.rate_limit_wait.empty())
            args.push_back("--rate-limit-wait=" + st.rate_limit_wait);
        if (!st.article_ttl.empty())
            args.push_back("--article-ttl=" + st.article_ttl);
        if (st.refresh_articles)
            args.push_back("--refresh-articles");
        if (st.allow_partial)
            args.push_back("--allow-partial");
        if (st.downloads_solutions())
        {
            if (!st.max_solutions.empty())
                args.push_back("--max-solutions=" + st.max_solutions);
            if (!st.solution_ttl.empty())
                args.push_back("--solution-ttl=" + st.solution_ttl);
            if (st.refresh_solutions)
                args.push_back("--refresh-solutions");
        }
        if (st.exporting())
        {
            if (st.downloads_solutions())
            {
                if (!st.solution_placement.empty())
                    args.push_back("--solution-placement=" + st.solution_placement);
                if (!st.problem_to_solution_link)
                    args.push_back("--no-problem-to-solution-link");
                if (!st.solution_to_problem_link)
                    args.push_back("--no-solution-to-problem-link");
            }
            if (!st.article_meta)
                args.push_back("--no-article-meta");
            if (st.latex() && !st.solution_toc)
                args.push_back("--no-solution-toc");
        }
    }

    return args;
}

// 把参数拼成可以直接复制执行的命令行（Cookie 串不打印明文）：
// 含空格的参数加引号（"--opt=值" 形式只给值加引号）
std::string shell_preview(const std::vector<std::string> &args)
{
    std::string out;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i)
            out += " ";
        std::string token = args[i];
        if (token.compare(0, 16, "--cookie-string=") == 0)
        {
            out += "--cookie-string=******";
            continue;
        }
        if (token.find(' ') == std::string::npos &&
            token.find('\t') == std::string::npos)
        {
            out += token;
            continue;
        }
        const size_t eq = token.find('=');
        if (eq == std::string::npos)
            out += "\"" + token + "\"";
        else
            out += token.substr(0, eq + 1) + "\"" + token.substr(eq + 1) + "\"";
    }
    return out;
}

// 第 7 步：输出完整的设置内容（含是否自动编译）并请求确认
void print_summary(State &st)
{
    const std::vector<Setting> settings = build_settings(st);
    print_settings(settings, "本次设置", false);
    const std::string compile_state =
        st.latex() ? toggle_state_text(st.compile_latex)
                   : value_state_text("不适用（本次不导出 LaTeX）", true);
    std::printf("  %s：%s\n", "导出完成后自动编译 LaTeX", compile_state.c_str());
    const std::string update_state =
        st.update_cache ? value_state_text("已在本次运行开始时更新", false)
                        : value_state_text("本次运行开始时未更新", true);
    std::printf("  %s：%s\n", "题目列表与标签缓存", update_state.c_str());
    if (needs_forced_all_filter(st))
        print_notice("  提示：「题目 + 文章 + 下载所有题目」会自动附加等价的"
                     "「全部类型」筛选（基础题 B、普通题 P 都选），"
                     "以免文章与题目一起导出时缺少题面。");
    print_line();
    print_line("等价的命令行参数（可直接复制执行）：");
    print_line("  " + shell_preview(build_args(st)));
    print_line();
}

// 第 7 步：是否开始（Y/n；n 返回第 5 步继续修改设置）
int ask_start(Session &session)
{
    for (;;)
    {
        std::string line;
        if (!session.ask("是否开始？[Y/n] ", line))
            return -1;
        if (line.empty())
            return 1;
        const std::string value = cliparse::to_lower_ascii(line);
        if (value == "y" || value == "yes")
            return 1;
        if (value == "n" || value == "no")
            return 0;
        print_error("请输入 Y（开始）或 n（返回上一步修改设置）");
    }
}

} // namespace

int interactive::run()
{
    Session session;
    State st;

    std::printf("%s %s 简易命令行交互程序\n", LUOGU_EXTRACT_PROJECT_NAME,
                LUOGU_EXTRACT_VERSION);
    print_line("带参数运行可跳过交互，直接按参数执行（-h 查看全部参数）。");
    print_line("提示：每一步都可以输入 q 并回车结束程序；直接回车表示使用默认值。");

    // ---- 第 2 步：是否更新题目列表缓存与标签缓存 ----
    if (!ask_update(session, st))
        return 0;

    // ---- 第 3 步起：选择操作；清理缓存与打印标签表完成后回到第 3 步 ----
    for (;;)
    {
        if (!ask_action(session, st))
            return 0;

        if (st.action == Action::Clean)
        {
            if (!ask_clean(session))
                return 0;
            continue;
        }
        if (st.action == Action::Tags)
        {
            app::run({"luogu-extract", "--tags"});
            continue;
        }

        // ---- 第 4 步：导出内容 ----
        if (st.action == Action::ArticlesOnly)
        {
            st.c_problems = false;
            st.c_solutions = false;
            st.c_articles = true;
            st.c_local = false;
            print_line();
            print_line("「只抓取并缓存文章」：本次只下载文章并写入缓存，不导出任何文件。");
            if (!ask_articles(session, st))
                return 0;
        }
        else
        {
            if (!ask_content(session, st))
                return 0;
            if (st.c_articles && !ask_articles(session, st))
                return 0;
            if (st.c_solutions && !ask_cookie(session, st))
                return 0;
            if (st.c_local && !ask_local_file(session, st))
                return 0;
        }

        // ---- 第 5~7 步：设置 → （LaTeX 时）自动编译 → 确认；n 回到第 5 步 ----
        bool start = false;
        while (!start)
        {
            if (!settings_loop(session, st))
                return 0;
            if (st.latex())
            {
                const int answer = ask_yes_no(
                    session,
                    "导出完成后自动编译 LaTeX 文档？（相当于执行 "
                    "latexmk --xelatex <输出文件名>.tex）",
                    st.compile_latex);
                if (answer < 0)
                    return 0;
                st.compile_latex = (answer == 1);
            }
            print_summary(st);
            const int answer = ask_start(session);
            if (answer < 0)
                return 0;
            start = (answer == 1);
            if (!start)
            {
                print_line();
                print_notice("已返回上一步：请重新输入要修改的设置序号"
                             "（或直接回车再次确认）。");
            }
        }

        // ---- 第 8~9 步：按设置执行（与非交互模式完全一致），完成后退出程序 ----
        return app::run(build_args(st));
    }
}
