/**
 * @file        system/stall_dump.cpp
 * @brief       Whole-system state snapshot for diagnosing hangs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/stall_dump.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#if !REX_PLATFORM_WIN32
#include <pthread.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_INT32(stall_watchdog_ms, 20000, "Diagnostics",
                     "Dump the whole-system stall state once if the title makes no progress for "
                     "this many milliseconds. Progress is a GPU swap. 0 disables. Diagnostic only "
                     "- it reports, it never changes behaviour.");

namespace rex::system {

namespace {

// The blocked-on critical section lives on XThread itself: a thread_local
// goes stale, and a global side table serialises every guest lock.

// Field offsets within X_RTL_CRITICAL_SECTION. The canonical definition lives
// in xboxkrnl_rtl.cpp and is not in a public header. Keep them in step.
constexpr uint32_t kCsRecursionCountOffset = 0x14;
constexpr uint32_t kCsOwningThreadOffset = 0x18;

const char* ObjectTypeName(XObject::Type type) {
  switch (type) {
    case XObject::Type::Event:
      return "Event";
    case XObject::Type::Mutant:
      return "Mutant";
    case XObject::Type::Semaphore:
      return "Semaphore";
    case XObject::Type::Timer:
      return "Timer";
    default:
      return "Other";
  }
}

std::mutex& SectionsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::function<void()>, std::less<>>& Sections() {
  static std::map<std::string, std::function<void()>, std::less<>> sections;
  return sections;
}

// Set from the SIGQUIT handler, so it must stay async-signal-safe: one atomic
// store and nothing else. Formatting happens on the watchdog thread.
std::atomic<bool> g_dump_requested{false};
std::atomic<int64_t> g_last_heartbeat_ms{0};
std::atomic<bool> g_watchdog_started{false};

int64_t SteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if !REX_PLATFORM_WIN32
extern "C" void StallDumpSignalHandler(int) {
  g_dump_requested.store(true, std::memory_order_release);
}
#endif

}  // namespace

void RegisterStallSection(std::string name, std::function<void()> emit) {
  std::lock_guard<std::mutex> lock(SectionsMutex());
  Sections()[std::move(name)] = std::move(emit);
}

void UnregisterStallSection(std::string_view name) {
  std::lock_guard<std::mutex> lock(SectionsMutex());
  auto it = Sections().find(name);
  if (it != Sections().end()) {
    Sections().erase(it);
  }
}

void NoteStallHeartbeat() {
  g_last_heartbeat_ms.store(SteadyNowMs(), std::memory_order_relaxed);
}

void StartStallWatchdog() {
  if (g_watchdog_started.exchange(true)) {
    return;
  }

#if !REX_PLATFORM_WIN32
  struct sigaction action = {};
  action.sa_handler = &StallDumpSignalHandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  if (sigaction(SIGQUIT, &action, nullptr) != 0) {
    REXLOG_WARN("Stall watchdog: could not install the SIGQUIT handler; `kill -QUIT` will not dump");
  }
#endif

  NoteStallHeartbeat();

  std::thread([]() {
#if !REX_PLATFORM_WIN32
    pthread_setname_np(pthread_self(), "Stall watchdog");
#endif
    bool watchdog_fired = false;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      if (g_dump_requested.exchange(false, std::memory_order_acq_rel)) {
        // On-demand dumps are always allowed to repeat: the whole point is to
        // sample a live hang more than once and see what changed.
        DumpStallState("SIGQUIT requested by the user");
        watchdog_fired = false;
      }

      const int32_t watchdog_ms = REXCVAR_GET(stall_watchdog_ms);
      if (watchdog_ms <= 0 || watchdog_fired) {
        continue;
      }
      const int64_t since =
          SteadyNowMs() - g_last_heartbeat_ms.load(std::memory_order_relaxed);
      if (since >= watchdog_ms) {
        watchdog_fired = true;
        DumpStallState(fmt::format(
            "no GPU swap for {} ms (--stall_watchdog_ms={}); reporting once, "
            "send SIGQUIT for another sample",
            since, watchdog_ms));
      }
    }
  }).detach();

#if !REX_PLATFORM_WIN32
  REXLOG_INFO("Stall watchdog armed: kill -QUIT {} to dump, auto-dump after {} ms without a swap",
              getpid(), REXCVAR_GET(stall_watchdog_ms));
#else
  REXLOG_INFO("Stall watchdog armed: auto-dump after {} ms without a swap (no SIGQUIT on Windows)",
              REXCVAR_GET(stall_watchdog_ms));
#endif
}

void NoteCriticalSectionBlock(uint32_t critical_section_ptr) {
  if (XThread* thread = XThread::GetCurrentThread()) {
    thread->set_blocked_on_critical_section(critical_section_ptr);
  }
}

void ClearCriticalSectionBlock() {
  if (XThread* thread = XThread::GetCurrentThread()) {
    thread->set_blocked_on_critical_section(0);
  }
}

void DumpStallState(std::string_view reason) {
  KernelState* kernel_state = KernelState::shared();
  if (!kernel_state) {
    REXLOG_ERROR("==== STALL DUMP ({}) ==== no kernel state; nothing to report", reason);
    return;
  }

  REXLOG_ERROR("======== STALL DUMP: {} ========", reason);

  auto* memory = kernel_state->memory();
  auto threads = kernel_state->object_table()->GetObjectsByType<XThread>(XObject::Type::Thread);

  // KTHREAD pointer -> readable thread name, so a lock owner reads as a name
  // rather than an address the reader then has to chase by hand.
  std::map<uint32_t, std::string> kthread_names;
  for (const auto& thread : threads) {
    if (thread && thread->guest_object()) {
      kthread_names[thread->guest_object()] =
          thread->thread_name().empty() ? fmt::format("tid {}", thread->thread_id())
                                        : thread->thread_name();
    }
  }
  auto describe_kthread = [&](uint32_t kthread) -> std::string {
    if (!kthread) {
      return "unowned";
    }
    auto it = kthread_names.find(kthread);
    return it != kthread_names.end() ? fmt::format("{:#010X} = {}", kthread, it->second)
                                     : fmt::format("{:#010X} = <unknown thread>", kthread);
  };

  REXLOG_ERROR("-- guest threads ({}) --", threads.size());
  for (const auto& thread : threads) {
    if (!thread) {
      continue;
    }
    std::string waiting;
    uint32_t cs_ptr = thread->blocked_on_critical_section();
    if (cs_ptr) {
      // A critical section pointer must be 4-byte aligned and inside the guest
      // address space. Anything else is a stale or corrupt registry entry.
      if ((cs_ptr & 0x3) != 0 || cs_ptr < 0x10000) {
        waiting = fmt::format("BLOCKED on CS {:#010X} (IMPLAUSIBLE POINTER - not reporting owner)",
                              cs_ptr);
      } else {
        auto* owner_ptr =
            memory->TranslateVirtual<rex::be<uint32_t>*>(cs_ptr + kCsOwningThreadOffset);
        auto* recursion_ptr =
            memory->TranslateVirtual<rex::be<int32_t>*>(cs_ptr + kCsRecursionCountOffset);
        if (owner_ptr && recursion_ptr) {
          waiting = fmt::format("BLOCKED on CS {:#010X} (owner {}, recursion {})", cs_ptr,
                                describe_kthread(uint32_t(*owner_ptr)), int32_t(*recursion_ptr));
        } else {
          waiting = fmt::format("BLOCKED on CS {:#010X} (unreadable)", cs_ptr);
        }
      }
    }
    const std::string& name = thread->thread_name();
    REXLOG_ERROR("  tid={:<4} handle={:08X} {:<30} running={} suspend={} {}", thread->thread_id(),
                 uint32_t(thread->handle()), name.empty() ? "<unnamed>" : name,
                 thread->is_running() ? "yes" : "NO ", thread->suspend_count(), waiting);
  }

  // Waitable kernel objects. A title deadlocked on an event that is never
  // signalled looks identical to one deadlocked on a lock without this.
  for (auto type : {XObject::Type::Event, XObject::Type::Mutant, XObject::Type::Semaphore,
                    XObject::Type::Timer}) {
    auto objects = kernel_state->object_table()->GetObjectsByType<XObject>(type);
    if (objects.empty()) {
      continue;
    }
    // Only report objects that are NOT signalled; a signalled event cannot be
    // what a thread is stuck on. signal_state is at 0x04 of X_DISPATCH_HEADER.
    size_t unsignalled = 0;
    std::string lines;
    for (const auto& object : objects) {
      if (!object || !object->guest_object()) {
        continue;
      }
      auto* signal_state = memory->TranslateVirtual<rex::be<uint32_t>*>(object->guest_object() + 4);
      if (!signal_state || uint32_t(*signal_state) != 0) {
        continue;
      }
      ++unsignalled;
      lines += fmt::format("  handle={:08X} guest={:#010X} {}\n", uint32_t(object->handle()),
                           object->guest_object(),
                           object->name().empty() ? "<unnamed>" : object->name());
    }
    REXLOG_ERROR("-- {} objects: {} total, {} UNSIGNALLED --", ObjectTypeName(type), objects.size(),
                 unsignalled);
    if (!lines.empty()) {
      REXLOG_ERROR("{}", lines);
    }
  }

  // Subsystem sections last, so the guest picture reads first. Copied out under
  // the lock and invoked without it, so a section cannot deadlock.
  std::vector<std::pair<std::string, std::function<void()>>> sections;
  {
    std::lock_guard<std::mutex> lock(SectionsMutex());
    sections.assign(Sections().begin(), Sections().end());
  }
  for (const auto& [name, emit] : sections) {
    REXLOG_ERROR("-- {} --", name);
    emit();
  }

  REXLOG_ERROR("======== END STALL DUMP ========");
}

}  // namespace rex::system
