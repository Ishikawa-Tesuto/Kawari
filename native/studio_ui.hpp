#pragma once
#include <vulkan/vulkan.h>
struct GLFWwindow;
namespace studio {
// Main renderer thread only. No resources are created in no-preview mode.
bool ui_begin(GLFWwindow*, VkInstance, VkPhysicalDevice, VkDevice, VkQueue,
              unsigned queue_family, unsigned image_count, VkFormat, VkExtent2D, int parameter_count);
void ui_draw(VkCommandBuffer);
void ui_shutdown();
void ui_model_changed();
}
