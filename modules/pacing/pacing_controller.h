/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_PACING_PACING_CONTROLLER_H_
#define MODULES_PACING_PACING_CONTROLLER_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <memory>
#include <vector>

#include "absl/types/optional.h"
#include "api/function_view.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/transport/field_trial_based_config.h"
#include "api/transport/network_types.h"
#include "api/transport/webrtc_key_value_config.h"
#include "modules/pacing/bitrate_prober.h"
#include "modules/pacing/interval_budget.h"
#include "modules/pacing/round_robin_packet_queue.h"
#include "modules/pacing/rtp_packet_pacer.h"
#include "modules/rtp_rtcp/include/rtp_packet_sender.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "rtc_base/experiments/field_trial_parser.h"
#include "rtc_base/thread_annotations.h"

namespace webrtc {

// This class implements a leaky-bucket packet pacing algorithm. It handles the
// logic of determining which packets to send when, but the actual timing of
// the processing is done externally (e.g. RtpPacketPacer). Furthermore, the
// forwarding of packets when they are ready to be sent is also handled
// externally, via the PacingController::PacketSender interface.
class PacingController {
 public:
  // 周期模式使用 IntervalBudget 类来追踪 bitrate 预算，并期望 ProcessPackets()
  // 以固定速率调用，例如 PacedSender 实现的每 5ms 一次。
  // 动态模式允许在调用 ProcessPackets 之间有任意时间差。
  enum class ProcessMode { kPeriodic, kDynamic };

  class PacketSender {
   public:
    virtual ~PacketSender() = default;
    virtual void SendPacket(std::unique_ptr<RtpPacketToSend> packet,
                            const PacedPacketInfo& cluster_info) = 0;
    // Should be called after each call to SendPacket().
    virtual std::vector<std::unique_ptr<RtpPacketToSend>> FetchFec() = 0;
    virtual std::vector<std::unique_ptr<RtpPacketToSend>> GeneratePadding(
        DataSize size) = 0;
  };

  // Expected max pacer delay. If ExpectedQueueTime() is higher than
  // this value, the packet producers should wait (eg drop frames rather than
  // encoding them). Bitrate sent may temporarily exceed target set by
  // UpdateBitrate() so that this limit will be upheld.
  static const TimeDelta kMaxExpectedQueueLength;
  // Pacing-rate relative to our target send rate.
  // Multiplicative factor that is applied to the target bitrate to calculate
  // the number of bytes that can be transmitted per interval.
  // Increasing this factor will result in lower delays in cases of bitrate
  // overshoots from the encoder.
  static const float kDefaultPaceMultiplier;
  // If no media or paused, wake up at least every `kPausedProcessIntervalMs` in
  // order to send a keep-alive packet so we don't get stuck in a bad state due
  // to lack of feedback.
  static const TimeDelta kPausedProcessInterval;

  static const TimeDelta kMinSleepTime;

  PacingController(Clock* clock,
                   PacketSender* packet_sender,
                   RtcEventLog* event_log,
                   const WebRtcKeyValueConfig* field_trials,
                   ProcessMode mode);

  ~PacingController();

  // 将数据包添加到队列并在发送时调用 PacketRouter::SendPacket()
  void EnqueuePacket(std::unique_ptr<RtpPacketToSend> packet);

  void CreateProbeCluster(DataRate bitrate, int cluster_id);

  void Pause();   // Temporarily pause all sending.
  void Resume();  // Resume sending packets.
  bool IsPaused() const;

  void SetCongestionWindow(DataSize congestion_window_size);
  void UpdateOutstandingData(DataSize outstanding_data);

  // Sets the pacing rates. Must be called once before packets can be sent.
  void SetPacingRates(DataRate pacing_rate, DataRate padding_rate);
  DataRate pacing_rate() const { return pacing_bitrate_; }

  // Currently audio traffic is not accounted by pacer and passed through.
  // With the introduction of audio BWE audio traffic will be accounted for
  // the pacer budget calculation. The audio traffic still will be injected
  // at high priority.
  void SetAccountForAudioPackets(bool account_for_audio);
  void SetIncludeOverhead();

  void SetTransportOverhead(DataSize overhead_per_packet);

  // Returns the time when the oldest packet was queued.
  Timestamp OldestPacketEnqueueTime() const;

  // Number of packets in the pacer queue.
  size_t QueueSizePackets() const;
  // Totals size of packets in the pacer queue.
  DataSize QueueSizeData() const;

  // Current buffer level, i.e. max of media and padding debt.
  DataSize CurrentBufferLevel() const;

  // Returns the time when the first packet was sent.
  absl::optional<Timestamp> FirstSentPacketTime() const;

  // Returns the number of milliseconds it will take to send the current
  // packets in the queue, given the current size and bitrate, ignoring prio.
  TimeDelta ExpectedQueueTime() const;

  void SetQueueTimeLimit(TimeDelta limit);

  // Enable bitrate probing. Enabled by default, mostly here to simplify
  // testing. Must be called before any packets are being sent to have an
  // effect.
  void SetProbingEnabled(bool enabled);

  // 返回我们预期下次调用 ProcessPackets() 的时间
  Timestamp NextSendTime() const;

  // 如果有预算，检查待发送的数据包队列，发送它们或者填充数据包
  void ProcessPackets();

  bool Congested() const;

  bool IsProbing() const;

 private:
  void EnqueuePacketInternal(std::unique_ptr<RtpPacketToSend> packet,
                             int priority);
  // 获取距离上一次处理过了多长时间，并更新相关时间
  TimeDelta UpdateTimeAndGetElapsed(Timestamp now);
  bool ShouldSendKeepalive(Timestamp now) const;

  // 时间流逝，增加预算
  void UpdateBudgetWithElapsedTime(TimeDelta delta);
  // 发送数据，减少预算
  void UpdateBudgetWithSentData(DataSize size);

  DataSize PaddingToAdd(DataSize recommended_probe_size,
                        DataSize data_sent) const;

  // 获取准备发送的数据包
  std::unique_ptr<RtpPacketToSend> GetPendingPacket(
      const PacedPacketInfo& pacing_info,
      Timestamp target_send_time,
      Timestamp now);
  void OnPacketSent(RtpPacketMediaType packet_type,
                    DataSize packet_size,
                    Timestamp send_time);
  void OnPaddingSent(DataSize padding_sent);

  Timestamp CurrentTime() const;

  const ProcessMode mode_;
  Clock* const clock_;
  PacketSender* const packet_sender_;
  const std::unique_ptr<FieldTrialBasedConfig> fallback_field_trials_;
  const WebRtcKeyValueConfig* field_trials_;

  const bool drain_large_queues_;  // 队列比较大时，是否启用排空功能
  const bool send_padding_if_silent_;
  const bool pace_audio_;
  const bool ignore_transport_overhead_;
  // In dynamic mode, indicates the target size when requesting padding,
  // expressed as a duration in order to adjust for varying padding rate.
  const TimeDelta padding_target_duration_;

  TimeDelta min_packet_limit_;

  DataSize transport_overhead_per_packet_;

  // TODO(webrtc:9716): Remove this when we are certain clocks are monotonic.
  // The last millisecond timestamp returned by `clock_`.
  mutable Timestamp last_timestamp_;
  bool paused_;

  // In dynamic mode, `media_budget_` and `padding_budget_` will be used to
  // track when packets can be sent.
  // In periodic mode, `media_debt_` and `padding_debt_` will be used together
  // with the target rates.

  // 这是 media 预算，用于跟踪在当前间隔内我们可以发出多少数据
  IntervalBudget media_budget_;
  // 这是 padding 预算，用于跟踪当前间隔内允许发送多少填充。
  // 在没有 media 数据要发送时，将使用这个预算。
  IntervalBudget padding_budget_;

  // media 和 padding 未处理数据量
  DataSize media_debt_;  // 媒体数据负债
  DataSize padding_debt_;
  DataRate media_rate_;  // 媒体数据发送速率
  DataRate padding_rate_;

  BitrateProber prober_;
  bool probing_send_failure_;

  DataRate pacing_bitrate_;  // 设置的发送码率，由 RtpTransportControllerSend 模块调用

  Timestamp last_process_time_;  // 上一次处理的时间
  Timestamp last_send_time_;  // 上一次发送的时间
  absl::optional<Timestamp> first_sent_packet_time_;

  RoundRobinPacketQueue packet_queue_;  // 存储数据包的优先级队列
  uint64_t packet_counter_;  // RTP 包入队列的顺序

  DataSize congestion_window_size_;
  DataSize outstanding_data_;  // 未处理数据量

  TimeDelta queue_time_limit;  // 期望的最大延迟时间
  bool account_for_audio_;
  bool include_overhead_;
};
}  // namespace webrtc

#endif  // MODULES_PACING_PACING_CONTROLLER_H_
