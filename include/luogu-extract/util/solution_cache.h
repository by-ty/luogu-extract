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

// include/luogu-extract/util/solution_cache.h
// 文章缓存（题解是与题目绑定的特殊文章）：<cache_dir>/solutions.ndjson 为题解列表（一行一题），
// <cache_dir>/articles/<lid>.<source>.json 为单篇正文（题解正文同理）。
// 不变量：列表按 pid 替换整行后整体原子替换；正文按全局唯一的文章编号 + 来源（official / save）入键，
// 切来源视为未命中，--article 与题解共用同一份缓存；读取校验 JSON 结构与字段类型，非法即未命中重抓；
// 写入一律「同目录临时文件 + fsync + 原子替换」，失败或被杀最多残留 .tmp.*，不会留下半截 JSON。
#ifndef LUOGU_EXTRACT_UTIL_SOLUTION_CACHE_H
#define LUOGU_EXTRACT_UTIL_SOLUTION_CACHE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "luogu-extract/contents/solution.h"
#include "luogu-extract/crawler/crawler.h"

namespace solcache
{
    /// 题解列表缓存（solutions.ndjson 中的一行）
    struct ListEntry
    {
        int version = 1;
        std::string pid;
        long long fetched_at = 0;   // Unix 时间戳
        std::string etag;
        int total_available = 0;
        int per_page = 10;
        bool no_solution = false;
        std::vector<solution::Summary> items;
    };

    /// 单篇正文缓存（articles/<lid>.<source>.json）；题解正文与 --article 文章共用此结构，
    /// pid 为空表示「按文章下载」（不与题目绑定）
    struct DocEntry
    {
        int version = 1;
        std::string lid, pid, source;
        long long fetched_at = 0;
        std::string etag;
        std::string title, author_name;
        int author_uid = 0, upvote = 0, reply_count = 0, favor_count = 0;
        int category = 0, promote_status = 0, difficulty = 0;
        long long time = 0;
        std::string solution_pid, solution_type, solution_name;
        bool content_full = false;
        std::string content;
        std::string url;   // 原文地址（来源为保存站时指向保存站）
    };

    /// 正文缓存目录 <cache_dir>/articles/（存 <lid>.<source>.json）
    std::filesystem::path articles_dir();

    /// <cache_dir>/solutions.ndjson
    std::filesystem::path solutions_index_path();

    /// <cache_dir>/articles/<lid>.<source>.json
    std::filesystem::path article_path(const std::string &lid, solution::Source src);

    /// 读某题的题解列表缓存；无该题、结构非法或字段类型不符时返回 false
    bool load_list(const std::string &pid, ListEntry &out);

    /// 写某题的题解列表缓存（替换该题那一行、其余保留、整体原子替换）；失败时原缓存不变
    bool store_list(const ListEntry &entry, std::string &error);

    /// 读正文缓存；pid 仅用于校验缓存里的题目编号一致（为空表示按文章下载，不校验）
    bool load_doc(const std::string &pid, const std::string &lid,
                  solution::Source src, DocEntry &out);

    /// 写正文缓存（原子替换；entry.pid 可为空，表示按文章下载）
    bool store_doc(const DocEntry &entry, std::string &error);

    /// TTL 判定：ttl_days < 0 永久有效（默认）、== 0 每次发 ETag 条件请求、> 0 超过该天数即过期
    bool is_fresh(long long fetched_at, int ttl_days);

    /// 清理超过 1 小时的 .tmp.* 残留（进程被杀时可能留下）
    void cleanup_stale_temp_files();

    /// articles/ 与 solutions.ndjson 占用的字节数
    std::uintmax_t cache_size();

    /// -CS, --clean-solutions：删除 solutions.ndjson；文件不存在视为已清空
    crawler::derror clean_solutions();

    /// -CA, --clean-articles：删除 articles/（题解正文与 --article 文章都在其中）；目录不存在视为已清空
    crawler::derror clean_articles();
} // namespace solcache

#endif // LUOGU_EXTRACT_UTIL_SOLUTION_CACHE_H
