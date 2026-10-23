#pragma once
#include <stdint.h>
#include <string>
#include <vulkan/vulkan.h>
#include "ar_platform.h"
#include "qx_input_pure.h"
#include "render_device.h"
#include "vulkan_device.h"

struct android_app;

namespace ar {

struct QxVulkan { VkInstance instance; VkPhysicalDevice physical; VkDevice device; uint32_t queue_family; VkQueue queue;
                  PFN_vkGetInstanceProcAddr get_instance_proc_addr;
                  std::vector<std::string> device_extensions;   // what create_vulkan enabled (VulkanDeviceInit's contract)
                  bool alpha_to_one = false; };
using QxSwapchainImage = VulkanExternalImage;  // what ExternalView::color points at: the device's own type, no pun

// The Quest platform: an OpenXR session on a Vulkan device (XR_KHR_vulkan_enable2), one stereo array swapchain and an
// XR_FB_passthrough layer under it. All members are for the render thread except api()->get_pad (any thread).
class QxPlatform {
public:
    // fixed_table_valid: the fixed debug table replaces the room scan's (no scene permission asked, no preview).
    // record_path: the remembered placement (files_dir/placement.json).
    struct Config { bool passthrough = true; float refresh_hz = 90.f; ar_pose fixed_table; bool fixed_table_valid = false;
                    std::string record_path; };
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
    void set_depth_written(bool written);   // before end_submission: false when the device left the depth image unwritten (no depth info is chained)
    void end_submission();                  // after the GPU work is submitted: release the swapchain image, xrEndFrame; a no-op when this frame had no xrBeginFrame (session not running)
    bool exiting() const;
    // The game is paused (the session reports VISIBLE, not FOCUSED, so the runtime steps no ticks and the audio pauses)
    // while on: arvos_set_background, as VosPlatform::set_background. Any thread.
    void set_paused(bool on);
    const ar_frame_info &last_frame() const;   // what the last wait_frame (called by the runtime's begin_frame) returned
    // The Touch controllers' sample of the last wait_frame and of the one before it (zeroed while unfocused), and
    // whether anything but the aim poses changed between them. Render thread.
    const QxActionState &actions() const;
    const QxActionState &actions_before() const;
    bool actions_changed() const;
    bool input_focused() const;               // the last sample came from a focused session (xrSyncActions ran)
    void hold_until_released(uint32_t mask);  // the pad drops these buttons until they are released (any thread)
    // The pointer router's decision for the last sample (computed once per wait_frame, here, so the placer and the
    // session see the same one; an unfocused sample ends whatever owned the ray). Render thread.
    const PointerDecision &pointer_decision() const;
    // The room scan's table (render thread). replace_table: the nearest table, offset reset (now, or once the scan has
    // tables). request_scene_capture: Space Setup (xrRequestSceneCaptureFB), once the scene permission is granted; the
    // scan is read again when it completes. scene_hint: "" or why the level has no table.
    void replace_table();
    void request_scene_capture();
    std::string scene_hint() const;

    struct Impl;
private:
    Impl *m_;
};

}
