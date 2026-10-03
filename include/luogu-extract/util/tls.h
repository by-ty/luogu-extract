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

// include/luogu-extract/util/tls.h
// TLS 信任库定位：curl 在编译期会把构建机上的 CA 包路径写死成 CURL_CA_BUNDLE
// （如 ubuntu runner 上的 /etc/ssl/certs/ca-certificates.crt）。该路径在用户机器上
// 不存在时 curl 不会回退到系统证书库，而是直接以 CURLE_SSL_CACERT_BADFILE 失败
// （Fedora / RHEL 用 /etc/pki/tls/certs/ca-bundle.crt，macOS 用 /etc/ssl/cert.pem）。
// 这里在运行期按「环境变量 → 各系统常见路径」重新探测，并显式设置给 curl。
#ifndef LUOGU_EXTRACT_UTIL_TLS_H
#define LUOGU_EXTRACT_UTIL_TLS_H

#include <string>
#include <curl/curl.h>

namespace luogu
{
namespace tls
{
    // 选中的 CA 包（PEM 文件）路径，找不到返回空串；只认「存在且为普通文件」的路径。
    // 顺序：SSL_CERT_FILE、CURL_CA_BUNDLE 环境变量，然后各发行版/系统常见路径
    const std::string &ca_bundle_file();

    // 选中的 CA 目录（OpenSSL 哈希目录）路径，找不到返回空串。
    // 顺序：SSL_CERT_DIR、CURL_CA_PATH 环境变量，然后各发行版常见目录
    const std::string &ca_bundle_dir();

    // 在 curl 句柄上显式设置 CAINFO（找到包）或 CAPATH（只有目录）；都没找到则保持 curl
    // 默认行为。Windows 用系统证书库（Schannel），不设置。幂等，可对同一句柄重复调用
    void apply_ca_bundle(CURL *curl);
} // namespace tls
} // namespace luogu

#endif
