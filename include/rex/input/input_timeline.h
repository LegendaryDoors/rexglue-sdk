#pragma once
/**
 * @file        input/input_timeline.h
 * @brief       The process-wide guest input timeline clock.
 *
 * @remarks     t0 is the guest's first user-0 controller poll, the single
 *              reference --input_script, --input_record, --screenshot_at and
 *              the F7 screenshot hotkey all timestamp against. Anything else
 *              that timestamps against the replay timeline must read this
 *              clock; a second clock would make the timestamps incomparable.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <cstdint>
#include <optional>

namespace rex::input {

class InputTimeline {
 public:
  // Marks t0 at the first call; later calls are one relaxed atomic load.
  static void MarkStarted();

  // Whether the guest has polled user-0 input at least once.
  static bool started();

  // Milliseconds since t0, or nullopt if the guest has not polled input yet.
  static std::optional<uint64_t> NowMs();

  // The t0 time point on std::chrono::steady_clock, for precise absolute
  // waits (t0 + offset), or nullopt if the guest has not polled input yet.
  static std::optional<std::chrono::steady_clock::time_point> StartTime();
};

}  // namespace rex::input
