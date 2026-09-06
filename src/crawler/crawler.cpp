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

// src/crawler/crawler.cpp
#include <string>
#include <functional>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <curl/curl.h>
#include <zlib.h>
#include <filesystem>
#include <cstring>
#include <limits>
#include <vector>
#include <algorithm>
#include <utility>
#include <nlohmann/json.hpp>
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/util/compat.h"

using nlohmann::json;

namespace
{
const char *kColorReset  = "\033[0m";
const char *kColorRed    = "\033[1;31m";
const char *kColorGreen  = "\033[1;32m";

// 多线程 worker 可能并发打印错误信息，用互斥锁避免输出交错
std::mutex &print_mutex()
{
    static std::mutex m;
    return m;
}

void print_error(const std::string &message)
{
    std::lock_guard<std::mutex> lock(print_mutex());
    fflush(stdout);
    // 颜色复位放在具体消息之前：只有 "error:" 用红色，消息保持默认色
    std::fprintf(stderr, "%serror:%s %s\n", kColorRed, kColorReset, message.c_str());
}

void print_success(const std::string &message)
{
    std::lock_guard<std::mutex> lock(print_mutex());
    fflush(stdout);
    std::printf("%s%s%s\n", kColorGreen, message.c_str(), kColorReset);
}

// FNV-1a 64 位哈希核心：以给定 seed 作为初始哈希值，逐字节异或后乘素数。
// 官方偏移基数与素数分别作为两个种子，用于生成 128 位（32 位十六进制）哈希
uint64_t fnv1a64(const std::string &s, uint64_t seed)
{
    uint64_t hash = seed;
    for (unsigned char c : s)
    {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// URL -> 缓存文件名：仅由哈希值与扩展名组成（不含 URL 原文）。
// 哈希 = 双种子 FNV-1a 拼 128 位：分别以官方偏移基数
// 0xcbf29ce484222325 与官方素数 0x100000001b3 为种子计算两路 64 位
// FNV-1a，高 64 位在前、低 64 位在后拼接成 128 位，输出 32 位十六进制；
// 不同 URL 生成的文件名不会碰撞（大小写不敏感文件系统、Unicode 等
// 情况下依然唯一）；扩展名取自 URL 路径并做白名单清洗（仅小写字母数字
// 1-5 位），非法/超长扩展名丢弃，避免 Windows 非法路径或超长路径
std::string image_cache_filename(const std::string &url)
{
    // 扩展名：URL 路径（忽略查询参数）最后一个 '.' 之后的字母数字串
    std::string path = url;
    const size_t query = path.find_first_of("?#");
    if (query != std::string::npos)
        path.resize(query);
    std::string ext;
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    {
        std::string candidate = path.substr(dot + 1);
        if (!candidate.empty() && candidate.size() <= 5)
        {
            bool ok = true;
            for (char &c : candidate)
            {
                if (std::isalnum(static_cast<unsigned char>(c)))
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                else
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
                ext = "." + candidate;
        }
    }

    // 双种子 FNV-1a：高 64 位以偏移基数为种子，低 64 位以素数为种子
    const uint64_t kFnvOffsetBasis = 14695981039346656037ULL; // 0xcbf29ce484222325
    const uint64_t kFnvPrime = 1099511628211ULL;              // 0x100000001b3
    const uint64_t high = fnv1a64(url, kFnvOffsetBasis);
    const uint64_t low = fnv1a64(url, kFnvPrime);

    char buf[33];
    snprintf(buf, sizeof(buf), "%016llx%016llx",
             static_cast<unsigned long long>(high),
             static_cast<unsigned long long>(low));
    return std::string(buf) + ext;
}

// 是否为洛谷图床（cdn.luogu.com.cn 等）的图片
bool is_luogu_image_host(const std::string &url)
{
    return url.find("luogu.com.cn") != std::string::npos;
}

// 随机延时 0.5~3 秒，避免下载洛谷图床图片时请求过快
void random_delay()
{
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(0.5, 3.0);
    std::this_thread::sleep_for(std::chrono::duration<double>(dist(rng)));
}

// 校验文件是否真的是图片（按文件头魔数判断 PNG/JPEG/GIF/WebP/BMP/SVG）
bool looks_like_image_file(const std::filesystem::path &path)
{
    FILE *in = luogu::compat::fopen(path, "rb");
    if (!in)
        return false;
    unsigned char head[12] = {0};
    const size_t n = std::fread(head, 1, sizeof(head), in);
    std::fclose(in);
    if (n >= 8 && std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0)
        return true;
    if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
        return true;
    if (n >= 6 && std::memcmp(head, "GIF8", 4) == 0)
        return true;
    if (n >= 12 && std::memcmp(head, "RIFF", 4) == 0 &&
        std::memcmp(head + 8, "WEBP", 4) == 0)
        return true;
    if (n >= 2 && head[0] == 'B' && head[1] == 'M')
        return true;
    if (n >= 4 && std::memcmp(head, "<svg", 4) == 0)
        return true;
    if (n >= 5 && std::memcmp(head, "<?xml", 5) == 0)
        return true;
    return false;
}
} // namespace

static bool decompress_gzip_file(const std::filesystem::path &input_path,
                                 const std::filesystem::path &output_path)
{
    gzFile in = luogu::compat::gzopen(input_path, "rb");
    if (!in)
        return false;

    if (!output_path.parent_path().empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(output_path.parent_path(), ec);
        if (ec)
        {
            gzclose(in);
            return false;
        }
    }

    FILE *out = luogu::compat::fopen(output_path, "wb");
    if (!out)
    {
        gzclose(in);
        return false;
    }

    // 解压后大小上限：防止 gzip bomb 写满磁盘。
    // 官方 latest.ndjson 数百 MB 级别，8 GiB 上限足够宽松
    const uint64_t kMaxOutputBytes = 8ULL * 1024 * 1024 * 1024;
    uint64_t written = 0;
    bool failed = false;
    char buffer[8192];
    int read_bytes = 0;
    while ((read_bytes = gzread(in, buffer, sizeof(buffer))) > 0)
    {
        written += static_cast<uint64_t>(read_bytes);
        if (written > kMaxOutputBytes)
        {
            failed = true; // 超过上限：按失败处理（疑似 gzip bomb）
            break;
        }
        if (std::fwrite(buffer, 1, static_cast<size_t>(read_bytes), out) !=
            static_cast<size_t>(read_bytes))
        {
            failed = true;
            break;
        }
    }

    if (!failed && read_bytes < 0)
        failed = true;

    if (std::fclose(out) != 0)
        failed = true;
    const int status = gzclose(in);
    if (!failed && status != Z_OK)
        failed = true;

    if (failed)
    {
        std::error_code ec;
        std::filesystem::remove(output_path, ec);
        return false;
    }
    return true;
}

// get_html 的响应体上限：防止服务器返回异常内容时无限吃内存
struct HtmlResponse
{
    std::string data;
    size_t max_bytes = 128 * 1024 * 1024;
};

static size_t write_callback_html(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    auto *response = static_cast<HtmlResponse *>(userp);
    if (total > response->max_bytes ||
        response->data.size() > response->max_bytes - total)
    {
        // 超过上限：返回 0 让 libcurl 中止传输（CURLE_WRITE_ERROR）
        return 0;
    }
    response->data.append(static_cast<char *>(contents), total);
    return total;
}

// downloadFile 的写盘回调：携带已写字节数，支持文件大小上限
struct FileResponse
{
    FILE *out = nullptr;
    // 2 GiB 上限；32 位平台（size_t 为 32 位）时退化为 SIZE_MAX，
    // 避免常量回绕成 0 导致所有下载失败
    size_t max_bytes = (sizeof(size_t) < 8)
                           ? std::numeric_limits<size_t>::max()
                           : static_cast<size_t>(2ULL * 1024 * 1024 * 1024);
    size_t written = 0;
    bool overflow = false;
};

size_t write_callback_file(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    if (total == 0)
        return 0;

    auto *response = static_cast<FileResponse *>(userp);
    if (total > response->max_bytes ||
        response->written > response->max_bytes - total)
    {
        response->overflow = true;
        return 0; // 超过上限：中止传输
    }
    if (std::fwrite(contents, 1, total, response->out) != total)
        return 0;
    response->written += total;
    return total;
}

// 默认进度回调：显示百分比（与旧行为一致）
void default_progress(const std::string &url, long long downloaded, long long total)
{
    (void)url;
    if (total > 0)
    {
        int cur = static_cast<int>(downloaded * 100 / total);
        printf("\033[u\033[K%3d %%", cur);   // 恢复位置 → 清到行尾 → 输出进度
        fflush(stdout);
    }
}

// libcurl 进度回调：转发到用户提供的回调
struct ProgressContext
{
    const crawler::download_progress_callback *callback;
    std::string url;
};

int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    auto *ctx = static_cast<ProgressContext *>(clientp);
    if (dltotal > 0 && ctx->callback)
        (*ctx->callback)(ctx->url,
                         static_cast<long long>(dlnow),
                         static_cast<long long>(dltotal));
    return 0;
}

std::filesystem::path crawler::get_cache_dir()
{
    // 环境变量一律按 UTF-8 读取：Windows 下 CRT 的 getenv 按 ANSI 代码页
    // 解释，含中文用户名等的路径会被破坏
    const std::string xdg_cache_home = luogu::compat::getenv_utf8("XDG_CACHE_HOME");
    if (!xdg_cache_home.empty())
        return luogu::compat::path_from_utf8(xdg_cache_home) / "luogu-extract";

    const std::string home_env = luogu::compat::getenv_utf8("HOME");
    if (!home_env.empty())
        return luogu::compat::path_from_utf8(home_env) / ".cache" / "luogu-extract";

#ifdef _WIN32
    // Windows 下按惯例使用 %LOCALAPPDATA% 作为用户缓存根目录
    const std::string local_app_data = luogu::compat::getenv_utf8("LOCALAPPDATA");
    if (!local_app_data.empty())
        return luogu::compat::path_from_utf8(local_app_data) / "luogu-extract";
#endif

    std::error_code ec;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec)
        return std::filesystem::path(".cache") / "luogu-extract";
    return temp_dir / "luogu-extract";
}

std::string crawler::get_html(const std::string &url, derror *error)
{
    if (error) *error = SUCCESS;

    HtmlResponse response;
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        print_error("Failed to initialize libcurl while fetching " + url);
        if (error) *error = INIT_ERROR;
        return "";
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback_html);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    // 网络资源上限：连接/总超时与最大响应体积（防挂起与无限吃内存）
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "luogu-extract/0.1");

    CURLcode curl_res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (curl_res == CURLE_WRITE_ERROR && http_code == 200)
    {
        print_error("Failed to fetch " + url + ": response too large");
        if (error) *error = DOWNLOAD_FAIL;
        return "";
    }
    if (curl_res != CURLE_OK)
    {
        print_error("Failed to fetch " + url + ": " + curl_easy_strerror(curl_res));
        if (error) *error = DOWNLOAD_FAIL;
        return "";
    }
    if (http_code != 200)
    {
        print_error("Failed to fetch " + url + ": HTTP status code " + std::to_string(http_code));
        if (error) *error = HTTP_ERROR;
        return "";
    }
    if (response.data.empty())
    {
        print_error("Failed to fetch " + url + ": empty response");
        if (error) *error = EMPTY_RESPONSE;
        return "";
    }
    return std::move(response.data);
}

crawler::derror crawler::downloadFile(const std::string &url,
                                      const std::filesystem::path &fpath,
                                      const crawler::download_progress_callback &progress)
{
    const std::filesystem::path parent = fpath.parent_path();
    if (!parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            print_error("Failed to create directory '" + luogu::compat::path_to_utf8(parent) +
                        "': " + ec.message());
            return CANT_CREAT_FILE;
        }
    }

    FILE *out_file = luogu::compat::fopen(fpath, "wb");
    if (!out_file)
    {
        print_error("Failed to open file '" + luogu::compat::path_to_utf8(fpath) +
                    "' for writing");
        return CANT_CREAT_FILE;
    }

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        print_error("Failed to initialize libcurl while downloading " + url);
        std::fclose(out_file);
        // 初始化失败时删除空文件：否则下一次 download_images 看到
        // exists 会把它当成已缓存图片跳过
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        return INIT_ERROR;
    }

    // 未提供回调时使用默认的百分比进度显示；只有默认进度才输出
    // \033[s 保存光标序列，自定义回调（如 download_images 的空回调）
    // 不输出任何转义序列，避免多线程并发写 stdout 互相干扰
    const bool use_default_progress = !progress;
    if (use_default_progress)
    {
        fflush(stdout);
        printf("\033[s");  // 保存光标位置
    }

    crawler::download_progress_callback effective =
        use_default_progress ? default_progress : progress;

    ProgressContext ctx;
    ctx.callback = &effective;
    ctx.url = url;

    FileResponse file_response;
    file_response.out = out_file;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file_response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "luogu-extract/0.1");

    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    std::fclose(out_file);

    if (file_response.overflow)
    {
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        print_error("Failed to download " + url + ": file too large");
        return DOWNLOAD_FAIL;
    }
    if (res != CURLE_OK)
    {
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        print_error("Failed to download " + url + ": " + curl_easy_strerror(res));
        return DOWNLOAD_FAIL;
    }
    if (http_code != 200)
    {
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        print_error("Failed to download " + url + ": HTTP status code " + std::to_string(http_code));
        return HTTP_ERROR;
    }

    return SUCCESS;
}

std::string crawler::get_html_prob(std::string p, derror *error)
{
    if (error) *error = SUCCESS;
    if (p.empty())
    {
        print_error("Invalid problem id: expected a non-empty string");
        if (error) *error = INVALID_ARGUMENT;
        return "";
    }

    std::string url = "https://www.luogu.com.cn/problem/" + p;
    derror fetch_error = SUCCESS;
    std::string html = crawler::get_html(url, &fetch_error);
    if (fetch_error != SUCCESS)
    {
        print_error("Failed to fetch problem page for '" + p + "'");
        if (error) *error = fetch_error;
        return "";
    }
    return html;
}

std::string crawler::get_html_article(std::string id, derror *error)
{
    if (error) *error = SUCCESS;
    if (id.empty())
    {
        print_error("Invalid article id: expected a non-empty string");
        if (error) *error = INVALID_ARGUMENT;
        return "";
    }

    std::string url = "https://www.luogu.com.cn/article/" + id;
    derror fetch_error = SUCCESS;
    std::string html = crawler::get_html(url, &fetch_error);
    if (fetch_error != SUCCESS)
    {
        print_error("Failed to fetch article page for '" + id + "'");
        if (error) *error = fetch_error;
        return "";
    }
    return html;
}

crawler::derror crawler::update_tags()
{
    std::filesystem::path cache_dir = crawler::get_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec)
    {
        print_error("Failed to create cache directory '" +
                    luogu::compat::path_to_utf8(cache_dir) + "': " + ec.message());
        return ENV_ERROR;
    }

    // 官方标签接口（题目列表页中通过 __luoguTagRequest 暴露）
    const std::string url = "https://www.luogu.com.cn/_lfe/tags/zh-CN";
    derror fetch_error = SUCCESS;
    printf("Downloading tags: ");
    std::string body = get_html(url, &fetch_error);
    printf("\n");

    if (fetch_error != SUCCESS)
    {
        print_error("Failed to update the tag cache (download failed)");
        return fetch_error;
    }

    try
    {
        json data = json::parse(body);
        if (!data.contains("tags") || !data["tags"].is_array())
        {
            print_error("Failed to update the tag cache (unexpected response format)");
            return EMPTY_RESPONSE;
        }

        // 收集 (标签 ID, 中文名, 分类)，按数字 ID 升序排列，便于人工查阅
        struct TagEntry
        {
            int id;
            std::string name;
            int type;
        };
        std::vector<TagEntry> entries;
        for (const auto &t : data["tags"])
        {
            if (!t.contains("id") || !t.contains("name") ||
                !t["id"].is_number_integer() || !t["name"].is_string())
                continue;

            // 官方数据中个别名称带 BOM 字符（如 \ufeff基础算法），入库前清理；
            // 同时过滤控制字符（\u0000 等），避免输出/解析时被截断
            std::string name = luogu::compat::strip_control_chars(
                t["name"].get<std::string>());
            const std::string bom = "\xEF\xBB\xBF";
            size_t pos;
            while ((pos = name.find(bom)) != std::string::npos)
                name.erase(pos, bom.size());

            int type = 0;
            if (t.contains("type") && t["type"].is_number_integer())
                type = t["type"].get<int>();

            entries.push_back({t["id"].get<int>(), std::move(name), type});
        }

        if (entries.empty())
        {
            print_error("Failed to update the tag cache (no valid tags found)");
            return EMPTY_RESPONSE;
        }
        std::sort(entries.begin(), entries.end(),
                  [](const TagEntry &a, const TagEntry &b) { return a.id < b.id; });

        // 每条记录：{"<数字ID>": {"name": "<中文名>", "type": <分类>}, ...}，
        // 程序里可直接按键查找；type 用于区分“算法”类标签
        json tag_map = json::object();
        for (const auto &e : entries)
        {
            json item = json::object();
            item["name"] = e.name;
            item["type"] = e.type;
            tag_map[std::to_string(e.id)] = std::move(item);
        }

        // 原子写：临时文件 + fsync + rename；写入中断/磁盘满不会破坏已有缓存
        std::filesystem::path save_path = cache_dir / "tags.json";
        const std::filesystem::path tmp_path =
            luogu::compat::temp_sibling_path(save_path);
        FILE *out = luogu::compat::fopen(tmp_path, "w");
        if (!out)
        {
            print_error("Failed to open '" + luogu::compat::path_to_utf8(tmp_path) +
                        "' for writing");
            return CANT_CREAT_FILE;
        }
        const std::string dump = tag_map.dump(4);
        const bool write_ok = std::fwrite(dump.data(), 1, dump.size(), out) ==
                                  dump.size() &&
                              std::fputc('\n', out) == '\n' && !std::ferror(out);
        const bool flushed = luogu::compat::flush_and_sync(out);
        const bool closed = std::fclose(out) == 0;
        if (!write_ok || !flushed || !closed)
        {
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
            print_error("Failed to write '" + luogu::compat::path_to_utf8(save_path) + "'");
            return CANT_CREAT_FILE;
        }
        std::filesystem::rename(tmp_path, save_path, ec);
        if (ec)
        {
            std::filesystem::remove(tmp_path, ec);
            print_error("Failed to write '" + luogu::compat::path_to_utf8(save_path) +
                        "': " + ec.message());
            return CANT_CREAT_FILE;
        }
    }
    catch (const std::exception &e)
    {
        print_error(std::string("Failed to parse tag data: ") + e.what());
        return EMPTY_RESPONSE;
    }

    print_success("Tag cache updated successfully");
    return SUCCESS;
}

crawler::derror crawler::download_images(const std::vector<std::string> &urls)
{
    std::filesystem::path cache_dir = crawler::get_cache_dir();
    std::filesystem::path image_dir = cache_dir / "images";
    std::error_code ec;
    std::filesystem::create_directories(image_dir, ec);
    if (ec)
    {
        print_error("Failed to create image cache directory '" +
                    luogu::compat::path_to_utf8(image_dir) + "': " + ec.message());
        return ENV_ERROR;
    }

    // URL 去重：同一 URL 只下载一次，避免多个线程同时写同一缓存文件
    std::vector<std::string> unique_urls;
    {
        std::set<std::string> seen;
        for (const auto &url : urls)
            if (seen.insert(url).second)
                unique_urls.push_back(url);
    }

    const int total = static_cast<int>(unique_urls.size());

    // 直接在同一行显示完整进度，避免与其他保存/恢复光标的序列冲突。
    // 先打印前缀，监视线程每次使用 '\r' 回到行首并重写整行内容。
    printf("Downloading image(s): ");

    // 并行下载：多个 worker 通过原子索引领取 URL，洛谷图床下载串行化并保持随机间隔
    std::atomic<size_t> next_index{0};
    std::atomic<int> downloaded{0}, skipped{0};
    std::atomic<bool> monitor_stop{false};
    std::mutex luogu_mutex;
    std::mutex error_mutex;
    bool first_luogu_download = true;
    derror first_error = SUCCESS;

    // 启动监视线程，定期读取 downloaded 并更新输出（基于 downloaded/total）
    std::thread monitor([&] {
        while (!monitor_stop.load())
        {
            int d = downloaded.load();
            // 使用浮点计算并四舍五入，避免长时间为 0 的地板除
            int cur = total > 0 ? static_cast<int>(std::floor((static_cast<double>(d) * 100.0) / static_cast<double>(total) + 0.5)) : 100;
            // 回到行首并清除到行尾，重写完整前缀 + 进度
            printf("\rDownloading image(s): %3d %% (%d/%d).\033[K", cur, d, total);
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        // 结束前再做一次最终输出并换行
        int d = downloaded.load();
        int cur = total > 0 ? static_cast<int>(std::floor((static_cast<double>(d) * 100.0) / static_cast<double>(total) + 0.5)) : 100;
        printf("\rDownloading image(s): %3d %% (%d/%d), done.\033[K\n", cur, d, total);
        fflush(stdout);
    });

    const size_t n_workers = std::min<size_t>(unique_urls.size(),
                                              std::max<size_t>(1, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    workers.reserve(n_workers);
    for (size_t w = 0; w < n_workers; ++w)
    {
        workers.emplace_back([&] {
            for (;;)
            {
                const size_t i = next_index.fetch_add(1, std::memory_order_relaxed);
                if (i >= unique_urls.size())
                    break;
                const std::string &url = unique_urls[i];
                if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
                    continue; // 只处理 http(s) 图片链接

                const std::filesystem::path save_path = image_dir / image_cache_filename(url);
                std::error_code exists_ec;
                if (std::filesystem::exists(save_path, exists_ec) && !exists_ec)
                {
                    // 已存在：校验文件头确实是图片。此前进程被杀等场景可能
                    // 留下半截文件，不校验会把它永远当成已缓存图片
                    if (looks_like_image_file(save_path))
                    {
                        ++skipped;
                        continue;
                    }
                    std::filesystem::remove(save_path, exists_ec);
                }

                auto download_one = [&] {
                    // 图片下载不显示进度条（可自定义回调）
                    const derror result = downloadFile(url, save_path,
                                                       [](const std::string &, long long, long long) {});
                    if (result != SUCCESS)
                    {
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (first_error == SUCCESS)
                            first_error = result;
                        return;
                    }

                    // 校验下载内容确实是图片；无效内容（如错误页）删除并视为失败
                    if (!looks_like_image_file(save_path))
                    {
                        std::error_code rm_ec;
                        std::filesystem::remove(save_path, rm_ec);
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (first_error == SUCCESS)
                            first_error = DOWNLOAD_FAIL;
                        return;
                    }
                    ++downloaded;
                };

                if (is_luogu_image_host(url))
                {
                    // 洛谷图床图片串行下载，之间随机间隔 0.5~3 秒
                    std::lock_guard<std::mutex> lock(luogu_mutex);
                    if (!first_luogu_download)
                        random_delay();
                    first_luogu_download = false;
                    download_one();
                }
                else
                {
                    download_one();
                }
            }
        });
    }
    for (auto &worker : workers)
        worker.join();

    // 停止监视线程并等待其结束
    monitor_stop.store(true);
    if (monitor.joinable())
        monitor.join();

    if (downloaded == 0 && skipped == 0)
    {
        print_error("No images to download");
        return first_error != SUCCESS ? first_error : EMPTY_RESPONSE;
    }
    return first_error;
}

std::filesystem::path crawler::image_cache_path(const std::string &url)
{
    return crawler::get_cache_dir() / "images" / image_cache_filename(url);
}

crawler::derror crawler::update()
{
    std::filesystem::path cache_dir = crawler::get_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec)
    {
        print_error("Failed to create cache directory '" +
                    luogu::compat::path_to_utf8(cache_dir) + "': " + ec.message());
        return ENV_ERROR;
    }

    std::string url = "https://cdn.luogu.com.cn/problemset-open/latest.ndjson.gz";
    std::filesystem::path save_path = cache_dir / "latest.ndjson.gz";
    std::filesystem::path extract_path = cache_dir / "latest.ndjson";
    printf("Downloading problems: ");
    derror result = downloadFile(url, save_path);
    printf("\n");

    if (result != SUCCESS)
    {
        print_error("Failed to update the problem list cache (download failed)");
        return result;
    }

    // 解压到临时文件，成功后 fsync + rename 原子替换 latest.ndjson：
    // 解压中断/磁盘满不会破坏已有可用缓存
    const std::filesystem::path tmp_extract =
        luogu::compat::temp_sibling_path(extract_path);
    if (!decompress_gzip_file(save_path, tmp_extract))
    {
        std::filesystem::remove(tmp_extract, ec);
        print_error("Failed to decompress the downloaded file '" +
                    luogu::compat::path_to_utf8(save_path) + "'");
        return DECOMPRESS_ERROR;
    }
    std::filesystem::rename(tmp_extract, extract_path, ec);
    if (ec)
    {
        std::filesystem::remove(tmp_extract, ec);
        print_error("Failed to write '" + luogu::compat::path_to_utf8(extract_path) +
                    "': " + ec.message());
        return CANT_CREAT_FILE;
    }

    print_success("Problem list cache updated successfully");

    // 题目列表更新成功后，顺带更新标签缓存（保存为 tags.json）
    return crawler::update_tags();
}
