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

// src/util/text_encoding.cpp
#include "luogu-extract/util/text_encoding.h"

#include <cstdio>
#include <string>
#include <vector>
// libxml2 自带 UTF-8 / UTF-16 / ISO-8859-1 等编码处理器，其余编码（如 GB18030）
// 走 iconv；libxml2 已是本项目的依赖，用它转码可同时覆盖 Windows / macOS / Linux
#include <libxml/encoding.h>
#include <libxml/tree.h>
#include "luogu-extract/util/compat.h"

namespace
{
// 单个本地文本文件的大小上限（64 MB）：超过时拒绝，避免一次性读入过多内存
const size_t kMaxTextBytes = 64u * 1024u * 1024u;

// 严格校验是否为合法 UTF-8（拒绝过长编码、代理区码点与越界码点）
bool valid_utf8(const std::string &s)
{
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        unsigned int cp = 0;
        if (c < 0x80)
        {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0)
        {
            len = 2;
            cp = c & 0x1Fu;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            len = 3;
            cp = c & 0x0Fu;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            len = 4;
            cp = c & 0x07u;
        }
        else
        {
            return false;
        }
        if (i + len > s.size())
            return false;
        for (size_t k = 1; k < len; ++k)
        {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // 过长编码 / 代理区 / 超出 Unicode 范围
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

void append_utf8(unsigned int cp, std::string &out)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD; // 非法码点一律替换成 U+FFFD
    if (cp < 0x80)
    {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// UTF-16（含代理对）→ UTF-8；奇数个字节时最后一个字节忽略
std::string utf16_to_utf8(const std::string &bytes, bool big_endian)
{
    std::string out;
    const size_t units = bytes.size() / 2;
    for (size_t i = 0; i < units; ++i)
    {
        const unsigned char a = static_cast<unsigned char>(bytes[i * 2]);
        const unsigned char b = static_cast<unsigned char>(bytes[i * 2 + 1]);
        unsigned int unit = big_endian ? ((static_cast<unsigned int>(a) << 8) | b)
                                       : ((static_cast<unsigned int>(b) << 8) | a);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units)
        {
            const unsigned char c = static_cast<unsigned char>(bytes[(i + 1) * 2]);
            const unsigned char d = static_cast<unsigned char>(bytes[(i + 1) * 2 + 1]);
            const unsigned int low =
                big_endian ? ((static_cast<unsigned int>(c) << 8) | d)
                           : ((static_cast<unsigned int>(d) << 8) | c);
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                append_utf8(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00), out);
                ++i;
                continue;
            }
        }
        append_utf8(unit, out);
    }
    return out;
}

// UTF-32 → UTF-8
std::string utf32_to_utf8(const std::string &bytes, bool big_endian)
{
    std::string out;
    const size_t units = bytes.size() / 4;
    for (size_t i = 0; i < units; ++i)
    {
        const unsigned char *p =
            reinterpret_cast<const unsigned char *>(bytes.data()) + i * 4;
        unsigned int cp = 0;
        if (big_endian)
            cp = (static_cast<unsigned int>(p[0]) << 24) |
                 (static_cast<unsigned int>(p[1]) << 16) |
                 (static_cast<unsigned int>(p[2]) << 8) | p[3];
        else
            cp = (static_cast<unsigned int>(p[3]) << 24) |
                 (static_cast<unsigned int>(p[2]) << 16) |
                 (static_cast<unsigned int>(p[1]) << 8) | p[0];
        append_utf8(cp, out);
    }
    return out;
}

// 按指定编码名转成 UTF-8（libxml2 编码处理器；GB 系列走 iconv）
bool transcode_to_utf8(const std::string &bytes, const char *encoding,
                       std::string &out)
{
    xmlCharEncodingHandlerPtr handler = xmlFindCharEncodingHandler(encoding);
    if (!handler)
        return false;

    xmlBufferPtr in = xmlBufferCreate();
    xmlBufferPtr buffer = xmlBufferCreate();
    if (!in || !buffer)
    {
        if (in)
            xmlBufferFree(in);
        if (buffer)
            xmlBufferFree(buffer);
        return false;
    }

    bool ok = xmlBufferAdd(in, reinterpret_cast<const xmlChar *>(bytes.data()),
                           static_cast<int>(bytes.size())) == 0;
    size_t last_remaining = static_cast<size_t>(-1);
    while (ok)
    {
        const size_t remaining = xmlBufferLength(in);
        if (remaining == 0)
            break;
        if (remaining == last_remaining)
        {
            ok = false; // 没有进展：编码无法继续转换（多半是非法字节序列）
            break;
        }
        last_remaining = remaining;
        const int written = xmlCharEncInFunc(handler, buffer, in);
        if (written < 0)
        {
            ok = false;
            break;
        }
    }

    if (ok)
        out.assign(reinterpret_cast<const char *>(xmlBufferContent(buffer)),
                   xmlBufferLength(buffer));

    xmlBufferFree(in);
    xmlBufferFree(buffer);
    xmlCharEncCloseFunc(handler);
    return ok;
}

// CRLF / CR 归一化为 LF；过滤 C0 控制字符（保留 \t 与 \n）
void normalize_text(std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\r')
        {
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n')
                ++i; // CRLF 只算一次换行
            continue;
        }
        if (c < 0x20 && c != '\t' && c != '\n')
            continue; // 其余 C0 控制字符丢弃
        if (c == 0x7F)
            continue; // DEL
        out += static_cast<char>(c);
    }
    s.swap(out);
}
} // namespace

bool textenc::read_text_file_utf8(const std::filesystem::path &path,
                                  std::string &out, std::string &error)
{
    out.clear();
    error.clear();

    const std::string shown = luogu::compat::path_to_utf8(path);
    FILE *file = luogu::compat::fopen(path, "rb");
    if (!file)
    {
        error = "无法打开本地文件 '" + shown + "'；请确认文件存在且可读";
        return false;
    }

    std::string bytes;
    char buffer[65536];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        bytes.append(buffer, n);
        if (bytes.size() > kMaxTextBytes)
        {
            std::fclose(file);
            error = "本地文件 '" + shown + "' 超过 64 MB，暂不支持转写";
            return false;
        }
    }
    const bool read_ok = !std::ferror(file);
    std::fclose(file);
    if (!read_ok)
    {
        error = "读取本地文件 '" + shown + "' 失败";
        return false;
    }
    if (bytes.empty())
    {
        error = "本地文件 '" + shown + "' 是空文件，没有可转写的内容";
        return false;
    }

    const unsigned char *head = reinterpret_cast<const unsigned char *>(bytes.data());
    const size_t total = bytes.size();
    bool decoded = false;

    // ---- 1. 按 BOM 判定（UTF-32 的 BOM 是 UTF-16 BOM 的前缀，必须先判 UTF-32）----
    if (total >= 4 && head[0] == 0xFF && head[1] == 0xFE && head[2] == 0x00 &&
        head[3] == 0x00)
    {
        out = utf32_to_utf8(bytes.substr(4), false);
        decoded = true;
    }
    else if (total >= 4 && head[0] == 0x00 && head[1] == 0x00 && head[2] == 0xFE &&
             head[3] == 0xFF)
    {
        out = utf32_to_utf8(bytes.substr(4), true);
        decoded = true;
    }
    else if (total >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)
    {
        out = bytes.substr(3); // UTF-8 BOM
        decoded = true;
    }
    else if (total >= 2 && head[0] == 0xFF && head[1] == 0xFE)
    {
        out = utf16_to_utf8(bytes.substr(2), false);
        decoded = true;
    }
    else if (total >= 2 && head[0] == 0xFE && head[1] == 0xFF)
    {
        out = utf16_to_utf8(bytes.substr(2), true);
        decoded = true;
    }

    // ---- 2. 无 BOM：合法 UTF-8 直接用；否则按 GB18030（GBK / GB2312）转码 ----
    if (!decoded)
    {
        if (valid_utf8(bytes))
        {
            out = bytes;
            decoded = true;
        }
        else
        {
            static const char *kChineseEncodings[] = {"GB18030", "GBK", "GB2312"};
            for (const char *encoding : kChineseEncodings)
            {
                if (transcode_to_utf8(bytes, encoding, out))
                {
                    decoded = true;
                    break;
                }
            }
            if (!decoded)
            {
                error = "本地文件 '" + shown +
                        "' 不是 UTF-8 编码，也无法按 GB18030 / GBK / GB2312 转码"
                        "（字节序列非法，或当前环境缺少相应的转码支持）；"
                        "请把文件另存为 UTF-8 后重试";
                return false;
            }
        }
    }

    // 归一化换行并过滤控制字符后重新确认仍有内容（例如整份文件都是控制字符）
    normalize_text(out);
    if (out.empty())
    {
        error = "本地文件 '" + shown + "' 转码后没有可用的文本内容";
        return false;
    }
    return true;
}
