/**
 * @file        kernel/crt/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>

#if defined(__linux__)
#include <execinfo.h>
#endif

namespace rex::kernel::crt {

// REX_WATCH_MEM=<phys_hex>:<len_hex> logs every hooked CRT memory write into
// that guest-physical range, with a host backtrace. Diagnostic only.

struct WatchMemRange {
  bool enabled = false;
  uint64_t phys_base = 0;
  uint64_t phys_len = 0;
};

static WatchMemRange GetWatchMemRange() {
  WatchMemRange r;
  const char* spec = getenv("REX_WATCH_MEM");
  if (!spec) {
    return r;
  }
  char* sep = nullptr;
  r.phys_base = strtoull(spec, &sep, 16);
  if (!sep || *sep != ':') {
    return r;
  }
  r.phys_len = strtoull(sep + 1, nullptr, 16);
  r.enabled = r.phys_len != 0;
  return r;
}

// Translate a host pointer inside any guest view back to a guest-physical
// offset. Returns UINT64_MAX when the pointer is not in a physical view.
static uint64_t HostPtrToGuestPhysical(const void* p) {
  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state) {
    return UINT64_MAX;
  }
  uint8_t* membase = kernel_state->memory()->virtual_membase();
  if (!membase || p < membase) {
    return UINT64_MAX;
  }
  uint64_t delta = reinterpret_cast<const uint8_t*>(p) - membase;
  if (delta >= 0x100000000ull) {
    // Physical view (virtual_membase + 4 GiB).
    uint64_t phys = delta - 0x100000000ull;
    return phys < 0x20000000ull ? phys : UINT64_MAX;
  }
  if (delta >= 0xA0000000ull && delta < 0xC0000000ull) {
    return delta - 0xA0000000ull;
  }
  if (delta >= 0xC0000000ull && delta < 0xE0000000ull) {
    return delta - 0xC0000000ull;
  }
  if (delta >= 0xE0000000ull && delta < 0xFFD00000ull) {
    // NOTE: the 0xE0000000 view carries a 4 KB host offset quirk; for a
    // multi-megabyte watch window the 0x1000 slack is acceptable.
    return delta - 0xE0000000ull;
  }
  return UINT64_MAX;
}

static void WatchMemCheck(const char* op, const void* dst, size_t n, int val) {
  static const WatchMemRange watch = GetWatchMemRange();
  if (!watch.enabled || n == 0) {
    return;
  }
  uint64_t phys = HostPtrToGuestPhysical(dst);
  if (phys == UINT64_MAX) {
    return;
  }
  if (phys >= watch.phys_base + watch.phys_len || phys + n <= watch.phys_base) {
    return;
  }
  static std::atomic<int> hits{0};
  int hit = hits.fetch_add(1);
  if (hit >= 64) {
    return;  // Rate limit; the first writers are the interesting ones.
  }
  REXKRNL_WARN("REX_WATCH_MEM hit #{}: {} dst_phys={:#x} len={:#x} val={:#x}", hit, op, phys, n,
               val);
#if defined(__linux__)
  void* frames[16];
  int frame_count = backtrace(frames, 16);
  char** symbols = backtrace_symbols(frames, frame_count);
  if (symbols) {
    for (int i = 0; i < frame_count; ++i) {
      REXKRNL_WARN("REX_WATCH_MEM   frame {}: {}", i, symbols[i]);
    }
    free(symbols);
  }
#endif
}

// ---------------------------------------------------------------------------
// Standard memory operations
// ---------------------------------------------------------------------------

static void* native_memcpy(void* dst, const void* src, size_t n) {
  WatchMemCheck("memcpy", dst, n, -1);
  return std::memcpy(dst, src, n);
}

static void* native_memmove(void* dst, const void* src, size_t n) {
  WatchMemCheck("memmove", dst, n, -1);
  return std::memmove(dst, src, n);
}

static void* native_memset(void* dst, int val, size_t n) {
  WatchMemCheck("memset", dst, n, val);
  return std::memset(dst, val, n);
}

static void* native_memchr(const void* ptr, int val, size_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------

static void* native_XMemCpy(void* dst, const void* src, size_t n) {
  WatchMemCheck("XMemCpy", dst, n, -1);
  return std::memcpy(dst, src, n);
}

static void* native_XMemSet(void* dst, int val, size_t n) {
  WatchMemCheck("XMemSet", dst, n, val);
  return std::memset(dst, val, n);
}

static void* native_XMemSet128(void* dst, int val, size_t n) {
  WatchMemCheck("XMemSet128", dst, n, val);
  return std::memset(dst, val, n);
}

static void* native_memset_vmx(void* dst, int val, size_t n) {
  WatchMemCheck("memset_vmx", dst, n, val);
  return std::memset(dst, val, n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

static int native_memcpy_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  WatchMemCheck("memcpy_s", dst, count, -1);
  std::memcpy(dst, src, count);
  return 0;
}

static int native_memmove_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  WatchMemCheck("memmove_s", dst, count, -1);
  std::memmove(dst, src, count);
  return 0;
}

}  // namespace rex::kernel::crt

REX_HOOK(rexcrt_memcpy, rex::kernel::crt::native_memcpy)
REX_HOOK(rexcrt_memmove, rex::kernel::crt::native_memmove)
REX_HOOK(rexcrt_memset, rex::kernel::crt::native_memset)
REX_HOOK(rexcrt_memchr, rex::kernel::crt::native_memchr)
REX_HOOK(rexcrt_XMemCpy, rex::kernel::crt::native_XMemCpy)
REX_HOOK(rexcrt_XMemSet, rex::kernel::crt::native_XMemSet)
REX_HOOK(rexcrt_XMemSet128, rex::kernel::crt::native_XMemSet128)
REX_HOOK(rexcrt_memset_vmx, rex::kernel::crt::native_memset_vmx)
REX_HOOK(rexcrt_memcpy_s, rex::kernel::crt::native_memcpy_s)
REX_HOOK(rexcrt_memmove_s, rex::kernel::crt::native_memmove_s)
