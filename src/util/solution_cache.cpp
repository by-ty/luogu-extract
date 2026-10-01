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
#include <nlohmann/json.hpp>
#include "luogu-extract/util/compat.h"

using nlohmann::json;

namespace
{
const char *kColorRed = "\033[1;31m";
const char *kColorReset = "\033[0m";

void print_error(const std::string &message)
{
    std::fflush(stdout);
    std::fprintf(stderr, "%s错误：%s %s\n", kColorRed, kColorReset, message.c_str());
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

std::filesystem::path solcache::solutions_dir()
{
    return crawler::get_cache_dir() / "solutions";
}

std::filesystem::path solcache::list_path(const std::string &pid)
{
    return solutions_dir() / pid / "list.json";
}

std::filesystem::path solcache::doc_path(const std::string &pid, const std::string &lid,
                                         solution::Source src)
{
    return solutions_dir() / pid /
           (lid + "." + solution::source_key(src) + ".json");
}

bool solcache::load_list(const std::string &pid, ListEntry &out)
{
    out = ListEntry();
    if (!safe_pid(pid))
        return false;

    std::string text;
    if (!read_file(list_path(pid), text) || text.empty())
        return false;

    json root;
    try
    {
        root = json::parse(text);
    }
    catch (const std::exception &)
    {
        return false; // 结构非法 → 视为未命中并重抓
    }
    if (!root.is_object() || int_value(root, "version") != 1)
        return false;
    if (!root.contains("items") || !root["items"].is_array())
        return false;

    out.version = 1;
    out.pid = safe_pid(str_value(root, "pid")) ? str_value(root, "pid") : pid;
    out.fetched_at = ll_value(root, "fetched_at");
    out.etag = str_value(root, "etag");
    out.total_available = int_value(root, "total_available");
    out.per_page = int_value(root, "per_page");
    out.no_solution = bool_value(root, "no_solution");

    for (const json &item : root["items"])
    {
        if (!item.is_object())
            continue;
        solution::Summary s;
        s.lid = str_value(item, "lid");
        if (!solution::valid_lid(s.lid))
            continue; // 缓存被篡改时同样按白名单过滤
        s.title = str_value(item, "title");
        s.author_uid = int_value(item, "author_uid");
        s.author_name = str_value(item, "author_name");
        s.time = ll_value(item, "time");
        s.upvote = int_value(item, "upvote");
        s.reply_count = int_value(item, "reply_count");
        s.favor_count = int_value(item, "favor_count");
        s.category = int_value(item, "category");
        s.solution_type = int_value(item, "solution_type");
        s.promote_status = int_value(item, "promote_status");
        s.difficulty = int_value(item, "difficulty");
        s.page = int_value(item, "page");
        if (s.page <= 0)
            s.page = 1;
        out.items.push_back(std::move(s));
    }
    if (out.per_page <= 0)
        out.per_page = 10;
    if (out.total_available <= 0)
        out.total_available = static_cast<int>(out.items.size());
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
    return write_file_atomic(list_path(entry.pid), root.dump(2) + "\n", error);
}

bool solcache::load_doc(const std::string &pid, const std::string &lid,
                        solution::Source src, DocEntry &out)
{
    out = DocEntry();
    if (!safe_pid(pid) || !solution::valid_lid(lid))
        return false;

    std::string text;
    if (!read_file(doc_path(pid, lid, src), text) || text.empty())
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

    out.version = 1;
    out.lid = lid;
    out.pid = pid;
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
    if (!safe_pid(entry.pid) || !solution::valid_lid(entry.lid))
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
    return write_file_atomic(doc_path(entry.pid, entry.lid, entry.source == "save"
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

void solcache::cleanup_stale_temp_files()
{
    std::error_code ec;
    const std::filesystem::path root = solutions_dir();
    if (!std::filesystem::exists(root, ec) || ec)
        return;

    const auto now = std::chrono::steady_clock::now();
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec))
    {
        if (ec)
            break;
        if (!it->is_regular_file(ec) || ec)
            continue;
        const std::string name = luogu::compat::path_to_utf8(it->path().filename());
        if (!is_temp_name(name))
            continue;
        std::error_code time_ec;
        const auto write_time = std::filesystem::last_write_time(it->path(), time_ec);
        if (time_ec)
            continue;
        // last_write_time 的时钟与 steady_clock 不同源：用 file_time_type 的
        // 当前时间做比较（同一时钟域）
        const auto file_now = std::filesystem::file_time_type::clock::now();
        const auto age = file_now - write_time;
        if (age > std::chrono::hours(1))
            std::filesystem::remove(it->path(), time_ec);
    }
    (void)now;
}

std::uintmax_t solcache::cache_size()
{
    std::error_code ec;
    std::uintmax_t total = 0;
    const std::filesystem::path root = solutions_dir();
    if (!std::filesystem::exists(root, ec) || ec)
        return 0;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec))
    {
        if (ec)
            break;
        std::error_code file_ec;
        if (it->is_regular_file(file_ec) && !file_ec)
            total += it->file_size(file_ec);
    }
    return total;
}

crawler::derror solcache::clean_solutions()
{
    std::error_code ec;
    std::filesystem::remove_all(solutions_dir(), ec);
    if (ec)
    {
        print_error("无法删除题解缓存目录 '" +
                    luogu::compat::path_to_utf8(solutions_dir()) + "'：" + ec.message());
        return crawler::CANT_REMOVE_FILE;
    }
    return crawler::SUCCESS;
}
