/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_HISTOGRAM_H_
#define MODULES_VIDEO_CODING_HISTOGRAM_H_

#include <cstddef>
#include <vector>

namespace webrtc {
namespace video_coding {
class Histogram {
 public:
  // 离散直方图，其中每个桶的范围为 [0, num_buckets)
  // 大于或等于 num_buckets 的值放在最后一个桶中
  Histogram(size_t num_buckets, size_t max_num_values);

  // 向直方图添加一个值。如果已经满了，则替换掉最旧的值
  void Add(size_t value);

  // 计算需要对多少个桶求和才能累计给定的概率
  size_t InverseCdf(float probability) const;

  // 这个直方图包含多少个值
  size_t NumValues() const;

 private:
  // 保存构成直方图的值的循环缓冲区
  std::vector<size_t> values_;
  std::vector<size_t> buckets_;
  size_t index_; // 环形缓冲区索引
};

}  // namespace video_coding
}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_HISTOGRAM_H_
