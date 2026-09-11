#pragma once
/**
 * @file        system/script_exit.h
 * @brief       --input_script_exit: clean shutdown when an unattended run is
 *              done.
 *
 * @remarks     A monitor thread waits until the --input_script timeline has
 *              played out and every --screenshot_at capture has been
 *              written, waits --input_script_exit_delay_ms, then requests
 *              the window's RequestClose() on the UI thread. It never
 *              terminates the process itself. Off by default; fatal when set
 *              without --input_script or without a windowed app context.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

namespace rex::ui {
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace rex::system {

// Starts the monitor thread; no-op when --input_script_exit is false.
// `window` may be null, in which case it falls back to QuitFromUIThread.
void StartScriptExitMonitor(ui::WindowedAppContext* app_context, ui::Window* window);

// Joins the monitor thread. Safe to call when never started.
void StopScriptExitMonitor();

}  // namespace rex::system
