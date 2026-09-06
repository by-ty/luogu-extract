// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 by-ty
//
// This file is part of luogu-export-next
// (https://github.com/by-ty/luogu-export-next), a fork of luogu-export
// (https://github.com/sacharei/luogu-export) which is licensed under the
// MIT License (Copyright (c) 2026 sacharei); see the "Original MIT License"
// section in the LICENSE file.
//
// luogu-export-next is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation, either version 3 of the License, or (at
// your option) any later version. See the LICENSE file or
// https://www.gnu.org/licenses/lgpl-3.0.html for the full license text.
//
// luogu-export-next is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
// License for more details.

// include/luogu-export/util/tag_cache.h
#ifndef LUOGU_EXPORT_TAG_CACHE_H
#define LUOGU_EXPORT_TAG_CACHE_H

#include <filesystem>
#include <string>
#include <unordered_map>

namespace tagcache
{
    // 去掉 UTF-8 BOM（\xEF\xBB\xBF），官方数据里个别名称带该字符
    inline std::string strip_bom(std::string s)
    {
        const std::string bom = "\xEF\xBB\xBF";
        size_t pos;
        while ((pos = s.find(bom)) != std::string::npos)
            s.erase(pos, bom.size());
        return s;
    }

    // 标签缓存（由 -U 生成的 tags.json 加载而来）
    // 兼容旧格式 {"<id>": "<名称>"} 与新格式 {"<id>": {"name": ..., "type": ...}}
    struct Cache
    {
        std::unordered_map<int, std::string> id_to_name;     // 标签 ID -> 中文名
        std::unordered_map<std::string, int> name_to_id;     // 中文名 -> 标签 ID
        std::unordered_map<std::string, int> name_to_type;   // 中文名 -> 官方分类 type

        // 从指定路径加载；成功且至少有一条记录时返回 true
        bool load(const std::filesystem::path &path);

        // 从程序缓存目录下的 tags.json 加载
        bool load_from_cache_dir();
    };

    // 进程内共享的标签缓存：第一次调用时从缓存目录读取并解析一次 tags.json，
    // 之后所有调用直接复用同一份数据，避免每处理一道题就重复读文件。
    const Cache &shared_cache();

    // tags.json 是否成功加载过（用于区分“缓存缺失”与“空缓存”）
    bool shared_cache_loaded();
}

#endif // LUOGU_EXPORT_TAG_CACHE_H
