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
        json articleData = data["data"]["article"];

        // 基本字段（过滤控制字符，避免 NUL 截断输出/破坏 LaTeX）
        lid = luogu::compat::strip_control_chars(articleData.value("lid", ""));
        title = luogu::compat::strip_control_chars(articleData.value("title", ""));
        category = articleData.value("category", 0);
        time = articleData.value("time", 0LL);

        // 作者信息
        if (articleData.contains("author") && !articleData["author"].is_null())
        {
            author_uid = articleData["author"].value("uid", 0);
            author_name = luogu::compat::strip_control_chars(
                articleData["author"].value("name", ""));
            author_avatar = luogu::compat::strip_control_chars(
                articleData["author"].value("avatar", ""));
        }

        // 统计数据
        upvote = articleData.value("upvote", 0);
        reply_count = articleData.value("replyCount", 0);
        favor_count = articleData.value("favorCount", 0);
        status = articleData.value("status", 0);

        // 对应的题解信息
        if (articleData.contains("solutionFor") && !articleData["solutionFor"].is_null())
        {
            solution_pid = luogu::compat::strip_control_chars(
                articleData["solutionFor"].value("pid", ""));
            solution_type = luogu::compat::strip_control_chars(
                articleData["solutionFor"].value("type", ""));
            solution_name = luogu::compat::strip_control_chars(
                articleData["solutionFor"].value("name", ""));
            solution_difficulty = articleData["solutionFor"].value("difficulty", 0);
        }

        promote_status = articleData.value("promoteStatus", 0);

        // 文章内容
        content = luogu::compat::strip_control_chars(
            articleData.value("content", ""));
        content_full = articleData.value("contentFull", false);

        if (articleData.contains("adminNote") && !articleData["adminNote"].is_null())
            admin_note = luogu::compat::strip_control_chars(
                articleData["adminNote"].get<std::string>());
    }
    catch (const std::exception &e)
    {
        // JSON 解析失败，保留默认值
        std::fprintf(stderr, "解析 JSON 失败：%s\n", e.what());
    }
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
