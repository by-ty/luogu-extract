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

// src/contents/solution.cpp
#include "luogu-extract/contents/solution.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "luogu-extract/crawler/request_gate.h"
#include "luogu-extract/util/compat.h"

using nlohmann::json;

namespace
{
// 重定向到文件/管道时不写 ANSI 转义序列
const char *kColorYellow = luogu::compat::stdout_is_tty() ? "\033[1;33m" : "";
const char *kColorReset = luogu::compat::stdout_is_tty() ? "\033[0m" : "";

void print_warning(const std::string &message)
{
    std::printf("%s%s%s\n", kColorYellow, message.c_str(), kColorReset);
    std::fflush(stdout);
}

std::string str_value(const json &obj, const char *key, const std::string &fallback = "")
{
    if (!obj.is_object() || !obj.contains(key))
        return fallback;
    const json &value = obj[key];
    if (!value.is_string())
        return fallback;
    return luogu::compat::strip_control_chars(value.get<std::string>());
}

int int_value(const json &obj, const char *key, int fallback = 0)
{
    if (!obj.is_object() || !obj.contains(key))
        return fallback;
    const json &value = obj[key];
    if (value.is_number_integer())
        return value.get<int>();
    if (value.is_number_unsigned())
        return static_cast<int>(value.get<unsigned long long>());
    if (value.is_number_float())
        return static_cast<int>(value.get<double>());
    return fallback;
}

long long ll_value(const json &obj, const char *key, long long fallback = 0)
{
    if (!obj.is_object() || !obj.contains(key))
        return fallback;
    const json &value = obj[key];
    if (value.is_number_integer())
        return value.get<long long>();
    if (value.is_number_unsigned())
        return static_cast<long long>(value.get<unsigned long long>());
    if (value.is_number_float())
        return static_cast<long long>(value.get<double>());
    return fallback;
}

// 定位题解数组：顶层可能是 data 或 currentData，数组字段优先 solutions 再 result（含嵌套形态）
bool locate_solutions(const json &root, const json *&array_out, const json *&holder_out)
{
    std::vector<const json *> candidates;
    if (root.is_object())
    {
        if (root.contains("data"))
            candidates.push_back(&root["data"]);
        if (root.contains("currentData"))
            candidates.push_back(&root["currentData"]);
        candidates.push_back(&root);
    }

    static const char *kArrayKeys[] = {"solutions", "result"};
    for (const json *cand : candidates)
    {
        if (!cand || !cand->is_object())
            continue;
        for (const char *key : kArrayKeys)
        {
            if (!cand->contains(key))
                continue;
            const json &value = (*cand)[key];
            if (value.is_array())
            {
                array_out = &value;
                holder_out = cand;
                return true;
            }
            if (value.is_object())
            {
                for (const char *inner : kArrayKeys)
                {
                    if (value.contains(inner) && value[inner].is_array())
                    {
                        array_out = &value[inner];
                        holder_out = &value;
                        return true;
                    }
                }
                if (value.contains("result") && value["result"].is_array())
                {
                    array_out = &value["result"];
                    holder_out = &value;
                    return true;
                }
            }
        }
    }
    return false;
}

// 已带 Cookie 却仍被判未登录时的补充提示（Cookie 过期）
std::string cookie_hint()
{
    if (crawler::gate_has_cookies())
        return "；当前已提供 Cookie，可能是 Cookie 已过期，请重新导出";
    return "";
}

bool looks_like_html(const std::string &body)
{
    std::string head = body.substr(0, 512);
    for (auto &c : head)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return head.find("<!doctype html") != std::string::npos ||
           head.find("<html") != std::string::npos;
}
} // namespace

const char *solution::source_display_name(Source src)
{
    if (src == Source::Save)
        return "洛谷保存站";
    if (src == Source::Auto)
        return "洛谷原站/保存站（自动）";
    return "洛谷原站";
}

const char *solution::source_key(Source src)
{
    return src == Source::Save ? "save" : "official";
}

bool solution::source_from_key(const std::string &key, Source &out)
{
    if (key == "official")
    {
        out = Source::Official;
        return true;
    }
    if (key == "save")
    {
        out = Source::Save;
        return true;
    }
    if (key == "auto")
    {
        out = Source::Auto;
        return true;
    }
    return false;
}

crawler::Channel solution::channel_of(Source src)
{
    return src == Source::Save ? crawler::Channel::Save : crawler::Channel::Official;
}

solution::Source solution::site_of(crawler::Channel ch)
{
    return ch == crawler::Channel::Save ? Source::Save : Source::Official;
}

bool solution::valid_lid(const std::string &lid)
{
    if (lid.size() < 6 || lid.size() > 32)
        return false;
    for (char c : lid)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

solution::ListPage solution::parse_list_page(const std::string &body, int page)
{
    ListPage out;
    out.error.clear();

    if (looks_like_html(body))
    {
        out.error = "题解列表接口返回了 HTML 页面（可能未登录或触发了风控）";
        return out;
    }

    json root;
    try
    {
        root = json::parse(body);
    }
    catch (const std::exception &e)
    {
        out.error = std::string("题解列表接口返回的 JSON 无法解析：") + e.what();
        return out;
    }

    const json *array = nullptr;
    const json *holder = nullptr;
    if (!locate_solutions(root, array, holder) || !array)
    {
        out.error = "题解列表接口返回结构不符合预期（找不到题解数组字段）";
        return out;
    }
    out.structure_ok = true;

    if (holder)
    {
        out.total = int_value(*holder, "count", int_value(*holder, "totalCount", 0));
        out.per_page = int_value(*holder, "perPage", int_value(*holder, "pageSize", 0));
    }

    for (const json &item : *array)
    {
        if (!item.is_object())
            continue;
        Summary s;
        s.lid = str_value(item, "lid");
        s.title = str_value(item, "title");
        s.page = page;
        s.time = ll_value(item, "time");
        s.upvote = int_value(item, "upvote");
        s.reply_count = int_value(item, "replyCount");
        s.favor_count = int_value(item, "favorCount");
        s.category = int_value(item, "category");
        s.promote_status = int_value(item, "promoteStatus");
        if (item.contains("author") && item["author"].is_object())
        {
            s.author_uid = int_value(item["author"], "uid");
            s.author_name = str_value(item["author"], "name");
        }
        if (item.contains("solutionFor") && item["solutionFor"].is_object())
        {
            // solutionFor 是所属题目信息：difficulty 为题目难度；type 是 "P"/"B" 等字符串而非数值，故 solution_type 保持 0
            s.difficulty = int_value(item["solutionFor"], "difficulty");
        }

        // lid 白名单校验：不匹配即丢弃该条并告警（防路径穿越）
        if (!valid_lid(s.lid))
        {
            print_warning("已丢弃一条题解条目：题解编号 '" +
                          (s.lid.empty() ? std::string("(空)") : s.lid) +
                          "' 不符合编号规则");
            continue;
        }
        out.items.push_back(std::move(s));
    }

    if (out.items.empty() && out.total == 0)
        out.total = 0; // 空数组 = 该题确实没有题解
    return out;
}

solution::ListFetch solution::fetch_list(const std::string &pid, int need,
                                         const std::string &etag)
{
    ListFetch result;
    const crawler::Endpoints &ep = crawler::endpoints();

    // need < 0 表示 all：翻到最后一页（受 kMaxListPages 保护）
    const bool want_all = need < 0;

    std::set<std::string> seen;
    int page = 1;
    for (; page <= kMaxListPages; ++page)
    {
        std::string url = ep.official_base + "/problem/solution/" + pid +
                          "?_contentOnly=1&page=" + std::to_string(page);
        crawler::RequestOptions opt;
        opt.url = url;
        opt.send_cookie = true;
        opt.json_content_only = true;
        opt.timeout_sec = 60;
        opt.max_bytes = 8 * 1024 * 1024;
        if (page == 1 && !etag.empty())
            opt.if_none_match = etag;

        const crawler::RequestResult response =
            crawler::http_get(opt, crawler::RequestClass::SolutionList);

        switch (response.status)
        {
        case crawler::RequestStatus::Ok:
            break;
        case crawler::RequestStatus::NotModified:
            result.ok = true;
            result.not_modified = true;
            return result;
        case crawler::RequestStatus::NeedLogin:
            result.need_login = true;
            result.error = "题解列表接口需要登录态（HTTP " +
                           std::to_string(response.http_code) +
                           "）；请登录洛谷后导出 cookies.txt，"
                           "并用 --cookie <file> 指定" + cookie_hint();
            return result;
        case crawler::RequestStatus::RateLimited:
            result.rate_limited = true;
            result.error = "题解列表接口被限流，已停止本次抓取";
            return result;
        case crawler::RequestStatus::Stopped:
            result.stopped = true;
            result.error = "已按你的选择停止抓取";
            return result;
        case crawler::RequestStatus::NotFound:
            result.error = "题目 '" + pid + "' 的题解列表不存在（HTTP 404）";
            return result;
        default:
            result.error = "获取题解列表失败（" + pid + "）：" + response.error;
            return result;
        }

        ++result.pages_fetched;
        if (page == 1)
            result.etag = response.etag;

        ListPage parsed = parse_list_page(response.body, page);
        if (!parsed.structure_ok)
        {
            // 「接口异常」绝不能当成「无题解」静默跳过
            result.error = "题解列表接口返回结构不符合预期（" + pid +
                           "）；请确认 Cookie 有效，或稍后重试。详情：" + parsed.error;
            return result;
        }
        if (parsed.total > 0 || !parsed.items.empty())
            result.total_available = std::max(result.total_available, parsed.total);
        if (parsed.per_page <= 0)
            parsed.per_page = 10;

        const size_t before = parsed.items.size();
        for (auto &item : parsed.items)
        {
            if (!seen.insert(item.lid).second)
                continue;
            result.items.push_back(std::move(item));
        }

        if (parsed.items.empty() || before < static_cast<size_t>(parsed.per_page))
            break;
        if (!want_all && static_cast<int>(result.items.size()) >= need)
            break;
        if (result.total_available > 0 &&
            static_cast<int>(result.items.size()) >= result.total_available)
            break;
    }

    if (page > kMaxListPages)
        print_warning("题解列表请求已达最大页数保护（" +
                      std::to_string(kMaxListPages) +
                      " 页），可能未取全；请检查题目编号是否正确");

    if (result.total_available == 0)
        result.total_available = static_cast<int>(result.items.size());
    result.no_solution = result.items.empty();
    result.ok = true;
    return result;
}

solution::ArticleFetch solution::fetch_article(Source src, const std::string &lid,
                                               const std::string &etag,
                                               crawler::Channel ch)
{
    // Auto 不是具体站点，必须已由调度器决定落到哪个站点
    if (src == Source::Auto)
        src = Source::Official;
    ArticleFetch result;
    const crawler::Endpoints &ep = crawler::endpoints();

    crawler::RequestOptions opt;
    if (src == Source::Save)
    {
        // 保存站：纯 markdown 直取，不发送任何 Cookie
        opt.url = ep.save_base + "/article/query/" + lid;
        opt.send_cookie = false;
        opt.json_content_only = false;
    }
    else
    {
        opt.url = ep.official_base + "/article/" + lid + "?_contentOnly=1";
        opt.send_cookie = true;
        opt.json_content_only = true;
    }
    opt.timeout_sec = 60;
    opt.max_bytes = kMaxArticleBytes + 64 * 1024;
    if (!etag.empty())
        opt.if_none_match = etag;

    const crawler::RequestResult response =
        crawler::http_get(opt, crawler::RequestClass::SolutionArticle, ch);
    result.http_status = response.http_code;
    result.etag = response.etag;

    switch (response.status)
    {
    case crawler::RequestStatus::Ok:
        break;
    case crawler::RequestStatus::NotModified:
        result.ok = true;
        result.not_modified = true;
        return result;
    case crawler::RequestStatus::NotFound:
        result.not_found = true;
        result.error = "题解 " + lid + " 不存在或已删除（HTTP 404）";
        return result;
    case crawler::RequestStatus::NeedLogin:
        result.need_login = true;
        result.error = "题解正文接口需要登录态（HTTP " +
                       std::to_string(response.http_code) +
                       "）；请登录洛谷后导出 cookies.txt，并用 --cookie <file> 指定" +
                       cookie_hint();
        return result;
    case crawler::RequestStatus::RateLimited:
        result.rate_limited = true;
        result.error = "题解正文接口被限流，已停止本次抓取";
        return result;
    case crawler::RequestStatus::Stopped:
        result.stopped = true;
        result.error = "已按你的选择停止抓取";
        return result;
    default:
        result.error = "获取题解正文失败（" + lid + "）：" + response.error;
        return result;
    }

    std::string error;
    bool parsed = false;
    if (src == Source::Save)
    {
        parsed = parse_save_article(response.body, result.article, error);
    }
    else if (looks_like_html(response.body))
    {
        // 接口未返回 JSON 时退回解析页面内嵌的 lentille-context
        result.article = article::Article(response.body);
        parsed = !result.article.content.empty();
        if (!parsed)
            error = "题解页面结构不符合预期（未找到 lentille-context 正文）";
    }
    else
    {
        parsed = parse_official_article(response.body, result.article, error);
    }

    if (!parsed)
    {
        result.error = "解析题解 " + lid + " 失败：" + error;
        return result;
    }

    if (result.article.lid.empty())
        result.article.lid = lid;

    // 单篇体量失控保护：超限截断并标记 content_full=false
    if (result.article.content.size() > kMaxArticleBytes)
    {
        result.article.content.resize(kMaxArticleBytes);
        result.article.content_full = false;
        print_warning("题解 " + lid + " 正文超过 2 MB，已截断并标记为不完整");
    }

    if (result.article.content.empty() || !result.article.content_full)
        result.incomplete = true;
    result.ok = true;
    return result;
}

bool solution::parse_official_article(const std::string &body, article::Article &out,
                                      std::string &error)
{
    error.clear();
    json root;
    try
    {
        root = json::parse(body);
    }
    catch (const std::exception &e)
    {
        error = std::string("JSON 无法解析：") + e.what();
        return false;
    }
    try
    {
        if (!root.contains("data") || !root["data"].is_object() ||
            !root["data"].contains("article"))
        {
            // 未登录错误模板会带 status=401
            if (root.contains("status") && root["status"].is_number_integer() &&
                root["status"].get<int>() == 401)
            {
                error = "需要登录态（服务器返回未登录错误模板）";
                return false;
            }
            error = "响应中找不到 data.article 字段";
            return false;
        }
        return out.from_json(root["data"]["article"], error);
    }
    catch (const std::exception &e)
    {
        // 兜底：字段类型不符等异常绝不能逃出抓取线程（auto 模式下本函数运行在 std::thread 里）
        error = std::string("题解数据字段异常：") + e.what();
        return false;
    }
}

bool solution::parse_save_article(const std::string &body, article::Article &out,
                                  std::string &error)
{
    error.clear();
    json root;
    try
    {
        root = json::parse(body);
    }
    catch (const std::exception &e)
    {
        error = std::string("JSON 无法解析：") + e.what();
        return false;
    }

    const int code = int_value(root, "code", 200);
    if (code != 200)
    {
        error = "保存站返回 code " + std::to_string(code) + "（" +
                str_value(root, "message", "无说明") + "）";
        return false;
    }
    if (!root.contains("data") || !root["data"].is_object())
    {
        error = "保存站响应中找不到 data 字段";
        return false;
    }

    const json &data = root["data"];
    out.lid = str_value(data, "id");
    out.title = str_value(data, "title");
    out.category = int_value(data, "category");
    out.time = ll_value(data, "publishTime", ll_value(data, "time", 0));
    out.upvote = int_value(data, "upvote");
    out.favor_count = int_value(data, "favorCount");
    out.author_uid = int_value(data, "authorId");
    out.status = int_value(data, "deleted") != 0 ? 0 : 2;
    out.promote_status = int_value(data, "priority");
    out.solution_pid = str_value(data, "solutionForPid");
    out.content = luogu::compat::strip_control_chars(str_value(data, "content"));

    if (data.contains("author") && data["author"].is_object())
    {
        out.author_name = str_value(data["author"], "name");
        if (out.author_uid == 0)
            out.author_uid = int_value(data["author"], "id");
    }

    // 保存站不提供 contentFull：正文非空即视为完整
    out.content_full = !out.content.empty();
    return true;
}
