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

// src/util/tls.cpp
#include "luogu-extract/util/tls.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace
{
bool is_regular_file(const char *path)
{
    if (!path || !*path)
        return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool is_directory(const char *path)
{
    if (!path || !*path)
        return false;
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

// 取非空环境变量；未设置或为空串时返回 nullptr（空串按「未设置」处理）
const char *env_value(const char *name)
{
    const char *value = std::getenv(name);
    return (value && *value) ? value : nullptr;
}

// SSL_CERT_DIR / CURL_CA_PATH 语义上可以是冒号分隔的目录列表：只要有一段存在就把整串交给
// OpenSSL（列表本身由 OpenSSL 解析），避免把 "a:b" 整个当成路径而误判为不存在
const char *env_dir_value(const char *name)
{
    const char *value = env_value(name);
    if (!value)
        return nullptr;
    const std::string list(value);
    size_t pos = 0;
    while (pos <= list.size())
    {
        const size_t sep = list.find(':', pos);
        const size_t end = (sep == std::string::npos) ? list.size() : sep;
        if (is_directory(list.substr(pos, end - pos).c_str()))
            return value;
        if (sep == std::string::npos)
            break;
        pos = sep + 1;
    }
    return nullptr;
}

// CA 包文件：前两个是用户显式指定（SSL_CERT_FILE 由 OpenSSL 读取；CURL_CA_BUNDLE 新版 libcurl
// 已不再读环境变量，这里按既有约定沿用），其余是各系统默认位置
const char *const kBundleEnvVars[] = {"SSL_CERT_FILE", "CURL_CA_BUNDLE"};
const char *const kBundleCandidates[] = {
    "/etc/ssl/certs/ca-certificates.crt",               // Debian / Ubuntu / Arch / Alpine / Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",                 // Fedora / RHEL / CentOS
    "/etc/ssl/ca-bundle.pem",                           // openSUSE
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS / RHEL 7
    "/etc/ssl/cert.pem",                                // macOS / FreeBSD / Alpine
    "/usr/local/share/certs/ca-root-nss.crt",           // FreeBSD ca_root_nss / Homebrew
    "/opt/homebrew/etc/ca-certificates/cert.pem",       // macOS(arm64) Homebrew
};

// CA 目录（OpenSSL 哈希目录）；仅在没有可用包文件时兜底
const char *const kDirEnvVars[] = {"SSL_CERT_DIR", "CURL_CA_PATH"};
const char *const kDirCandidates[] = {
    "/etc/ssl/certs",
    "/etc/pki/tls/certs",
    "/usr/local/share/certs",
};

std::string detect_bundle_file()
{
    for (const char *name : kBundleEnvVars)
        if (is_regular_file(env_value(name)))
            return env_value(name);
    for (const char *path : kBundleCandidates)
        if (is_regular_file(path))
            return path;
    return std::string();
}

std::string detect_bundle_dir()
{
    for (const char *name : kDirEnvVars)
        if (env_dir_value(name))
            return env_dir_value(name);
    for (const char *path : kDirCandidates)
        if (is_directory(path))
            return path;
    return std::string();
}

bool ends_with(const std::string &s, const char *suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool is_hashed_cert_name(const std::string &name)
{
    if (name.size() != 10 || name.compare(8, 2, ".0") != 0)
        return false;
    for (size_t i = 0; i < 8; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(name[i])))
            return false;
    return true;
}

// 目录兜底用：目录里挑一个证书文件。curl 先加载 CAINFO 再加载 CAPATH，而 CAINFO 的默认值
// 是编译期写死的构建机路径，加载失败会直接报错，所以只设 CAPATH 是不够的，必须让 CAINFO
// 指向一个能加载成功的文件（真正的信任锚仍由 CAPATH 提供）。优先哈希证书（XXXXXXXX.0）
std::string first_cert_file_in_dir(const std::string &dir)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec)
        return std::string();
    std::string hashed;
    std::string pem;
    std::string crt;
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec))
    {
        if (ec)
            break;
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec) || type_ec)
            continue;
        const std::string name = it->path().filename().string();
        if (hashed.empty() && is_hashed_cert_name(name))
            hashed = it->path().string();
        else if (pem.empty() && ends_with(name, ".pem"))
            pem = it->path().string();
        else if (crt.empty() && ends_with(name, ".crt"))
            crt = it->path().string();
        if (!hashed.empty() && !pem.empty() && !crt.empty())
            break;
    }
    if (!hashed.empty())
        return hashed;
    if (!pem.empty())
        return pem;
    return crt;
}

// 只探测一次（结果与进程生命周期一致；函数内静态量在 C++11 起初始化线程安全）
bool env_dir_is_set()
{
    for (const char *name : kDirEnvVars)
        if (env_dir_value(name))
            return true;
    return false;
}
} // namespace

const std::string &luogu::tls::ca_bundle_file()
{
#ifdef _WIN32
    // Schannel 用 Windows 证书存储，没有 CA 包文件的概念
    static const std::string kEmpty;
    return kEmpty;
#else
    static const std::string kPath = detect_bundle_file();
    return kPath;
#endif
}

const std::string &luogu::tls::ca_bundle_dir()
{
#ifdef _WIN32
    static const std::string kEmpty;
    return kEmpty;
#else
    static const std::string kPath = detect_bundle_dir();
    return kPath;
#endif
}

void luogu::tls::apply_ca_bundle(CURL *curl)
{
    if (!curl)
        return;
#ifdef _WIN32
    (void)curl; // Schannel 忽略 CURLOPT_CAINFO / CURLOPT_CAPATH
#else
    const std::string &file = ca_bundle_file();
    const std::string &dir = ca_bundle_dir();
    if (!file.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, file.c_str());
    else if (!dir.empty())
    {
        // 只有目录（如某些容器镜像）时：CAINFO 指向目录内的一个证书文件，避免 curl 先按
        // 编译期写死的路径加载失败，信任锚由下面的 CAPATH 提供
        const std::string cert = first_cert_file_in_dir(dir);
        if (!cert.empty())
            curl_easy_setopt(curl, CURLOPT_CAINFO, cert.c_str());
    }
    // 用户通过环境变量显式给了目录时一并设置；否则只在没有包文件时用目录兜底
    if ((env_dir_is_set() || file.empty()) && !dir.empty())
        curl_easy_setopt(curl, CURLOPT_CAPATH, dir.c_str());
#endif
}
