#include <rex/graphics/diagnostic_gate.h>

#include <atomic>
#include <cstdlib>

namespace rex::graphics::diag {

namespace {
std::atomic<uint64_t> g_current_submission{0};
std::atomic<uint32_t> g_draw_count{0};

uint64_t FirstLoggedSubmission() {
  static const uint64_t first = [] {
    const char* value = std::getenv("REX_LOG_FROM_SUB");
    return value ? std::strtoull(value, nullptr, 10) : 0;
  }();
  return first;
}
}  // namespace

bool LogGateOpen() {
  return g_current_submission.load(std::memory_order_relaxed) >= FirstLoggedSubmission();
}

void SetCurrentSubmission(uint64_t submission) {
  if (g_current_submission.exchange(submission, std::memory_order_relaxed) != submission) {
    g_draw_count.store(0, std::memory_order_relaxed);
  }
}

uint64_t CurrentSubmission() {
  return g_current_submission.load(std::memory_order_relaxed);
}

void BeginDraw() {
  g_draw_count.fetch_add(1, std::memory_order_relaxed);
}

uint32_t CurrentDrawIndex() {
  uint32_t count = g_draw_count.load(std::memory_order_relaxed);
  return count ? count - 1 : 0;
}

bool VerifyTextures() {
  static const bool enabled = [] {
    const char* value = std::getenv("REX_VERIFY_TEXTURES");
    return value && value[0] && value[0] != '0';
  }();
  return enabled;
}

uint32_t LoggedTextureBase() {
  static const uint32_t base = [] {
    const char* value = std::getenv("REX_LOG_TEXTURE_BASE");
    return value ? uint32_t(std::strtoul(value, nullptr, 16)) : 0u;
  }();
  return base;
}

bool RangeCoversLoggedTexture(uint32_t start, uint32_t length) {
  uint32_t base = LoggedTextureBase();
  return base != 0 && start <= base && base - start < length;
}

}  // namespace rex::graphics::diag
