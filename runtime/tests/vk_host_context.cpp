#include "vk_host_context.h"
#include <cstring>
#include <vector>
namespace ar {
bool vk_host_create(VulkanDeviceInit *out, std::string *err) {
    *out = VulkanDeviceInit{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "sm64ar_test";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vr = vkCreateInstance(&ici, nullptr, &instance);
    if (vr != VK_SUCCESS) { *err = "vkCreateInstance failed: VkResult " + std::to_string((int)vr); return false; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    if (n) vkEnumeratePhysicalDevices(instance, &n, pds.data());
    if (n == 0) { vkDestroyInstance(instance, nullptr); *err = "no Vulkan physical device"; return false; }
    VkPhysicalDevice physical = pds[0];
    for (VkPhysicalDevice p : pds) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(p, &props);
        if (std::strstr(props.deviceName, "llvmpipe")) { physical = p; break; }
    }
    auto fail = [&](const std::string &why) { vkDestroyInstance(instance, nullptr); *err = why; return false; };
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qf.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    if (family == UINT32_MAX) return fail("no graphics queue family");
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &en, exts.data());
    auto has = [&](const char *name) {
        for (const auto &e : exts)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    std::vector<const char *> want;
    if (!has(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME)) return fail("the device lacks " VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
    want.push_back(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
    if (has(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) && has(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) {
        want.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        want.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
    }
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(physical, &have);
    if (!have.shaderClipDistance || !have.depthClamp || !have.samplerAnisotropy || !have.fragmentStoresAndAtomics)
        return fail("the device lacks a required feature");
    VkPhysicalDeviceFeatures features{};
    features.shaderClipDistance = VK_TRUE;
    features.depthClamp = VK_TRUE;
    features.samplerAnisotropy = VK_TRUE;
    features.fragmentStoresAndAtomics = VK_TRUE;
    const float priority = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)want.size();
    dci.ppEnabledExtensionNames = want.data();
    dci.pEnabledFeatures = &features;
    VkDevice device = VK_NULL_HANDLE;
    vr = vkCreateDevice(physical, &dci, nullptr, &device);
    if (vr != VK_SUCCESS) return fail("vkCreateDevice failed: VkResult " + std::to_string((int)vr));
    out->instance = instance;
    out->physical = physical;
    out->device = device;
    out->queue_family = family;
    vkGetDeviceQueue(device, family, 0, &out->queue);
    out->get_instance_proc_addr = vkGetInstanceProcAddr;
    return true;
}
void vk_host_destroy(const VulkanDeviceInit &init) {
    if (init.device) vkDestroyDevice(init.device, nullptr);
    if (init.instance) vkDestroyInstance(init.instance, nullptr);
}
}
