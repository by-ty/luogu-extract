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

// src/contents/article.cpp
#include <cstdio>
#include <string>
#include <limits>
// FindLibXml2 / pkg-config 提供的 include 目录一般是 <prefix>/include/libxml2，
// 因此直接写 <libxml/...>（写 <libxml2/libxml/...> 在 Homebrew/vcpkg 等
// 只给出 libxml2 目录的环境会编译失败）
#include <libxml/parser.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <nlohmann/json.hpp>
#include "luogu-extract/contents/article.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/image_util.h"

using nlohmann::json;
using article::Article;

namespace
{
// 官方接口的字段类型并不稳定（例如 "content": null、"upvote": "5"、
// "author": 3）：nlohmann 的 value()/get<>() 在「键存在但类型不符」时抛
// type_error，而本文件的解析跑在抓取线程里，异常逃出线程函数即 terminate。
// 因此统一走下面几个容错取值：键缺失或类型不符一律取默认值，绝不抛异常。
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

bool bool_value(const json &obj, const char *key, bool fallback = false)
{
    if (!obj.is_object() || !obj.contains(key))
        return fallback;
    const json &value = obj[key];
    if (!value.is_boolean())
        return fallback;
    return value.get<bool>();
}
} // namespace

article::Article::Article() { return; }

article::Article::Article(std::string html)
{
    if (html.empty())
        return;

    if (html.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        std::fprintf(stderr, "HTML 内容过大，无法解析\n");
        return;
    }

    // 1. 解析 HTML
    htmlDocPtr doc = htmlReadMemory(html.c_str(), static_cast<int>(html.size()), nullptr, "UTF-8", HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return;

    // 2. 使用 XPath 查找 id="lentille-context" 的 script 标签
    xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
    if (!xpathCtx)
    {
        xmlFreeDoc(doc);
        return;
    }
    xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(BAD_CAST "//script[@id='lentille-context']", xpathCtx);
    if (!xpathObj || !xpathObj->nodesetval || xpathObj->nodesetval->nodeNr == 0)
    {
        xmlXPathFreeObject(xpathObj);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return;
    }

    // 3. 获取 script 标签内的文本内容
    xmlNodePtr node = xpathObj->nodesetval->nodeTab[0];
    if (!node)
    {
        xmlXPathFreeObject(xpathObj);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return;
    }
    xmlChar *xmlContent = xmlNodeGetContent(node);
    if (!xmlContent)
    {
        xmlXPathFreeObject(xpathObj);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return;
    }
    std::string jsonStr(reinterpret_cast<const char *>(xmlContent));
    xmlFree(xmlContent);
    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);

    // 4. 解析 JSON
    try
    {
        json data = json::parse(jsonStr);
        if (!data.contains("data") || !data["data"].is_object() ||
            !data["data"].contains("article"))
        {
            std::fprintf(stderr, "题解页面结构不符合预期（找不到 data.article）\n");
            return;
        }
        std::string parse_error;
        from_json(data["data"]["article"], parse_error);
    }
    catch (const std::exception &e)
    {
        // JSON 解析失败，保留默认值
        std::fprintf(stderr, "解析 JSON 失败：%s\n", e.what());
    }
}

bool article::Article::from_json(const json &articleData, std::string &error)
{
    error.clear();
    if (!articleData.is_object())
    {
        error = "题解数据不是 JSON 对象";
        return false;
    }

    // 基本字段（str_value 内部已过滤控制字符，避免 NUL 截断输出/破坏 LaTeX）
    lid = str_value(articleData, "lid");
    title = str_value(articleData, "title");
    category = int_value(articleData, "category");
    time = ll_value(articleData, "time");

    // 作者信息（author 可能是 null，也可能不是对象）
    if (articleData.contains("author") && articleData["author"].is_object())
    {
        author_uid = int_value(articleData["author"], "uid");
        author_name = str_value(articleData["author"], "name");
        author_avatar = str_value(articleData["author"], "avatar");
    }

    // 统计数据
    upvote = int_value(articleData, "upvote");
    reply_count = int_value(articleData, "replyCount");
    favor_count = int_value(articleData, "favorCount");
    status = int_value(articleData, "status");

    // 对应的题解信息
    if (articleData.contains("solutionFor") && articleData["solutionFor"].is_object())
    {
        solution_pid = str_value(articleData["solutionFor"], "pid");
        solution_type = str_value(articleData["solutionFor"], "type");
        solution_name = str_value(articleData["solutionFor"], "name");
        solution_difficulty = int_value(articleData["solutionFor"], "difficulty");
    }

    promote_status = int_value(articleData, "promoteStatus");

    // 文章内容
    content = str_value(articleData, "content");
    content_full = bool_value(articleData, "contentFull");

    admin_note = str_value(articleData, "adminNote");

    return true;
}

std::vector<std::string> Article::image_urls() const
{
    return image_util::extract_urls(content);
}

void Article::print()
{
    printf("题解编号：%s  标题：%s\n", lid.c_str(), title.c_str());
    printf("分类：%d  发布时间：%lld\n", category, time);
    printf("作者 UID：%d  作者昵称：%s\n\n", author_uid, author_name.c_str());

    printf("点赞数：%d  回复数：%d  收藏数：%d\n", upvote, reply_count, favor_count);
    printf("状态：%d  推荐状态：%d\n", status, promote_status);

    if(!solution_pid.empty())
        printf("对应题解：题号 %s  类型 %s  名称 %s  难度 %d\n\n",
               solution_pid.c_str(), solution_type.c_str(), solution_name.c_str(), solution_difficulty);

    printf("正文：\n%s\n", content.c_str());
}
