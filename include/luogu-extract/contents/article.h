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

// include/luogu-export/contents/article.h
#ifndef LUOGU_EXPORT_CONTENTS_ARTICLE_H
#define LUOGU_EXPORT_CONTENTS_ARTICLE_H

#include <string>
#include <vector>

namespace article
{
    struct Article
    {
    public:
        std::string lid, title;
        int category = 0;
        long long time = 0;
        int author_uid = 0; std::string author_name, author_avatar;
        int upvote = 0, reply_count = 0, favor_count = 0;
        int status = 0;
        std::string solution_pid, solution_type, solution_name;
        int solution_difficulty = 0;
        int promote_status = 0;
        std::string content, admin_note;
        bool content_full = false;

        Article();
        Article(std::string html);

        // 扫描文章正文 markdown 中的图片链接，返回去重后的链接列表
        std::vector<std::string> image_urls() const;

        void print();
    };
}

#endif // LUOGU_EXPORT_CONTENTS_ARTICLE_H
