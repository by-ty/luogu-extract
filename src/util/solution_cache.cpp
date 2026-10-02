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

// src/util/solution_cache.cpp
#include "luogu-extract/util/solution_cache.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "luogu-extract/util/compat.h"

using nlohmann::json;

namespace
{
// 重定向到文件/管道时不写 ANSI 转义序列
const char *kColorRed = luogu::compat::stderr_is_tty() ? "\033[1;31m" : "";
const char *kColorYellow = luogu::compat::stderr_is_tty() ? "\033[1;33m" : "";
const char *kColorReset = luogu::compat::stderr_is_tty() ? "\033[0m" : "";

void print_error(const std::string &message)
{
    std::fflush(stdout);
    std::fprintf(stderr, "%s错误：%s %s\n", kColorRed, kColorReset, message.c_str());
}

void print_warning(const std::string &message)
{
    std::fflush(stdout);
    std::fprintf(stderr, "%s警告：%s %s\n", kColorYellow, kColorReset, message.c_str());
}

// PID 二次校验：只允许字母、数字与下划线（题目编号形状），
// 拼接文件名前再确认一次，杜绝路径穿越
bool safe_pid(const std::string &pid)
{
    if (pid.empty() || pid.size() > 24)
        return false;
    for (char c : pid)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return true;
}

// 缓存里的题目编号白名单校验：缓存文件本地可写，不合规的编号按「缺字段」
// 处理（置空）并告警，不让被篡改的编号进入后续流程
void drop_bad_cached_pid(std::string &pid, const std::filesystem::path &path)
{
    if (pid.empty() || safe_pid(pid))
        return;
    print_warning("文章缓存 '" + luogu::compat::path_to_utf8(path) +
                  "' 中的题目编号不合法，已忽略");
    pid.clear();
}

// 读文件全部内容（二进制）；失败返回 false
bool read_file(const std::filesystem::path &path, std::string &out)
{
    FILE *file = luogu::compat::fopen(path, "rb");
    if (!file)
        return false;
    out.clear();
    char buffer[65536];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        out.append(buffer, n);
    const bool ok = !std::ferror(file);
    std::fclose(file);
    return ok;
}

// 原子写：同目录临时文件 + fsync + 原子替换。
// 任一步失败只删除临时文件，原缓存保持不变。
bool write_file_atomic(const std::filesystem::path &target, const std::string &data,
                       std::string &error)
{
    std::error_code ec;
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            error = "无法创建目录 '" + luogu::compat::path_to_utf8(parent) +
                    "'：" + ec.message();
            return false;
        }
    }

    const std::filesystem::path tmp = luogu::compat::temp_sibling_path(target);
    FILE *out = luogu::compat::fopen(tmp, "wb");
    if (!out)
    {
        error = "无法打开文件 '" + luogu::compat::path_to_utf8(tmp) + "'（写入）";
        return false;
    }
    const bool write_ok = std::fwrite(data.data(), 1, data.size(), out) == data.size();
    const bool flushed = luogu::compat::flush_and_sync(out);
    const bool closed = std::fclose(out) == 0;
    if (!write_ok || !flushed || !closed)
    {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        error = "写入文件 '" + luogu::compat::path_to_utf8(target) + "' 失败";
        return false;
    }

    std::string replace_error;
    if (!luogu::compat::atomic_replace(tmp, target, replace_error))
    {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        error = "无法把缓存写入 '" + luogu::compat::path_to_utf8(target) +
                "'：" + replace_error;
        return false;
    }
    return true;
}

std::string str_value(const json &obj, const char *key)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_string())
        return "";
    return obj[key].get<std::string>();
}

int int_value(const json &obj, const char *key)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number_integer())
        return 0;
    return obj[key].get<int>();
}

long long ll_value(const json &obj, const char *key)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number_integer())
        return 0;
    return obj[key].get<long long>();
}

bool bool_value(const json &obj, const char *key)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_boolean())
        return false;
    return obj[key].get<bool>();
}

// 判断是否为残留的临时文件（*.tmp.<时间戳>.<计数>）
bool is_temp_name(const std::string &name)
{
    return name.find(".tmp.") != std::string::npos;
}
} // namespace

std::filesystem::path solcache::articles_dir()
{
    return crawler::get_cache_dir() / "articles";
}

std::filesystem::path solcache::solutions_index_path()
{
    return crawler::get_cache_dir() / "solutions.ndjson";
}

std::filesystem::path solcache::article_path(const std::string &lid,
                                             solution::Source src)
{
    return articles_dir() / (lid + "." + solution::source_key(src) + ".json");
}

// 把一行 JSON 解析成列表缓存条目；结构非法时置 bad = true（调用方据此按
// 「这一行不可用」处理），pid 不是合法题号时返回 false
bool parse_list_entry(const std::string &line, solcache::ListEntry &out, bool &bad)
{
    bad = false;
    json root;
    try
    {
        root = json::parse(line);
    }
    catch (const std::exception &)
    {
        bad = true;
        return false;
    }
    if (!root.is_object() || int_value(root, "version") != 1 ||
        !root.contains("items") || !root["items"].is_array())
    {
        bad = true;
        return false;
    }
    const std::string line_pid = str_value(root, "pid");
    if (!safe_pid(line_pid))
        return false;

    out.version = 1;
    out.pid = line_pid;
    out.fetched_at = ll_value(root, "fetched_at");
    out.etag = str_value(root, "etag");
    out.total_available = int_value(root, "total_available");
    out.per_page = int_value(root, "per_page");
    out.no_solution = bool_value(root, "no_solution");

    for (const json &item : root["items"])
    {
        if (!item.is_object())
            continue;
        solution::Summary sm;
        sm.lid = str_value(item, "lid");
        if (!solution::valid_lid(sm.lid))
            continue; // 缓存被篡改时同样按白名单过滤
        sm.title = str_value(item, "title");
        sm.author_uid = int_value(item, "author_uid");
        sm.author_name = str_value(item, "author_name");
        sm.time = ll_value(item, "time");
        sm.upvote = int_value(item, "upvote");
        sm.reply_count = int_value(item, "reply_count");
        sm.favor_count = int_value(item, "favor_count");
        sm.category = int_value(item, "category");
        sm.solution_type = int_value(item, "solution_type");
        sm.promote_status = int_value(item, "promote_status");
        sm.difficulty = int_value(item, "difficulty");
        sm.page = int_value(item, "page");
        if (sm.page <= 0)
            sm.page = 1;
        out.items.push_back(std::move(sm));
    }
    if (out.per_page <= 0)
        out.per_page = 10;
    if (out.total_available <= 0)
        out.total_available = static_cast<int>(out.items.size());
    return true;
}

// 该行是否为指定题目的记录；该行不是本题目的记录时返回 false，
// 结构非法时置 bad = true（调用方据此跳过该行）
bool parse_list_line(const std::string &line, const std::string &pid,
                     solcache::ListEntry &out, bool &bad)
{
    if (!parse_list_entry(line, out, bad))
        return false;
    return out.pid == pid;
}

// 把列表缓存条目序列化成 solutions.ndjson 的一行
json list_entry_json(const solcache::ListEntry &entry)
{
    json root = json::object();
    root["version"] = 1;
    root["pid"] = entry.pid;
    root["fetched_at"] = entry.fetched_at;
    root["etag"] = entry.etag;
    root["total_available"] = entry.total_available;
    root["per_page"] = entry.per_page;
    root["no_solution"] = entry.no_solution;
    json items = json::array();
    for (const auto &s : entry.items)
    {
        if (!solution::valid_lid(s.lid))
            continue;
        json item = json::object();
        item["lid"] = s.lid;
        item["title"] = s.title;
        item["author_uid"] = s.author_uid;
        item["author_name"] = s.author_name;
        item["time"] = s.time;
        item["upvote"] = s.upvote;
        item["reply_count"] = s.reply_count;
        item["favor_count"] = s.favor_count;
        item["category"] = s.category;
        item["solution_type"] = s.solution_type;
        item["promote_status"] = s.promote_status;
        item["difficulty"] = s.difficulty;
        item["page"] = s.page;
        items.push_back(std::move(item));
    }
    root["items"] = std::move(items);
    return root;
}

// 按行拆分 NDJSON（忽略空行；不保留行尾换行）
std::vector<std::string> split_index_lines(const std::string &text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t nl = text.find('\n', start);
        std::string line = (nl == std::string::npos) ? text.substr(start)
                                                     : text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line.find_first_not_of(" \t") != std::string::npos)
            lines.push_back(line);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return lines;
}

namespace
{
// ---- 题解列表索引的内存缓存 ----
// solutions.ndjson 一行一个题目。make_plan 会对每道题调用一次 load_list
// （每轮计划一次，一次运行至少两轮），若每次都整读整解析整个文件，P 道题
// 就是 O(P²) 的 I/O 与 JSON 解析；这里首次使用时整读一遍并按 pid 建表，
// 之后按 pid 直接查表。
// 表只由本文件改写：store_list 写盘成功后同步更新；同时记录文件的
// (存在性, mtime, size)，被外部删除或改写（-C 清空缓存、另一个进程写入、
// 手工编辑）时下次访问自动重新载入，进程内多次 app.run 也不会读到旧内容。
struct IndexStamp
{
    bool exists = false;
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
};

struct ListIndex
{
    bool loaded = false;
    IndexStamp stamp;
    std::vector<std::string> lines;                 // 有效行的原文（坏行不入表）
    std::unordered_map<std::string, size_t> by_pid; // pid → lines 下标
};

// 进程内单例。注意：当前只在 solution_task 的主线程（工作线程启动之前）
// 调用 load_list/store_list，因此这里不加锁；若将来从工作线程调用，
// 需要给内存表加互斥量。另外指纹只取 (存在性, mtime, size)：mtime 粒度粗
// 的文件系统（如 FAT 2 秒）上，外部进程在同一粒度内写入「同尺寸不同内容」
// 可能漏检一次（本进程自己的写入由 store_list 主动刷新指纹，不受影响）。
ListIndex &list_index()
{
    static ListIndex index;
    return index;
}

IndexStamp stamp_of(const std::filesystem::path &path)
{
    IndexStamp stamp;
    std::error_code ec;
    stamp.mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
        return stamp; // 不存在或读不到属性：按「文件不存在」处理
    stamp.exists = true;
    stamp.size = std::filesystem::file_size(path, ec);
    if (ec)
        stamp = IndexStamp();
    return stamp;
}

bool same_stamp(const IndexStamp &a, const IndexStamp &b)
{
    if (a.exists != b.exists)
        return false;
    return !a.exists || (a.mtime == b.mtime && a.size == b.size);
}

// 整读文件重建内存表：结构非法的行只丢弃这一行（原来是遇到第一行非法就
// 整体未命中，会让所有题目一起重抓），并统计告警
void load_list_index(const std::filesystem::path &path, const IndexStamp &stamp)
{
    ListIndex &index = list_index();
    index.loaded = true;
    index.stamp = stamp;
    index.lines.clear();
    index.by_pid.clear();

    std::string text;
    if (!stamp.exists)
        return; // 缓存文件不存在：按空表处理
    if (!read_file(path, text))
    {
        // 读取失败（如权限不足）：不缓存空表，下次访问重试
        index.loaded = false;
        return;
    }
    if (text.empty())
        return;

    int bad_lines = 0;
    for (const std::string &line : split_index_lines(text))
    {
        solcache::ListEntry entry;
        bool bad = false;
        if (!parse_list_entry(line, entry, bad))
        {
            if (bad)
                ++bad_lines;
            continue;
        }
        if (index.by_pid.count(entry.pid))
            continue; // 同一题目有多行时沿用「首次出现优先」
        index.by_pid.emplace(entry.pid, index.lines.size());
        index.lines.push_back(line);
    }
    if (bad_lines > 0)
        print_warning("题解列表缓存中有 " + std::to_string(bad_lines) +
                      " 行结构非法，已忽略（不影响其它题目的缓存）");
}

// 保证内存表与磁盘一致；文件未变时只多一次 stat
void ensure_list_index(const std::filesystem::path &path)
{
    const IndexStamp stamp = stamp_of(path);
    ListIndex &index = list_index();
    if (index.loaded && same_stamp(index.stamp, stamp))
        return;
    load_list_index(path, stamp);
}

// 缓存文件被清空/删除后调用：下次访问重新从磁盘载入
void invalidate_list_index()
{
    ListIndex &index = list_index();
    index.loaded = false;
    index.stamp = IndexStamp();
    index.lines.clear();
    index.by_pid.clear();
}
} // namespace

bool solcache::load_list(const std::string &pid, ListEntry &out)
{
    out = ListEntry();
    if (!safe_pid(pid))
        return false;

    ensure_list_index(solutions_index_path());
    const ListIndex &index = list_index();
    const auto it = index.by_pid.find(pid);
    if (it == index.by_pid.end())
        return false;

    ListEntry entry;
    bool bad = false;
    if (!parse_list_line(index.lines[it->second], pid, entry, bad))
        return false; // 载入时已过滤坏行，这里理论上不会发生
    out = std::move(entry);
    return true;
}

bool solcache::store_list(const ListEntry &entry, std::string &error)
{
    error.clear();
    if (!safe_pid(entry.pid))
    {
        error = "题目编号 '" + entry.pid + "' 不合法，拒绝写入题解缓存";
        return false;
    }

    // 一题一行：本题目已有记录时替换该行，否则追加到最后；其余行原样保留
    // （结构非法的行在载入内存表时就已丢弃，重写时一并自愈残留的半截记录），
    // 最后整体原子替换：一个题目一行，不会重复
    const std::filesystem::path path = solutions_index_path();
    ensure_list_index(path);
    ListIndex &index = list_index();

    const std::string new_line = list_entry_json(entry).dump();
    const auto it = index.by_pid.find(entry.pid);
    const bool exists = it != index.by_pid.end();
    const size_t pos = exists ? it->second : 0;

    std::string data;
    if (exists)
    {
        for (size_t i = 0; i < index.lines.size(); ++i)
            data += (i == pos ? new_line : index.lines[i]) + "\n";
    }
    else
    {
        for (const std::string &line : index.lines)
            data += line + "\n";
        data += new_line + "\n";
    }

    // 写盘失败时内存表保持原样，与磁盘上的缓存仍然一致
    if (!write_file_atomic(path, data, error))
        return false;

    if (exists)
        index.lines[pos] = new_line;
    else
    {
        index.by_pid.emplace(entry.pid, index.lines.size());
        index.lines.push_back(new_line);
    }
    index.stamp = stamp_of(path);
    index.loaded = true;
    return true;
}

bool solcache::load_doc(const std::string &pid, const std::string &lid,
                        solution::Source src, DocEntry &out)
{
    out = DocEntry();
    // pid 为空表示「按文章下载」（--article）：文章编号全局唯一，缓存文件只有
    // 一份，因此不校验所属题目，也能直接复用先前作为某题题解抓到的同一篇正文
    if ((!pid.empty() && !safe_pid(pid)) || !solution::valid_lid(lid))
        return false;

    const std::filesystem::path path = article_path(lid, src);
    std::string text;
    if (!read_file(path, text) || text.empty())
        return false;

    json root;
    try
    {
        root = json::parse(text);
    }
    catch (const std::exception &)
    {
        return false;
    }
    if (!root.is_object() || int_value(root, "version") != 1)
        return false;
    if (!root.contains("content") || !root["content"].is_string())
        return false;

    // 来源必须与请求一致，否则视为未命中
    const std::string source_key = str_value(root, "source");
    if (!source_key.empty() && source_key != solution::source_key(src))
        return false;
    const std::string stored_lid = str_value(root, "lid");
    if (!stored_lid.empty() && stored_lid != lid)
        return false;
    // 正文缓存按文章编号存放（不再按题目分目录），题目编号必须与请求一致；
    // 请求方不关心所属题目时（--article 按文章下载，pid 为空）跳过该校验
    std::string stored_pid = str_value(root, "pid");
    drop_bad_cached_pid(stored_pid, path);
    if (!pid.empty() && !stored_pid.empty() && stored_pid != pid)
        return false;

    out.version = 1;
    out.lid = lid;
    out.pid = stored_pid.empty() ? pid : stored_pid;
    out.source = solution::source_key(src);
    out.fetched_at = ll_value(root, "fetched_at");
    out.etag = str_value(root, "etag");
    out.title = str_value(root, "title");
    out.author_name = str_value(root, "author_name");
    out.author_uid = int_value(root, "author_uid");
    out.upvote = int_value(root, "upvote");
    out.reply_count = int_value(root, "reply_count");
    out.favor_count = int_value(root, "favor_count");
    out.category = int_value(root, "category");
    out.promote_status = int_value(root, "promote_status");
    out.difficulty = int_value(root, "difficulty");
    out.time = ll_value(root, "time");
    out.solution_pid = str_value(root, "solution_pid");
    drop_bad_cached_pid(out.solution_pid, path);
    out.solution_type = str_value(root, "solution_type");
    out.solution_name = str_value(root, "solution_name");
    out.content_full = bool_value(root, "content_full");
    out.content = root["content"].get<std::string>();
    out.url = str_value(root, "url");
    return true;
}

bool solcache::store_doc(const DocEntry &entry, std::string &error)
{
    error.clear();
    // pid 为空表示按文章下载（--article）：文章与题目本就没有绑定关系
    if ((!entry.pid.empty() && !safe_pid(entry.pid)) ||
        !solution::valid_lid(entry.lid))
    {
        error = "题解编号或题目编号不合法，拒绝写入题解缓存";
        return false;
    }

    json root = json::object();
    root["version"] = 1;
    root["lid"] = entry.lid;
    root["pid"] = entry.pid;
    root["source"] = entry.source;
    root["fetched_at"] = entry.fetched_at;
    root["etag"] = entry.etag;
    root["title"] = entry.title;
    root["author_uid"] = entry.author_uid;
    root["author_name"] = entry.author_name;
    root["time"] = entry.time;
    root["upvote"] = entry.upvote;
    root["reply_count"] = entry.reply_count;
    root["favor_count"] = entry.favor_count;
    root["category"] = entry.category;
    root["promote_status"] = entry.promote_status;
    root["difficulty"] = entry.difficulty;
    root["solution_pid"] = entry.solution_pid;
    root["solution_type"] = entry.solution_type;
    root["solution_name"] = entry.solution_name;
    root["content_full"] = entry.content_full;
    root["url"] = entry.url;
    root["content"] = entry.content;
    return write_file_atomic(article_path(entry.lid, entry.source == "save"
                                                          ? solution::Source::Save
                                                          : solution::Source::Official),
                             root.dump(2) + "\n", error);
}

bool solcache::is_fresh(long long fetched_at, int ttl_days)
{
    // ttl_days < 0：有效期无限（默认），只要缓存里有就一律优先使用
    if (ttl_days < 0)
        return true;
    // ttl_days == 0：不按时间判定，改为每次发 ETag 条件请求
    if (ttl_days == 0)
        return false;
    if (fetched_at <= 0)
        return false;
    const long long now = static_cast<long long>(std::time(nullptr));
    const long long ttl = static_cast<long long>(ttl_days) * 24 * 3600;
    return now - fetched_at <= ttl;
}

namespace
{
// 删除目录里（含子目录）超过 1 小时的 .tmp.* 残留
void remove_stale_temps(const std::filesystem::path &root, bool recursive)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec)
        return;

    auto handle = [](const std::filesystem::directory_entry &entry) {
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec)
            return;
        const std::string name = luogu::compat::path_to_utf8(entry.path().filename());
        if (!is_temp_name(name))
            return;
        std::error_code time_ec;
        const auto write_time = std::filesystem::last_write_time(entry.path(), time_ec);
        if (time_ec)
            return;
        // last_write_time 的时钟与 steady_clock 不同源：用 file_time_type 的
        // 当前时间做比较（同一时钟域）
        const auto file_now = std::filesystem::file_time_type::clock::now();
        if (file_now - write_time > std::chrono::hours(1))
            std::filesystem::remove(entry.path(), time_ec);
    };

    if (!recursive)
    {
        for (std::filesystem::directory_iterator it(root, ec), end; it != end;
             it.increment(ec))
        {
            if (ec)
                break;
            handle(*it);
        }
        return;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec))
    {
        if (ec)
            break;
        handle(*it);
    }
}
} // namespace

void solcache::cleanup_stale_temp_files()
{
    // 正文缓存的临时文件在 articles/ 里（递归），题解列表的临时文件在
    // 缓存目录根下（solutions.ndjson.tmp.*，只看这一层）
    remove_stale_temps(articles_dir(), true);
    remove_stale_temps(solutions_index_path().parent_path(), false);
}

std::uintmax_t solcache::cache_size()
{
    std::error_code ec;
    std::uintmax_t total = 0;
    const std::filesystem::path root = articles_dir();
    if (std::filesystem::exists(root, ec) && !ec)
    {
        for (std::filesystem::recursive_directory_iterator it(root, ec), end;
             it != end; it.increment(ec))
        {
            if (ec)
                break;
            std::error_code file_ec;
            if (it->is_regular_file(file_ec) && !file_ec)
                total += it->file_size(file_ec);
        }
    }
    std::error_code index_ec;
    const std::filesystem::path index = solutions_index_path();
    if (std::filesystem::exists(index, index_ec) && !index_ec)
        total += std::filesystem::file_size(index, index_ec);
    return total;
}

crawler::derror solcache::clean_solutions()
{
    // -CS, --clean-solutions：只清除题解列表缓存（solutions.ndjson）。
    // store_list 的原子写中断时可能残留 solutions.ndjson.tmp.*（命名规则见
    // compat::temp_sibling_path），与 clean_problems 一样按文件名前缀一并清除
    const std::filesystem::path index = solutions_index_path();
    std::vector<std::filesystem::path> targets = {index};

    const std::filesystem::path dir = index.parent_path();
    std::error_code dir_ec;
    if (std::filesystem::is_directory(dir, dir_ec) && !dir_ec)
    {
        const std::string prefix =
            luogu::compat::path_to_utf8(index.filename()) + ".tmp.";
        std::error_code iter_ec;
        for (std::filesystem::directory_iterator it(dir, iter_ec), end;
             !iter_ec && it != end; it.increment(iter_ec))
        {
            const std::string name =
                luogu::compat::path_to_utf8(it->path().filename());
            if (name.rfind(prefix, 0) == 0)
                targets.push_back(it->path());
        }
        if (iter_ec)
        {
            print_error("读取缓存目录 '" + luogu::compat::path_to_utf8(dir) +
                        "' 失败：" + iter_ec.message());
            return crawler::CANT_REMOVE_FILE;
        }
    }

    for (const std::filesystem::path &target : targets)
    {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        if (ec)
        {
            print_error("无法删除题解列表缓存 '" +
                        luogu::compat::path_to_utf8(target) + "'：" + ec.message());
            return crawler::CANT_REMOVE_FILE;
        }
    }
    // 缓存已清空：内存表下次访问重新从磁盘载入
    invalidate_list_index();
    return crawler::SUCCESS;
}

crawler::derror solcache::clean_articles()
{
    // -CA, --clean-articles：只清除题解正文缓存（articles/ 目录）
    std::error_code ec;
    std::filesystem::remove_all(articles_dir(), ec);
    if (ec)
    {
        print_error("无法删除题解正文缓存目录 '" +
                    luogu::compat::path_to_utf8(articles_dir()) + "'：" + ec.message());
        return crawler::CANT_REMOVE_FILE;
    }
    return crawler::SUCCESS;
}
