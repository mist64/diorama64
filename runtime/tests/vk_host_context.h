// The host tests' Vulkan device: lavapipe (llvmpipe) when present, with the features and extensions the Quest device
// enables (qx_platform.cpp create_vulkan), so the emitted shaders load the same way on both.
#pragma once
#include <string>
#include "vulkan_device.h"
namespace ar {
// false (and *err) when no instance or no usable device exists: the caller skips.
bool vk_host_create(VulkanDeviceInit *out, std::string *err);
void vk_host_destroy(const VulkanDeviceInit &init);
}
