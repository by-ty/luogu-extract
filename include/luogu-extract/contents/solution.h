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

// include/luogu-extract/contents/solution.h
// 题解抓取：题解列表（JSON 接口 + 分页）、正文获取（来源适配）。
//
// 三条硬约束（设计 §三）：
// 1. 题解列表接口需要登录态，--cookie 是启用题解功能的前提；
// 2. 题解列表恒取洛谷原站（保存站没有等价的「题目 → 题解列表」入口），
//    --solution-source 只决定正文来源；
// 3. 列表一律走 JSON 接口，程序不解析任何题解列表页面的 HTML DOM。
#ifndef LUOGU_EXTRACT_CONTENTS_SOLUTION_H
#define LUOGU_EXTRACT_CONTENTS_SOLUTION_H

#include <string>
#include <vector>
#include "luogu-extract/contents/article.h"
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/crawler/request_gate.h"

namespace solution
{
    /// 题解列表中的一条摘要（字段容错：未知/缺失一律取默认值，不抛异常）
    struct Summary
    {
        std::string lid, title;
        int author_uid = 0;
        std::string author_name;
        long long time = 0;
        int upvote = 0, reply_count = 0, favor_count = 0;
        int category = 0, solution_type = 0, promote_status = 0;
        int difficulty = 0;   // 所属题目的难度（接口 solutionFor.difficulty）
        int page = 1;   // 来自第几页，便于调试与续抓
    };

    /// 正文来源：官方原站 / 保存站（第三方镜像）/ 自动（两者并用）
    ///
    /// Auto 只影响「这次去哪里抓」：已经在缓存里的题解一律直接用
    /// （两个来源都有时优先原站），未命中的按原站、保存站轮流分配，
    /// 两条通道并行抓取，各自计算延时与限流等待。缓存仍按实际来源入键，
    /// 因此 Auto 不是缓存键。
    enum class Source
    {
        Official,
        Save,
        Auto,
    };

    const char *source_display_name(Source src); // 洛谷原站 / 洛谷保存站 / 自动
    const char *source_key(Source src);          // official / save（用于缓存文件名）
    bool source_from_key(const std::string &key, Source &out);

    /// Auto 模式下每条抓取通道对应的具体来源与通道号
    crawler::Channel channel_of(Source src);
    Source site_of(crawler::Channel ch);

    /// lid 白名单校验：仅接受 [a-z0-9]{6,32}。
    /// lid 来自网络数据，可能被构造成 "../../x"，必须在使用前校验；
    /// 不匹配的条目一律丢弃并告警（防路径穿越）。
    bool valid_lid(const std::string &lid);

    /// 列表接口的最大页数保护（避免异常响应导致无限翻页）
    const int kMaxListPages = 50;

    /// 单页解析结果
    struct ListPage
    {
        std::vector<Summary> items;
        int per_page = 0;          // perPage / pageSize
        int total = 0;             // count / totalCount（缺失时为 0）
        bool structure_ok = false; // 是否找到了承载题解数组的字段
        std::string error;
    };

    /// 解析题解列表接口的响应体（顶层结构容错：
    /// {"currentData":{…}} 与 {"status":200,"data":{…}} 两种形态都尝试，
    /// 数组字段优先 solutions，其次 result）。
    ListPage parse_list_page(const std::string &body, int page);

    /// 列表抓取结果
    struct ListFetch
    {
        bool ok = false;
        bool not_modified = false;   // ETag 条件请求命中 304
        bool no_solution = false;    // 结构可解析但该题确实没有题解
        bool rate_limited = false;   // 因限流中止
        bool stopped = false;        // 用户主动停止
        bool need_login = false;     // 需要登录态
        std::vector<Summary> items;  // 保持接口返回顺序，按 lid 去重
        int pages_fetched = 0;
        int total_available = 0;
        std::string etag;
        std::string error;
    };

    /// 抓取题解列表。
    /// @param pid  题目编号
    /// @param need 需要的篇数；< 0 表示全部（受 kMaxListPages 与每题上限保护）
    /// @param etag 上次的 ETag（非空时发条件请求，命中 304 时 not_modified 为 true）
    ListFetch fetch_list(const std::string &pid, int need, const std::string &etag);

    /// 正文抓取结果
    struct ArticleFetch
    {
        bool ok = false;
        bool not_modified = false;
        bool not_found = false;     // 404 / 已删除 / 无权限
        bool need_login = false;
        bool rate_limited = false;
        bool stopped = false;
        bool incomplete = false;    // contentFull=false 或正文为空
        article::Article article;
        std::string etag;
        std::string error;
        long http_status = 0;
    };

    /// 按来源抓取一篇题解正文（src 必须是具体来源 Official / Save）。
    /// - official：洛谷原站 /article/<lid>（带 Cookie）；
    /// - save：保存站 markdown 接口（第三方镜像，不发送任何 Cookie）。
    /// ch 指定请求闸门通道：两条通道各自计算延时、限流等待与放弃状态。
    ArticleFetch fetch_article(Source src, const std::string &lid,
                               const std::string &etag,
                               crawler::Channel ch = crawler::Channel::Official);

    /// 单篇正文上限（2 MB）：超限截断并标记 content_full=false
    const size_t kMaxArticleBytes = 2 * 1024 * 1024;

    /// 解析洛谷原站的正文 JSON（data.article）
    bool parse_official_article(const std::string &body, article::Article &out,
                                std::string &error);
    /// 解析保存站的正文 JSON（data.content 为 markdown 原文）
    bool parse_save_article(const std::string &body, article::Article &out,
                            std::string &error);
} // namespace solution

#endif // LUOGU_EXTRACT_CONTENTS_SOLUTION_H
