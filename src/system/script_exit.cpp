/**
 * @file        system/script_exit.cpp
 * @brief       --input_script_exit implementation. See script_exit.h for the
 *              contract and ordering guarantees.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/script_exit.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/input/input_playback.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/screenshot.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if !REX_PLATFORM_WIN32
#include <pthread.h>
#endif

REXCVAR_DECLARE(std::string, input_script);

REXCVAR_DEFINE_BOOL(input_script_exit, false, "Input",
                    "Shut the title down cleanly once the --input_script timeline has fully "
                    "played out and every --screenshot_at capture has been written, instead of "
                    "idling until an external timeout. Requires --input_script.");
REXCVAR_DEFINE_INT32(input_script_exit_delay_ms, 2000, "Input",
                     "Settling delay in milliseconds between the last scripted event / capture "
                     "and the shutdown requested by --input_script_exit.")
    .range(0, 600000);

namespace rex::system {

namespace {

struct ScriptExitMonitor {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
};
ScriptExitMonitor* g_monitor = nullptr;

}  // namespace

void StartScriptExitMonitor(ui::WindowedAppContext* app_context, ui::Window* window) {
  if (!REXCVAR_GET(input_script_exit)) {
    return;
  }
  if (g_monitor) {
    REXSYS_WARN("[script_exit] monitor already running; second start ignored");
    return;
  }
  // Misconfigurations fail loudly: an unattended run whose exit condition can
  // never fire would sit until the external timeout.
  if (REXCVAR_GET(input_script).empty()) {
    rex::FatalError("--input_script_exit requires --input_script (there is no script whose "
                    "completion could trigger the exit)");
  }
  if (!app_context) {
    rex::FatalError("--input_script_exit requires a windowed app context (no UI loop to request "
                    "the shutdown on)");
  }

  const int32_t delay_ms = REXCVAR_GET(input_script_exit_delay_ms);
  REXSYS_INFO(
      "[script_exit] armed: will exit {} ms after the input script and any --screenshot_at "
      "captures complete",
      delay_ms);

  g_monitor = new ScriptExitMonitor();
  g_monitor->thread = std::thread([app_context, window, delay_ms]() {
#if !REX_PLATFORM_WIN32
    pthread_setname_np(pthread_self(), "ScriptExit");
#endif
    ScriptExitMonitor& m = *g_monitor;
    std::unique_lock<std::mutex> lock(m.mutex);
    auto poll = std::chrono::milliseconds(100);
    // Phase 1: the script's last event has been applied to guest input.
    while (!m.stop && !input::ScriptReplayComplete()) {
      m.cv.wait_for(lock, poll);
    }
    if (m.stop) {
      return;
    }
    // Phase 2: every --screenshot_at capture has been written. Offsets may lie
    // beyond the script's last event. False immediately when unset.
    while (!m.stop && system::ScreenshotSchedulePending()) {
      m.cv.wait_for(lock, poll);
    }
    if (m.stop) {
      return;
    }
    // Phase 3: settling delay, so in-flight work (e.g. the PNG write that
    // cleared the pending flag a moment ago) lands before teardown begins.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    while (!m.stop && std::chrono::steady_clock::now() < deadline) {
      m.cv.wait_until(lock, deadline);
    }
    if (m.stop) {
      return;
    }
    REXSYS_INFO(
        "[script_exit] input script and screenshot schedule complete; requesting clean shutdown");
    // Fire-and-forget onto the UI thread; never terminate from this thread.
    // RequestClose is the same path closing the window takes.
    app_context->CallInUIThreadDeferred([app_context, window]() {
      if (window) {
        window->RequestClose();
      }
      app_context->QuitFromUIThread();
    });
  });
}

void StopScriptExitMonitor() {
  if (!g_monitor) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_monitor->mutex);
    g_monitor->stop = true;
  }
  g_monitor->cv.notify_all();
  if (g_monitor->thread.joinable()) {
    g_monitor->thread.join();
  }
  delete g_monitor;
  g_monitor = nullptr;
}

}  // namespace rex::system
