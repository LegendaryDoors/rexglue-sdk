/**
 * @file        graphics/vulkan/trace_viewer.cpp
 * @brief       Vulkan backend for the GPU trace viewer, plus its entry point
 *
 * @remarks     Replays a .xtr GPU trace a command at a time, showing register
 *              state, shaders (guest ucode and translated SPIR-V) and vertex
 *              and index buffers. A separate executable rather than an entry
 *              point in rexgpu-xenos, which every title loads.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/trace_viewer.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

namespace rex::graphics::vulkan {

class VulkanTraceViewer final : public TraceViewer {
 public:
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& app_context) {
    return std::unique_ptr<rex::ui::WindowedApp>(new VulkanTraceViewer(app_context));
  }

 protected:
  std::unique_ptr<GraphicsSystem> CreateGraphicsSystem() override {
    return std::unique_ptr<GraphicsSystem>(new VulkanGraphicsSystem());
  }

  // Not implemented, and deliberately not faked: sampling a render target from
  // the UI pass needs a borrowed-image path and layout transitions first.
  uintptr_t GetColorRenderTarget(uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
                                 xenos::ColorRenderTargetFormat format) override {
    (void)pitch;
    (void)samples;
    (void)base;
    (void)format;
    return 0;
  }

  uintptr_t GetDepthRenderTarget(uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
                                 xenos::DepthRenderTargetFormat format) override {
    (void)pitch;
    (void)samples;
    (void)base;
    (void)format;
    return 0;
  }

  uintptr_t GetTextureEntry(const TextureInfo& texture_info,
                            const SamplerInfo& sampler_info) override {
    (void)texture_info;
    (void)sampler_info;
    return 0;
  }

 private:
  explicit VulkanTraceViewer(rex::ui::WindowedAppContext& app_context)
      : TraceViewer(app_context, "rex-trace-viewer") {}
};

}  // namespace rex::graphics::vulkan

// XE_UI_WINDOWED_APPS_IN_LIBRARY is Android-only, so on desktop this defines
// rex::ui::GetWindowedAppCreator(): one app per executable.
REX_DEFINE_APP(rex_trace_viewer, rex::graphics::vulkan::VulkanTraceViewer::Create);
