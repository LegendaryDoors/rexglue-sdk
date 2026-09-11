/**
 * @file        system/stall_dump.h
 * @brief       Whole-system state snapshot for diagnosing hangs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Prints every guest thread and its state, which critical
 *              section each blocked thread waits on and who owns it, and
 *              every waitable kernel object with its signal state. Callers
 *              layer their own sections on top. There is no guest program
 *              counter: static recompilation has none, and the host call
 *              stack is the guest call stack.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace rex::system {

// Prints a snapshot of guest kernel state to the log. `reason` names the
// trigger. Safe to call from any thread; takes no guest locks.
void DumpStallState(std::string_view reason);

// Registers a section that contributes to every dump, whatever triggered it.
// `emit` is called with no locks held. A repeated name replaces it.
void RegisterStallSection(std::string name, std::function<void()> emit);
void UnregisterStallSection(std::string_view name);

// Starts the on-demand and automatic triggers: kill -QUIT dumps at once,
// and --stall_watchdog_ms dumps once after that long without a heartbeat.
void StartStallWatchdog();
void NoteStallHeartbeat();

// Records that the calling guest thread is about to block on a critical
// section, and clears it once acquired. Feeds the blocked-on/owned-by chain.
void NoteCriticalSectionBlock(uint32_t critical_section_ptr);
void ClearCriticalSectionBlock();

}  // namespace rex::system
