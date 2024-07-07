/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/round_robin_packet_queue.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "absl/strings/match.h"
#include "rtc_base/checks.h"

namespace webrtc {
namespace {
static constexpr DataSize kMaxLeadingSize = DataSize::Bytes(1400);
}

RoundRobinPacketQueue::QueuedPacket::QueuedPacket(const QueuedPacket& rhs) =
    default;
RoundRobinPacketQueue::QueuedPacket::~QueuedPacket() = default;

RoundRobinPacketQueue::QueuedPacket::QueuedPacket(
    int priority,
    Timestamp enqueue_time,
    uint64_t enqueue_order,
    std::multiset<Timestamp>::iterator enqueue_time_it,
    std::unique_ptr<RtpPacketToSend> packet)
    : priority_(priority),
      enqueue_time_(enqueue_time),
      enqueue_order_(enqueue_order),
      is_retransmission_(packet->packet_type() == RtpPacketMediaType::kRetransmission),
      enqueue_time_it_(enqueue_time_it),
      owned_packet_(packet.release()) {}

bool RoundRobinPacketQueue::QueuedPacket::operator<(
    const RoundRobinPacketQueue::QueuedPacket& other) const {
  if (priority_ != other.priority_) {
    // 优先队列底层小根堆，要用大于号
    return priority_ > other.priority_;
  }
  if (is_retransmission_ != other.is_retransmission_)
    return other.is_retransmission_;

  return enqueue_order_ > other.enqueue_order_;
}

int RoundRobinPacketQueue::QueuedPacket::Priority() const {
  return priority_;
}

RtpPacketMediaType RoundRobinPacketQueue::QueuedPacket::Type() const {
  return *owned_packet_->packet_type();
}

uint32_t RoundRobinPacketQueue::QueuedPacket::Ssrc() const {
  return owned_packet_->Ssrc();
}

Timestamp RoundRobinPacketQueue::QueuedPacket::EnqueueTime() const {
  return enqueue_time_;
}

bool RoundRobinPacketQueue::QueuedPacket::IsRetransmission() const {
  return Type() == RtpPacketMediaType::kRetransmission;
}

uint64_t RoundRobinPacketQueue::QueuedPacket::EnqueueOrder() const {
  return enqueue_order_;
}

RtpPacketToSend* RoundRobinPacketQueue::QueuedPacket::RtpPacket() const {
  return owned_packet_;
}

void RoundRobinPacketQueue::QueuedPacket::UpdateEnqueueTimeIterator(
    std::multiset<Timestamp>::iterator it) {
  enqueue_time_it_ = it;
}

std::multiset<Timestamp>::iterator
RoundRobinPacketQueue::QueuedPacket::EnqueueTimeIterator() const {
  return enqueue_time_it_;
}

// 将 RTP 包的入队时间减去指定 pause 时间
void RoundRobinPacketQueue::QueuedPacket::SubtractPauseTime(
    TimeDelta pause_time_sum) {
  enqueue_time_ -= pause_time_sum;
}

RoundRobinPacketQueue::PriorityPacketQueue::const_iterator
RoundRobinPacketQueue::PriorityPacketQueue::begin() const {
  return c.begin();
}

RoundRobinPacketQueue::PriorityPacketQueue::const_iterator
RoundRobinPacketQueue::PriorityPacketQueue::end() const {
  return c.end();
}

RoundRobinPacketQueue::Stream::Stream() : size(DataSize::Zero()), ssrc(0) {}
RoundRobinPacketQueue::Stream::Stream(const Stream& stream) = default;
RoundRobinPacketQueue::Stream::~Stream() = default;

bool IsEnabled(const WebRtcKeyValueConfig* field_trials, const char* name) {
  if (!field_trials) {
    return false;
  }
  return absl::StartsWith(field_trials->Lookup(name), "Enabled");
}

RoundRobinPacketQueue::RoundRobinPacketQueue(
    Timestamp start_time,
    const WebRtcKeyValueConfig* field_trials)
    : transport_overhead_per_packet_(DataSize::Zero()),
      time_last_updated_(start_time),
      paused_(false),
      size_packets_(0),
      size_(DataSize::Zero()),
      max_size_(kMaxLeadingSize),
      queue_time_sum_(TimeDelta::Zero()),
      pause_time_sum_(TimeDelta::Zero()),
      include_overhead_(false) {}

RoundRobinPacketQueue::~RoundRobinPacketQueue() {
  // Make sure to release any packets owned by raw pointer in QueuedPacket.
  while (!Empty()) {
    Pop();
  }
}

void RoundRobinPacketQueue::Push(int priority,
                                 Timestamp enqueue_time,
                                 uint64_t enqueue_order,
                                 std::unique_ptr<RtpPacketToSend> packet) {
  RTC_DCHECK(packet->packet_type().has_value());
  // 没有存储任何报文时，直接存在 single_packet_queue_ 中，不进入队列
  if (size_packets_ == 0) {
    single_packet_queue_.emplace(
        QueuedPacket(priority, enqueue_time, enqueue_order,
                     enqueue_times_.end(), std::move(packet)));
    UpdateQueueTime(enqueue_time);
    single_packet_queue_->SubtractPauseTime(pause_time_sum_);
    size_packets_ = 1;
    size_ += PacketSize(*single_packet_queue_);
  } else {
    // 如果 single_packet_queue_ 有数据，先把里面的数据 push 到 queue 中，然后重置它
    MaybePromoteSinglePacketToNormalQueue();
    // 插入到队列中
    Push(QueuedPacket(priority, enqueue_time, enqueue_order,
                      enqueue_times_.insert(enqueue_time), std::move(packet)));
  }
}

std::unique_ptr<RtpPacketToSend> RoundRobinPacketQueue::Pop() {
  // single_packet_queue_ 中有数据直接返回
  if (single_packet_queue_.has_value()) {
    std::unique_ptr<RtpPacketToSend> rtp_packet(
        single_packet_queue_->RtpPacket());
    single_packet_queue_.reset();
    queue_time_sum_ = TimeDelta::Zero();
    size_packets_ = 0;
    size_ = DataSize::Zero();
    return rtp_packet;
  }

  // 返回优先级最高的 Stream，获取最前面的数据
  Stream* stream = GetHighestPriorityStream();
  const QueuedPacket& queued_packet = stream->packet_queue.top();

  stream_priorities_.erase(stream->priority_it);

  // 计算此数据包在非暂停状态下在队列中花费的时间。
  // 请注意，在 Push 数据包时，会从 packet.enqueue_time_ms 中减去 pause_time_sum_ms_，
  // 现在通过减去它，我们有效消除了在暂停状态下在队列中花费的时间。
  TimeDelta time_in_non_paused_state =
      time_last_updated_ - queued_packet.EnqueueTime() - pause_time_sum_;
  queue_time_sum_ -= time_in_non_paused_state;
  // 删除该报文的 queue time
  enqueue_times_.erase(queued_packet.EnqueueTimeIterator());

  // 报文发送后，更新 Stream 发送的报文字节数，发送较少的 Stream 应该有更高的调度优先级。
  // 为了避免发送码率较低的 Stream 一直处于较高优先级发送过多，限制了最低发送字节数。
  DataSize packet_size = PacketSize(queued_packet);
  stream->size =
      std::max(stream->size + packet_size, max_size_ - kMaxLeadingSize);
  max_size_ = std::max(max_size_, stream->size);

  size_ -= packet_size;
  size_packets_ -= 1;

  std::unique_ptr<RtpPacketToSend> rtp_packet(queued_packet.RtpPacket());
  stream->packet_queue.pop();

  // 如果还有数据包需要发送，再次调度该流
  if (stream->packet_queue.empty()) {
    stream->priority_it = stream_priorities_.end();
  } else {
    int priority = stream->packet_queue.top().Priority();
    stream->priority_it = stream_priorities_.emplace(
        StreamPrioKey(priority, stream->size), stream->ssrc);
  }

  return rtp_packet;
}

bool RoundRobinPacketQueue::Empty() const {
  if (size_packets_ == 0) {
    return true;
  }
  return false;
}

size_t RoundRobinPacketQueue::SizeInPackets() const {
  return size_packets_;
}

DataSize RoundRobinPacketQueue::Size() const {
  return size_;
}

absl::optional<Timestamp> RoundRobinPacketQueue::LeadingAudioPacketEnqueueTime()
    const {
  if (single_packet_queue_.has_value()) {
    if (single_packet_queue_->Type() == RtpPacketMediaType::kAudio) {
      return single_packet_queue_->EnqueueTime();
    }
    return absl::nullopt;
  }

  if (stream_priorities_.empty()) {
    return absl::nullopt;
  }
  uint32_t ssrc = stream_priorities_.begin()->second;

  const auto& top_packet = streams_.find(ssrc)->second.packet_queue.top();
  if (top_packet.Type() == RtpPacketMediaType::kAudio) {
    return top_packet.EnqueueTime();
  }
  return absl::nullopt;
}

Timestamp RoundRobinPacketQueue::OldestEnqueueTime() const {
  if (single_packet_queue_.has_value()) {
    return single_packet_queue_->EnqueueTime();
  }

  if (Empty())
    return Timestamp::MinusInfinity();
  return *enqueue_times_.begin();
}

void RoundRobinPacketQueue::UpdateQueueTime(Timestamp now) {
  if (now == time_last_updated_)
    return;

  TimeDelta delta = now - time_last_updated_;

  if (paused_) {
    pause_time_sum_ += delta;
  } else {
    queue_time_sum_ += TimeDelta::Micros(delta.us() * size_packets_);
  }

  time_last_updated_ = now;
}

void RoundRobinPacketQueue::SetPauseState(bool paused, Timestamp now) {
  if (paused_ == paused)
    return;
  UpdateQueueTime(now);
  paused_ = paused;
}

void RoundRobinPacketQueue::SetIncludeOverhead() {
  MaybePromoteSinglePacketToNormalQueue();
  include_overhead_ = true;
  // We need to update the size to reflect overhead for existing packets.
  for (const auto& stream : streams_) {
    for (const QueuedPacket& packet : stream.second.packet_queue) {
      size_ += DataSize::Bytes(packet.RtpPacket()->headers_size()) +
               transport_overhead_per_packet_;
    }
  }
}

void RoundRobinPacketQueue::SetTransportOverhead(DataSize overhead_per_packet) {
  MaybePromoteSinglePacketToNormalQueue();
  if (include_overhead_) {
    DataSize previous_overhead = transport_overhead_per_packet_;
    // We need to update the size to reflect overhead for existing packets.
    for (const auto& stream : streams_) {
      int packets = stream.second.packet_queue.size();
      size_ -= packets * previous_overhead;
      size_ += packets * overhead_per_packet;
    }
  }
  transport_overhead_per_packet_ = overhead_per_packet;
}

TimeDelta RoundRobinPacketQueue::AverageQueueTime() const {
  if (Empty())
    return TimeDelta::Zero();
  return queue_time_sum_ / size_packets_;
}

void RoundRobinPacketQueue::Push(QueuedPacket packet) {
  // 根据报文 ssrc 查找对应的 stream，没找到则创建一个新的 Stream
  auto stream_info_it = streams_.find(packet.Ssrc());
  if (stream_info_it == streams_.end()) {
    stream_info_it = streams_.emplace(packet.Ssrc(), Stream()).first;
    // 暂时还没确定该 Stream 的优先级，即没有被调度
    stream_info_it->second.priority_it = stream_priorities_.end();
    stream_info_it->second.ssrc = packet.Ssrc();
  }

  Stream* stream = &stream_info_it->second;
  // 调整流的优先级
  if (stream->priority_it == stream_priorities_.end()) {
    // 如果该 SSRC 没有被调度，加入到 stream_priorities_
    stream->priority_it = stream_priorities_.emplace(
        StreamPrioKey(packet.Priority(), stream->size), packet.Ssrc());
  } else if (packet.Priority() < stream->priority_it->first.priority) {
    // 数值越小，优先级越高
    // 当前报文优先级比之前的小，说明该流优先级变高，需要更新 Stream 的优先级
    stream_priorities_.erase(stream->priority_it);
    stream->priority_it = stream_priorities_.emplace(
        StreamPrioKey(packet.Priority(), stream->size), packet.Ssrc());
  }

  // 还没有入队时间，说明是从 single_packet_queue_ 中提升来的，其他情况已向 enqueue_times_ 中插入了
  // 注意 packet 只保存 enqueue_times_ 的 iterator
  if (packet.EnqueueTimeIterator() == enqueue_times_.end()) {
    // Promotion from single-packet queue. Just add to enqueue times.
    packet.UpdateEnqueueTimeIterator(
        enqueue_times_.insert(packet.EnqueueTime()));
  } else {
    // 为了计算一个数据包在非暂停状态下在队列中花费的时间，我们减去到目前为止队列暂停的时间，
    // 当数据包被弹出时，我们再减去那时队列暂停的时间。
    // 这样我们就减去了数据包在暂停状态下在队列中花费的时间。
    UpdateQueueTime(packet.EnqueueTime());
    packet.SubtractPauseTime(pause_time_sum_);

    size_packets_ += 1;
    size_ += PacketSize(packet);
  }

  // 插入到 Stream 的 queue 中
  stream->packet_queue.push(packet);
}

// 获取指定 packet 字节数
DataSize RoundRobinPacketQueue::PacketSize(const QueuedPacket& packet) const {
  DataSize packet_size = DataSize::Bytes(packet.RtpPacket()->payload_size() +
                                         packet.RtpPacket()->padding_size());
  if (include_overhead_) {
    packet_size += DataSize::Bytes(packet.RtpPacket()->headers_size()) +
                   transport_overhead_per_packet_;
  }
  return packet_size;
}

// 将 single_packet_queue_ 中数据移到正常优先队列中
void RoundRobinPacketQueue::MaybePromoteSinglePacketToNormalQueue() {
  if (single_packet_queue_.has_value()) {
    Push(*single_packet_queue_);
    single_packet_queue_.reset();
  }
}

RoundRobinPacketQueue::Stream*
RoundRobinPacketQueue::GetHighestPriorityStream() {
  uint32_t ssrc = stream_priorities_.begin()->second;

  auto stream_info_it = streams_.find(ssrc);
  return &stream_info_it->second;
}

bool RoundRobinPacketQueue::IsSsrcScheduled(uint32_t ssrc) const {
  for (const auto& scheduled_stream : stream_priorities_) {
    if (scheduled_stream.second == ssrc)
      return true;
  }
  return false;
}

}  // namespace webrtc
