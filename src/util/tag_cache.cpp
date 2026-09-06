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

// src/util/tag_cache.cpp
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/tag_cache.h"

using nlohmann::json;

bool tagcache::Cache::load(const std::filesystem::path &path)
{
    id_to_name.clear();
    name_to_id.clear();
    name_to_type.clear();

    FILE *in = luogu::compat::fopen(path, "rb");
    if (!in)
        return false;

    std::string content;
    try
    {
        // 一次性读入内存再解析：比逐字符流式解析更快
        char buffer[65536];
        size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0)
            content.append(buffer, n);
    }
    catch (...)
    {
        std::fclose(in);
        return false;
    }
    std::fclose(in);

    try
    {
        json data = json::parse(content);
        if (!data.is_object())
            return false;

        for (auto it = data.begin(); it != data.end(); ++it)
        {
            int id = 0;
            try
            {
                id = std::stoi(it.key());
            }
            catch (...)
            {
                continue;
            }

            std::string name;
            int type = 0;
            if (it.value().is_string())
            {
                // 旧格式：{"<id>": "<名称>"}
                name = luogu::compat::strip_control_chars(
                    strip_bom(it.value().get<std::string>()));
            }
            else if (it.value().is_object())
            {
                // 新格式：{"<id>": {"name": "<名称>", "type": <分类>}}
                if (it.value().contains("name") && it.value()["name"].is_string())
                    name = luogu::compat::strip_control_chars(
                        strip_bom(it.value()["name"].get<std::string>()));
                if (it.value().contains("type") && it.value()["type"].is_number_integer())
                    type = it.value()["type"].get<int>();
            }
            else
            {
                continue;
            }

            if (name.empty())
                continue;
            id_to_name[id] = name;
            name_to_id[name] = id;
            name_to_type[name] = type;
        }
    }
    catch (...)
    {
        // 解析中途失败：清空三个容器，避免调用方拿到半份缓存
        // （shared_cache_loaded() 为 false 时不应残留不一致数据）
        id_to_name.clear();
        name_to_id.clear();
        name_to_type.clear();
        return false;
    }
    return !id_to_name.empty();
}

bool tagcache::Cache::load_from_cache_dir()
{
    return load(crawler::get_cache_dir() / "tags.json");
}

namespace
{
// 持有共享缓存及加载状态；函数内 static 保证整个进程只构造（加载）一次，
// 且 C++11 起的初始化是线程安全的。
struct SharedCacheHolder
{
    tagcache::Cache cache;
    bool loaded;

    SharedCacheHolder() : loaded(cache.load_from_cache_dir()) {}
};

const SharedCacheHolder &shared_holder()
{
    static const SharedCacheHolder holder;
    return holder;
}
} // namespace

const tagcache::Cache &tagcache::shared_cache()
{
    return shared_holder().cache;
}

bool tagcache::shared_cache_loaded()
{
    return shared_holder().loaded;
}
