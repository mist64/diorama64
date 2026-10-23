#include "qx_platform.h"
#include <jni.h>
#include <vulkan/vulkan.h>
#include <android_native_app_glue.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <string.h>
#include <time.h>
#include <vector>
#include "qx_convert.h"
#include "vos_log.h"

namespace ar {

namespace {

const XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr uint32_t kEyes = 2;

// Extension entry points, loaded once through xrGetInstanceProcAddr in start().
struct XrExt {
    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    PFN_xrGetVulkanGraphicsRequirements2KHR get_vulkan_graphics_requirements2 = nullptr;
    PFN_xrCreateVulkanInstanceKHR create_vulkan_instance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_vulkan_graphics_device2 = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_vulkan_device = nullptr;
    PFN_xrCreatePassthroughFB create_passthrough = nullptr;
    PFN_xrDestroyPassthroughFB destroy_passthrough = nullptr;
    PFN_xrPassthroughStartFB passthrough_start = nullptr;
    PFN_xrPassthroughPauseFB passthrough_pause = nullptr;
    PFN_xrCreatePassthroughLayerFB create_passthrough_layer = nullptr;
    PFN_xrDestroyPassthroughLayerFB destroy_passthrough_layer = nullptr;
    PFN_xrPassthroughLayerPauseFB passthrough_layer_pause = nullptr;
    PFN_xrPassthroughLayerResumeFB passthrough_layer_resume = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate_display_refresh_rates = nullptr;
    PFN_xrGetDisplayRefreshRateFB get_display_refresh_rate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request_display_refresh_rate = nullptr;
};

void sleep_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, nullptr);
}

bool has_ext(const std::vector<XrExtensionProperties> &exts, const char *name) {
    for (const auto &e : exts)
        if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

}  // namespace

struct QxPlatform::Impl {
    android_app *app;
    QxPlatform::Config cfg;
    ar_platform api;
    XrExt ext;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace app_space = XR_NULL_HANDLE;
    bool stage = false;
    bool has_passthrough_ext = false;
    bool has_refresh_ext = false;

    QxVulkan vk{};

    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<QxSwapchainImage> images;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool linearize = false;

    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthrough_layer = XR_NULL_HANDLE;
    bool passthrough_on = false;

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool session_running = false;
    bool exit_requested = false;
    bool resumed = false;
    float display_hz = 0.f;

    // This frame.
    bool waited = false;          // xrWaitFrame succeeded
    bool begun = false;           // xrBeginFrame succeeded
    bool acquired = false;        // a swapchain image is acquired and waited
    uint32_t image_index = 0;
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    XrView views[kEyes];
    XrViewStateFlags view_flags = 0;
    ar_submit_info submit;
    bool submit_valid = false;
    ar_pose last_head;
    ar_frame_info last_info;      // what the last wait_frame returned

    Impl(android_app *a, const QxPlatform::Config &c) : app(a), cfg(c) {
        memset(&api, 0, sizeof api);
        memset(&submit, 0, sizeof submit);
        memset(&last_head, 0, sizeof last_head);
        memset(&last_info, 0, sizeof last_info);
        last_head.quat[3] = 1.f;
        for (auto &v : views) v = XrView{XR_TYPE_VIEW};
    }

    // Logs "AR_QUEST_XR <step> failed: <name>" and fills *err; true when r is a success code.
    bool ok(XrResult r, const char *step, std::string *err = nullptr) {
        if (XR_SUCCEEDED(r)) return true;
        char name[XR_MAX_RESULT_STRING_SIZE];
        if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, r, name)))
            snprintf(name, sizeof name, "%d", (int)r);
        vos_log("AR_QUEST_XR %s failed: %s", step, name);
        if (err) *err = std::string(step) + " failed: " + name;
        if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST) exit_requested = true;
        return false;
    }
    bool fail(const char *step, const std::string &why, std::string *err) {
        vos_log("AR_QUEST_XR %s failed: %s", step, why.c_str());
        if (err) *err = std::string(step) + " failed: " + why;
        return false;
    }
    template <class F> bool load(const char *name, F *out, std::string *err) {
        PFN_xrVoidFunction fn = nullptr;
        if (!ok(xrGetInstanceProcAddr(instance, name, &fn), name, err)) return false;
        *out = reinterpret_cast<F>(fn);
        return true;
    }

    bool create_instance(std::string *err);
    bool create_vulkan(std::string *err);
    bool create_session(std::string *err);
    bool create_swapchain(std::string *err);
    bool create_passthrough(std::string *err);
    void request_refresh();
    void poll_events();
    void set_passthrough(bool on);
    void end_frame();
    void destroy();
};

bool QxPlatform::Impl::create_instance(std::string *err) {
    PFN_xrVoidFunction fn = nullptr;
    if (!ok(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &fn), "xrGetInstanceProcAddr(xrInitializeLoaderKHR)", err))
        return false;
    ext.initialize_loader = reinterpret_cast<PFN_xrInitializeLoaderKHR>(fn);
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = app->activity->vm;
    li.applicationContext = app->activity->clazz;
    if (!ok(ext.initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&li)), "xrInitializeLoaderKHR", err))
        return false;

    uint32_t n = 0;
    if (!ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr), "xrEnumerateInstanceExtensionProperties", err))
        return false;
    std::vector<XrExtensionProperties> avail(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (!ok(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, avail.data()), "xrEnumerateInstanceExtensionProperties", err))
        return false;
    avail.resize(n);
    std::vector<const char *> want = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    for (const char *w : want)
        if (!has_ext(avail, w)) return fail("xrCreateInstance", std::string("runtime lacks ") + w, err);
    has_passthrough_ext = has_ext(avail, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    has_refresh_ext = has_ext(avail, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (has_passthrough_ext) want.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (has_refresh_ext) want.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    vos_log("AR_QUEST_EXT passthrough=%d refresh=%d", has_passthrough_ext ? 1 : 0, has_refresh_ext ? 1 : 0);

    XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = app->activity->vm;
    ai.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    strncpy(ci.applicationInfo.applicationName, "Diorama64", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "sm64ar", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = (uint32_t)want.size();
    ci.enabledExtensionNames = want.data();
    if (!ok(xrCreateInstance(&ci, &instance), "xrCreateInstance", err)) return false;

    if (!load("xrGetVulkanGraphicsRequirements2KHR", &ext.get_vulkan_graphics_requirements2, err) ||
        !load("xrCreateVulkanInstanceKHR", &ext.create_vulkan_instance, err) ||
        !load("xrGetVulkanGraphicsDevice2KHR", &ext.get_vulkan_graphics_device2, err) ||
        !load("xrCreateVulkanDeviceKHR", &ext.create_vulkan_device, err))
        return false;
    if (has_passthrough_ext &&
        (!load("xrCreatePassthroughFB", &ext.create_passthrough, err) ||
         !load("xrDestroyPassthroughFB", &ext.destroy_passthrough, err) ||
         !load("xrPassthroughStartFB", &ext.passthrough_start, err) ||
         !load("xrPassthroughPauseFB", &ext.passthrough_pause, err) ||
         !load("xrCreatePassthroughLayerFB", &ext.create_passthrough_layer, err) ||
         !load("xrDestroyPassthroughLayerFB", &ext.destroy_passthrough_layer, err) ||
         !load("xrPassthroughLayerPauseFB", &ext.passthrough_layer_pause, err) ||
         !load("xrPassthroughLayerResumeFB", &ext.passthrough_layer_resume, err)))
        return false;
    if (has_refresh_ext &&
        (!load("xrEnumerateDisplayRefreshRatesFB", &ext.enumerate_display_refresh_rates, err) ||
         !load("xrGetDisplayRefreshRateFB", &ext.get_display_refresh_rate, err) ||
         !load("xrRequestDisplayRefreshRateFB", &ext.request_display_refresh_rate, err)))
        return false;

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    return ok(xrGetSystem(instance, &si, &system), "xrGetSystem", err);
}

bool QxPlatform::Impl::create_vulkan(std::string *err) {
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!ok(ext.get_vulkan_graphics_requirements2(instance, system, &req), "xrGetVulkanGraphicsRequirements2KHR", err))
        return false;
    const XrVersion want_api = XR_MAKE_VERSION(1, 1, 0);
    if (XR_MAKE_VERSION(XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported), 0) > want_api)
        return fail("xrGetVulkanGraphicsRequirements2KHR", "runtime needs Vulkan " +
                    std::to_string(XR_VERSION_MAJOR(req.minApiVersionSupported)) + "." +
                    std::to_string(XR_VERSION_MINOR(req.minApiVersionSupported)) + ", app uses 1.1", err);

    vk.get_instance_proc_addr = vkGetInstanceProcAddr;
    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "Diorama64";
    app_info.applicationVersion = 1;
    app_info.pEngineName = "sm64ar";
    app_info.engineVersion = 1;
    app_info.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app_info;
    XrVulkanInstanceCreateInfoKHR xi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xi.systemId = system;
    xi.pfnGetInstanceProcAddr = vk.get_instance_proc_addr;
    xi.vulkanCreateInfo = &ici;
    VkResult vr = VK_SUCCESS;
    if (!ok(ext.create_vulkan_instance(instance, &xi, &vk.instance, &vr), "xrCreateVulkanInstanceKHR", err)) return false;
    if (vr != VK_SUCCESS) return fail("xrCreateVulkanInstanceKHR", "VkResult " + std::to_string((int)vr), err);

    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = system;
    gi.vulkanInstance = vk.instance;
    if (!ok(ext.get_vulkan_graphics_device2(instance, &gi, &vk.physical), "xrGetVulkanGraphicsDevice2KHR", err)) return false;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &qn, qf.data());
    vk.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk.queue_family = i; break; }
    if (vk.queue_family == UINT32_MAX) return fail("vkGetPhysicalDeviceQueueFamilyProperties", "no graphics queue family", err);

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> dev_exts(en);
    vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &en, dev_exts.data());
    auto dev_has = [&](const char *name) {
        for (const auto &e : dev_exts)
            if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    std::vector<const char *> dev_want;
    if (!dev_has(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME))
        return fail("xrCreateVulkanDeviceKHR", "the GPU lacks " VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME
                    " (the stereo vertex stage writes gl_Layer)", err);
    dev_want.push_back(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
    if (dev_has(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME)) {
        // VK_KHR_depth_stencil_resolve depends on VK_KHR_create_renderpass2 under Vulkan 1.1.
        if (dev_has(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) {
            dev_want.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
            dev_want.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        }
    }

    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(vk.physical, &have);
    if (!have.shaderClipDistance || !have.depthClamp || !have.samplerAnisotropy || !have.fragmentStoresAndAtomics)
        return fail("xrCreateVulkanDeviceKHR", "the GPU lacks a required feature (clip=" + std::to_string(have.shaderClipDistance) +
                    " depthClamp=" + std::to_string(have.depthClamp) + " anisotropy=" + std::to_string(have.samplerAnisotropy) +
                    " fragmentStores=" + std::to_string(have.fragmentStoresAndAtomics) + ")", err);
    VkPhysicalDeviceFeatures features{};
    features.shaderClipDistance = VK_TRUE;
    features.depthClamp = VK_TRUE;
    features.samplerAnisotropy = VK_TRUE;
    features.fragmentStoresAndAtomics = VK_TRUE;
    features.alphaToOne = have.alphaToOne;   // alpha-to-coverage pipelines also set alpha-to-one (as Metal) when offered

    const float priority = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dev_want.size();
    dci.ppEnabledExtensionNames = dev_want.data();
    dci.pEnabledFeatures = &features;
    XrVulkanDeviceCreateInfoKHR xd{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xd.systemId = system;
    xd.pfnGetInstanceProcAddr = vk.get_instance_proc_addr;
    xd.vulkanPhysicalDevice = vk.physical;
    xd.vulkanCreateInfo = &dci;
    if (!ok(ext.create_vulkan_device(instance, &xd, &vk.device, &vr), "xrCreateVulkanDeviceKHR", err)) return false;
    if (vr != VK_SUCCESS) return fail("xrCreateVulkanDeviceKHR", "VkResult " + std::to_string((int)vr), err);
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
    vk.device_extensions.assign(dev_want.begin(), dev_want.end());
    vk.alpha_to_one = have.alphaToOne == VK_TRUE;
    return true;
}

bool QxPlatform::Impl::create_session(std::string *err) {
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vk.instance;
    binding.physicalDevice = vk.physical;
    binding.device = vk.device;
    binding.queueFamilyIndex = vk.queue_family;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    if (!ok(xrCreateSession(instance, &sci, &session), "xrCreateSession", err)) return false;

    uint32_t n = 0;
    if (!ok(xrEnumerateReferenceSpaces(session, 0, &n, nullptr), "xrEnumerateReferenceSpaces", err)) return false;
    std::vector<XrReferenceSpaceType> spaces(n);
    if (!ok(xrEnumerateReferenceSpaces(session, n, &n, spaces.data()), "xrEnumerateReferenceSpaces", err)) return false;
    stage = false;
    for (uint32_t i = 0; i < n; i++)
        if (spaces[i] == XR_REFERENCE_SPACE_TYPE_STAGE) stage = true;
    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.referenceSpaceType = stage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    rci.poseInReferenceSpace.orientation.w = 1.f;
    if (!ok(xrCreateReferenceSpace(session, &rci, &app_space), "xrCreateReferenceSpace", err)) return false;
    vos_log("AR_QUEST_SPACE stage=%d", stage ? 1 : 0);
    return true;
}

bool QxPlatform::Impl::create_swapchain(std::string *err) {
    uint32_t n = 0;
    if (!ok(xrEnumerateViewConfigurationViews(instance, system, kViewConfig, 0, &n, nullptr), "xrEnumerateViewConfigurationViews", err))
        return false;
    std::vector<XrViewConfigurationView> cv(n, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!ok(xrEnumerateViewConfigurationViews(instance, system, kViewConfig, n, &n, cv.data()), "xrEnumerateViewConfigurationViews", err))
        return false;
    if (n != kEyes) return fail("xrEnumerateViewConfigurationViews", "expected 2 views, got " + std::to_string(n), err);
    width = cv[0].recommendedImageRectWidth;
    height = cv[0].recommendedImageRectHeight;

    if (!ok(xrEnumerateSwapchainFormats(session, 0, &n, nullptr), "xrEnumerateSwapchainFormats", err)) return false;
    std::vector<int64_t> formats(n);
    if (!ok(xrEnumerateSwapchainFormats(session, n, &n, formats.data()), "xrEnumerateSwapchainFormats", err)) return false;
    const int64_t picked = qx_pick_swapchain_format(formats.data(), n, &linearize);
    if (picked == 0) return fail("xrEnumerateSwapchainFormats", "no RGBA8/BGRA8 swapchain format", err);
    format = (VkFormat)picked;

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // TRANSFER_DST: the skeleton (and the resolve path) write the image with transfer commands.
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = picked;
    sci.sampleCount = 1;
    sci.width = width;
    sci.height = height;
    sci.faceCount = 1;
    sci.arraySize = kEyes;
    sci.mipCount = 1;
    if (!ok(xrCreateSwapchain(session, &sci, &swapchain), "xrCreateSwapchain", err)) return false;

    if (!ok(xrEnumerateSwapchainImages(swapchain, 0, &n, nullptr), "xrEnumerateSwapchainImages", err)) return false;
    std::vector<XrSwapchainImageVulkan2KHR> xi(n, XrSwapchainImageVulkan2KHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    if (!ok(xrEnumerateSwapchainImages(swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(xi.data())),
            "xrEnumerateSwapchainImages", err))
        return false;
    images.clear();
    for (uint32_t i = 0; i < n; i++) {
        QxSwapchainImage img{xi[i].image, VK_NULL_HANDLE, format, width, height, kEyes};
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kEyes};
        VkResult vr = vkCreateImageView(vk.device, &vci, nullptr, &img.view);
        if (vr != VK_SUCCESS) return fail("vkCreateImageView", "VkResult " + std::to_string((int)vr), err);
        images.push_back(img);
    }
    vos_log("AR_QUEST_SWAPCHAIN %ux%u format=%d linearize=%d", width, height, (int)format, linearize ? 1 : 0);
    return true;
}

bool QxPlatform::Impl::create_passthrough(std::string *err) {
    if (!cfg.passthrough) return true;
    if (!has_passthrough_ext) {
        vos_log("AR_QUEST_XR passthrough unavailable: runtime lacks %s", XR_FB_PASSTHROUGH_EXTENSION_NAME);
        return true;
    }
    XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    if (!ok(ext.create_passthrough(session, &pci, &passthrough), "xrCreatePassthroughFB", err)) return false;
    XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    lci.passthrough = passthrough;
    lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    if (!ok(ext.create_passthrough_layer(session, &lci, &passthrough_layer), "xrCreatePassthroughLayerFB", err)) return false;
    if (!ok(ext.passthrough_start(passthrough), "xrPassthroughStartFB", err)) return false;
    if (!ok(ext.passthrough_layer_resume(passthrough_layer), "xrPassthroughLayerResumeFB", err)) return false;
    passthrough_on = true;
    return true;
}

void QxPlatform::Impl::request_refresh() {
    if (!has_refresh_ext) return;
    uint32_t n = 0;
    std::vector<float> rates;
    if (ok(ext.enumerate_display_refresh_rates(session, 0, &n, nullptr), "xrEnumerateDisplayRefreshRatesFB")) {
        rates.resize(n);
        if (ok(ext.enumerate_display_refresh_rates(session, n, &n, rates.data()), "xrEnumerateDisplayRefreshRatesFB"))
            rates.resize(n);
        else
            rates.clear();
    }
    bool supported = false;
    for (float r : rates)
        if (r == cfg.refresh_hz) supported = true;
    if (supported)
        ok(ext.request_display_refresh_rate(session, cfg.refresh_hz), "xrRequestDisplayRefreshRateFB");
    else
        vos_log("AR_QUEST_XR xrRequestDisplayRefreshRateFB skipped: %g Hz not among %zu supported rates", cfg.refresh_hz,
                rates.size());
    float got = 0.f;
    if (ok(ext.get_display_refresh_rate(session, &got), "xrGetDisplayRefreshRateFB")) display_hz = got;
    vos_log("AR_QUEST_REFRESH requested=%g got=%g", cfg.refresh_hz, got);
}

void QxPlatform::Impl::poll_events() {
    if (instance == XR_NULL_HANDLE) return;
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult r = xrPollEvent(instance, &ev);
        if (r == XR_EVENT_UNAVAILABLE) break;
        if (!ok(r, "xrPollEvent")) break;
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            vos_log("AR_QUEST_XR instance loss pending");
            exit_requested = true;
            break;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto *e = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB *>(&ev);
            display_hz = e->toDisplayRefreshRate;
            vos_log("AR_QUEST_REFRESH requested=%g got=%g", cfg.refresh_hz, display_hz);
            break;
        }
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto *e = reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
            if (e->session != session) break;
            state = e->state;
            vos_log("AR_QUEST_STATE %d", (int)state);
            if (state == XR_SESSION_STATE_READY && !session_running) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = kViewConfig;
                if (ok(xrBeginSession(session, &bi), "xrBeginSession")) session_running = true;
            } else if (state == XR_SESSION_STATE_STOPPING && session_running) {
                ok(xrEndSession(session), "xrEndSession");
                session_running = false;
                waited = begun = acquired = submit_valid = false;   // an ended session has no frame outstanding
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                exit_requested = true;
            }
            break;
        }
        default: break;
        }
    }
}

void QxPlatform::Impl::set_passthrough(bool on) {
    if (passthrough_layer == XR_NULL_HANDLE || on == passthrough_on) return;
    if (on) {
        if (ok(ext.passthrough_layer_resume(passthrough_layer), "xrPassthroughLayerResumeFB")) passthrough_on = true;
    } else {
        if (ok(ext.passthrough_layer_pause(passthrough_layer), "xrPassthroughLayerPauseFB")) passthrough_on = false;
    }
}

void QxPlatform::Impl::end_frame() {
    if (!begun) {   // waited stays set: the next wait_frame begins (and ends) that frame before waiting again
        acquired = submit_valid = false;
        return;
    }
    bool released = false;
    if (acquired) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        released = ok(xrReleaseSwapchainImage(swapchain, &ri), "xrReleaseSwapchainImage");
    }
    XrCompositionLayerPassthroughFB pt_layer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    XrCompositionLayerProjectionView pviews[kEyes];
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layer_count = 0;
    const bool oriented = (view_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (frame_state.shouldRender && passthrough_on) {   // even when the projection cannot be sent: passthrough, not black
        pt_layer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        pt_layer.space = XR_NULL_HANDLE;
        pt_layer.layerHandle = passthrough_layer;
        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&pt_layer);
    }
    if (frame_state.shouldRender && released && submit_valid && oriented) {
        for (uint32_t eye = 0; eye < kEyes; eye++) {
            pviews[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pviews[eye].pose = views[eye].pose;
            pviews[eye].fov = views[eye].fov;
            pviews[eye].subImage.swapchain = swapchain;
            pviews[eye].subImage.imageRect.offset = {0, 0};
            pviews[eye].subImage.imageRect.extent = {(int32_t)width, (int32_t)height};
            pviews[eye].subImage.imageArrayIndex = eye;
        }
        proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        proj.space = app_space;
        proj.viewCount = kEyes;
        proj.views = pviews;
        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj);
    }
    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
    fe.displayTime = frame_state.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = layer_count;
    fe.layers = layer_count ? layers : nullptr;
    ok(xrEndFrame(session, &fe), "xrEndFrame");
    waited = begun = acquired = submit_valid = false;
}

void QxPlatform::Impl::destroy() {
    if (vk.device != VK_NULL_HANDLE) vkDeviceWaitIdle(vk.device);
    if (passthrough_layer != XR_NULL_HANDLE) ext.destroy_passthrough_layer(passthrough_layer);
    if (passthrough != XR_NULL_HANDLE) ext.destroy_passthrough(passthrough);
    passthrough_layer = XR_NULL_HANDLE;
    passthrough = XR_NULL_HANDLE;
    for (auto &img : images)
        if (img.view != VK_NULL_HANDLE) vkDestroyImageView(vk.device, img.view, nullptr);
    images.clear();
    if (swapchain != XR_NULL_HANDLE) xrDestroySwapchain(swapchain);
    if (app_space != XR_NULL_HANDLE) xrDestroySpace(app_space);
    if (session != XR_NULL_HANDLE) {
        if (session_running) xrEndSession(session);
        xrDestroySession(session);
    }
    swapchain = XR_NULL_HANDLE;
    app_space = XR_NULL_HANDLE;
    session = XR_NULL_HANDLE;
    session_running = false;
    if (vk.device != VK_NULL_HANDLE) vkDestroyDevice(vk.device, nullptr);
    if (vk.instance != VK_NULL_HANDLE) vkDestroyInstance(vk.instance, nullptr);
    vk.device = VK_NULL_HANDLE;
    vk.instance = VK_NULL_HANDLE;
    if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;
}

// ar_platform callbacks. user is the Impl.
namespace {

using Impl = QxPlatform::Impl;

int wait_frame(Impl *m, ar_frame_info *out) {
    if (m->waited && !m->begun && m->session != XR_NULL_HANDLE) {
        // The last xrBeginFrame failed: a waited frame that is never begun can make the next xrWaitFrame block forever.
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        if (m->ok(xrBeginFrame(m->session, &bi), "xrBeginFrame (retry)")) {
            m->begun = true;
            m->acquired = m->submit_valid = false;
            m->end_frame();   // no projection layer (passthrough only)
        } else {
            // Still not begun: never issue another xrWaitFrame over it. Keep waited and report a frame with nothing to
            // render until the begin succeeds or the session stops running (poll_events then clears waited).
            m->poll_events();
            if (m->exit_requested) return 0;
            if (m->session_running && m->waited) {
                sleep_ms(10);
                memset(out, 0, sizeof *out);
                out->tracking_from_head = m->last_head;
                return 1;
            }
        }
    }
    if (m->begun) m->end_frame();   // the caller skipped end_submission: keep the XR frame loop in sync
    m->poll_events();
    m->waited = m->begun = m->acquired = m->submit_valid = false;
    if (m->exit_requested) return 0;
    if (!m->session_running) {
        sleep_ms(10);
        memset(out, 0, sizeof *out);
        out->tracking_from_head = m->last_head;
        return 1;
    }
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    m->frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
    if (!m->ok(xrWaitFrame(m->session, &wi, &m->frame_state), "xrWaitFrame")) {
        memset(out, 0, sizeof *out);
        out->tracking_from_head = m->last_head;
        return m->exit_requested ? 0 : 1;
    }
    m->waited = true;

    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = kViewConfig;
    li.displayTime = m->frame_state.predictedDisplayTime;
    li.space = m->app_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    for (auto &v : m->views) v = XrView{XR_TYPE_VIEW};
    m->view_flags = 0;
    if (m->ok(xrLocateViews(m->session, &li, &vs, kEyes, &n, m->views), "xrLocateViews") && n == kEyes) m->view_flags = vs.viewStateFlags;

    float hz = m->display_hz;
    if (hz <= 0.f && m->frame_state.predictedDisplayPeriod > 0) hz = 1e9f / (float)m->frame_state.predictedDisplayPeriod;
    if (!qx_fill_frame_info(m->views, kEyes, m->view_flags, m->frame_state.predictedDisplayTime, hz, m->width, m->height, 0.f,
                            m->last_head, out)) {
        memset(out, 0, sizeof *out);
        out->tracking_from_head = m->last_head;
    }
    if (out->head_tracked) m->last_head = out->tracking_from_head;
    out->should_render = m->frame_state.shouldRender ? 1 : 0;
    return 1;
}

int cb_wait_frame(void *user, ar_frame_info *out) {
    Impl *m = static_cast<Impl *>(user);
    const int r = wait_frame(m, out);
    m->last_info = *out;
    return r;
}

void cb_begin_frame(void *user) {
    Impl *m = static_cast<Impl *>(user);
    if (!m->waited || m->begun) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (!m->ok(xrBeginFrame(m->session, &bi), "xrBeginFrame")) return;
    m->begun = true;
    if (!m->frame_state.shouldRender) return;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!m->ok(xrAcquireSwapchainImage(m->swapchain, &ai, &m->image_index), "xrAcquireSwapchainImage")) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!m->ok(xrWaitSwapchainImage(m->swapchain, &wi), "xrWaitSwapchainImage")) return;
    m->acquired = m->image_index < m->images.size();
}

void cb_end_frame(void *user, const ar_submit_info *used) {
    Impl *m = static_cast<Impl *>(user);
    if (!m->begun || !used) return;
    m->submit = *used;
    m->submit_valid = used->view_count == kEyes;
}

int cb_get_table(void *user, ar_table *out) {
    Impl *m = static_cast<Impl *>(user);
    memset(out, 0, sizeof *out);
    if (!m->cfg.fixed_table_valid) {
        out->tracking_from_table.quat[3] = 1.f;
        return 0;
    }
    out->tracking_from_table = m->cfg.fixed_table;
    // The fixed table is authored in floor-origin (STAGE) coordinates; LOCAL has its origin at the head.
    if (!m->stage) out->tracking_from_table.pos[1] -= 1.5f;
    return 1;
}

int cb_get_pad(void *, uint32_t, ar_pad *out) {
    memset(out, 0, sizeof *out);   // game thread: touches no XR state
    return 0;
}

ar_session_state cb_session_state(void *user) {
    Impl *m = static_cast<Impl *>(user);
    return qx_session_state(m->state, m->exit_requested);
}

void cb_set_passthrough(void *user, int on) { static_cast<Impl *>(user)->set_passthrough(on != 0); }

}  // namespace

QxPlatform::QxPlatform(android_app *app, const Config &c) : m_(new Impl(app, c)) {
    ar_platform &a = m_->api;
    a.abi_version = AR_PLATFORM_ABI_VERSION;
    a.user = m_;
    a.wait_frame = cb_wait_frame;
    a.get_table = cb_get_table;
    a.get_pad = cb_get_pad;
    a.session_state = cb_session_state;
    a.begin_frame = cb_begin_frame;
    a.end_frame = cb_end_frame;
    a.set_passthrough = cb_set_passthrough;
    a.persist_anchor = nullptr;
    a.share_anchor = nullptr;
    a.get_environment = nullptr;
    a.get_preview = nullptr;
}

QxPlatform::~QxPlatform() {
    m_->destroy();
    delete m_;
}

bool QxPlatform::start(std::string *err) {
    if (!m_->create_instance(err) || !m_->create_vulkan(err) || !m_->create_session(err) || !m_->create_swapchain(err) ||
        !m_->create_passthrough(err))
        return false;
    m_->request_refresh();
    return true;
}

void QxPlatform::handle_cmd(int32_t cmd) {
    switch (cmd) {
    case APP_CMD_RESUME: m_->resumed = true; break;
    case APP_CMD_PAUSE: m_->resumed = false; break;
    case APP_CMD_DESTROY: m_->exit_requested = true; break;
    default: break;
    }
}

void QxPlatform::poll_events() { m_->poll_events(); }
const ar_platform *QxPlatform::api() const { return &m_->api; }
const QxVulkan &QxPlatform::vulkan() const { return m_->vk; }
bool QxPlatform::frame_renderable() const { return m_->waited && m_->begun && m_->frame_state.shouldRender && m_->acquired; }

bool QxPlatform::external_view(uint32_t i, ExternalView *out) const {
    if (!frame_renderable() || i >= kEyes) return false;
    *out = ExternalView{};
    out->color = &m_->images[m_->image_index];
    out->slice = i;
    out->viewport = Viewport{0.f, 0.f, (float)m_->width, (float)m_->height};
    out->linearize = m_->linearize;
    return true;
}

bool QxPlatform::color_wants_linear() const { return m_->linearize; }
uint32_t QxPlatform::swapchain_width() const { return m_->width; }
uint32_t QxPlatform::swapchain_height() const { return m_->height; }
void QxPlatform::end_submission() { m_->end_frame(); }
const ar_frame_info &QxPlatform::last_frame() const { return m_->last_info; }
bool QxPlatform::exiting() const { return m_->exit_requested || m_->app->destroyRequested; }

}  // namespace ar
