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

// include/luogu-extract/contents/solution_task.h
// 题解抓取的「计划阶段」与「抓取阶段」：计划阶段只读缓存、不访问网络
// （resolve_lists 例外），算出待抓篇数 N 与列表请求数 P；抓取阶段逐题串行抓取。
#ifndef LUOGU_EXTRACT_CONTENTS_SOLUTION_TASK_H
#define LUOGU_EXTRACT_CONTENTS_SOLUTION_TASK_H

#include <string>
#include <vector>
#include "luogu-extract/contents/problem.h"
#include "luogu-extract/contents/solution.h"
#include "luogu-extract/crawler/request_gate.h"
#include "luogu-extract/export/common.h"

namespace solution
{
    /// 题解 / 文章抓取与导出的运行参数
    struct TaskOptions
    {
        Source source = Source::Official; // --article-source
        // --article：按文章编号单独下载（顺序即导出顺序，main 已校验并去重）；
        // 与题解正文共用来源、TTL、刷新与完整性控制选项
        std::vector<std::string> article_lids;
        // --max-solutions：正整数 = 每题取前 n 篇；< 0 = all（该题全部，不设上限）
        int max_articles = 1;
        // --solution-ttl：题解**列表**有效期（天）；< 0 无限（默认），
        // 0 表示每次发 ETag 条件请求（304 时只刷新时间戳）
        int list_ttl_days = -1;
        // --article-ttl：题解**正文**有效期（天），语义同上
        int article_ttl_days = -1;
        bool refresh_solutions = false;   // --refresh-solutions
        bool refresh_articles = false;    // --refresh-articles
        bool allow_partial = false;       // --allow-partial
    };

    /// 计划阶段：一道题里的一篇题解
    struct ArticlePlan
    {
        Summary summary;                 // 列表摘要（顺序即列表顺序）
        bool cached = false;             // 已命中缓存（无需网络请求）
        Source cached_source = Source::Official; // 命中的缓存来自哪个来源
        // auto 模式分配到的通道（0 原站 / 1 保存站）；非 auto 固定为所选来源的通道，
        // cached 为 true 时无意义
        int site = 0;
    };

    /// 计划阶段：每道题的计划
    struct ProblemPlan
    {
        std::string pid;
        std::string name;
        bool list_cached = false;    // 缓存里有列表
        bool list_known = false;     // 列表已就绪（缓存可用，或计划阶段已抓取）
        bool list_from_network = false; // 计划阶段通过网络抓到的列表（抓取阶段只落盘）
        bool list_not_modified = false; // 计划阶段条件请求命中 304
        std::string list_etag;
        std::vector<Summary> list_items;
        int total_available = 0;
        int available = 0;           // 已知可用篇数
        int wanted = 0;              // 本次打算抓的篇数（含已命中缓存）
        int cached = 0;              // 已命中缓存的篇数
        int to_fetch = 0;            // 需要网络请求的篇数
        bool no_solution = false;    // 列表显示该题没有题解
        std::vector<ArticlePlan> articles; // 本次处理的篇目（前 wanted 篇）
    };

    /// 计划阶段：一篇按文章编号下载的文章（--article）
    struct StandaloneArticlePlan
    {
        std::string lid;
        bool cached = false;             // 已命中缓存（无需网络请求）
        Source cached_source = Source::Official; // 命中的缓存来自哪个来源
        // 含义同 ArticlePlan::site
        int site = 0;
    };

    /// 计划阶段的汇总（风险分级与延时系数的输入）
    struct Plan
    {
        std::vector<ProblemPlan> items;
        // --article 指定的文章（顺序即导出顺序），统一放在文档最后
        std::vector<StandaloneArticlePlan> standalone_articles;
        long long problems = 0;          // 选中题目数
        long long articles_to_fetch = 0; // N：实际待抓正文篇数（题解 + 文章）
        long long cached_articles = 0;   // 已命中缓存的篇数（题解 + 文章）
        long long solution_to_fetch = 0; // 其中题解正文待抓篇数
        long long cached_solutions = 0;  // 其中题解正文命中缓存的篇数
        long long standalone_to_fetch = 0; // 其中文章待抓篇数
        long long cached_standalone = 0;   // 其中文章命中缓存的篇数
        long long list_requests = 0;     // P：需要重新获取列表的题目数
        long long total_requests = 0;    // N + P
        long long no_solution_problems = 0;
    };

    /// 计划阶段的结果状态
    enum class PlanStatus
    {
        Ok,
        Error,        // 列表接口异常（结构异常 / 网络错误等）
        NeedLogin,    // 需要登录态
        RateLimited,  // 因限流中止
        Stopped,      // 用户主动停止
    };

    struct PlanResult
    {
        PlanStatus status = PlanStatus::Ok;
        std::string error;
    };

    /// 计划阶段。默认只读缓存、不发网络请求；resolve_lists 为 true 时把缺失/过期的
    /// 列表在计划阶段就抓回来（列表请求本就是 N + P 中的 P，受请求闸门控制），
    /// 使风险确认前就能得到精确正文篇数（--max-solutions all 无上限，必须靠它）。
    /// opt.article_lids（--article）指定的文章同样计入 N。
    PlanResult make_plan(const std::vector<problem::Problem> &problems,
                         const TaskOptions &opt, bool resolve_lists, Plan &plan);

    /// 抓取阶段的统计
    struct CrawlStats
    {
        int problems_handled = 0;      // 处理过的题目数
        int fetched = 0;               // 本次新抓取的正文篇数（题解 + 文章）
        int fetched_official = 0;      // 其中取自洛谷原站的篇数
        int fetched_save = 0;          // 其中取自保存站的篇数
        int fetched_list = 0;          // 本次新抓取的列表数
        int cached = 0;                // 命中缓存的正文篇数（题解 + 文章）
        int not_modified = 0;          // 条件请求命中 304 的正文篇数
        int failed = 0;                // 抓取失败的篇数
        int problems_no_solution = 0;  // 确实没有题解的题目数
        int skipped_inaccessible = 0;  // 不可访问（已删除/无权限/付费）的篇数
        int skipped_incomplete = 0;    // 正文不完整且未允许导出的篇数
        int incomplete_exported = 0;   // 正文不完整但按 --allow-partial 导出的篇数
        // --article：本次处理的文章数，以及其中新抓取 / 命中缓存 / 未拿到的篇数
        int articles_total = 0;
        int articles_fetched = 0;
        int articles_cached = 0;
        int articles_skipped = 0;
        bool stopped_by_user = false;  // 用户主动停止（退出码 0）
        bool stopped_by_rate_limit = false; // 因限流中止（退出码非 0）
        std::string stop_reason;
    };

    /// 抓取阶段：逐题串行抓取列表与正文，落缓存并组装导出用的题解包；
    /// --article 指定的文章按同一套来源/延时/限流规则抓取，顺序与命令行一致。
    /// 返回 false 表示因错误中止（error 给出中文说明）；用户主动停止 / 限流中止
    /// 不算错误，通过 stats 的标志位返回
    bool crawl(const Plan &plan, const TaskOptions &opt,
               luogu::SolutionBundle &bundle, luogu::ArticleBundle &articles,
               CrawlStats &stats, std::string &error);

    /// 把抓取阶段的统计汇总成一行中文说明
    std::string describe_crawl_stats(const CrawlStats &stats);
} // namespace solution

#endif // LUOGU_EXTRACT_CONTENTS_SOLUTION_TASK_H
