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
    /// 爬取相关的错误码
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

    /// @param url 目标网址
    /// @param error 可选输出参数，成功时为 SUCCESS，失败时为对应错误码
    /// @return 成功返回网页内容，失败返回空字符串
    /// @note 目标主机为环回/私网/链路本地地址时拒绝抓取（SSRF 防护），
    ///       返回空字符串并置 error 为 DOWNLOAD_FAIL
    std::string get_html(const std::string& url, derror* error = nullptr);

    /// 下载进度回调：参数为 (url, 已下载字节数, 总字节数)；total 为 0 表示未知
    using download_progress_callback = std::function<void(const std::string &url,
                                                         long long downloaded,
                                                         long long total)>;

    /// @param url      下载地址
    /// @param fpath    保存路径（Windows 下按 UTF-8/宽字符处理，中文路径可用）
    /// @param progress 可选进度回调；不传时使用默认的百分比进度显示
    /// @return SUCCESS 或对应错误码
    /// @note 只允许 http/https；每一跳（含 302 之后的连接）都会校验实际连接
    ///       的 IP，属于环回/私网/链路本地/云元数据等内部地址时拒绝下载
    ///       （SSRF 防护），不创建文件并返回 DOWNLOAD_FAIL。
    ///       设置 LUOGU_EXTRACT_ALLOW_PRIVATE_IMAGE_HOST=1 可关闭地址检查
    ///       （协议白名单仍然生效）
    derror downloadFile(const std::string &url, const std::filesystem::path &fpath,
                        const download_progress_callback &progress = nullptr);

    /// @return the base cache directory used by this program
    std::filesystem::path get_cache_dir();

    /// 更新标签缓存（来源：官方标签接口 /_lfe/tags/zh-CN）
    /// 保存为 <cache_dir>/tags.json：JSON 对象，键为标签数字 ID，
    /// 值为 {"name": 中文名称, "type": 官方分类}，按数字 ID 升序排列，
    /// 便于直接按键查找。
    /// @return SUCCESS 或对应错误码
    derror update_tags();

    /// 下载一批图片链接到缓存目录（<cache_dir>/images/）。
    /// 文件名由完整 URL 的双种子 FNV-1a 128 位哈希（32 位十六进制）
    /// 与白名单扩展名组成（不含 URL 原文），避免不同图床同名互相覆盖；
    /// 已存在的文件直接跳过。
    /// @param urls 图片链接列表
    /// @param redownload 为 true 时忽略缓存中已有的图片，全部重新下载：
    ///        新图片先下载到缓存目录中的临时文件，校验通过后再原子替换
    ///        缓存中的同名文件；下载失败或内容无效时只删除临时文件，
    ///        原有缓存保持不变
    /// @return SUCCESS 或对应错误码（部分失败时返回第一个错误码）
    derror download_images(const std::vector<std::string> &urls,
                           bool redownload = false);

    /// 返回图片 URL 在缓存中对应的文件路径（<cache_dir>/images/<文件名>）。
    /// 文件名由完整 URL 的哈希生成，与 download_images 的落盘位置一致。
    std::filesystem::path image_cache_path(const std::string &url);

    /// 清空全部缓存：删除缓存目录 <cache_dir> 本身（含题目列表、标签、
    /// 图片与字体缓存）。缓存目录不存在时视为已清空。
    /// @return SUCCESS 或对应错误码
    derror clean_all();

    /// 清空图片缓存：删除 <cache_dir>/images/ 目录及其中的全部图片。
    /// 目录不存在时视为已清空。
    /// @return SUCCESS 或对应错误码
    derror clean_images();

    /// 清空字体缓存：删除 <cache_dir>/fonts/ 目录及其中的全部字体文件
    /// （--set-font-* 传入无扩展名的字体文件时会复制到该目录）。
    /// 目录不存在时视为已清空。
    /// @return SUCCESS 或对应错误码
    derror clean_fonts();

    /// 清空题目列表缓存：删除 <cache_dir>/latest.ndjson 与
    /// <cache_dir>/latest.ndjson.gz，以及更新中断时可能残留的
    /// latest.ndjson.tmp.* 临时文件。文件不存在时视为已清空。
    /// @return SUCCESS 或对应错误码
    derror clean_problems();

    /// @return SUCCESS 或对应错误码
    derror update();
}

#endif // LUOGU_EXTRACT_CRAWLER_CRAWLER_H
