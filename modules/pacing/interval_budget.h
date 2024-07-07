/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_PACING_INTERVAL_BUDGET_H_
#define MODULES_PACING_INTERVAL_BUDGET_H_

#include <stddef.h>
#include <stdint.h>

namespace webrtc {

// TODO(tschumim): Reflector IntervalBudget so that we can set a under- and
// over-use budget in ms.
class IntervalBudget {
 public:
  explicit IntervalBudget(int initial_target_rate_kbps);
  IntervalBudget(int initial_target_rate_kbps, bool can_build_up_underuse);
  // 设置目标发送码率
  void set_target_rate_kbps(int target_rate_kbps);

  // 时间流逝后增加预算
  void IncreaseBudget(int64_t delta_time_ms);
  // 发送数据后减少预算
  void UseBudget(size_t bytes);

  // 剩余预算
  size_t bytes_remaining() const;
  // 剩余预算与最大预算比值
  double budget_ratio() const;
  // 目标发送码率
  int target_rate_kbps() const;

 private:
  int target_rate_kbps_;  // 目标码率
  int64_t max_bytes_in_budget_;  // 最大预算
  int64_t bytes_remaining_;  // 剩余可发送字节数
  bool can_build_up_underuse_;  // 是否可使用上周期剩余字节数
};

}  // namespace webrtc

#endif  // MODULES_PACING_INTERVAL_BUDGET_H_
