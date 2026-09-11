/**
 * @file        system/screenshot.cpp
 * @brief       Live screenshot capture. See screenshot.h for the contract and
 *              exactly what the PNG is (and is not).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/screenshot.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_timeline.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/png_write.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#if !REX_PLATFORM_WIN32
#include <pthread.h>
#endif

REXCVAR_DEFINE_STRING(
    screenshot_at, "", "Diagnostics",
    "Comma-separated offsets in milliseconds on the input timeline (t0 = the guest's first "
    "controller poll, the same reference --input_script and --input_record use), e.g. "
    "40000,75000. At each offset the latest presented guest frame is written as a PNG into "
    "--screenshot_dir. Offsets can be harvested from the log lines an F7 press writes.");
REXCVAR_DEFINE_STRING(
    screenshot_host_at, "", "Diagnostics",
    "Comma-separated offsets in milliseconds after the window is ready, e.g. 3000,8000. At "
    "each offset the window contents - the guest output as displayed plus any UI drawn over "
    "it - are read back from the next presented frame and written as "
    "screenshot_host_<ms>ms.png into --screenshot_dir. Unlike --screenshot_at this does not "
    "wait for the guest, so screens shown before the game starts can be captured.");
REXCVAR_DEFINE_STRING(
    screenshot_dir, "screenshots", "Diagnostics",
    "Directory F7 / --screenshot_at PNGs are written into, created on demand. A relative "
    "path is resolved against the working directory.");

namespace rex::system {

namespace {

// Serializes filename choice + write between F7 (UI thread) and the scheduler
// thread, so the exists-check-then-write below cannot race.
std::mutex g_write_mutex;

// Picks "<stem>.png", or "<stem>_2.png" etc. if taken, so files self-describe
// and never collide. Caller holds g_write_mutex.
std::filesystem::path PickOutputPath(const std::filesystem::path& dir, const std::string& stem) {
  std::filesystem::path candidate = dir / (stem + ".png");
  std::error_code ec;
  for (int n = 2; std::filesystem::exists(candidate, ec) && n < 1000; ++n) {
    candidate = dir / fmt::format("{}_{}.png", stem, n);
  }
  return candidate;
}

struct ScreenshotScheduler {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
};
ScreenshotScheduler* g_scheduler = nullptr;
ScreenshotScheduler* g_host_scheduler = nullptr;

// True from arming until the last offset is processed, or the scheduler is
// stopped early. Read by ScreenshotSchedulePending() for the exit monitor.
std::atomic<bool> g_schedule_pending{false};
std::atomic<bool> g_host_schedule_pending{false};

void StopScheduler(ScreenshotScheduler*& scheduler) {
  if (!scheduler) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(scheduler->mutex);
    scheduler->stop = true;
  }
  scheduler->cv.notify_all();
  if (scheduler->thread.joinable()) {
    scheduler->thread.join();
  }
  delete scheduler;
  scheduler = nullptr;
}

// Parses "40000,75000,80000" into sorted, deduplicated offsets. False on
// anything else, so a malformed schedule is never silently dropped.
bool ParseScheduleMs(const std::string& spec, std::vector<uint64_t>& out) {
  out.clear();
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t comma = spec.find(',', pos);
    std::string token =
        spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = comma == std::string::npos ? spec.size() + 1 : comma + 1;
    // Trim surrounding whitespace.
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
      token.erase(token.begin());
    }
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
      token.pop_back();
    }
    if (token.empty()) {
      continue;
    }
    if (token.find_first_not_of("0123456789") != std::string::npos) {
      REXSYS_ERROR("[screenshot] cannot parse schedule token '{}'", token);
      return false;
    }
    try {
      out.push_back(std::stoull(token));
    } catch (const std::exception&) {
      REXSYS_ERROR("[screenshot] schedule token '{}' out of range", token);
      return false;
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

}  // namespace

bool CaptureScreenshot(IGraphicsSystem* graphics_system, std::optional<uint64_t> scheduled_ms,
                       const char* trigger) {
  ui::Presenter* presenter = graphics_system ? graphics_system->presenter() : nullptr;
  if (!presenter) {
    REXSYS_WARN("[screenshot] {}: no presenter; capture ignored", trigger);
    return false;
  }

  // Stamp before the readback: the captured image is the newest frame
  // completed at this moment, and the fenced GPU copy below takes time.
  std::optional<uint64_t> timeline_ms = input::InputTimeline::NowMs();

  ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image)) {
    // False here means no guest output exists yet (or the readback failed and
    // already logged); it never returns a stale or partial frame.
    REXSYS_WARN("[screenshot] {}: no guest output image to capture yet", trigger);
    return false;
  }

  std::filesystem::path dir = rex::to_path(REXCVAR_GET(screenshot_dir));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  // Scheduled captures are named by the offset that triggered them, so the
  // filename matches the --screenshot_at flag; manual ones by the clock.
  std::string stem;
  if (scheduled_ms) {
    stem = fmt::format("screenshot_{}ms", *scheduled_ms);
  } else if (timeline_ms) {
    stem = fmt::format("screenshot_f7_{}ms", *timeline_ms);
  } else {
    stem = "screenshot_f7_pre-timeline";
  }

  std::lock_guard<std::mutex> lock(g_write_mutex);
  std::filesystem::path path = PickOutputPath(dir, stem);
  if (!rex::WritePng(path, image.data.data(), image.width, image.height, image.stride)) {
    REXSYS_ERROR("[screenshot] {}: failed to write '{}'", trigger, rex::path_to_utf8(path));
    return false;
  }

  std::string path_utf8 = rex::path_to_utf8(std::filesystem::absolute(path, ec));
  if (scheduled_ms) {
    REXSYS_INFO(
        "[screenshot] {}: wrote {}x{} '{}' (scheduled {} ms, actual {} ms on the input timeline)",
        trigger, image.width, image.height, path_utf8, *scheduled_ms,
        timeline_ms ? fmt::format("{}", *timeline_ms) : std::string("<not started>"));
  } else if (timeline_ms) {
    REXSYS_INFO("[screenshot] {}: wrote {}x{} '{}' at {} ms on the input timeline", trigger,
                image.width, image.height, path_utf8, *timeline_ms);
  } else {
    REXSYS_WARN(
        "[screenshot] {}: wrote {}x{} '{}' BEFORE the input timeline started (the guest has not "
        "polled input yet, so this moment cannot be expressed as a --screenshot_at offset)",
        trigger, image.width, image.height, path_utf8);
  }
  return true;
}

void StartScreenshotScheduler(IGraphicsSystem* graphics_system) {
  const std::string& spec = REXCVAR_GET(screenshot_at);
  if (spec.empty()) {
    return;
  }
  if (g_scheduler) {
    REXSYS_WARN("[screenshot] scheduler already running; second start ignored");
    return;
  }

  std::vector<uint64_t> schedule;
  if (!ParseScheduleMs(spec, schedule) || schedule.empty()) {
    rex::FatalError("--screenshot_at: cannot parse '" + spec +
                    "' (expected comma-separated millisecond offsets, e.g. 40000,75000)");
  }
  if (!graphics_system || !graphics_system->presenter()) {
    rex::FatalError(
        "--screenshot_at is set but there is no graphics presenter to capture from "
        "(headless or native-rendering configuration)");
  }

  std::string schedule_text;
  for (uint64_t offset_ms : schedule) {
    schedule_text += (schedule_text.empty() ? "" : ", ") + std::to_string(offset_ms);
  }
  REXSYS_INFO(
      "[screenshot] --screenshot_at: {} capture(s) armed ({} ms); waiting for the guest's first "
      "controller poll (timeline t0)",
      schedule.size(), schedule_text);

  g_scheduler = new ScreenshotScheduler();
  g_schedule_pending.store(true, std::memory_order_release);
  g_scheduler->thread =
      std::thread([graphics_system, schedule = std::move(schedule)]() {
#if !REX_PLATFORM_WIN32
        pthread_setname_np(pthread_self(), "Screenshot");
#endif
        // Cleared on every exit path: completion, and early stop (shutdown) -
        // in the latter case nothing should keep waiting on it.
        struct PendingClear {
          ~PendingClear() { g_schedule_pending.store(false, std::memory_order_release); }
        } pending_clear;
        ScreenshotScheduler& s = *g_scheduler;
        std::unique_lock<std::mutex> lock(s.mutex);
        // Phase 1: t0 does not exist until the guest polls input, so poll for
        // it (10 ms granularity, only until it appears).
        while (!s.stop && !input::InputTimeline::started()) {
          s.cv.wait_for(lock, std::chrono::milliseconds(10));
        }
        if (s.stop) {
          return;
        }
        auto t0 = *input::InputTimeline::StartTime();
        // Phase 2: absolute waits against t0, immune to capture duration.
        for (uint64_t offset_ms : schedule) {
          auto target = t0 + std::chrono::milliseconds(offset_ms);
          while (!s.stop && std::chrono::steady_clock::now() < target) {
            s.cv.wait_until(lock, target);
          }
          if (s.stop) {
            return;
          }
          lock.unlock();
          CaptureScreenshot(graphics_system, offset_ms, "screenshot_at");
          lock.lock();
        }
        REXSYS_INFO("[screenshot] --screenshot_at: schedule complete ({} offsets processed)",
                    schedule.size());
      });
}

bool CaptureHostScreenshot(ui::Presenter* presenter, uint64_t offset_ms, const char* trigger) {
  if (!presenter) {
    REXSYS_WARN("[screenshot] {}: no presenter; host capture ignored", trigger);
    return false;
  }
  ui::RawImage image;
  if (!presenter->CaptureHostOutput(image)) {
    // The presenter has logged why: no paint in time, or a backend or surface
    // that cannot read its swapchain back.
    REXSYS_WARN("[screenshot] {}: host output capture failed at {} ms", trigger, offset_ms);
    return false;
  }

  std::filesystem::path dir = rex::to_path(REXCVAR_GET(screenshot_dir));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  std::lock_guard<std::mutex> lock(g_write_mutex);
  std::filesystem::path path = PickOutputPath(dir, fmt::format("screenshot_host_{}ms", offset_ms));
  if (!rex::WritePng(path, image.data.data(), image.width, image.height, image.stride)) {
    REXSYS_ERROR("[screenshot] {}: failed to write '{}'", trigger, rex::path_to_utf8(path));
    return false;
  }
  REXSYS_INFO("[screenshot] {}: wrote {}x{} host output '{}' (scheduled {} ms after the window "
              "was ready)",
              trigger, image.width, image.height,
              rex::path_to_utf8(std::filesystem::absolute(path, ec)), offset_ms);
  return true;
}

void StartHostScreenshotScheduler(ui::Presenter* presenter) {
  const std::string& spec = REXCVAR_GET(screenshot_host_at);
  if (spec.empty()) {
    return;
  }
  if (g_host_scheduler) {
    // Armed by the app when the window got its presenter; the runtime's later
    // start is expected and changes nothing.
    return;
  }

  std::vector<uint64_t> schedule;
  if (!ParseScheduleMs(spec, schedule) || schedule.empty()) {
    rex::FatalError("--screenshot_host_at: cannot parse '" + spec +
                    "' (expected comma-separated millisecond offsets, e.g. 3000,8000)");
  }
  if (!presenter) {
    rex::FatalError(
        "--screenshot_host_at is set but there is no presenter to capture the window from "
        "(headless or native-rendering configuration)");
  }

  std::string schedule_text;
  for (uint64_t offset_ms : schedule) {
    schedule_text += (schedule_text.empty() ? "" : ", ") + std::to_string(offset_ms);
  }
  REXSYS_INFO("[screenshot] --screenshot_host_at: {} capture(s) armed ({} ms from now)",
              schedule.size(), schedule_text);

  g_host_scheduler = new ScreenshotScheduler();
  g_host_schedule_pending.store(true, std::memory_order_release);
  g_host_scheduler->thread = std::thread([presenter, schedule = std::move(schedule)]() {
#if !REX_PLATFORM_WIN32
    pthread_setname_np(pthread_self(), "HostScreenshot");
#endif
    struct PendingClear {
      ~PendingClear() { g_host_schedule_pending.store(false, std::memory_order_release); }
    } pending_clear;
    ScreenshotScheduler& s = *g_host_scheduler;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(s.mutex);
    for (uint64_t offset_ms : schedule) {
      auto target = t0 + std::chrono::milliseconds(offset_ms);
      while (!s.stop && std::chrono::steady_clock::now() < target) {
        s.cv.wait_until(lock, target);
      }
      if (s.stop) {
        return;
      }
      lock.unlock();
      CaptureHostScreenshot(presenter, offset_ms, "screenshot_host_at");
      lock.lock();
    }
    REXSYS_INFO("[screenshot] --screenshot_host_at: schedule complete ({} offsets processed)",
                schedule.size());
  });
}

bool ScreenshotSchedulePending() {
  return g_schedule_pending.load(std::memory_order_acquire) ||
         g_host_schedule_pending.load(std::memory_order_acquire);
}

void StopScreenshotScheduler() { StopScheduler(g_scheduler); }

void StopHostScreenshotScheduler() { StopScheduler(g_host_scheduler); }

}  // namespace rex::system
