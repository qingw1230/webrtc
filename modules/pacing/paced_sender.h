/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_PACING_PACED_SENDER_H_
#define MODULES_PACING_PACED_SENDER_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <memory>
#include <vector>

#include "absl/types/optional.h"
#include "api/function_view.h"
#include "api/transport/field_trial_based_config.h"
#include "api/transport/network_types.h"
#include "api/transport/webrtc_key_value_config.h"
#include "modules/include/module.h"
#include "modules/pacing/bitrate_prober.h"
#include "modules/pacing/interval_budget.h"
#include "modules/pacing/pacing_controller.h"
#include "modules/pacing/packet_router.h"
#include "modules/pacing/rtp_packet_pacer.h"
#include "modules/rtp_rtcp/include/rtp_packet_sender.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "modules/utility/include/process_thread.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/thread_annotations.h"

namespace webrtc {
class Clock;
class RtcEventLog;

class PacedSender : public Module,
                    public RtpPacketPacer,
                    public RtpPacketSender {
 public:
  // 预期的最大 pacer 延迟（以 ms 为单位）。
  // 如果 ExpectedQueueTime() 高于此值，数据包生产者应该等待（例如，丢弃帧而不是编码它们）。
  // 发送的 bitrate 可能会暂时超过 UpdateBitrate() 设置的目标，以便维持此限制。
  static const int64_t kMaxQueueLengthMs;
  // Pacing-rate relative to our target send rate.
  // Multiplicative factor that is applied to the target bitrate to calculate
  // the number of bytes that can be transmitted per interval.
  // Increasing this factor will result in lower delays in cases of bitrate
  // overshoots from the encoder.
  static const float kDefaultPaceMultiplier;

  PacedSender(Clock* clock,
              PacketRouter* packet_router,
              RtcEventLog* event_log,
              const WebRtcKeyValueConfig* field_trials = nullptr,
              ProcessThread* process_thread = nullptr);

  ~PacedSender() override;

  // Methods implementing RtpPacketSender.

  // 调用 PacingController::EnqueuePacket 将数据包添加到队列
  void EnqueuePackets(
      std::vector<std::unique_ptr<RtpPacketToSend>> packet) override;

  // Methods implementing RtpPacketPacer:

  void CreateProbeCluster(DataRate bitrate, int cluster_id) override;

  // Temporarily pause all sending.
  void Pause() override;

  // Resume sending packets.
  void Resume() override;

  void SetCongestionWindow(DataSize congestion_window_size) override;
  void UpdateOutstandingData(DataSize outstanding_data) override;

  // Sets the pacing rates. Must be called once before packets can be sent.
  void SetPacingRates(DataRate pacing_rate, DataRate padding_rate) override;

  // Currently audio traffic is not accounted by pacer and passed through.
  // With the introduction of audio BWE audio traffic will be accounted for
  // the pacer budget calculation. The audio traffic still will be injected
  // at high priority.
  void SetAccountForAudioPackets(bool account_for_audio) override;

  void SetIncludeOverhead() override;
  void SetTransportOverhead(DataSize overhead_per_packet) override;

  // Returns the time since the oldest queued packet was enqueued.
  TimeDelta OldestPacketWaitTime() const override;

  DataSize QueueSizeData() const override;

  // Returns the time when the first packet was sent;
  absl::optional<Timestamp> FirstSentPacketTime() const override;

  // Returns the number of milliseconds it will take to send the current
  // packets in the queue, given the current size and bitrate, ignoring prio.
  TimeDelta ExpectedQueueTime() const override;

  void SetQueueTimeLimit(TimeDelta limit) override;

  // Below are methods specific to this implementation, such as things related
  // to module processing thread specifics or methods exposed for test.

 private:
  // Methods implementing Module.
  // TODO(bugs.webrtc.org/10937): Remove the inheritance from Module once all
  // use of it has been cleared up.

  // Returns the number of milliseconds until the module want a worker thread
  // to call Process.
  int64_t TimeUntilNextProcess() override;

  // TODO(bugs.webrtc.org/10937): Make this private (and non virtual) once
  // dependencies have been updated to not call this via the PacedSender
  // interface.
 public:
  // Process any pending packets in the queue(s).
  void Process() override;

 private:
  // Called when the prober is associated with a process thread.
  void ProcessThreadAttached(ProcessThread* process_thread) override;

  // In dynamic process mode, refreshes the next process time.
  void MaybeWakupProcessThread();

  // Private implementation of Module to not expose those implementation details
  // publicly and control when the class is registered/deregistered.
  class ModuleProxy : public Module {
   public:
    explicit ModuleProxy(PacedSender* delegate) : delegate_(delegate) {}

   private:
    int64_t TimeUntilNextProcess() override {
      return delegate_->TimeUntilNextProcess();
    }
    void Process() override { return delegate_->Process(); }
    void ProcessThreadAttached(ProcessThread* process_thread) override {
      return delegate_->ProcessThreadAttached(process_thread);
    }

    PacedSender* const delegate_;
  } module_proxy_{this};

  mutable Mutex mutex_;
  const PacingController::ProcessMode process_mode_;
  PacingController pacing_controller_ RTC_GUARDED_BY(mutex_);

  Clock* const clock_;
  ProcessThread* const process_thread_;
};
}  // namespace webrtc
#endif  // MODULES_PACING_PACED_SENDER_H_
