// The Vulkan IRenderDevice (the Quest's renderer back end), the counterpart of metal_device.mm. Plan 1 implements the
// subset the cube needs: textures, targets, the passes, the resolve into the OpenXR swapchain, frames, read-back and the
// combiner pipelines; the depth maps, AO, the environment reduction and composite are stubs (plan 2).
//
// Threading: every IRenderDevice call is made on one thread (the render thread). end_frame's on_gpu_done runs on the
// device's completion thread.
#pragma once
#include <memory>
#include <string>
#include <vulkan/vulkan.h>
#include "render_device.h"
namespace ar {
// The same fields as QxVulkan (qx_platform.h), plus where the pipeline cache lives ("" = no file).
struct VulkanDeviceInit { VkInstance instance; VkPhysicalDevice physical; VkDevice device; uint32_t queue_family; VkQueue queue;
                          PFN_vkGetInstanceProcAddr get_instance_proc_addr; std::string pipeline_cache_path; };
// Same layout as QxSwapchainImage: what ExternalView::color (and ::depth) point at. view is the image's 2D_ARRAY view. The
// image is in COLOR_ATTACHMENT_OPTIMAL (a depth image: DEPTH_STENCIL_ATTACHMENT_OPTIMAL) when encode_resolve records and
// is left in it.
struct VulkanExternalImage { VkImage image; VkImageView view; VkFormat format; uint32_t width, height, layers; };
std::unique_ptr<IRenderDevice> create_vulkan_device(const VulkanDeviceInit &init);
}
