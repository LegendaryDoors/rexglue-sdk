/**
 * @file        input/input_playback.cpp
 * @brief       Timeline-based guest input record/replay implementation.
 *
 * See input_playback.h for the design rationale and file format.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/input_playback.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include <rex/cvar.h>
#include <rex/input/input_timeline.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(input_script, "", "Input",
                      "Replay a recorded guest input timeline from this file");
REXCVAR_DEFINE_STRING(input_record, "", "Input",
                      "Record the guest-visible input timeline to this file");

namespace rex::input {

namespace {

// Process-wide replay status (see the header). Monotonic false -> true.
std::atomic<bool> g_replay_loaded{false};
std::atomic<bool> g_replay_complete{false};

}  // namespace

bool ScriptReplayLoaded() { return g_replay_loaded.load(std::memory_order_acquire); }
bool ScriptReplayComplete() { return g_replay_complete.load(std::memory_order_acquire); }

namespace {

// Replay values. Stick magnitude comfortably exceeds any cardinal-direction
// threshold a game uses; trigger is full pull.
constexpr uint8_t kReplayTriggerValue = 0xFF;
constexpr int16_t kReplayStickMagnitude = 30000;

// Record thresholds: analog value at which a pseudo-button counts as DOWN.
constexpr uint8_t kRecordTriggerThreshold = 64;
constexpr int16_t kRecordStickThreshold = 16384;

struct ChannelName {
  const char* name;
  uint32_t channel;
};

// Canonical names (used when recording) first, aliases after.
constexpr ChannelName kChannelNames[] = {
    {"DPAD_UP", X_INPUT_GAMEPAD_DPAD_UP},
    {"DPAD_DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
    {"DPAD_LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},
    {"DPAD_RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
    {"START", X_INPUT_GAMEPAD_START},
    {"BACK", X_INPUT_GAMEPAD_BACK},
    {"LEFT_THUMB", X_INPUT_GAMEPAD_LEFT_THUMB},
    {"RIGHT_THUMB", X_INPUT_GAMEPAD_RIGHT_THUMB},
    {"LEFT_SHOULDER", X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {"RIGHT_SHOULDER", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {"GUIDE", X_INPUT_GAMEPAD_GUIDE},
    {"A", X_INPUT_GAMEPAD_A},
    {"B", X_INPUT_GAMEPAD_B},
    {"X", X_INPUT_GAMEPAD_X},
    {"Y", X_INPUT_GAMEPAD_Y},
    {"LT", kChannelLT},
    {"RT", kChannelRT},
    {"LS_UP", kChannelLSUp},
    {"LS_DOWN", kChannelLSDown},
    {"LS_LEFT", kChannelLSLeft},
    {"LS_RIGHT", kChannelLSRight},
    {"RS_UP", kChannelRSUp},
    {"RS_DOWN", kChannelRSDown},
    {"RS_LEFT", kChannelRSLeft},
    {"RS_RIGHT", kChannelRSRight},
    // Aliases (parse only).
    {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {"L3", X_INPUT_GAMEPAD_LEFT_THUMB},
    {"R3", X_INPUT_GAMEPAD_RIGHT_THUMB},
    {"SELECT", X_INPUT_GAMEPAD_BACK},
};

uint32_t ChannelFromName(const std::string& name) {
  for (const auto& entry : kChannelNames) {
    if (name == entry.name) {
      return entry.channel;
    }
  }
  return 0;
}

const char* NameFromChannel(uint32_t channel) {
  for (const auto& entry : kChannelNames) {
    if (channel == entry.channel) {
      return entry.name;
    }
  }
  return "?";
}

// Applies an extended channel mask to a gamepad state: buttons ORed, triggers
// maxed, stick axes overridden while a direction pseudo-button is held.
void MergeMaskIntoState(uint32_t mask, X_INPUT_STATE* state) {
  state->gamepad.buttons =
      static_cast<uint16_t>(static_cast<uint16_t>(state->gamepad.buttons) |
                            static_cast<uint16_t>(mask & 0xFFFF));
  if (mask & kChannelLT) {
    state->gamepad.left_trigger =
        std::max<uint8_t>(state->gamepad.left_trigger, kReplayTriggerValue);
  }
  if (mask & kChannelRT) {
    state->gamepad.right_trigger =
        std::max<uint8_t>(state->gamepad.right_trigger, kReplayTriggerValue);
  }
  auto axis = [](bool pos, bool neg) -> int16_t {
    if (pos == neg) return 0;
    return pos ? kReplayStickMagnitude
               : static_cast<int16_t>(-kReplayStickMagnitude);
  };
  if (mask & (kChannelLSUp | kChannelLSDown)) {
    state->gamepad.thumb_ly =
        axis((mask & kChannelLSUp) != 0, (mask & kChannelLSDown) != 0);
  }
  if (mask & (kChannelLSLeft | kChannelLSRight)) {
    state->gamepad.thumb_lx =
        axis((mask & kChannelLSRight) != 0, (mask & kChannelLSLeft) != 0);
  }
  if (mask & (kChannelRSUp | kChannelRSDown)) {
    state->gamepad.thumb_ry =
        axis((mask & kChannelRSUp) != 0, (mask & kChannelRSDown) != 0);
  }
  if (mask & (kChannelRSLeft | kChannelRSRight)) {
    state->gamepad.thumb_rx =
        axis((mask & kChannelRSRight) != 0, (mask & kChannelRSLeft) != 0);
  }
}

// Extracts the extended channel mask the guest would perceive from a state.
uint32_t MaskFromState(const X_INPUT_STATE& state) {
  uint32_t mask = static_cast<uint16_t>(state.gamepad.buttons);
  if (state.gamepad.left_trigger >= kRecordTriggerThreshold) mask |= kChannelLT;
  if (state.gamepad.right_trigger >= kRecordTriggerThreshold) mask |= kChannelRT;
  int16_t lx = state.gamepad.thumb_lx;
  int16_t ly = state.gamepad.thumb_ly;
  int16_t rx = state.gamepad.thumb_rx;
  int16_t ry = state.gamepad.thumb_ry;
  if (ly >= kRecordStickThreshold) mask |= kChannelLSUp;
  if (ly <= -kRecordStickThreshold) mask |= kChannelLSDown;
  if (lx <= -kRecordStickThreshold) mask |= kChannelLSLeft;
  if (lx >= kRecordStickThreshold) mask |= kChannelLSRight;
  if (ry >= kRecordStickThreshold) mask |= kChannelRSUp;
  if (ry <= -kRecordStickThreshold) mask |= kChannelRSDown;
  if (rx <= -kRecordStickThreshold) mask |= kChannelRSLeft;
  if (rx >= kRecordStickThreshold) mask |= kChannelRSRight;
  return mask;
}

}  // namespace

std::unique_ptr<InputReplayer> InputReplayer::FromFile(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    REXLOG_ERROR("[input] replay: cannot open input script '{}'", path);
    return nullptr;
  }

  auto replayer = std::unique_ptr<InputReplayer>(new InputReplayer());
  std::string line;
  int line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    auto hash = line.find('#');
    if (hash != std::string::npos) {
      line.resize(hash);
    }
    std::istringstream tokens(line);
    uint64_t ms = 0;
    std::string action;
    std::string button;
    if (!(tokens >> ms)) {
      // Blank or comment-only line.
      std::istringstream probe(line);
      std::string any;
      if (probe >> any) {
        REXLOG_ERROR("[input] replay: {}:{}: expected '<ms> <DOWN|UP> <BUTTON>', got '{}'",
                     path, line_number, line);
        return nullptr;
      }
      continue;
    }
    if (!(tokens >> action >> button)) {
      REXLOG_ERROR("[input] replay: {}:{}: expected '<ms> <DOWN|UP> <BUTTON>', got '{}'",
                   path, line_number, line);
      return nullptr;
    }
    std::transform(action.begin(), action.end(), action.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    std::transform(button.begin(), button.end(), button.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    bool down;
    if (action == "DOWN") {
      down = true;
    } else if (action == "UP") {
      down = false;
    } else {
      REXLOG_ERROR("[input] replay: {}:{}: action must be DOWN or UP, got '{}'",
                   path, line_number, action);
      return nullptr;
    }
    uint32_t channel = ChannelFromName(button);
    if (!channel) {
      REXLOG_ERROR("[input] replay: {}:{}: unknown button '{}'", path,
                   line_number, button);
      return nullptr;
    }
    replayer->events_.push_back(Event{ms, down, channel});
  }

  if (replayer->events_.empty()) {
    REXLOG_ERROR("[input] replay: '{}' contains no events", path);
    return nullptr;
  }

  std::stable_sort(replayer->events_.begin(), replayer->events_.end(),
                   [](const Event& a, const Event& b) { return a.ms < b.ms; });

  REXLOG_INFO("[input] replay: loaded {} events from '{}' (last at {} ms)",
              replayer->events_.size(), path, replayer->events_.back().ms);
  g_replay_loaded.store(true, std::memory_order_release);
  return replayer;
}

void InputReplayer::Apply(X_INPUT_STATE* state) {
  std::lock_guard<std::mutex> lock(mutex_);
  // The shared timeline clock is normally marked by InputSystem::GetState in
  // this same call; marking here too keeps Apply correct standalone.
  InputTimeline::MarkStarted();
  if (!started_) {
    started_ = true;
    REXLOG_INFO("[input] replay: timeline started (first guest poll)");
  }
  uint64_t elapsed_ms = *InputTimeline::NowMs();

  while (next_ < events_.size() && events_[next_].ms <= elapsed_ms) {
    const Event& event = events_[next_];
    if (event.down) {
      mask_ |= event.channel;
    } else {
      mask_ &= ~event.channel;
    }
    ++applied_;
    REXLOG_INFO("[input] replay: {} {} (scripted {} ms, actual {} ms)",
                event.down ? "DOWN" : "UP", NameFromChannel(event.channel),
                event.ms, elapsed_ms);
    ++next_;
  }
  if (next_ == events_.size() && !completion_logged_) {
    completion_logged_ = true;
    g_replay_complete.store(true, std::memory_order_release);
    REXLOG_INFO("[input] replay: script complete ({} events applied)",
                applied_);
  }

  MergeMaskIntoState(mask_, state);
  // Bump packet_number by the running event count so titles that diff packet
  // numbers observe a change on every edge. Monotonic.
  state->packet_number =
      static_cast<uint32_t>(state->packet_number) + applied_;
}

std::unique_ptr<InputRecorder> InputRecorder::ToFile(const std::string& path) {
  auto recorder = std::unique_ptr<InputRecorder>(new InputRecorder());
  recorder->out_.open(path, std::ios::out | std::ios::trunc);
  if (!recorder->out_) {
    REXLOG_ERROR("[input] record: cannot open '{}' for writing", path);
    return nullptr;
  }
  recorder->out_ << "# rexglue guest input timeline\n"
                    "# format: <ms> <DOWN|UP> <BUTTON>   ('#' comments)\n"
                    "# ms is relative to the guest's first controller poll.\n"
                    "# replay with --input_script=<this file>\n";
  recorder->out_.flush();
  REXLOG_INFO("[input] record: writing guest input timeline to '{}'", path);
  return recorder;
}

void InputRecorder::Observe(const X_INPUT_STATE& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Same t0 as replay and --screenshot_at: the shared timeline clock, marked
  // at the guest's first user-0 poll.
  InputTimeline::MarkStarted();
  uint32_t mask = MaskFromState(state);
  uint32_t changed = mask ^ prev_mask_;
  if (!changed) {
    return;
  }
  uint64_t elapsed_ms = *InputTimeline::NowMs();
  for (const auto& entry : kChannelNames) {
    if (!(changed & entry.channel)) {
      continue;
    }
    // Skip alias entries so each edge is written once under its canonical name.
    // Aliases share a channel bit and come after the canonical names.
    changed &= ~entry.channel;
    bool down = (mask & entry.channel) != 0;
    out_ << elapsed_ms << ' ' << (down ? "DOWN" : "UP") << ' ' << entry.name
         << '\n';
  }
  out_.flush();
  prev_mask_ = mask;
}

}  // namespace rex::input
