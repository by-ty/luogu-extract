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

// include/luogu-extract/contents/problem.h
#ifndef LUOGU_EXTRACT_CONTENTS_PROBLEM_H
#define LUOGU_EXTRACT_CONTENTS_PROBLEM_H

#include <vector>
#include <utility>
#include <unordered_map>
#include <nlohmann/json.hpp>

typedef std::pair<std::string, std::string> pss;

namespace problem
{
    struct Problem
    {
    public:
        std::string pid, type;
        int difficulty = 0; std::vector<std::string> tags; // 标签名（中文，与缓存一致）
        std::string name, background, description, formatI, formatO, hint;
        std::vector<pss> samples;
        std::vector<int> time, memory;
        nlohmann::json translations; // 多语言题面；当前仅保留 en，可能为空对象
        
        Problem();

        // 从缓存 NDJSON 的一行构造；tag_id_to_name 用于把数字 ID 形式的 tags 翻译成名称
        Problem(const nlohmann::json &data,
                const std::unordered_map<int, std::string> *tag_id_to_name = nullptr);
        
        // 扫描题面 markdown 中的图片链接，返回去重后的列表
        std::vector<std::string> image_urls() const;
    };
}

#endif // LUOGU_EXTRACT_CONTENTS_PROBLEM_H
