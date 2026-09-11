/**
 * @file        input/input_timeline.cpp
 * @brief       Process-wide guest input timeline clock. See input_timeline.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/input_timeline.h>

#include <atomic>
#include <mutex>

namespace rex::input {

namespace {
std::atomic<bool> g_started{false};
// g_start is written once under g_start_mutex before g_started is set with
// release order, so a reader observing g_started sees a written g_start.
std::mutex g_start_mutex;
std::chrono::steady_clock::time_point g_start;
}  // namespace

void InputTimeline::MarkStarted() {
  if (g_started.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_start_mutex);
  if (!g_started.load(std::memory_order_relaxed)) {
    g_start = std::chrono::steady_clock::now();
    g_started.store(true, std::memory_order_release);
  }
}

bool InputTimeline::started() { return g_started.load(std::memory_order_acquire); }

std::optional<uint64_t> InputTimeline::NowMs() {
  if (!started()) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - g_start)
                                   .count());
}

std::optional<std::chrono::steady_clock::time_point> InputTimeline::StartTime() {
  if (!started()) {
    return std::nullopt;
  }
  return g_start;
}

}  // namespace rex::input
