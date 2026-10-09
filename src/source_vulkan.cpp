// source_vulkan -- Vulkan IFrameSource: phase 2, not implemented yet. See docs/VULKAN.md for the plan.
// The layer already detects Vulkan sessions and records VkImage swapchain images; this factory is the only missing piece.
#include "frame_source.h"
#include "log.h"

std::unique_ptr<IFrameSource> CreateVulkanFrameSource() {
    LOGW("Vulkan not yet supported (phase 2, see docs/VULKAN.md) -- capture disabled, the game runs normally");
    return nullptr;
}
