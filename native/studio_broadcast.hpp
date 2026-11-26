#pragma once
#include <vulkan/vulkan.h>
namespace studio {
bool broadcast_requested();
// Called from the existing render thread. No Vulkan resources on the UI thread.
bool broadcast_extensions(VkPhysicalDevice gpu);
void broadcast_init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device,
                    VkQueue queue, uint32_t family);
void broadcast_frame(VkImage source, VkFormat format, uint32_t width, uint32_t height);
void broadcast_shutdown();
}
