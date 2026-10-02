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

// include/luogu-extract/crawler/request_gate.h
// 请求闸门：题解相关请求的串行节流、限流检测与暂停重试、凭据通道。
// 受控：题解列表（含分页）与题解正文；不受控：题目列表、标签接口、题面页面、
// 图片下载与本地操作（图片另有洛谷图床串行 + 0.5~3 秒随机间隔）。
// 限流只做等待或停止，不换域名硬冲、不降延时重试、不绕风控。
#ifndef LUOGU_EXTRACT_CRAWLER_REQUEST_GATE_H
#define LUOGU_EXTRACT_CRAWLER_REQUEST_GATE_H

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

namespace crawler
{
    /// 请求类别：只有题解相关请求受 --request-delay 控制
    enum class RequestClass
    {
        SolutionList,    // 题解列表接口（含分页）
        SolutionArticle, // 题解正文
        Image,           // 图片下载（本闸门不干预）
        Other,           // 其它（本闸门不干预）
    };

    /// 端点配置。可用环境变量 LUOGU_EXTRACT_BASE_OFFICIAL /
    /// LUOGU_EXTRACT_BASE_SAVE 指向本地 mock 服务器（测试用，帮助文本不列出）
    struct Endpoints
    {
        std::string official_base = "https://www.luogu.com.cn";
        std::string save_base = "https://api.luogu.me";
    };
    const Endpoints &endpoints();

    /// --request-delay 解析结果（毫秒）
    struct DelaySpec
    {
        long min_ms = 3500;
        long max_ms = 6500;
    };

    /// 解析 --request-delay："5"（均值，±30% 抖动）、"2.5"（小数）、
    /// "8-15"（闭区间）。要求正数、0 < min ≤ max、单项上限 300 秒
    bool parse_delay_spec(const std::string &spec, DelaySpec &out, std::string &error);

    /// 延时自动递增系数：按本次运行的实际请求数 Np 定档
    double auto_scale_for(long long planned_requests);

    /// 闸门配置
    struct GateConfig
    {
        DelaySpec delay;                  // 基础延时区间
        bool auto_scale = true;           // --no-delay-auto-scale 置 false
        int rate_limit_wait_sec = 120;    // --rate-limit-wait（0 = 检测到限流直接停止）
        int max_rate_limit_rounds = 2;    // 一次运行内最多触发的等待轮数
        long timeout_sec = 60;            // 单次请求超时
        // 命中限流时是否在闸门内交互式等待并重试：true（单来源）等待后重试；
        // false（auto 并行）立即返回被限流，由调度器把该站点下线并转给另一站点
        bool interactive_retry = true;
    };

    /// 抓取通道：原站与保存站各自维护延时、限流与放弃状态，
    /// auto 模式下两条通道并行工作，互不影响
    enum class Channel
    {
        Official = 0,
        Save = 1,
    };
    const int kChannelCount = 2;

    /// 计划阶段配置延时并一次算定自动递增系数，运行期保持恒定（可复现、可打印）。
    /// 单参数版本配置原站通道（单来源模式下的唯一通道）
    void gate_configure(const GateConfig &cfg, long long planned_requests);
    void gate_configure_channel(Channel ch, const GateConfig &cfg,
                                long long planned_requests);

    /// 拿到更精确的请求数后重算自动递增系数（不清空限流等待轮数与已生效的放大）
    void gate_set_planned_requests(long long planned_requests);
    void gate_set_planned_requests(Channel ch, long long planned_requests);

    /// 生效延时区间（毫秒）与均值（秒），含自动递增系数与限流后的额外放大
    std::pair<long, long> gate_effective_delay_ms(Channel ch = Channel::Official);
    double gate_effective_delay_seconds(Channel ch = Channel::Official);
    double gate_scale(Channel ch = Channel::Official);  // 含限流后的 1.5 倍放大
    bool gate_scale_boosted();  // 本次运行是否已因限流放大过

    // 限流记账（auto 模式调度器使用）

    /// 一次「疑似限流」的结果
    struct ChannelLimitInfo
    {
        bool abandoned = false;  // 连续限流达 3 次，本通道已放弃
        long wait_ms = 0;        // 本通道需要等待多久（0 表示不必等待）
        std::string reason;
    };

    /// 记录一次限流：连续计数 +1，达 3 次即放弃该通道；
    /// 否则设置该通道的等待截止时间（等待期间任务临时转给另一通道）
    ChannelLimitInfo gate_note_rate_limit(Channel ch, const std::string &reason);

    /// 记录一次成功（连续限流计数清零）
    void gate_note_success(Channel ch);

    /// 主动放弃某个通道（连续 3 次限流，或用户选择）
    void gate_abandon_channel(Channel ch);
    bool gate_channel_abandoned(Channel ch);

    /// 该通道还要等多久才能再次使用（毫秒；0 表示现在可用）
    long long gate_channel_block_remaining_ms(Channel ch);

    /// 最近一次限流原因（返回拷贝：内部锁已释放，不能把内部引用交出去）
    std::string gate_channel_limit_reason(Channel ch);

    /// 复位某个通道的限流状态（不影响 Cookie 与延时配置）
    void gate_reset_channel(Channel ch);

    /// 是否两个通道都被放弃（auto 模式下应当终止下载）
    bool gate_all_channels_abandoned();

    /// 打印一行「延时自动提升」说明（未提升时打印基础区间）
    void gate_print_delay_notice(long long planned_requests);

    /// 按请求类别等待间隔（同一通道内串行）。等待可被用户打断：
    /// 按 S 立即停止（返回 false），按 C 跳过剩余等待
    bool gate_wait(RequestClass cls, Channel ch = Channel::Official);

    // 凭据通道

    /// 载入 Netscape 格式 cookies.txt
    bool gate_load_cookies(const std::filesystem::path &file, std::string &error,
                           std::string *warnings = nullptr);
    /// 载入 --cookie-string 的 "k=v; k2=v2"
    bool gate_set_cookie_string(const std::string &text, std::string &error);
    size_t gate_cookie_count();
    bool gate_has_cookies();
    /// 清空内存 jar、提示用文件名，以及两个请求句柄里 libcurl 已装入的 Cookie
    /// （引擎保持启用，可继续收下发 Cookie）
    void gate_clear_cookies();

    /// Cookie 文件路径（仅用于错误提示，绝不打印内容）
    std::string gate_cookie_file_hint();

    // 请求执行

    enum class RequestStatus
    {
        Ok,            // 200 且有内容
        NotModified,   // 304（ETag 条件请求命中）
        NeedLogin,     // 401 / 未登录错误模板
        NotFound,      // 404
        Forbidden,     // 403（疑似风控，已按限流流程处理仍失败）
        RateLimited,   // 限流且已用尽等待轮数（中止本次抓取）
        HttpError,     // 其它非 200 状态码
        NetworkError,  // 连接失败 / 超时 / 传输错误
        TooLarge,      // 响应超过体积上限
        Stopped,       // 用户主动停止（按 S）
    };

    struct RequestOptions
    {
        std::string url;
        bool send_cookie = false;        // 仅洛谷原站请求置 true
        bool json_content_only = false;  // 加 x-lentille-request: content-only
        std::string if_none_match;       // ETag 条件请求
        long timeout_sec = 60;
        size_t max_bytes = 4 * 1024 * 1024;
    };

    struct RequestResult
    {
        RequestStatus status = RequestStatus::NetworkError;
        long http_code = 0;
        std::string body;
        std::string etag;
        std::string retry_after;
        std::string error;   // 中文错误说明（不含 Cookie 内容）
        int attempts = 0;    // 实际发出的请求次数（含重试）
    };

    /// 经请求闸门发起一次 GET：延时等待（可中断）→ 请求 → 限流判定 →
    /// 命中则暂停并重试一次（interactive_retry=false 时直接返回 RateLimited）
    RequestResult http_get(const RequestOptions &opt, RequestClass cls,
                           Channel ch = Channel::Official);

    /// 用户是否在等待期间按 S 主动停止（退出码 0）
    bool gate_user_stopped();

    /// 请求停止（调度器判定必须终止，或某个工作线程收到 S 键）
    void gate_request_stop(const std::string &reason = "");
    /// 是否因限流而中止（退出码非 0）
    bool gate_rate_limited_out();
    /// 最近一次限流原因（返回拷贝，同 gate_channel_limit_reason）
    std::string gate_rate_limit_reason();

    /// 清空本次运行的停止/限流状态与各通道的限流计数、放弃标志、限流放大
    /// （不影响 Cookie 与延时配置）。交互模式多次执行 app::run 时应在入口调用
    void gate_reset_state();
} // namespace crawler

#endif // LUOGU_EXTRACT_CRAWLER_REQUEST_GATE_H
