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

// src/contents/solution_task.cpp
#include "luogu-extract/contents/solution_task.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "luogu-extract/crawler/request_gate.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/solution_cache.h"

namespace
{
const char *kColorYellow = "\033[1;33m";
const char *kColorReset = "\033[0m";

void print_warning(const std::string &message)
{
    // auto 模式下两个工作线程都会打印，用互斥锁避免输出交错
    static std::mutex warn_mutex;
    std::lock_guard<std::mutex> lock(warn_mutex);
    std::printf("%s%s%s\n", kColorYellow, message.c_str(), kColorReset);
    std::fflush(stdout);
}

long long now_seconds() { return static_cast<long long>(std::time(nullptr)); }

// 该题本次打算抓取的篇数（含已命中缓存的部分）。
// all（max_solutions < 0）没有任何篇数上限：列表已知时就是全部可用篇数；
// 列表未知时按一页（10 篇）估算，仅用于给请求闸门一个初步的延时系数——
// all 模式下计划阶段会把列表抓全，随后用精确值重新计算。
int wanted_count(int max_solutions, int available, bool known)
{
    if (max_solutions < 0)
        return known ? available : 10;
    if (known)
        return std::min(max_solutions, available);
    return max_solutions;
}

// 正文缓存是否可直接使用（未过期且完整、未要求强制刷新）。
// 有效期由 --article-ttl 单独控制：默认无限，即正文一律优先用缓存
bool doc_cache_usable(const solcache::DocEntry &doc, const solution::TaskOptions &opt)
{
    if (opt.refresh_articles)
        return false;
    if (!doc.content_full || doc.content.empty())
        return false; // 上次抓到的正文不完整：本次重抓
    return solcache::is_fresh(doc.fetched_at, opt.article_ttl_days);
}

// 列表缓存是否可直接使用。有效期由 --solution-ttl 单独控制：
// 默认无限，即只要缓存里有列表就一直用
bool list_cache_usable(const solcache::ListEntry &list, const solution::TaskOptions &opt)
{
    if (opt.refresh_solutions)
        return false;
    return solcache::is_fresh(list.fetched_at, opt.list_ttl_days);
}

std::string article_url_for(const std::string &lid)
{
    return crawler::endpoints().official_base + "/article/" + lid;
}
} // namespace

solution::PlanResult solution::make_plan(const std::vector<problem::Problem> &problems,
                                           const TaskOptions &opt, bool resolve_lists,
                                           Plan &plan)
{
    PlanResult result;
    plan = Plan();
    plan.problems = static_cast<long long>(problems.size());
    plan.items.reserve(problems.size());

    for (const auto &p : problems)
    {
        ProblemPlan item;
        item.pid = p.pid;
        item.name = p.name;

        solcache::ListEntry cached;
        const bool has_cache = solcache::load_list(p.pid, cached);
        item.list_cached = has_cache;
        const bool cache_usable = has_cache && list_cache_usable(cached, opt);

        if (cache_usable)
        {
            item.list_known = true;
            item.list_items = cached.items;
            item.list_etag = cached.etag;
            item.total_available = cached.total_available;
        }
        else
        {
            // 需要重新获取列表（翻页数在抓取时才知道，按 1 页起步估算）
            ++plan.list_requests;
            if (resolve_lists)
            {
                // 计划阶段就把列表抓回来（受请求闸门控制）：
                // 这样「实际待抓正文篇数」在风险确认前就是精确值
                const int need = (opt.max_solutions < 0) ? -1 : opt.max_solutions;
                const std::string etag = has_cache ? cached.etag : std::string();
                ListFetch fetched = fetch_list(p.pid, need, etag);

                // 列表接口异常（含结构异常）绝不当作「无题解」静默跳过
                if (fetched.stopped || fetched.rate_limited || fetched.need_login ||
                    !fetched.ok)
                {
                    result.status = fetched.stopped
                                        ? PlanStatus::Stopped
                                        : (fetched.rate_limited
                                               ? PlanStatus::RateLimited
                                               : (fetched.need_login
                                                      ? PlanStatus::NeedLogin
                                                      : PlanStatus::Error));
                    result.error = fetched.error;
                    return result;
                }

                item.list_known = true;
                item.list_from_network = true;
                item.list_not_modified = fetched.not_modified && has_cache;
                if (item.list_not_modified)
                {
                    // 304：沿用缓存里的条目与 ETag，只会在抓取阶段刷新时间戳
                    item.list_items = cached.items;
                    item.list_etag = cached.etag;
                    item.total_available = cached.total_available;
                }
                else
                {
                    item.list_items = fetched.items;
                    item.list_etag = fetched.etag;
                    item.total_available = fetched.total_available;
                }
            }
        }

        item.available = static_cast<int>(item.list_items.size());
        item.no_solution = item.list_known && item.list_items.empty();
        item.wanted = wanted_count(opt.max_solutions, item.available, item.list_known);

        // 逐篇计划：判定缓存命中（auto 模式下两个来源都算，优先原站），
        // 需要抓取的按来源轮流分配站点
        if (item.list_known)
        {
            const int take =
                std::min(item.wanted, static_cast<int>(item.list_items.size()));
            for (int i = 0; i < take; ++i)
            {
                ArticlePlan article;
                article.summary = item.list_items[i];

                auto usable_cache = [&](Source src, solcache::DocEntry &doc) {
                    return solcache::load_doc(p.pid, article.summary.lid, src, doc) &&
                           doc_cache_usable(doc, opt);
                };

                solcache::DocEntry doc;
                if (opt.source == Source::Auto)
                {
                    // 两个来源的缓存都优先使用，同时存在时优先原站
                    if (usable_cache(Source::Official, doc))
                    {
                        article.cached = true;
                        article.cached_source = Source::Official;
                    }
                    else if (usable_cache(Source::Save, doc))
                    {
                        article.cached = true;
                        article.cached_source = Source::Save;
                    }
                }
                else if (usable_cache(opt.source, doc))
                {
                    article.cached = true;
                    article.cached_source = opt.source;
                }

                if (article.cached)
                {
                    ++item.cached;
                }
                else
                {
                    ++item.to_fetch;
                    // auto：原站、保存站轮流分配（这篇原站 → 下篇保存站 → …）
                    article.site = (opt.source == Source::Auto)
                                       ? static_cast<int>(plan.articles_to_fetch % 2)
                                       : (opt.source == Source::Save ? 1 : 0);
                    ++plan.articles_to_fetch;
                }
                item.articles.push_back(std::move(article));
            }
        }
        else
        {
            // 列表未知（非 all 模式、且缓存不可用）：按最坏情况估算待抓篇数
            item.to_fetch = item.wanted;
            plan.articles_to_fetch += item.wanted;
        }

        if (item.no_solution)
            ++plan.no_solution_problems;
        plan.cached_articles += item.cached;
        plan.items.push_back(std::move(item));
    }

    plan.problems = static_cast<long long>(problems.size());
    plan.total_requests = plan.articles_to_fetch + plan.list_requests;
    return result;
}

namespace
{
// 一次正文抓取的处理结果
struct FetchOutcome
{
    enum class Kind
    {
        Ok,                 // 抓取成功（可能是 304，沿用缓存正文）
        NotFound,           // 不可访问（已删除 / 无权限）
        SkippedIncomplete,  // 正文不完整且未允许导出
        PartialExported,    // 正文不完整但按 --allow-partial 导出
        Failed,             // 网络或解析失败
        NeedLogin,          // 需要登录态
        RateLimited,        // 被限流（auto 模式交给调度器处理）
        Stopped,            // 用户停止
    };
    Kind kind = Kind::Ok;
    solcache::DocEntry doc;
    std::string error;
    bool not_modified = false;
};

// 抓取一篇题解的正文并落缓存（顺序模式与 auto 模式共用）
FetchOutcome fetch_one_article(const std::string &pid, const solution::Summary &summary,
                               solution::Source site, const solution::TaskOptions &opt)
{
    FetchOutcome out;
    const crawler::Channel ch = solution::channel_of(site);

    // 该来源已有的缓存：用于 ETag 条件请求
    solcache::DocEntry existing;
    const bool has_existing =
        solcache::load_doc(pid, summary.lid, site, existing) &&
        existing.content_full && !existing.content.empty();
    const std::string etag = has_existing ? existing.etag : std::string();

    solution::ArticleFetch fetched = solution::fetch_article(site, summary.lid, etag, ch);

    if (fetched.stopped)
    {
        out.kind = FetchOutcome::Kind::Stopped;
        out.error = fetched.error;
        return out;
    }
    if (fetched.rate_limited)
    {
        out.kind = FetchOutcome::Kind::RateLimited;
        out.error = fetched.error.empty() ? std::string("被限流") : fetched.error;
        return out;
    }
    if (fetched.not_found)
    {
        out.kind = FetchOutcome::Kind::NotFound;
        out.error = fetched.error;
        return out;
    }
    if (fetched.need_login)
    {
        out.kind = FetchOutcome::Kind::NeedLogin;
        out.error = fetched.error;
        return out;
    }
    if (!fetched.ok)
    {
        out.kind = FetchOutcome::Kind::Failed;
        out.error = fetched.error;
        return out;
    }

    if (fetched.not_modified && has_existing)
    {
        existing.fetched_at = now_seconds();
        std::string store_error;
        if (!solcache::store_doc(existing, store_error))
            print_warning("刷新题解缓存时间失败：" + store_error);
        out.doc = std::move(existing);
        out.not_modified = true;
        return out;
    }

    const article::Article &a = fetched.article;
    solcache::DocEntry entry;
    entry.pid = pid;
    entry.lid = summary.lid;
    entry.source = solution::source_key(site);
    entry.fetched_at = now_seconds();
    entry.etag = fetched.etag;
    entry.title = a.title;
    entry.author_uid = a.author_uid;
    entry.author_name = a.author_name;
    entry.time = a.time;
    entry.upvote = a.upvote;
    entry.reply_count = a.reply_count;
    entry.favor_count = a.favor_count;
    entry.category = a.category;
    entry.promote_status = a.promote_status;
    entry.difficulty = summary.difficulty;
    entry.solution_pid = a.solution_pid;
    entry.solution_type = a.solution_type;
    entry.solution_name = a.solution_name;
    entry.content_full = a.content_full;
    entry.content = a.content;
    entry.url = article_url_for(summary.lid);

    if (a.content.empty() || !a.content_full)
    {
        // 正文不完整：默认跳过并计入汇总；--allow-partial 才导出并标注
        if (!opt.allow_partial)
        {
            out.kind = FetchOutcome::Kind::SkippedIncomplete;
            out.error = "正文不完整";
            return out;
        }
        out.kind = FetchOutcome::Kind::PartialExported;
    }

    std::string store_error;
    if (!solcache::store_doc(entry, store_error))
        print_warning("写入题解缓存失败：" + store_error);
    out.doc = std::move(entry);
    return out;
}

// 把抓取到的正文组装成导出用的题解视图
luogu::SolutionView make_view(const solution::Summary &summary,
                              const solcache::DocEntry &doc,
                              solution::Source site)
{
    luogu::SolutionView view;
    view.lid = summary.lid;
    view.title = !summary.title.empty()
                     ? summary.title
                     : (doc.title.empty() ? summary.lid : doc.title);
    view.author_name =
        !doc.author_name.empty() ? doc.author_name : summary.author_name;
    view.time = doc.time > 0 ? doc.time : summary.time;
    view.upvote = doc.upvote > 0 ? doc.upvote : summary.upvote;
    view.content = doc.content;
    view.content_full = doc.content_full;
    view.source_name = solution::source_display_name(site);
    view.source_url = doc.url.empty() ? article_url_for(summary.lid) : doc.url;
    return view;
}

// 顺序模式：把一次抓取结果计入统计
void apply_outcome(const FetchOutcome &outcome, solution::CrawlStats &stats)
{
    switch (outcome.kind)
    {
    case FetchOutcome::Kind::Ok:
        ++stats.fetched;
        if (outcome.doc.source == "save")
            ++stats.fetched_save;
        else
            ++stats.fetched_official;
        if (outcome.not_modified)
            ++stats.not_modified;
        break;
    case FetchOutcome::Kind::PartialExported:
        ++stats.fetched;
        if (outcome.doc.source == "save")
            ++stats.fetched_save;
        else
            ++stats.fetched_official;
        ++stats.incomplete_exported;
        break;
    case FetchOutcome::Kind::NotFound:
        ++stats.skipped_inaccessible;
        break;
    case FetchOutcome::Kind::SkippedIncomplete:
        ++stats.skipped_incomplete;
        break;
    case FetchOutcome::Kind::Failed:
        ++stats.failed;
        break;
    default:
        break;
    }
}

// ---- auto 模式：两条通道并行抓取的调度 ----

struct AutoTask
{
    size_t bucket = 0;          // 属于第几道题
    std::string pid;
    solution::Summary summary;
    int preferred = 0;          // 首选通道：0 原站 / 1 保存站
    int retries = 0;            // 已因「本站点没有这篇/抓取失败」改派过几次
};

struct AutoContext
{
    std::vector<AutoTask> tasks;
    std::vector<luogu::SolutionView> slots;  // 与 tasks 一一对应
    std::vector<char> slot_state;            // 0=未完成 1=成功（可导出） 2=跳过
    const solution::TaskOptions *opt = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<size_t> queue;
    size_t in_flight = 0;
    bool abandoned[2] = {false, false};
    bool stop = false;
    bool both_abandoned = false;
    bool stopped_by_user = false;
    std::string stop_reason;

    // 统计（在 mutex 保护下更新）
    int fetched = 0, fetched_official = 0, fetched_save = 0;
    int not_modified = 0, failed = 0, skipped_inaccessible = 0;
    int skipped_incomplete = 0, incomplete_exported = 0;
};

bool auto_site_available(AutoContext &ctx, int site)
{
    if (ctx.abandoned[site])
        return false;
    const crawler::Channel ch =
        site == 0 ? crawler::Channel::Official : crawler::Channel::Save;
    return crawler::gate_channel_block_remaining_ms(ch) == 0;
}

// 挑选下一个可执行的任务：只取「首选本通道」的任务。
//
// 不做贪婪接管是有意为之：某个站点被限流时，只有**当前这一篇**会临时
// 转给另一个站点，本站点其余任务留在自己队列里，等限流延时到期后重试；
// 重试若再次被限流才继续累计次数，连续 3 次才放弃该站点并把剩余任务
// 全部转走（如果允许另一侧随便接管，本站点就再也不会有重试机会，
// 「连续 3 次」的判定也就永远不会触发）。
size_t auto_pick_task(AutoContext &ctx, int site)
{
    for (size_t i = 0; i < ctx.queue.size(); ++i)
        if (ctx.tasks[ctx.queue[i]].preferred == site)
            return i;
    return static_cast<size_t>(-1);
}

// 把抓取结果计入自动模式的统计（调用方已持有 mutex）
void auto_apply_locked(AutoContext &ctx, size_t slot, int site,
                       const FetchOutcome &outcome)
{
    switch (outcome.kind)
    {
    case FetchOutcome::Kind::Ok:
    case FetchOutcome::Kind::PartialExported:
        ctx.slots[slot] = make_view(
            ctx.tasks[slot].summary, outcome.doc,
            solution::site_of(site == 0 ? crawler::Channel::Official
                                        : crawler::Channel::Save));
        ctx.slot_state[slot] = 1;
        if (site == 0)
            ++ctx.fetched_official;
        else
            ++ctx.fetched_save;
        if (outcome.not_modified)
            ++ctx.not_modified;
        if (outcome.kind == FetchOutcome::Kind::PartialExported)
            ++ctx.incomplete_exported;
        break;
    case FetchOutcome::Kind::NotFound:
        ctx.slot_state[slot] = 2;
        ++ctx.skipped_inaccessible;
        break;
    case FetchOutcome::Kind::SkippedIncomplete:
        ctx.slot_state[slot] = 2;
        ++ctx.skipped_incomplete;
        break;
    case FetchOutcome::Kind::Failed:
        ctx.slot_state[slot] = 2;
        ++ctx.failed;
        break;
    default:
        break;
    }
}

void auto_worker(AutoContext &ctx, int site)
{
    const crawler::Channel ch =
        site == 0 ? crawler::Channel::Official : crawler::Channel::Save;
    for (;;)
    {
        size_t slot = 0;
        {
            std::unique_lock<std::mutex> lock(ctx.mutex);
            for (;;)
            {
                if (ctx.stop)
                    return;
                if (crawler::gate_user_stopped())
                {
                    ctx.stopped_by_user = true;
                    ctx.stop_reason = "已按你的选择停止抓取";
                    ctx.stop = true;
                    ctx.cv.notify_all();
                    return;
                }
                if (ctx.abandoned[site])
                {
                    // 本站点已放弃：首选本通道的任务全部改派给另一个站点
                    for (size_t idx : ctx.queue)
                        if (ctx.tasks[idx].preferred == site)
                            ctx.tasks[idx].preferred = 1 - site;
                    ctx.cv.notify_all();
                    return;
                }
                const long long wait_ms =
                    crawler::gate_channel_block_remaining_ms(ch);
                if (wait_ms > 0)
                {
                    // 本通道处于限流等待：期间任务由另一个站点接管，
                    // 等自己的等待到期后再继续
                    ctx.cv.wait_for(lock, std::chrono::milliseconds(
                                              std::min<long long>(wait_ms, 200)));
                    continue;
                }
                if (ctx.queue.empty())
                {
                    if (ctx.in_flight == 0)
                        return; // 全部完成
                    ctx.cv.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
                const size_t picked = auto_pick_task(ctx, site);
                if (picked == static_cast<size_t>(-1))
                {
                    ctx.cv.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
                slot = ctx.queue[picked];
                ctx.queue.erase(ctx.queue.begin() +
                                static_cast<std::ptrdiff_t>(picked));
                ++ctx.in_flight;
                break;
            }
        }

        const AutoTask task = ctx.tasks[slot];
        FetchOutcome outcome =
            fetch_one_article(task.pid, task.summary, solution::site_of(ch),
                              *ctx.opt);

        std::string warning;
        {
            std::lock_guard<std::mutex> lock(ctx.mutex);
            --ctx.in_flight;
            if (outcome.kind == FetchOutcome::Kind::Stopped)
            {
                ctx.stopped_by_user = true;
                ctx.stop_reason = outcome.error;
                ctx.stop = true;
            }
            else if (outcome.kind == FetchOutcome::Kind::NeedLogin &&
                     site == 0)
            {
                // 原站需要登录态：这一条不算限流，直接作为致命错误上报
                ctx.stop = true;
                ctx.stop_reason = outcome.error;
            }
            else if ((outcome.kind == FetchOutcome::Kind::NotFound ||
                      outcome.kind == FetchOutcome::Kind::Failed) &&
                     ctx.tasks[slot].retries == 0 &&
                     auto_site_available(ctx, 1 - site))
            {
                // 换另一个站点再试一次：保存站是第三方镜像，可能没有收录某篇
                // 题解；原站的文章也可能已被删除而镜像仍有副本。
                // 只改派一次，避免两个站点之间来回弹跳。
                ++ctx.tasks[slot].retries;
                ctx.tasks[slot].preferred = 1 - site;
                ctx.queue.push_back(slot);
                warning = std::string("    题解 ") + task.summary.lid + " 在" +
                          (site == 0 ? "洛谷原站" : "保存站") +
                          "抓取失败，改用" + (site == 0 ? "保存站" : "洛谷原站") +
                          "重试";
            }
            else if (outcome.kind == FetchOutcome::Kind::RateLimited)
            {
                const crawler::ChannelLimitInfo info =
                    crawler::gate_note_rate_limit(ch, outcome.error);
                if (info.abandoned)
                {
                    // 连续被限流 3 次：放弃该站点，全部任务转给另一个站点
                    ctx.abandoned[site] = true;
                    print_warning(std::string("洛谷") +
                                  (site == 0 ? "原站" : "保存站") +
                                  " 连续被限流 3 次，已放弃该站点，"
                                  "剩余任务全部转给另一个站点");
                    ctx.tasks[slot].preferred = 1 - site;
                }
                else
                {
                    // 暂时重新分配到另一个站点；若另一个站点也不可用，
                    // 就留在自己这里等限流延时到期后重试
                    ctx.tasks[slot].preferred =
                        auto_site_available(ctx, 1 - site) ? (1 - site) : site;
                }
                ctx.queue.push_back(slot);
                if (ctx.abandoned[0] && ctx.abandoned[1])
                {
                    ctx.both_abandoned = true;
                    ctx.stop = true;
                    ctx.stop_reason = "两个站点都连续被限流，已终止下载";
                }
            }
            else
            {
                crawler::gate_note_success(ch);
                auto_apply_locked(ctx, slot, site, outcome);
                ++ctx.fetched;

                // 跳过与失败的提示与顺序模式保持一致（不静默），
                // 文案先攒好，出了锁再打印，避免持锁做 I/O
                switch (outcome.kind)
                {
                case FetchOutcome::Kind::NotFound:
                    warning = "    题解 " + task.summary.lid +
                              " 不可访问（已删除或无权限），已跳过";
                    break;
                case FetchOutcome::Kind::SkippedIncomplete:
                    warning = "    题解 " + task.summary.lid +
                              " 正文不完整，已跳过（可用 --allow-partial 导出）";
                    break;
                case FetchOutcome::Kind::Failed:
                    warning = "    题解 " + task.summary.lid + " 抓取失败：" +
                              outcome.error;
                    if (site == 1)
                        warning += "；可用 --solution-source official 改用原站";
                    break;
                default:
                    break;
                }
            }
            ctx.cv.notify_all();
        }
        if (!warning.empty())
            print_warning(warning);
    }
}
} // namespace

bool solution::crawl(const Plan &plan, const TaskOptions &opt,
                     luogu::SolutionBundle &bundle, CrawlStats &stats,
                     std::string &error)
{
    error.clear();
    bundle.items.clear();
    stats = CrawlStats();

    // 启动时清理超过 1 小时的 .tmp.* 残留（进程被杀时可能留下）
    solcache::cleanup_stale_temp_files();

    const bool auto_mode = (opt.source == Source::Auto);
    const int total = static_cast<int>(plan.items.size());

    // 每道题一个导出桶：题解按题目顺序、列表顺序排列
    std::vector<std::vector<luogu::SolutionView>> buckets(plan.items.size());
    std::vector<std::vector<char>> bucket_state(plan.items.size());
    std::vector<bool> bucket_used(plan.items.size(), false);

    AutoContext auto_ctx;
    auto_ctx.opt = &opt;

    // 顺序模式下的待抓任务
    std::vector<std::pair<size_t, size_t>> seq_tasks; // (题目下标, 篇目下标)

    for (size_t pi = 0; pi < plan.items.size(); ++pi)
    {
        const ProblemPlan &item = plan.items[pi];
        ++stats.problems_handled;
        std::printf("\r正在抓取题解：[%zu/%d] %s %s        \n", pi + 1, total,
                    item.pid.c_str(), item.name.c_str());
        std::fflush(stdout);

        // ---- 1. 题解列表：计划阶段已就绪（缓存可用或已抓取）----
        if (item.list_from_network)
        {
            // 计划阶段抓到的列表现在才落盘（确认之前不改动缓存）；
            // 304 时沿用缓存里的条目，只刷新时间戳
            solcache::ListEntry entry;
            entry.pid = item.pid;
            entry.fetched_at = now_seconds();
            entry.etag = item.list_etag;
            entry.total_available = item.total_available;
            entry.per_page = 10;
            entry.no_solution = item.list_items.empty();
            entry.items = item.list_items;
            if (item.list_not_modified)
            {
                solcache::ListEntry old;
                if (solcache::load_list(item.pid, old))
                {
                    old.fetched_at = entry.fetched_at;
                    entry = old;
                }
            }
            std::string store_error;
            if (!solcache::store_list(entry, store_error))
                print_warning("写入题解列表缓存失败：" + store_error);
            else
                ++stats.fetched_list;
        }

        if (item.list_items.empty())
        {
            ++stats.problems_no_solution;
            std::printf("    %s 暂无题解\n", item.pid.c_str());
            continue;
        }

        // ---- 2. 逐篇：缓存命中直接用，其余进入待抓队列 ----
        bucket_used[pi] = true;
        buckets[pi].resize(item.articles.size());
        bucket_state[pi].assign(item.articles.size(), 0);

        for (size_t ai = 0; ai < item.articles.size(); ++ai)
        {
            const ArticlePlan &article = item.articles[ai];
            if (!article.cached)
            {
                if (auto_mode)
                {
                    AutoTask task;
                    task.bucket = pi;
                    task.pid = item.pid;
                    task.summary = article.summary;
                    task.preferred = article.site;
                    auto_ctx.queue.push_back(auto_ctx.tasks.size());
                    auto_ctx.tasks.push_back(std::move(task));
                    auto_ctx.slots.emplace_back();
                    auto_ctx.slot_state.push_back(0);
                }
                else
                {
                    seq_tasks.emplace_back(pi, ai);
                }
                continue;
            }

            solcache::DocEntry doc;
            if (!solcache::load_doc(item.pid, article.summary.lid,
                                    article.cached_source, doc))
            {
                // 理论上不会发生（计划阶段刚判定命中）：退化为重抓
                if (auto_mode)
                {
                    AutoTask task;
                    task.bucket = pi;
                    task.pid = item.pid;
                    task.summary = article.summary;
                    task.preferred = article.site;
                    auto_ctx.queue.push_back(auto_ctx.tasks.size());
                    auto_ctx.tasks.push_back(std::move(task));
                    auto_ctx.slots.emplace_back();
                    auto_ctx.slot_state.push_back(0);
                }
                else
                {
                    seq_tasks.emplace_back(pi, ai);
                }
                continue;
            }
            buckets[pi][ai] =
                make_view(article.summary, doc, article.cached_source);
            bucket_state[pi][ai] = 1;
            ++stats.cached;
        }
    }

    // ---- 3. 抓取正文 ----
    if (auto_mode)
    {
        std::printf("题解正文来源：原站与保存站轮流分配、并行抓取"
                    "（缓存命中的篇目直接使用缓存）\n");
        std::fflush(stdout);

        std::thread official_thread(auto_worker, std::ref(auto_ctx), 0);
        std::thread save_thread(auto_worker, std::ref(auto_ctx), 1);
        official_thread.join();
        save_thread.join();

        // 汇总统计
        stats.fetched = auto_ctx.fetched;
        stats.fetched_official = auto_ctx.fetched_official;
        stats.fetched_save = auto_ctx.fetched_save;
        stats.not_modified = auto_ctx.not_modified;
        stats.failed = auto_ctx.failed;
        stats.skipped_inaccessible = auto_ctx.skipped_inaccessible;
        stats.skipped_incomplete = auto_ctx.skipped_incomplete;
        stats.incomplete_exported = auto_ctx.incomplete_exported;

        // 把 slot 填回各题的桶（保持题目顺序与列表顺序）
        size_t slot = 0;
        for (size_t pi = 0; pi < plan.items.size(); ++pi)
        {
            for (size_t ai = 0; ai < plan.items[pi].articles.size(); ++ai)
            {
                if (plan.items[pi].articles[ai].cached)
                    continue;
                if (auto_ctx.slot_state[slot] == 1)
                {
                    buckets[pi][ai] = auto_ctx.slots[slot];
                    bucket_state[pi][ai] = 1;
                }
                else
                {
                    bucket_state[pi][ai] = 2;
                }
                ++slot;
            }
        }

        if (auto_ctx.both_abandoned)
        {
            stats.stopped_by_rate_limit = true;
            stats.stop_reason = auto_ctx.stop_reason;
        }
        else if (auto_ctx.stopped_by_user)
        {
            stats.stopped_by_user = true;
            stats.stop_reason = auto_ctx.stop_reason;
        }
        else if (!auto_ctx.stop_reason.empty())
        {
            // 原站需要登录态等致命错误
            error = auto_ctx.stop_reason;
            return false;
        }
    }
    else
    {
        for (const auto &task : seq_tasks)
        {
            const size_t pi = task.first;
            const size_t ai = task.second;
            const ProblemPlan &item = plan.items[pi];
            const Summary &summary = item.articles[ai].summary;
            const Source site = (item.articles[ai].site == 1) ? Source::Save
                                                              : Source::Official;

            const FetchOutcome outcome =
                fetch_one_article(item.pid, summary, site, opt);

            if (outcome.kind == FetchOutcome::Kind::Stopped)
            {
                stats.stopped_by_user = true;
                stats.stop_reason = outcome.error;
                break;
            }
            if (outcome.kind == FetchOutcome::Kind::RateLimited)
            {
                stats.stopped_by_rate_limit = true;
                stats.stop_reason = outcome.error;
                break;
            }
            if (outcome.kind == FetchOutcome::Kind::NeedLogin)
            {
                error = outcome.error;
                return false;
            }
            apply_outcome(outcome, stats);
            if (outcome.kind == FetchOutcome::Kind::NotFound)
            {
                print_warning("    题解 " + summary.lid +
                              " 不可访问（已删除或无权限），已跳过");
                bucket_state[pi][ai] = 2;
                continue;
            }
            if (outcome.kind == FetchOutcome::Kind::SkippedIncomplete)
            {
                print_warning("    题解 " + summary.lid + " 正文不完整，已跳过"
                              "（可用 --allow-partial 导出）");
                bucket_state[pi][ai] = 2;
                continue;
            }
            if (outcome.kind == FetchOutcome::Kind::Failed)
            {
                std::string hint = "    题解 " + summary.lid + " 抓取失败：" +
                                   outcome.error;
                if (site == Source::Save)
                    hint += "；可用 --solution-source official 改用原站";
                print_warning(hint);
                bucket_state[pi][ai] = 2;
                continue;
            }
            buckets[pi][ai] = make_view(summary, outcome.doc, site);
            bucket_state[pi][ai] = 1;
        }
    }

    // ---- 4. 组装导出用的题解包（跳过未成功的篇目）----
    for (size_t pi = 0; pi < plan.items.size(); ++pi)
    {
        if (!bucket_used[pi])
            continue;
        luogu::ProblemSolutionSet set;
        set.pid = plan.items[pi].pid;
        set.problem_title = plan.items[pi].name;
        for (size_t ai = 0; ai < buckets[pi].size(); ++ai)
            if (bucket_state[pi][ai] == 1)
                set.solutions.push_back(std::move(buckets[pi][ai]));
        if (!set.solutions.empty())
            bundle.items.push_back(std::move(set));
    }

    // 一篇都没拿到（且没有缓存可用）时视为失败：
    // 避免「全部失败却报告成功」（例如保存站不可用）
    if (stats.fetched == 0 && stats.cached == 0 && stats.not_modified == 0 &&
        stats.failed > 0)
    {
        error = "题解正文全部抓取失败（" + std::to_string(stats.failed) + " 篇）";
        if (opt.source == Source::Save)
            error += "；保存站请求失败，可用 --solution-source official 改用原站";
        return false;
    }

    return true;
}

std::string solution::describe_crawl_stats(const CrawlStats &stats)
{
    std::string out = "成功 " + std::to_string(stats.fetched) + " 篇";
    if (stats.fetched_list > 0)
        out += "，新抓题解列表 " + std::to_string(stats.fetched_list) + " 个";
    if (stats.cached > 0)
        out += "，命中缓存 " + std::to_string(stats.cached) + " 篇";
    if (stats.not_modified > 0)
        out += "，304 未修改 " + std::to_string(stats.not_modified) + " 篇";
    if (stats.problems_no_solution > 0)
        out += "，" + std::to_string(stats.problems_no_solution) + " 道题暂无题解";
    if (stats.skipped_inaccessible > 0)
        out += "，" + std::to_string(stats.skipped_inaccessible) +
               " 篇不可访问（已删除或无权限）";
    if (stats.skipped_incomplete > 0)
        out += "，" + std::to_string(stats.skipped_incomplete) +
               " 篇正文不完整（可用 --allow-partial 导出）";
    if (stats.incomplete_exported > 0)
        out += "，" + std::to_string(stats.incomplete_exported) +
               " 篇正文不完整（已按 --allow-partial 导出，正文可能有缺失）";
    if (stats.failed > 0)
        out += "，" + std::to_string(stats.failed) + " 篇抓取失败";
    if (stats.stopped_by_user)
        out += "，已按你的选择取消";
    if (stats.stopped_by_rate_limit)
        out += "，因限流中止";
    return out;
}
