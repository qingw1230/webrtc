/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_PACING_ROUND_ROBIN_PACKET_QUEUE_H_
#define MODULES_PACING_ROUND_ROBIN_PACKET_QUEUE_H_

#include <stddef.h>
#include <stdint.h>

#include <list>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <unordered_map>

#include "absl/types/optional.h"
#include "api/transport/webrtc_key_value_config.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {

class RoundRobinPacketQueue {
 public:
  RoundRobinPacketQueue(Timestamp start_time,
                        const WebRtcKeyValueConfig* field_trials);
  ~RoundRobinPacketQueue();

  // 插入不同优先级的报文
  void Push(int priority,
            Timestamp enqueue_time,
            uint64_t enqueue_order,
            std::unique_ptr<RtpPacketToSend> packet);
  // 弹出一个即将发送的报文
  std::unique_ptr<RtpPacketToSend> Pop();

  // queue 是否为空
  bool Empty() const;
  // 报文总数
  size_t SizeInPackets() const;
  // 报文总字节数
  DataSize Size() const;
  // 如果下一个数据包（调用 pop()）是音频数据包，返回该包的入队时间。
  // 如果队列为空或不是音频包，则返回 nullptr。
  absl::optional<Timestamp> LeadingAudioPacketEnqueueTime() const;

  // 队列中最早的入队时间
  Timestamp OldestEnqueueTime() const;
  // 队列中的报文平均排队时间
  TimeDelta AverageQueueTime() const;
  // 更新内部时间状态
  void UpdateQueueTime(Timestamp now);
  // 设置 queue 暂停状态
  void SetPauseState(bool paused, Timestamp now);
  // 计算是考虑传输层 overhead，默认不考虑
  void SetIncludeOverhead();
  // 设置传输层的 overhead 大小
  void SetTransportOverhead(DataSize overhead_per_packet);

 private:
  // queue 中存储的数据包
  struct QueuedPacket {
   public:
    QueuedPacket(int priority,
                 Timestamp enqueue_time,
                 uint64_t enqueue_order,
                 std::multiset<Timestamp>::iterator enqueue_time_it,
                 std::unique_ptr<RtpPacketToSend> packet);
    QueuedPacket(const QueuedPacket& rhs);
    ~QueuedPacket();

    // 放入一个优先队列中，每次获取优先级最高（数值最小的）
    bool operator<(const QueuedPacket& other) const;

    int Priority() const;
    RtpPacketMediaType Type() const;
    uint32_t Ssrc() const;
    Timestamp EnqueueTime() const;
    bool IsRetransmission() const;
    uint64_t EnqueueOrder() const;
    RtpPacketToSend* RtpPacket() const;

    std::multiset<Timestamp>::iterator EnqueueTimeIterator() const;
    void UpdateEnqueueTimeIterator(std::multiset<Timestamp>::iterator it);
    void SubtractPauseTime(TimeDelta pause_time_sum);

   private:
    int priority_;  // RTP 包的优先级
    Timestamp enqueue_time_;  // RTP 包入队列时间
    uint64_t enqueue_order_;  // RTP 包入队列的顺序
    bool is_retransmission_;  // 为了性能而缓存
    std::multiset<Timestamp>::iterator enqueue_time_it_;
    // 原始指针，因为 priotiry_queue 不允许从容器移出
    RtpPacketToSend* owned_packet_;
  };

  class PriorityPacketQueue : public std::priority_queue<QueuedPacket> {
   public:
    using const_iterator = container_type::const_iterator;
    const_iterator begin() const;
    const_iterator end() const;
  };

  // Stream 的优先级 key，用于 multimap
  struct StreamPrioKey {
    StreamPrioKey(int priority, DataSize size)
        : priority(priority), size(size) {}

    // 以优先级排升序（小的优先级高），相同时累计发送数据更少的优先级更高
    bool operator<(const StreamPrioKey& other) const {
      if (priority != other.priority)
        return priority < other.priority;
      return size < other.size;
    }

    const int priority;  // Stream 里面 RTP 数据包的最大优先级
    const DataSize size;  // Stream 累计发送的字节数
  };

  struct Stream {
    Stream();
    Stream(const Stream&);

    virtual ~Stream();

    DataSize size;  // 已发送的报文总大小，发送较少的 Stream 通常会优先调度
    uint32_t ssrc;  // 流的 SSRC
    // 一个根据 StreamPrioKey 确定优先级的优先队列，队列中存储的是 QueuedPacket
    PriorityPacketQueue packet_queue;

    // 每当为该流插入一个数据包时，检查 |priority_it| 是否指向 |stream_priorities_|
    // 中的元素，如果是，则表示该流已被调度，如果调度的优先级低于传入数据包的优先级，
    // 我们将用这个更高的优先级重新调度这个流。
    std::multimap<StreamPrioKey, uint32_t>::iterator priority_it;
  };

  void Push(QueuedPacket packet);

  DataSize PacketSize(const QueuedPacket& packet) const;
  void MaybePromoteSinglePacketToNormalQueue();

  // 获取优先级最高的流
  Stream* GetHighestPriorityStream();

  // Just used to verify correctness.
  bool IsSsrcScheduled(uint32_t ssrc) const;

  DataSize transport_overhead_per_packet_;

  Timestamp time_last_updated_;  // 上一次更新时间

  bool paused_;
  size_t size_packets_; // 报文总数
  DataSize size_;  // 报文总字节数
  DataSize max_size_;
  TimeDelta queue_time_sum_;  // 排队总时间，每个包排队时间总和
  TimeDelta pause_time_sum_;  // 暂停总时间

  // 一个用优先级确定下一个要发送的流的映射。
  // 使用 multimap 而不是 priority_queue，因为随着数据包的插入，流的优先级可能会发生变化，
  // 而 multimap 支持在优先级增加时删除，然后重新插入 StreamPrioKey。
  // StreamPrioKey SSRC map
  std::multimap<StreamPrioKey, uint32_t> stream_priorities_;

  // SSRC Stream map
  std::unordered_map<uint32_t, Stream> streams_;

  // 当前队列中每个数据包的入队时间。
  // 用于确定队列中最老的报文时间。
  std::multiset<Timestamp> enqueue_times_;

  // 存一个 packct（首个 packct 时使用）的 queue
  absl::optional<QueuedPacket> single_packet_queue_;

  bool include_overhead_;
};
}  // namespace webrtc

#endif  // MODULES_PACING_ROUND_ROBIN_PACKET_QUEUE_H_
