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

// src/util/problem_info.cpp
#include "luogu-extract/util/problem_info.h"
#include "luogu-extract/util/tag_cache.h"

std::vector<std::string> luogu::filter_display_tags(const std::vector<std::string> &tags, bool show_all)
{
    if (show_all)
        return tags;

    // 复用进程内共享缓存：tags.json 只读取/解析一次
    const tagcache::Cache &cache = tagcache::shared_cache();
    std::vector<std::string> shown;
    for (const auto &t : tags)
    {
        auto it = cache.name_to_type.find(t);
        if (it == cache.name_to_type.end() || it->second != 2)
            shown.push_back(t);
    }
    return shown;
}

std::vector<std::string> luogu::filter_tags_by_type(const std::vector<std::string> &tags, int type)
{
    const tagcache::Cache &cache = tagcache::shared_cache();
    std::vector<std::string> out;
    out.reserve(tags.size());
    for (const auto &t : tags)
    {
        auto it = cache.name_to_type.find(t);
        if (it != cache.name_to_type.end() && it->second == type)
            out.push_back(t);
    }
    return out;
}
