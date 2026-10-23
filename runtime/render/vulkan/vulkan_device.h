// The Vulkan IRenderDevice (the Quest's renderer back end), the counterpart of metal_device.mm: textures and their mips,
// targets (multisample with a depth resolve), the passes, the resolve into the OpenXR swapchain, frames, read-back, the
// combiner pipelines with their warm-up and the pipeline cache, the room-lit look's depth maps (shadow, underside, water
// footprint), the AO bake and composite. No environment probe: the reduction and copies are no-ops.
//
// Threading: every IRenderDevice call is made on one thread (the render thread). end_frame's on_gpu_done runs on the
// device's completion thread.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "render_device.h"
namespace ar {
// The same fields as QxVulkan (qx_platform.h), plus where the pipeline cache lives ("" = no file).
// The creator of the VkDevice states what it enabled: device_extensions, every extension it enabled (the device uses
// VK_KHR_create_renderpass2 + VK_KHR_depth_stencil_resolve for multisample depth resolves and
// VK_EXT_extended_dynamic_state for a dynamic cull mode only when listed here); alpha_to_one, the alphaToOne feature
// (enable it when the physical device offers it: alpha-to-coverage pipelines then also set alpha-to-one, as Metal).
struct VulkanDeviceInit { VkInstance instance; VkPhysicalDevice physical; VkDevice device; uint32_t queue_family; VkQueue queue;
                          PFN_vkGetInstanceProcAddr get_instance_proc_addr; std::string pipeline_cache_path;
                          std::vector<std::string> device_extensions = {}; bool alpha_to_one = false; };
// What ExternalView::color (and ::depth) point at. view is the image's 2D_ARRAY view. The
// image is in COLOR_ATTACHMENT_OPTIMAL (a depth image: DEPTH_STENCIL_ATTACHMENT_OPTIMAL) when encode_resolve records and
// is left in it.
struct VulkanExternalImage { VkImage image; VkImageView view; VkFormat format; uint32_t width, height, layers; };
// nullptr when the device's own initialisation fails (logged as "AR_VULKAN init failed: ...").
std::unique_ptr<IRenderDevice> create_vulkan_device(const VulkanDeviceInit &init);
// False when anything in the open (or last) frame could not be vouched for: begin_frame failed, a target has no images
// after a (re)size, or an encode_resolve recorded nothing. Reset by each begin_frame. d must come from create_vulkan_device.
bool vulkan_device_frame_ok(IRenderDevice &d);
// For host tests: accept a depth resolve mode other than MAX (lavapipe offers SAMPLE_ZERO only), so supported_samples can
// exceed 1 and the multisample path runs there. Call before creating multisample targets. d must come from
// create_vulkan_device.
void vulkan_device_accept_any_depth_resolve(IRenderDevice &d);
}
