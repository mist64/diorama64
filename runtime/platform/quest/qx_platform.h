#pragma once
#include <stdint.h>
#include <string>
#include <vulkan/vulkan.h>
#include "ar_platform.h"
#include "render_device.h"

struct android_app;

namespace ar {

struct QxVulkan { VkInstance instance; VkPhysicalDevice physical; VkDevice device; uint32_t queue_family; VkQueue queue;
                  PFN_vkGetInstanceProcAddr get_instance_proc_addr; };
struct QxSwapchainImage { VkImage image; VkImageView view; VkFormat format; uint32_t width, height, layers; };  // what ExternalView::color points at

// The Quest platform: an OpenXR session on a Vulkan device (XR_KHR_vulkan_enable2), one stereo array swapchain and an
// XR_FB_passthrough layer under it. All members are for the render thread except api()->get_pad (any thread).
class QxPlatform {
public:
    struct Config { bool passthrough = true; float refresh_hz = 90.f; ar_pose fixed_table; bool fixed_table_valid = false; };
    QxPlatform(android_app *app, const Config &c);
    ~QxPlatform();
    QxPlatform(const QxPlatform &) = delete;
    QxPlatform &operator=(const QxPlatform &) = delete;
    bool start(std::string *err);           // loader init, instance, system, Vulkan via XR_KHR_vulkan_enable2, session, spaces, swapchain, passthrough
    void handle_cmd(int32_t cmd);           // APP_CMD_RESUME / PAUSE / DESTROY (android_app callbacks forward here)
    void poll_events();                     // xrPollEvent; begins / ends the session; called inside wait_frame too
    const ar_platform *api() const;
    const QxVulkan &vulkan() const;
    bool frame_renderable() const;          // this frame's shouldRender and an acquired swapchain image
    bool external_view(uint32_t i, ExternalView *out) const;   // color = &QxSwapchainImage, slice i, viewport full size, linearize per format
    bool color_wants_linear() const;
    uint32_t swapchain_width() const; uint32_t swapchain_height() const;
    void end_submission();                  // after the GPU work is submitted: release the swapchain image, xrEndFrame; a no-op when this frame had no xrBeginFrame (session not running)
    bool exiting() const;
    const ar_frame_info &last_frame() const;   // what the last wait_frame (called by the runtime's begin_frame) returned

    struct Impl;
private:
    Impl *m_;
};

}
