/**
 * @file        graphics/vulkan/trace_dump.cpp
 * @brief       Vulkan backend for the headless GPU trace dumper, and its entry point
 *
 * @remarks     Replays a .xtr GPU trace to a chosen frame and command and
 *              writes the guest output as a PNG. No window, no GUI and no
 *              input, so a frame can be bisected draw by draw. Companion to
 *              rex-trace-viewer, which is the interactive version.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/trace_dump.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/logging.h>

namespace rex::graphics::vulkan {

class VulkanTraceDump final : public TraceDump {
 public:
  VulkanTraceDump() = default;

 protected:
  std::unique_ptr<GraphicsSystem> CreateGraphicsSystem() override {
    return std::unique_ptr<GraphicsSystem>(new VulkanGraphicsSystem());
  }

  // Hooks for driving a host graphics debugger around the replay. No-ops:
  // those tools bound a frame at vkQueuePresentKHR, guest work does not.
  void BeginHostCapture() override {}
  void EndHostCapture() override {}
};

}  // namespace rex::graphics::vulkan

int main(int argc, char** argv) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  // TraceDump::Main takes argv-style arguments: [0] is the program name, [1]
  // the trace path, [2] an optional output prefix.
  std::vector<std::string> args;
  args.reserve(remaining.size() + 1);
  args.emplace_back(argc > 0 ? argv[0] : "rex-trace-dump");
  for (const auto& arg : remaining) {
    args.push_back(arg);
  }

  rex::graphics::vulkan::VulkanTraceDump dump;
  return dump.Main(args);
}
