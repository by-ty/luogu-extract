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
// 文章（题解属于与题目一一绑定的特殊文章）缓存：
//   <cache_dir>/solutions.ndjson                  所有已获取到的题解列表
//                                                 （一行一个题目）
//   <cache_dir>/articles/<lid>.<source>.json      单篇文章正文（题解正文同理）
//
// - 题解列表集中在 solutions.ndjson 里：读取时按 pid 找对应行，写入时替换
//   该行后整体原子替换，一个题目一行、不会重复；
// - 正文直接按文章编号入键（不再按题目分目录；文章编号全局唯一），
//   并按来源分别入键（official / save）：两个来源的正文可能不一致，
//   切换来源时另一来源视为未命中；按文章下载（--article）与按题解下载
//   共用同一份缓存，同一篇文章不会重复保存；
// - 一律「临时文件（同目录）+ fsync + 原子替换」，写入失败或中断时
//   原缓存保持不变，进程被杀最多残留 .tmp.*，不会产生半截 JSON；
// - 读取时校验 JSON 结构与字段类型，非法即视为未命中并重抓。
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
    /// 题解列表缓存（<cache_dir>/solutions.ndjson 中的一行）
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

    /// 单篇文章正文缓存（<cache_dir>/articles/<lid>.<source>.json）。
    /// 题解正文与按 --article 下载的文章共用同一结构：pid 为空表示
    /// 「按文章下载」（该文章不与题目绑定）
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

    /// 文章正文缓存目录 <cache_dir>/articles/（存放 <lid>.<source>.json）
    std::filesystem::path articles_dir();

    /// <cache_dir>/solutions.ndjson
    std::filesystem::path solutions_index_path();

    /// <cache_dir>/articles/<lid>.<source>.json
    std::filesystem::path article_path(const std::string &lid, solution::Source src);

    /// 读取某个题目的题解列表缓存；文件中没有该题目、结构非法或字段类型
    /// 不符时返回 false
    bool load_list(const std::string &pid, ListEntry &out);

    /// 写入某个题目的题解列表缓存（替换 solutions.ndjson 中该题目那一行，
    /// 其余行原样保留，整体原子替换）；失败时原缓存保持不变
    bool store_list(const ListEntry &entry, std::string &error);

    /// 读取正文缓存（pid 只用于校验缓存里的题目编号是否一致；
    /// pid 为空表示按文章下载，不校验所属题目）
    bool load_doc(const std::string &pid, const std::string &lid,
                  solution::Source src, DocEntry &out);

    /// 写入正文缓存（原子替换；entry.pid 可为空，表示按文章下载）
    bool store_doc(const DocEntry &entry, std::string &error);

    /// 时间戳是否在 TTL 内。
    /// ttl_days < 0：有效期无限（默认），缓存存在即视为新鲜；
    /// ttl_days == 0：不按时间判定，改为每次发 ETag 条件请求；
    /// ttl_days > 0：超过该天数即视为过期。
    bool is_fresh(long long fetched_at, int ttl_days);

    /// 清理超过 1 小时的 .tmp.* 残留（进程被杀时可能留下）
    void cleanup_stale_temp_files();

    /// 题解缓存（articles/ 与 solutions.ndjson）占用的字节数
    std::uintmax_t cache_size();

    /// -CS, --clean-solutions：删除题解列表缓存 <cache_dir>/solutions.ndjson；
    /// 文件不存在时视为已清空
    crawler::derror clean_solutions();

    /// -CA, --clean-articles：删除文章正文缓存目录 <cache_dir>/articles/
    /// （题解正文与按 --article 下载的文章都在其中）；目录不存在时视为已清空
    crawler::derror clean_articles();
} // namespace solcache

#endif // LUOGU_EXTRACT_UTIL_SOLUTION_CACHE_H
