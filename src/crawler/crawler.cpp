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
#ifdef _WIN32
// Windows 下主机名解析相关的类型与函数来自 winsock2/ws2tcpip；
// 按惯例在包含 curl 之前先包含 winsock2.h（并禁用 min/max 宏）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif
#include <curl/curl.h>
#include <zlib.h>
#include <filesystem>
#include <cstring>
#include <limits>
#include <map>
#include <vector>
#include <algorithm>
#include <utility>
#include <nlohmann/json.hpp>
#include "luogu-extract/crawler/crawler.h"
#include "luogu-extract/util/compat.h"

using nlohmann::json;

namespace
{
// 重定向到文件/管道时不写 ANSI 转义序列：错误/警告走 stderr，成功提示走 stdout
const char *kColorReset  = luogu::compat::stderr_is_tty() ? "\033[0m" : "";
const char *kColorRed    = luogu::compat::stderr_is_tty() ? "\033[1;31m" : "";
const char *kColorGreen  = luogu::compat::stdout_is_tty() ? "\033[1;32m" : "";
const char *kColorYellow = luogu::compat::stderr_is_tty() ? "\033[1;33m" : "";

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
    // 颜色复位放在具体消息之前：只有 "错误：" 用红色，消息保持默认色
    std::fprintf(stderr, "%s错误：%s %s\n", kColorRed, kColorReset, message.c_str());
}

void print_success(const std::string &message)
{
    std::lock_guard<std::mutex> lock(print_mutex());
    fflush(stdout);
    std::printf("%s%s%s\n", kColorGreen, message.c_str(), kColorReset);
}

// 非致命问题（如主机名解析失败）的黄色警告提示，与错误信息一样加锁
void print_warning(const std::string &message)
{
    std::lock_guard<std::mutex> lock(print_mutex());
    fflush(stdout);
    std::fprintf(stderr, "%s警告：%s %s\n", kColorYellow, kColorReset, message.c_str());
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

// 取出 URL 的主机名：小写、不含 userinfo 与端口，IPv6 字面量去掉方括号。
// 不带 "://" 或缺少主机名（如 data:、bilibili: 等伪协议）时返回空字符串
std::string url_host(const std::string &url)
{
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        return "";
    const size_t begin = scheme_end + 3;
    size_t end = url.find_first_of("/?#", begin);
    if (end == std::string::npos)
        end = url.size();
    std::string authority = url.substr(begin, end - begin);
    // 去掉 userinfo（user:password@host）
    const size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority.erase(0, at + 1);
    if (!authority.empty() && authority[0] == '[')
    {
        // IPv6 字面量：取方括号内的地址，忽略端口
        const size_t close = authority.find(']');
        if (close == std::string::npos)
            return "";
        authority = authority.substr(1, close - 1);
    }
    else
    {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos)
            authority.erase(colon);
    }
    for (char &c : authority)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return authority;
}

// 解析数字形式 IPv4 地址的一段（支持十进制、0 开头八进制、0x 开头十六进制）
bool parse_ipv4_number(const std::string &text, unsigned long long &value)
{
    if (text.empty())
        return false;
    int base = 10;
    size_t i = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        i = 2;
    }
    else if (text.size() > 1 && text[0] == '0')
    {
        base = 8;
        i = 1;
    }
    unsigned long long result = 0;
    for (; i < text.size(); ++i)
    {
        const char c = text[i];
        int digit = -1;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        if (digit < 0 || digit >= base || result > 0xFFFFFFFFULL / base)
            return false;
        result = result * base + static_cast<unsigned long long>(digit);
        if (result > 0xFFFFFFFFULL)
            return false;
    }
    value = result;
    return true;
}

// 按 inet_aton 语义把「1~4 段」的数字主机名解析成 IPv4 地址。
// curl 自己接受 http://2130706433/、http://0x7f.0.0.1/ 这类写法，
// 而 getaddrinfo 在部分平台（如 Windows）解析不出来，必须在本地识别
bool parse_numeric_ipv4(const std::string &host, uint32_t &address)
{
    unsigned long long parts[4] = {0, 0, 0, 0};
    size_t begin = 0;
    int count = 0;
    for (;;)
    {
        if (count == 4)
            return false; // 超过 4 段：不是 IPv4 字面量
        const size_t dot = host.find('.', begin);
        const std::string part =
            host.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin);
        if (!parse_ipv4_number(part, parts[count]))
            return false;
        ++count;
        if (dot == std::string::npos)
            break;
        begin = dot + 1;
    }

    // inet_aton 语义：1 段为 32 位，2 段为 8+24，3 段为 8+8+16，4 段为 8×4
    uint32_t result = 0;
    for (int i = 0; i < count; ++i)
    {
        const int bits = (i < count - 1) ? 8 : (32 - 8 * (count - 1));
        if (parts[i] > ((1ULL << bits) - 1))
            return false;
        result = (result << bits) | static_cast<uint32_t>(parts[i]);
    }
    address = result;
    return true;
}

// IPv4（主机字节序）是否属于环回/私网/链路本地等内部网段
bool is_blocked_ipv4(uint32_t address)
{
    if ((address >> 24) == 0)                     // 0.0.0.0/8（“本网络”）
        return true;
    if ((address >> 24) == 127 || (address >> 24) == 10) // 127.0.0.0/8、10.0.0.0/8
        return true;
    if ((address >> 20) == 0xAC1)                 // 172.16.0.0/12
        return true;
    if ((address >> 16) == 0xC0A8 ||              // 192.168.0.0/16
        (address >> 16) == 0xA9FE)                // 169.254.0.0/16（链路本地）
        return true;
    // 运营商级 NAT 与云厂商元数据等同样属于「内部」地址：
    // 100.64.0.0/10（含阿里云元数据 100.100.100.200）、
    // 168.63.129.16（Azure wire server）、192.0.0.0/24（IETF 协议分配）、
    // 198.18.0.0/15（基准测试网段）
    if ((address >> 22) == 0x191)                 // 100.64.0.0/10
        return true;
    if (address == 0xA83F8110u)                   // 168.63.129.16
        return true;
    if ((address >> 16) == 0xC000)                // 192.0.0.0/24
        return true;
    if ((address >> 17) == 0x6300)                // 198.18.0.0/15
        return true;
    return false;
}

// 解析出来的地址是否属于内部网段（IPv4 / IPv6 两种族）
bool is_blocked_sockaddr(const struct sockaddr *addr)
{
    if (addr == nullptr)
        return false;
    if (addr->sa_family == AF_INET)
    {
        const auto *v4 = reinterpret_cast<const struct sockaddr_in *>(addr);
        return is_blocked_ipv4(ntohl(v4->sin_addr.s_addr));
    }
    if (addr->sa_family == AF_INET6)
    {
        const auto *v6 = reinterpret_cast<const struct sockaddr_in6 *>(addr);
        const unsigned char *b = v6->sin6_addr.s6_addr;
        bool all_zero = true;
        bool loopback = b[15] == 1;
        for (int i = 0; i < 16; ++i)
            if (b[i] != 0)
                all_zero = false;
        for (int i = 0; i < 15; ++i)
            if (b[i] != 0)
                loopback = false;
        if (all_zero || loopback)                 // :: 与 ::1
            return true;
        if ((b[0] & 0xFE) == 0xFC)                // fc00::/7（唯一本地地址）
            return true;
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) // fe80::/10（链路本地）
            return true;
        // IPv4 映射/兼容地址（::ffff:127.0.0.1、::127.0.0.1）按 IPv4 规则判断
        bool v4_prefix = true;
        for (int i = 0; i < 10; ++i)
            if (b[i] != 0)
                v4_prefix = false;
        if (v4_prefix &&
            ((b[10] == 0xFF && b[11] == 0xFF) || (b[10] == 0 && b[11] == 0)))
        {
            const uint32_t ip = (static_cast<uint32_t>(b[12]) << 24) |
                                (static_cast<uint32_t>(b[13]) << 16) |
                                (static_cast<uint32_t>(b[14]) << 8) |
                                static_cast<uint32_t>(b[15]);
            return is_blocked_ipv4(ip);
        }
    }
    return false;
}

// 主机名是否解析到内部网段（带进程内缓存）。
// 解析失败时返回 false（放行）：离线、内网镜像或 DNS 临时故障时无法判断
// 目标，误拒会破坏正常下载；真正的下载失败仍由 libcurl 报出并提示
bool is_blocked_host_name(const std::string &host)
{
    struct Resolved
    {
        bool resolved;
        bool blocked;
    };
    // 同一主机的图片经常成批下载：缓存解析结果，避免每个 URL 都做一次 DNS
    // 查询（DNS 故障时每次都要等解析超时），解析失败的警告也只提示一次
    static std::mutex cache_mutex;
    static std::map<std::string, Resolved> cache;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = cache.find(host);
        if (it != cache.end())
            return it->second.blocked;
    }

    Resolved entry = {false, false};
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0)
    {
        entry.resolved = true;
        for (const struct addrinfo *p = result; p != nullptr && !entry.blocked;
             p = p->ai_next)
            entry.blocked = is_blocked_sockaddr(p->ai_addr);
        freeaddrinfo(result);
    }

    bool first_lookup = false;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        // emplace 保留其他线程已写入的结果，只由第一个线程负责警告
        first_lookup = cache.emplace(host, entry).second;
    }
    if (first_lookup && !entry.resolved)
        // 主机名来自远程内容：去掉控制字符，避免 \n 之类伪造日志行
        print_warning("无法解析主机名 '" +
                      luogu::compat::strip_control_chars(host) +
                      "'，跳过内网地址检查");
    return entry.blocked;
}

// SSRF 防护：URL 是否指向环回/私网/链路本地等内部地址。
// 图片链接来自题面/题解/文章与 --local 本地文件（可能被第三方内容控制），
// 不能拿来探测用户内网
// 环境变量开关：允许下载内网/本机图床的图片（默认关闭）。SSRF 防护默认
// 拒绝环回/私网/链路本地地址，但内网镜像、自建图床或本地调试（如
// .dev/test_redownload.cpp 直接传 127.0.0.1 的图片 URL）确实需要它，
// 因此留一个显式开关；协议白名单（只允许 http/https）不受该开关影响。
bool allow_private_image_host()
{
    static const bool allowed = [] {
        const std::string v = luogu::compat::getenv_utf8("LUOGU_EXTRACT_ALLOW_PRIVATE_IMAGE_HOST");
        return !v.empty() && v != "0" && v != "false" && v != "no";
    }();
    return allowed;
}

bool is_blocked_download_url(const std::string &url)
{
    if (allow_private_image_host())
        return false; // 用户显式允许内网地址：只保留协议白名单
    const std::string host = url_host(url);
    if (host.empty())
        return false; // 非 http(s) 或缺少主机名：交给协议白名单与 libcurl 处理
    // 明显的本机名字直接拒绝，不依赖解析结果（/etc/hosts 缺失时也能拦住）
    if (host == "localhost" || host == "localhost.localdomain" ||
        host == "ip6-localhost" || host == "ip6-loopback" ||
        host == "localhost4" || host == "localhost6" || host == "ip6-localnet")
        return true;
    if (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0)
        return true;

    uint32_t numeric_address = 0;
    if (parse_numeric_ipv4(host, numeric_address))
        return is_blocked_ipv4(numeric_address);
    return is_blocked_host_name(host);
}

// 字面 IP 字符串（curl 回调里给出的实际连接地址）是否属于内部网段。
// 与 is_blocked_download_url 的区别：这里不解析域名，只看 IP 本身。
bool is_blocked_ip_literal(const std::string &ip)
{
    if (ip.empty())
        return false;
    if (ip.find(':') != std::string::npos)
    {
        struct sockaddr_in6 addr6;
        std::memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, ip.c_str(), &addr6.sin6_addr) == 1)
            return is_blocked_sockaddr(reinterpret_cast<const struct sockaddr *>(&addr6));
        return false;
    }
    struct in_addr addr4;
    if (inet_pton(AF_INET, ip.c_str(), &addr4) == 1)
        return is_blocked_ipv4(ntohl(addr4.s_addr));
    return false;
}

#if LIBCURL_VERSION_NUM >= 0x075000
// curl 7.80+：每次建立连接（含跳转后的新连接）前都会调用本回调，
// 回调里给出的是**即将连接的 IP**，因此 302 到内网、DNS 重绑定都能拦住。
// 返回 ABORT 会让 curl_easy_perform 以 CURLE_ABORTED_BY_CALLBACK 结束。
int curl_prereq_callback(void * /*clientp*/, char *conn_primary_ip,
                         char * /*conn_local_ip*/, int /*conn_primary_port*/,
                         int /*conn_local_port*/)
{
    if (allow_private_image_host())
        return CURL_PREREQFUNC_OK;
    if (conn_primary_ip && is_blocked_ip_literal(conn_primary_ip))
        return CURL_PREREQFUNC_ABORT;
    return CURL_PREREQFUNC_OK;
}
#endif

// 统一的「禁止内网地址」curl 选项设置：跳转后的每一跳都校验。
void set_ssrf_guard(CURL *curl)
{
#if LIBCURL_VERSION_NUM >= 0x075000
    curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, curl_prereq_callback);
#endif
    // 老版本 curl 没有 PREREQFUNCTION：只能靠调用方在 perform 之后用
    // CURLINFO_EFFECTIVE_URL 复核最终地址（见 check_effective_url_blocked）
}

// perform 之后的兜底校验：最终地址（跳转链的终点）指向内网时视为失败。
// 老版本 curl 的唯一防线；新版本上与 PREREQFUNCTION 互为补充。
bool check_effective_url_blocked(CURL *curl)
{
    char *effective = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective) != CURLE_OK ||
        effective == nullptr)
        return false;
    return is_blocked_download_url(effective);
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

// 递归删除文件或目录（不存在时不算失败），用于清除缓存。
// 返回 true 表示删除成功（removed 为删除的条目数，路径不存在时为 0）；
// 失败时返回 false，并通过 error 输出失败原因。
bool remove_cache_entry(const std::filesystem::path &path, std::uintmax_t &removed,
                        std::string &error)
{
    std::error_code ec;
    removed = std::filesystem::remove_all(path, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
    return true;
}

// 把路径列表拼成「'a'、'b'」形式的提示文本
std::string join_quoted_paths(const std::vector<std::filesystem::path> &paths)
{
    std::string out;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        if (i)
            out += "、";
        out += "'" + luogu::compat::path_to_utf8(paths[i]) + "'";
    }
    return out;
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

// 进度行重写：回到行首 → 重新输出「前缀 + 百分比」→ 清到行尾。
// 不使用 ANSI 保存/恢复光标序列（\033[s / \033[u）：这类序列依赖终端的
// 兼容实现（Windows 传统控制台要启用虚拟终端处理后才是模拟支持），
// 「行首 + 清行重写」只用到 \r 与 \033[K，与 download_images 监视线程一致
static void print_progress_line(const char *prefix, long long downloaded, long long total)
{
    if (total <= 0)
        return;
    const int cur = static_cast<int>(downloaded * 100 / total);
    printf("\r%s%3d %%\033[K", prefix, cur);
    fflush(stdout);
}

// 默认进度回调：显示百分比（与旧行为一致）
void default_progress(const std::string &url, long long downloaded, long long total)
{
    (void)url;
    print_progress_line("正在下载：", downloaded, total);
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

    // 临时目录：先看各平台常见的环境变量（只接受绝对路径），
    // 再看系统临时目录；两者都不可用时退回绝对路径，不再返回依赖 CWD 的
    // 相对路径（README 描述的是「系统临时目录」）
    for (const char *name : {"TMPDIR", "TEMP", "TMP"})
    {
        const std::string value = luogu::compat::getenv_utf8(name);
        if (value.empty())
            continue;
        const std::filesystem::path dir = luogu::compat::path_from_utf8(value);
        if (dir.is_absolute())
            return dir / "luogu-extract";
    }

    std::error_code ec;
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);
    if (!ec && temp_dir.is_absolute())
        return temp_dir / "luogu-extract";

#ifdef _WIN32
    // 兜底 1：%USERPROFILE% 即用户主目录（与 README 的「用户主目录」一致）
    const std::string user_profile = luogu::compat::getenv_utf8("USERPROFILE");
    if (!user_profile.empty())
    {
        const std::filesystem::path dir = luogu::compat::path_from_utf8(user_profile);
        if (dir.is_absolute())
            return dir / ".cache" / "luogu-extract";
    }
#endif

    // 兜底 2：以上都不可用（环境变量与系统临时目录均缺失或非法）时，
    // 明确警告后退回当前工作目录下的隐藏目录，并保证是绝对路径
    std::error_code cwd_ec;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_ec);
    std::filesystem::path fallback;
    if (!cwd_ec)
        fallback = cwd;
    fallback /= ".cache";
    fallback /= "luogu-extract";
    // 只警告一次：缓存目录会被频繁查询（如逐张图片计算缓存路径）
    static std::once_flag fallback_warned;
    std::call_once(fallback_warned, [&fallback] {
        print_warning("无法确定系统临时目录，缓存将写入 '" +
                      luogu::compat::path_to_utf8(fallback) + "'");
    });
    return fallback;
}

std::string crawler::get_html(const std::string &url, derror *error)
{
    if (error) *error = SUCCESS;

    // SSRF 防护：拒绝抓取环回/私网/链路本地等内部地址
    // （判定逻辑见 is_blocked_download_url）
    if (is_blocked_download_url(url))
    {
        print_error("拒绝抓取 " + url +
                    "：目标主机指向环回/私网/链路本地地址（如确需访问内网镜像，"
                    "可设置环境变量 LUOGU_EXTRACT_ALLOW_PRIVATE_IMAGE_HOST=1）");
        if (error) *error = DOWNLOAD_FAIL;
        return "";
    }

    HtmlResponse response;
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        print_error("初始化 libcurl 失败（抓取 " + url + "）");
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
    // 协议白名单：只允许 http/https，跳转目标同样受限，
    // 避免被重定向到 file:// 等本地协议
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "luogu-extract/0.1");
    // 内网地址防护覆盖跳转后的每一跳（见 set_ssrf_guard）
    set_ssrf_guard(curl);

    CURLcode curl_res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    const bool redirected_to_private = check_effective_url_blocked(curl);
    curl_easy_cleanup(curl);

    if (curl_res == CURLE_ABORTED_BY_CALLBACK || redirected_to_private)
    {
        print_error("拒绝抓取 " + url + "：跳转目标指向环回/私网/链路本地地址"
                    "（LUOGU_EXTRACT_ALLOW_PRIVATE_IMAGE_HOST=1 可关闭该检查）");
        if (error) *error = DOWNLOAD_FAIL;
        return "";
    }

    if (curl_res == CURLE_WRITE_ERROR && http_code == 200)
    {
        print_error("抓取 " + url + " 失败：响应内容过大");
        if (error) *error = DOWNLOAD_FAIL;
        return "";
    }
    if (curl_res != CURLE_OK)
    {
        print_error("抓取 " + url + " 失败：libcurl 报告错误 " + curl_easy_strerror(curl_res));
        if (error) *error = DOWNLOAD_FAIL;
        return "";
    }
    if (http_code != 200)
    {
        print_error("抓取 " + url + " 失败：HTTP 状态码 " + std::to_string(http_code));
        if (error) *error = HTTP_ERROR;
        return "";
    }
    if (response.data.empty())
    {
        print_error("抓取 " + url + " 失败：响应内容为空");
        if (error) *error = EMPTY_RESPONSE;
        return "";
    }
    return std::move(response.data);
}

crawler::derror crawler::downloadFile(const std::string &url,
                                      const std::filesystem::path &fpath,
                                      const crawler::download_progress_callback &progress)
{
    // SSRF 防护：拒绝下载环回/私网/链路本地等内部地址（判定逻辑见
    // is_blocked_download_url）；拒绝时不创建任何文件
    if (is_blocked_download_url(url))
    {
        print_error("拒绝下载 " + url +
                    "：目标主机指向环回/私网/链路本地地址（如确需下载内网图床，"
                    "可设置环境变量 LUOGU_EXTRACT_ALLOW_PRIVATE_IMAGE_HOST=1）");
        return DOWNLOAD_FAIL;
    }

    const std::filesystem::path parent = fpath.parent_path();
    if (!parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            print_error("无法创建目录 '" + luogu::compat::path_to_utf8(parent) +
                        "'：" + ec.message());
            return CANT_CREAT_FILE;
        }
    }

    FILE *out_file = luogu::compat::fopen(fpath, "wb");
    if (!out_file)
    {
        print_error("无法打开文件 '" + luogu::compat::path_to_utf8(fpath) +
                    "'（写入）");
        return CANT_CREAT_FILE;
    }

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        print_error("初始化 libcurl 失败（下载 " + url + "）");
        std::fclose(out_file);
        // 初始化失败时删除空文件：否则下一次 download_images 看到
        // exists 会把它当成已缓存图片跳过
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        return INIT_ERROR;
    }

    // 未提供回调时使用默认的百分比进度显示；默认进度在行首重写整行，
    // 自定义回调（如 download_images 的空回调）不输出任何转义序列，
    // 避免多线程并发写 stdout 互相干扰
    const bool use_default_progress = !progress;

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
    // 协议白名单：只允许 http/https，跳转目标同样受限
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "luogu-extract/0.1");

    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // 内网地址防护覆盖跳转后的每一跳（见 set_ssrf_guard）
    set_ssrf_guard(curl);
    CURLcode res = curl_easy_perform(curl);
    const bool redirected_to_private = check_effective_url_blocked(curl);
    if (res == CURLE_ABORTED_BY_CALLBACK || redirected_to_private)
    {
        print_error("拒绝下载 " + url + "：跳转目标指向环回/私网/链路本地地址"
                    "（LUOGU_EXTRACT_ALLOW_PRIVATE_IMAGE_HOST=1 可关闭该检查）");
        curl_easy_cleanup(curl);
        std::fclose(out_file); // 先关闭再删（Windows 上占用中的文件删不掉）
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        return DOWNLOAD_FAIL;
    }
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    std::fclose(out_file);

    if (file_response.overflow)
    {
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        print_error("下载 " + url + " 失败：文件过大");
        return DOWNLOAD_FAIL;
    }
    if (res != CURLE_OK)
    {
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        print_error("下载 " + url + " 失败：libcurl 报告错误 " + curl_easy_strerror(res));
        return DOWNLOAD_FAIL;
    }
    if (http_code != 200)
    {
        std::error_code ec;
        std::filesystem::remove(fpath, ec);
        print_error("下载 " + url + " 失败：HTTP 状态码 " + std::to_string(http_code));
        return HTTP_ERROR;
    }

    return SUCCESS;
}

crawler::derror crawler::update_tags()
{
    std::filesystem::path cache_dir = crawler::get_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec)
    {
        print_error("无法创建缓存目录 '" +
                    luogu::compat::path_to_utf8(cache_dir) + "'：" + ec.message());
        return ENV_ERROR;
    }

    // 官方标签接口（题目列表页中通过 __luoguTagRequest 暴露）
    const std::string url = "https://www.luogu.com.cn/_lfe/tags/zh-CN";
    derror fetch_error = SUCCESS;
    printf("正在下载标签缓存：");
    std::string body = get_html(url, &fetch_error);
    printf("\n");

    if (fetch_error != SUCCESS)
    {
        print_error("标签缓存更新失败（下载失败）");
        return fetch_error;
    }

    try
    {
        json data = json::parse(body);
        if (!data.contains("tags") || !data["tags"].is_array())
        {
            print_error("标签缓存更新失败（接口返回的数据格式不符合预期）");
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
            print_error("标签缓存更新失败（没有解析出任何有效标签）");
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
            print_error("无法打开文件 '" + luogu::compat::path_to_utf8(tmp_path) +
                        "'（写入）");
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
            print_error("写入文件 '" + luogu::compat::path_to_utf8(save_path) + "' 失败");
            return CANT_CREAT_FILE;
        }
        // 原子替换必须用 compat::atomic_replace：Windows 下
        // std::filesystem::rename 在目标已存在时会失败
        std::string replace_error;
        if (!luogu::compat::atomic_replace(tmp_path, save_path, replace_error))
        {
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
            print_error("写入文件 '" + luogu::compat::path_to_utf8(save_path) +
                        "'：" + replace_error);
            return CANT_CREAT_FILE;
        }
    }
    catch (const std::exception &e)
    {
        print_error(std::string("解析标签数据失败：") + e.what());
        return EMPTY_RESPONSE;
    }

    print_success("标签缓存更新成功");
    return SUCCESS;
}

crawler::derror crawler::download_images(const std::vector<std::string> &urls,
                                         bool redownload)
{
    std::filesystem::path cache_dir = crawler::get_cache_dir();
    std::filesystem::path image_dir = cache_dir / "images";
    std::error_code ec;
    std::filesystem::create_directories(image_dir, ec);
    if (ec)
    {
        print_error("无法创建图片缓存目录 '" +
                    luogu::compat::path_to_utf8(image_dir) + "'：" + ec.message());
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
    printf("正在下载图片：");

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
            // 回到行首并清除到行尾，重写完整前缀 + 进度。
            // 与 print_error/print_warning 共用 print_mutex：否则 worker 的错误
            // 信息可能与进度行互相插行（同一行里混进两种输出）
            {
                std::lock_guard<std::mutex> lock(print_mutex());
                printf("\r正在下载图片：%3d %% (%d/%d).\033[K", cur, d, total);
                fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        // 结束前再做一次最终输出并换行
        int d = downloaded.load();
        int cur = total > 0 ? static_cast<int>(std::floor((static_cast<double>(d) * 100.0) / static_cast<double>(total) + 0.5)) : 100;
        {
            std::lock_guard<std::mutex> lock(print_mutex());
            printf("\r正在下载图片：%3d %% (%d/%d)，完成。\033[K\n", cur, d, total);
            fflush(stdout);
        }
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
                if (!redownload)
                {
                    // 未要求重新下载：缓存中已有该图片时直接跳过
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
                }

                // --new-download（redownload = true）时不使用缓存中已有的图片：
                // 先下载到同目录的临时文件，校验通过后再原子替换缓存中的同名
                // 文件（rename 覆盖），因此下载失败/内容无效只影响临时文件，
                // 原有缓存图片保持不变
                const std::filesystem::path temp_path =
                    redownload ? luogu::compat::temp_sibling_path(save_path)
                               : std::filesystem::path();
                const std::filesystem::path target_path =
                    redownload ? temp_path : save_path;

                auto download_one = [&] {
                    // 图片下载不显示进度条（可自定义回调）
                    const derror result = downloadFile(url, target_path,
                                                       [](const std::string &, long long, long long) {});
                    if (result != SUCCESS)
                    {
                        if (redownload)
                        {
                            std::error_code rm_ec;
                            std::filesystem::remove(temp_path, rm_ec);
                        }
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (first_error == SUCCESS)
                            first_error = result;
                        return;
                    }

                    // 校验下载内容确实是图片；无效内容（如错误页）删除并视为失败
                    if (!looks_like_image_file(target_path))
                    {
                        std::error_code rm_ec;
                        std::filesystem::remove(target_path, rm_ec);
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (first_error == SUCCESS)
                            first_error = DOWNLOAD_FAIL;
                        return;
                    }

                    if (redownload)
                    {
                        // 原子替换缓存中的同名图片（同名旧图片在替换前一直可用）。
                        // 必须用 compat::atomic_replace：Windows 下
                        // std::filesystem::rename 在目标已存在时会失败
                        std::string replace_error;
                        if (!luogu::compat::atomic_replace(temp_path, save_path,
                                                           replace_error))
                        {
                            std::error_code rm_ec;
                            std::filesystem::remove(temp_path, rm_ec);
                            std::lock_guard<std::mutex> lock(error_mutex);
                            if (first_error == SUCCESS)
                                first_error = CANT_CREAT_FILE;
                            return;
                        }
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
        // 重新下载时所有图片都算“需要下载”，此时没有下载成功意味着全部失败，
        // 报“没有需要下载的图片”会误导（未重新下载时该提示是准确的）
        if (redownload && first_error != SUCCESS)
            print_error("图片重新下载失败");
        else
            print_error("没有需要下载的图片");
        return first_error != SUCCESS ? first_error : EMPTY_RESPONSE;
    }
    return first_error;
}

std::filesystem::path crawler::image_cache_path(const std::string &url)
{
    return crawler::get_cache_dir() / "images" / image_cache_filename(url);
}

crawler::derror crawler::clean_all()
{
    const std::filesystem::path cache_dir = crawler::get_cache_dir();
    std::uintmax_t removed = 0;
    std::string error;
    if (!remove_cache_entry(cache_dir, removed, error))
    {
        print_error("清空缓存目录 '" + luogu::compat::path_to_utf8(cache_dir) +
                    "' 失败：" + error);
        return CANT_REMOVE_FILE;
    }
    if (removed == 0)
        print_success("缓存目录 '" + luogu::compat::path_to_utf8(cache_dir) +
                      "' 不存在，无需清理");
    else
        print_success("已清空缓存目录 '" + luogu::compat::path_to_utf8(cache_dir) +
                      "'（含题目列表、标签、图片与字体缓存）");
    return SUCCESS;
}

crawler::derror crawler::clean_images()
{
    const std::filesystem::path image_dir = crawler::get_cache_dir() / "images";
    std::uintmax_t removed = 0;
    std::string error;
    if (!remove_cache_entry(image_dir, removed, error))
    {
        print_error("清空图片缓存目录 '" + luogu::compat::path_to_utf8(image_dir) +
                    "' 失败：" + error);
        return CANT_REMOVE_FILE;
    }
    if (removed == 0)
        print_success("图片缓存目录 '" + luogu::compat::path_to_utf8(image_dir) +
                      "' 不存在，无需清理");
    else
        print_success("已清空图片缓存目录 '" + luogu::compat::path_to_utf8(image_dir) +
                      "'");
    return SUCCESS;
}

crawler::derror crawler::clean_fonts()
{
    const std::filesystem::path font_dir = crawler::get_cache_dir() / "fonts";
    std::uintmax_t removed = 0;
    std::string error;
    if (!remove_cache_entry(font_dir, removed, error))
    {
        print_error("清空字体缓存目录 '" + luogu::compat::path_to_utf8(font_dir) +
                    "' 失败：" + error);
        return CANT_REMOVE_FILE;
    }
    if (removed == 0)
        print_success("字体缓存目录 '" + luogu::compat::path_to_utf8(font_dir) +
                      "' 不存在，无需清理");
    else
        print_success("已清空字体缓存目录 '" + luogu::compat::path_to_utf8(font_dir) +
                      "'");
    return SUCCESS;
}

crawler::derror crawler::clean_problems()
{
    const std::filesystem::path cache_dir = crawler::get_cache_dir();
    std::vector<std::filesystem::path> targets = {
        cache_dir / "latest.ndjson",
        cache_dir / "latest.ndjson.gz",
    };

    // 更新中断时可能残留 latest.ndjson.tmp.* 临时文件，一并清除
    std::error_code dir_ec;
    if (std::filesystem::is_directory(cache_dir, dir_ec) && !dir_ec)
    {
        std::error_code iter_ec;
        for (std::filesystem::directory_iterator it(cache_dir, iter_ec), end;
             !iter_ec && it != end; it.increment(iter_ec))
        {
            const std::string name =
                luogu::compat::path_to_utf8(it->path().filename());
            if (name.rfind("latest.ndjson.tmp.", 0) == 0)
                targets.push_back(it->path());
        }
        if (iter_ec)
        {
            print_error("读取缓存目录 '" + luogu::compat::path_to_utf8(cache_dir) +
                        "' 失败：" + iter_ec.message());
            return CANT_REMOVE_FILE;
        }
    }

    std::vector<std::filesystem::path> removed_paths;
    for (const auto &target : targets)
    {
        std::uintmax_t removed = 0;
        std::string error;
        if (!remove_cache_entry(target, removed, error))
        {
            print_error("清除题目列表缓存 '" + luogu::compat::path_to_utf8(target) +
                        "' 失败：" + error);
            return CANT_REMOVE_FILE;
        }
        if (removed > 0)
            removed_paths.push_back(target);
    }

    if (removed_paths.empty())
        print_success("题目列表缓存（latest.ndjson / latest.ndjson.gz）不存在，无需清理");
    else
        print_success("已清除题目列表缓存：" + join_quoted_paths(removed_paths));
    return SUCCESS;
}

crawler::derror crawler::update()
{
    std::filesystem::path cache_dir = crawler::get_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec)
    {
        print_error("无法创建缓存目录 '" +
                    luogu::compat::path_to_utf8(cache_dir) + "'：" + ec.message());
        return ENV_ERROR;
    }

    std::string url = "https://cdn.luogu.com.cn/problemset-open/latest.ndjson.gz";
    std::filesystem::path save_path = cache_dir / "latest.ndjson.gz";
    std::filesystem::path extract_path = cache_dir / "latest.ndjson";
    printf("正在下载题目列表：");
    // 进度在行首整行重写（见 print_progress_line），保留「题目列表」前缀；
    // downloadFile 的默认进度不带该前缀，故这里自行提供回调
    derror result = downloadFile(url, save_path,
                                 [](const std::string &, long long downloaded,
                                    long long total) {
                                     print_progress_line("正在下载题目列表：", downloaded,
                                                         total);
                                 });
    printf("\n");

    if (result != SUCCESS)
    {
        print_error("题目列表缓存更新失败（下载失败）");
        return result;
    }

    // 解压到临时文件，成功后 fsync + 原子替换 latest.ndjson：
    // 解压中断/磁盘满不会破坏已有可用缓存
    const std::filesystem::path tmp_extract =
        luogu::compat::temp_sibling_path(extract_path);
    if (!decompress_gzip_file(save_path, tmp_extract))
    {
        std::filesystem::remove(tmp_extract, ec);
        print_error("解压下载的文件 '" +
                    luogu::compat::path_to_utf8(save_path) + "'");
        return DECOMPRESS_ERROR;
    }
    // 原子替换必须用 compat::atomic_replace：Windows 下
    // std::filesystem::rename 在目标已存在时会失败
    std::string replace_error;
    if (!luogu::compat::atomic_replace(tmp_extract, extract_path, replace_error))
    {
        std::filesystem::remove(tmp_extract, ec);
        print_error("写入文件 '" + luogu::compat::path_to_utf8(extract_path) +
                    "'：" + replace_error);
        return CANT_CREAT_FILE;
    }

    print_success("题目列表缓存更新成功");

    // 题目列表更新成功后，顺带更新标签缓存（保存为 tags.json）
    return crawler::update_tags();
}
