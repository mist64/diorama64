#include <android_native_app_glue.h>
#include <android/looper.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vulkan/vulkan.h>
#include "qx_platform.h"
#include "vos_log.h"

namespace {

double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

void on_cmd(android_app *app, int32_t cmd) {
    ar::vos_log("APP_CMD_%d", (int)cmd);
    if (app->userData) static_cast<ar::QxPlatform *>(app->userData)->handle_cmd(cmd);
}

void pump(android_app *app, int timeout_ms) {
    for (;;) {
        android_poll_source *src = nullptr;
        int events = 0;
        if (ALooper_pollOnce(timeout_ms, nullptr, &events, (void **)&src) < 0) break;
        if (src) src->process(app, src);
        if (timeout_ms != 0) break;
    }
}

// One reusable command buffer that clears the acquired swapchain image to transparent.
struct Clearer {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    bool init(const ar::QxVulkan &vk) {
        device = vk.device;
        queue = vk.queue;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = vk.queue_family;
        if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        return vkAllocateCommandBuffers(device, &ai, &cmd) == VK_SUCCESS;
    }
    void destroy() {
        if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
    bool clear(const ar::QxSwapchainImage &img) {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return false;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img.layers};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        const VkClearColorValue transparent{{0.f, 0.f, 0.f, 0.f}};
        vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1, &range);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
        return vkQueueWaitIdle(queue) == VK_SUCCESS;
    }
};

}  // namespace

extern "C" void android_main(android_app *app) {
    ar::vos_log_open((std::string(app->activity->internalDataPath) + "/sm64ar.log").c_str());
    ar::vos_log("AR_QUEST_START");
    app->onAppCmd = on_cmd;

    ar::QxPlatform::Config cfg;
    memset(&cfg.fixed_table, 0, sizeof cfg.fixed_table);
    cfg.fixed_table.quat[3] = 1.f;
    ar::QxPlatform platform(app, cfg);
    app->userData = &platform;
    std::string err;
    bool running = platform.start(&err);
    Clearer clearer;
    if (running && !clearer.init(platform.vulkan())) {
        err = "command buffer setup failed";
        running = false;
    }
    if (!running) ar::vos_log("AR_QUEST_FAIL %s", err.c_str());

    const ar_platform *api = platform.api();
    double last_head_log = 0.0;
    while (!app->destroyRequested) {
        if (!running) {   // keep the activity alive (and its log readable) after a failed start
            pump(app, -1);
            continue;
        }
        pump(app, 0);
        if (platform.exiting()) {
            ANativeActivity_finish(app->activity);
            running = false;
            continue;
        }
        ar_frame_info fi;
        if (!api->wait_frame(api->user, &fi)) continue;   // exiting(): finish on the next pass
        api->begin_frame(api->user);
        if (fi.head_tracked && now_s() - last_head_log >= 1.0) {
            last_head_log = now_s();
            ar::vos_log("AR_QUEST_HEAD %.3f %.3f %.3f", fi.tracking_from_head.pos[0], fi.tracking_from_head.pos[1],
                        fi.tracking_from_head.pos[2]);
        }
        if (platform.frame_renderable()) {
            ar::ExternalView ev;
            if (platform.external_view(0, &ev) && clearer.clear(*static_cast<const ar::QxSwapchainImage *>(ev.color))) {
                ar_submit_info used;
                memset(&used, 0, sizeof used);
                used.display_time_ns = fi.display_time_ns;
                used.view_count = fi.view_count;
                memcpy(used.views, fi.views, sizeof used.views);
                ar_table table;
                used.table_valid = (uint8_t)api->get_table(api->user, &table);
                used.tracking_from_table = table.tracking_from_table;
                api->end_frame(api->user, &used);
            } else {
                ar::vos_log("AR_QUEST_FAIL swapchain clear");
            }
        }
        platform.end_submission();
    }
    if (platform.vulkan().device != VK_NULL_HANDLE) vkDeviceWaitIdle(platform.vulkan().device);
    clearer.destroy();
    app->userData = nullptr;
    ar::vos_log("AR_QUEST_EXIT");
}
