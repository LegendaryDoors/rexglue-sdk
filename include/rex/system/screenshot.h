#pragma once
/**
 * @file        system/screenshot.h
 * @brief       Live screenshot capture of the presented guest frame.
 *
 * @remarks     F7 captures the current frame and logs its timestamp on the
 *              input timeline; --screenshot_at=<ms,...> captures at those
 *              offsets on the same timeline for unattended runs.
 *
 *              The PNG is the guest output image the GPU emulation last
 *              submitted for presentation, at guest frontbuffer resolution,
 *              converted 10bpc to 8bpc. It is not the host swapchain: host
 *              scaling, dithering, letterboxing and overlays are absent.
 *
 *              --screenshot_host_at=<ms,...> captures the window instead,
 *              which is what the display shows, and does not wait for the
 *              guest, so screens drawn before the game starts are captured.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <optional>

namespace rex::ui {
class Presenter;
}

namespace rex::system {

class IGraphicsSystem;

// Captures the latest presented guest frame as a PNG in --screenshot_dir.
// `scheduled_ms` names the file; nullopt names it after the current time.
bool CaptureScreenshot(IGraphicsSystem* graphics_system, std::optional<uint64_t> scheduled_ms,
                       const char* trigger);

// Starts the --screenshot_at scheduler thread; no-op when the cvar is empty.
// Fatal on a malformed list or a missing presenter.
void StartScreenshotScheduler(IGraphicsSystem* graphics_system);

// Joins the scheduler thread. Safe to call when never started. Must run
// before the graphics system it captures from shuts down.
void StopScreenshotScheduler();

// Captures what the window shows into --screenshot_dir as
// screenshot_host_<offset_ms>ms.png. Must not be called from the UI thread.
bool CaptureHostScreenshot(ui::Presenter* presenter, uint64_t offset_ms, const char* trigger);

// Starts the --screenshot_host_at scheduler thread; offsets count from this
// call. No-op when the cvar is empty; fatal on a malformed list.
void StartHostScreenshotScheduler(ui::Presenter* presenter);

// Joins the host scheduler thread. Safe to call when never started. Must run
// before the presenter it captures from goes away.
void StopHostScreenshotScheduler();

// True while --screenshot_at or --screenshot_host_at captures are still
// outstanding. False when neither was set.
bool ScreenshotSchedulePending();

}  // namespace rex::system
