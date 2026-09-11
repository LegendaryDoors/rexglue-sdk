/**
 * @file        system/trace_schedule.cpp
 * @brief       --trace_gpu_stream_at implementation. See trace_schedule.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/trace_schedule.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/input/input_timeline.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/interfaces/graphics.h>

#if !REX_PLATFORM_WIN32
#include <pthread.h>
#endif

REXCVAR_DEFINE_STRING(
    trace_gpu_stream_at, "", "Diagnostics",
    "One window '<start_ms>-<stop_ms>' on the input timeline (t0 = the guest's first controller "
    "poll, the same reference --input_script and --screenshot_at use), e.g. 138000-176000. "
    "A GPU stream trace is started at the first offset and stopped at the second, exactly as "
    "two F8 presses would, into --trace_gpu_prefix.");

namespace rex::system {

namespace {

struct TraceScheduler {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
};
TraceScheduler* g_scheduler = nullptr;

bool ParseWindowMs(const std::string& spec, uint64_t& start_ms, uint64_t& stop_ms) {
  const size_t dash = spec.find('-');
  if (dash == std::string::npos) {
    return false;
  }
  const std::string a = spec.substr(0, dash);
  const std::string b = spec.substr(dash + 1);
  if (a.empty() || b.empty() || a.find_first_not_of("0123456789") != std::string::npos ||
      b.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  try {
    start_ms = std::stoull(a);
    stop_ms = std::stoull(b);
  } catch (const std::exception&) {
    return false;
  }
  return stop_ms > start_ms;
}

}  // namespace

void StartTraceScheduler(IGraphicsSystem* graphics_system) {
  const std::string& spec = REXCVAR_GET(trace_gpu_stream_at);
  if (spec.empty()) {
    return;
  }
  if (g_scheduler) {
    REXSYS_WARN("[trace_at] scheduler already running; second start ignored");
    return;
  }

  uint64_t start_ms = 0;
  uint64_t stop_ms = 0;
  if (!ParseWindowMs(spec, start_ms, stop_ms)) {
    rex::FatalError("--trace_gpu_stream_at: cannot parse '" + spec +
                    "' (expected '<start_ms>-<stop_ms>' with stop after start, e.g. "
                    "138000-176000)");
  }
  if (!graphics_system) {
    rex::FatalError("--trace_gpu_stream_at is set but there is no graphics system to trace");
  }
  // The prefix cvar lives in the GPU plugin, so it is looked up by name.
  if (REXCVAR_QUERY(std::string, trace_gpu_prefix).empty()) {
    rex::FatalError("--trace_gpu_stream_at requires --trace_gpu_prefix=<dir>");
  }

  REXSYS_INFO(
      "[trace_at] --trace_gpu_stream_at: trace armed for {} ms .. {} ms; waiting for the "
      "guest's first controller poll (timeline t0)",
      start_ms, stop_ms);

  g_scheduler = new TraceScheduler();
  g_scheduler->thread = std::thread([graphics_system, start_ms, stop_ms]() {
#if !REX_PLATFORM_WIN32
    pthread_setname_np(pthread_self(), "TraceSchedule");
#endif
    TraceScheduler& s = *g_scheduler;
    std::unique_lock<std::mutex> lock(s.mutex);
    while (!s.stop && !input::InputTimeline::started()) {
      s.cv.wait_for(lock, std::chrono::milliseconds(10));
    }
    if (s.stop) {
      return;
    }
    const auto t0 = *input::InputTimeline::StartTime();
    const auto start = t0 + std::chrono::milliseconds(start_ms);
    while (!s.stop && std::chrono::steady_clock::now() < start) {
      s.cv.wait_until(lock, start);
    }
    if (s.stop) {
      return;
    }
    REXSYS_INFO("[trace_at] starting the stream trace ({} ms on the input timeline)", start_ms);
    graphics_system->BeginTracing();

    const auto stop = t0 + std::chrono::milliseconds(stop_ms);
    while (!s.stop && std::chrono::steady_clock::now() < stop) {
      s.cv.wait_until(lock, stop);
    }
    // Reached either by the schedule or by shutdown; both must close the file
    // so what was recorded replays.
    REXSYS_INFO("[trace_at] stopping the stream trace ({} ms on the input timeline{})", stop_ms,
                s.stop ? ", early: shutdown" : "");
    graphics_system->EndTracing();
  });
}

void StopTraceScheduler() {
  if (!g_scheduler) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_scheduler->mutex);
    g_scheduler->stop = true;
  }
  g_scheduler->cv.notify_all();
  if (g_scheduler->thread.joinable()) {
    g_scheduler->thread.join();
  }
  delete g_scheduler;
  g_scheduler = nullptr;
}

}  // namespace rex::system
