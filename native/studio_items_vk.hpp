#pragma once
#include <vulkan/vulkan.h>
namespace studio { namespace items {
void init(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,unsigned family);
void shutdown();
void render(VkImage target,VkFormat format,unsigned w,unsigned h);
}}
