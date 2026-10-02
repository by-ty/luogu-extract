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

// include/luogu-extract/crawler/crawler.h
#ifndef LUOGU_EXTRACT_CRAWLER_CRAWLER_H
#define LUOGU_EXTRACT_CRAWLER_CRAWLER_H

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace crawler
{
    /// 爬取错误码
    enum derror
    {
        SUCCESS = 0,
        CANT_CREAT_FILE,     // 无法创建或打开目标文件
        INIT_ERROR,          // libcurl 初始化失败
        DOWNLOAD_FAIL,       // 网络传输失败
        HTTP_ERROR,          // 服务器返回非 200 状态码
        EMPTY_RESPONSE,      // 响应内容为空
        INVALID_ARGUMENT,    // 参数无效（如空的题目/文章编号）
        ENV_ERROR,           // 缺少必要的环境变量
        DECOMPRESS_ERROR,    // gzip 解压失败
        CANT_REMOVE_FILE,    // 无法删除缓存文件或目录
    };

    /// 抓取网页内容；失败返回空字符串并置 error。
    /// 目标主机为环回/私网/链路本地地址时拒绝抓取（SSRF 防护）
    std::string get_html(const std::string& url, derror* error = nullptr);

    /// 下载进度回调：(url, 已下载字节数, 总字节数)；total 为 0 表示未知
    using download_progress_callback = std::function<void(const std::string &url,
                                                         long long downloaded,
                                                         long long total)>;

    /// 下载 url 到 fpath（Windows 下支持中文路径）。
    /// 只允许 http/https，且每一跳（含重定向后的连接）都校验实际连接 IP，
    /// 环回/私网/链路本地/云元数据等内部地址一律拒绝（SSRF 防护）。
    /// 设 LUOGU_EXTRACT_ALLOW_PRIVATE_IMAGE_HOST=1 可关闭地址检查（协议白名单仍生效）
    derror downloadFile(const std::string &url, const std::filesystem::path &fpath,
                        const download_progress_callback &progress = nullptr);

    /// 程序使用的缓存根目录
    std::filesystem::path get_cache_dir();

    /// 更新标签缓存（官方 /_lfe/tags/zh-CN），保存为 <cache_dir>/tags.json：
    /// 键为标签数字 ID，值为 {"name": 中文名称, "type": 官方分类}，按 ID 升序
    derror update_tags();

    /// 下载一批图片到 <cache_dir>/images/。文件名 = 完整 URL 的双种子 FNV-1a
    /// 128 位哈希（32 位十六进制）+ 白名单扩展名（不含 URL 原文），已存在则跳过。
    /// redownload=true 时忽略已有图片：先写临时文件，校验通过后原子替换，
    /// 失败只删临时文件，原缓存不变
    derror download_images(const std::vector<std::string> &urls,
                           bool redownload = false);

    /// 图片 URL 对应的缓存文件路径（与 download_images 落盘位置一致）
    std::filesystem::path image_cache_path(const std::string &url);

    /// 删除整个缓存目录（题目列表、标签、图片与字体）；不存在视为已清空
    derror clean_all();

    /// 删除 <cache_dir>/images/；不存在视为已清空
    derror clean_images();

    /// 删除 <cache_dir>/fonts/（--set-font-* 传入无扩展名字体时会复制到该目录）；
    /// 不存在视为已清空
    derror clean_fonts();

    /// 删除 latest.ndjson、latest.ndjson.gz 及更新中断残留的 latest.ndjson.tmp.*；
    /// 不存在视为已清空
    derror clean_problems();

    /// 更新题目列表缓存
    derror update();
}

#endif // LUOGU_EXTRACT_CRAWLER_CRAWLER_H
