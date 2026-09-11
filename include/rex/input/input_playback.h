#pragma once
/**
 * @file        input/input_playback.h
 * @brief       Timeline-based guest input record and replay.
 *
 * @remarks     Both halves run at the InputSystem::GetState merge point, so
 *              replay needs neither window focus nor a physical device.
 *
 * @format      One event per line, '#' starts a comment:
 *                <ms> <DOWN|UP> <BUTTON>
 *              <ms> counts from the guest's first user-0 controller poll,
 *              the t0 of the shared InputTimeline (input_timeline.h).
 *              Events are sorted by timestamp on load. Button names are
 *              listed in kChannelNames in input_playback.cpp.
 *
 * @usage       --input_script=<file> replays, --input_record=<file> records.
 *              Both are off by default. User 0 only.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input.h>

namespace rex::input {

// Process-wide replay status, read by --input_script_exit. Monotonic: at most
// one replayer exists per process. True once a script has been loaded.
bool ScriptReplayLoaded();
// True once every event of the loaded script has been applied to guest input.
// Always false when no script was loaded.
bool ScriptReplayComplete();

// Extended channel mask: low 16 bits are the real X_INPUT_GAMEPAD_* button
// bits; higher bits are pseudo-buttons for triggers and stick directions.
enum InputPlaybackChannel : uint32_t {
  kChannelLT = 1u << 16,
  kChannelRT = 1u << 17,
  kChannelLSUp = 1u << 18,
  kChannelLSDown = 1u << 19,
  kChannelLSLeft = 1u << 20,
  kChannelLSRight = 1u << 21,
  kChannelRSUp = 1u << 22,
  kChannelRSDown = 1u << 23,
  kChannelRSLeft = 1u << 24,
  kChannelRSRight = 1u << 25,
};

/// Replays a recorded timeline into guest-visible controller state.
/// Thread-safe; Apply() is called from whichever guest thread polls input.
class InputReplayer {
 public:
  /// Parses the script. Returns nullptr (after logging the reason) on any
  /// error -- a truncated or mistyped script must fail loudly, not half-run.
  static std::unique_ptr<InputReplayer> FromFile(const std::string& path);

  /// Advances the timeline to now and merges the result into *state: buttons
  /// ORed, triggers maxed, sticks overridden while a direction is held.
  void Apply(X_INPUT_STATE* state);

 private:
  struct Event {
    uint64_t ms;
    bool down;
    uint32_t channel;  // exactly one bit
  };

  InputReplayer() = default;

  std::mutex mutex_;
  std::vector<Event> events_;
  size_t next_ = 0;
  uint32_t applied_ = 0;      // total events applied (packet_number bump)
  uint32_t mask_ = 0;         // current extended channel state
  bool started_ = false;      // for the one-time "timeline started" log only;
                              // the clock itself is the shared InputTimeline
  bool completion_logged_ = false;
};

/// Records the guest-visible controller state as a timeline of DOWN/UP edges.
/// Never alters what the guest sees. Lines are flushed as written.
class InputRecorder {
 public:
  /// Opens the output file and writes a format header. Returns nullptr
  /// (after logging) if the file cannot be opened.
  static std::unique_ptr<InputRecorder> ToFile(const std::string& path);

  /// Diffs state against the previous poll and appends one line per edge.
  /// t=0 is the shared InputTimeline t0, the same reference replay uses.
  void Observe(const X_INPUT_STATE& state);

 private:
  InputRecorder() = default;

  std::mutex mutex_;
  std::ofstream out_;
  uint32_t prev_mask_ = 0;
};

}  // namespace rex::input
