#pragma once
/**
 * @file        system/trace_schedule.h
 * @brief       --trace_gpu_stream_at: start and stop a GPU stream trace at
 *              offsets on the input timeline.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

namespace rex::system {

class IGraphicsSystem;

// Starts the --trace_gpu_stream_at scheduler thread; no-op when the cvar is
// empty. Fatal on a malformed window or a missing --trace_gpu_prefix.
void StartTraceScheduler(IGraphicsSystem* graphics_system);

// Joins the scheduler thread, ending an open trace first. Safe to call when
// never started. Must run before the graphics system shuts down.
void StopTraceScheduler();

}  // namespace rex::system
