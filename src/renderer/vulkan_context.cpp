#include "renderer/vulkan_context.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <vector>

// ── Validation layers ─────────────────────────────────────────────────
#ifdef CLAUDEUI_DEBUG
static constexpr bool VALIDATION = true;
#else
static constexpr bool VALIDATION = false;
#endif

static constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";
static constexpr const char* DEVICE_EXT       = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

// ── Debug callback ────────────────────────────────────────────────────
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[Vulkan] " << data->pMessage << '\n';
    return VK_FALSE;
}

// ── Ctor / Dtor ───────────────────────────────────────────────────────
VulkanContext::VulkanContext(GLFWwindow* window) : m_window(window)
{
    createInstance();
    if constexpr (VALIDATION) createDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createDevice();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createFramebuffers();
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
    createImGuiPool();
}

VulkanContext::~VulkanContext()
{
    vkDeviceWaitIdle(m_device);
    // VkOwned members destroy themselves automatically in reverse declaration order
}

// ── Instance ──────────────────────────────────────────────────────────
void VulkanContext::createInstance()
{
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "ClaudeUI";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "ClaudeUI";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_3;

    uint32_t glfwCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwCount);
    std::vector<const char*> exts(glfwExts, glfwExts + glfwCount);
    if constexpr (VALIDATION)
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo        = &app;
    info.enabledExtensionCount   = (uint32_t)exts.size();
    info.ppEnabledExtensionNames = exts.data();

    if constexpr (VALIDATION)
    {
        info.enabledLayerCount   = 1;
        info.ppEnabledLayerNames = &VALIDATION_LAYER;
    }

    VkInstance raw = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&info, nullptr, &raw));
    m_instance = VkOwned<VkInstance>{raw, [](VkInstance i){ vkDestroyInstance(i, nullptr); }};
}

// ── Debug messenger ───────────────────────────────────────────────────
void VulkanContext::createDebugMessenger()
{
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT     |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT  |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;

    auto createFn = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
    if (!createFn) return;

    VkDebugUtilsMessengerEXT raw = VK_NULL_HANDLE;
    VK_CHECK(createFn(m_instance, &info, nullptr, &raw));

    VkInstance inst = m_instance.get();
    m_debugMessenger = VkOwned<VkDebugUtilsMessengerEXT>{raw,
        [inst](VkDebugUtilsMessengerEXT m) {
            auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT");
            if (fn) fn(inst, m, nullptr);
        }
    };
}

// ── Surface ───────────────────────────────────────────────────────────
void VulkanContext::createSurface()
{
    VkSurfaceKHR raw = VK_NULL_HANDLE;
    VK_CHECK(glfwCreateWindowSurface(m_instance, m_window, nullptr, &raw));
    VkInstance inst = m_instance.get();
    m_surface = VkOwned<VkSurfaceKHR>{raw,
        [inst](VkSurfaceKHR s){ vkDestroySurfaceKHR(inst, s, nullptr); }
    };
}

// ── Physical device ───────────────────────────────────────────────────
QueueFamilies VulkanContext::queryFamilies(VkPhysicalDevice dev) const
{
    QueueFamilies fam;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, props.data());

    for (uint32_t i = 0; i < count; ++i)
    {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            fam.graphics = i;

        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, m_surface, &present);
        if (present) fam.present = i;

        if (fam.complete()) break;
    }
    return fam;
}

bool VulkanContext::isSuitable(VkPhysicalDevice dev) const
{
    if (!queryFamilies(dev).complete()) return false;

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());

    bool hasSwapchain = false;
    for (auto& e : exts)
        if (strcmp(e.extensionName, DEVICE_EXT) == 0) { hasSwapchain = true; break; }
    if (!hasSwapchain) return false;

    uint32_t fmtCount = 0, modeCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, m_surface, &fmtCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, m_surface, &modeCount, nullptr);
    return fmtCount > 0 && modeCount > 0;
}

void VulkanContext::pickPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) throw std::runtime_error("No Vulkan-capable GPU found");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    for (auto dev : devices)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(dev, &p);
        if (isSuitable(dev) && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            m_physDevice = dev;
            return;
        }
    }
    for (auto dev : devices)
        if (isSuitable(dev)) { m_physDevice = dev; return; }

    throw std::runtime_error("No suitable GPU found");
}

// ── Logical device ────────────────────────────────────────────────────
void VulkanContext::createDevice()
{
    m_families = queryFamilies(m_physDevice);

    std::set<uint32_t> uniqueFamilies = { *m_families.graphics, *m_families.present };
    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (uint32_t f : uniqueFamilies)
    {
        VkDeviceQueueCreateInfo qi{};
        qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = f;
        qi.queueCount       = 1;
        qi.pQueuePriorities = &prio;
        queueInfos.push_back(qi);
    }

    VkPhysicalDeviceFeatures features{};
    features.fillModeNonSolid = VK_TRUE;

    VkDeviceCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount    = (uint32_t)queueInfos.size();
    info.pQueueCreateInfos       = queueInfos.data();
    info.enabledExtensionCount   = 1;
    info.ppEnabledExtensionNames = &DEVICE_EXT;
    info.pEnabledFeatures        = &features;

    // Device layers are deprecated since Vulkan 1.0 — validation is enabled
    // at instance level only; enabledLayerCount must stay 0 here
    // (VUID-VkDeviceCreateInfo-enabledLayerCount-12384).

    VkDevice raw = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(m_physDevice, &info, nullptr, &raw));
    m_device = VkOwned<VkDevice>{raw, [](VkDevice d){ vkDestroyDevice(d, nullptr); }};

    vkGetDeviceQueue(m_device, *m_families.graphics, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, *m_families.present,  0, &m_presentQueue);
}

// ── Swapchain helpers ─────────────────────────────────────────────────
VkSurfaceFormatKHR VulkanContext::chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& fmts) const
{
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    return fmts[0];
}

VkPresentModeKHR VulkanContext::choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) const
{
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanContext::chooseExtent(const VkSurfaceCapabilitiesKHR& caps) const
{
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
        return caps.currentExtent;

    int w, h;
    glfwGetFramebufferSize(m_window, &w, &h);
    return {
        std::clamp((uint32_t)w, caps.minImageExtent.width,  caps.maxImageExtent.width),
        std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height)
    };
}

// ── Swapchain ─────────────────────────────────────────────────────────
void VulkanContext::createSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDevice, m_surface, &caps);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &fmtCount, fmts.data());

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physDevice, m_surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physDevice, m_surface, &modeCount, modes.data());

    auto fmt    = chooseSurfaceFormat(fmts);
    auto mode   = choosePresentMode(modes);
    auto extent = chooseExtent(caps);

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
        imgCount = caps.maxImageCount;

    m_minImageCount = caps.minImageCount;
    m_swapFormat    = fmt.format;
    m_extent        = extent;

    VkSwapchainCreateInfoKHR info{};
    info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface          = m_surface;
    info.minImageCount    = imgCount;
    info.imageFormat      = fmt.format;
    info.imageColorSpace  = fmt.colorSpace;
    info.imageExtent      = extent;
    info.imageArrayLayers = 1;
    info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.preTransform     = caps.currentTransform;
    info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode      = mode;
    info.clipped          = VK_TRUE;

    uint32_t familyArr[] = { *m_families.graphics, *m_families.present };
    if (*m_families.graphics != *m_families.present)
    {
        info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices   = familyArr;
    }
    else
    {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkSwapchainKHR raw = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(m_device, &info, nullptr, &raw));
    VkDevice dev = m_device.get();
    m_swapchain = VkOwned<VkSwapchainKHR>{raw,
        [dev](VkSwapchainKHR s){ vkDestroySwapchainKHR(dev, s, nullptr); }
    };

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
    m_swapImages.resize(count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_swapImages.data());

    // One "render done" semaphore per swap image (see declaration comment) —
    // sized here since it depends on the image count just queried above.
    VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    m_renderDone.clear();
    m_renderDone.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        VkSemaphore s = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSemaphore(m_device, &semInfo, nullptr, &s));
        m_renderDone.emplace_back(s, [dev](VkSemaphore sem){ vkDestroySemaphore(dev, sem, nullptr); });
    }
}

void VulkanContext::createImageViews()
{
    VkDevice dev = m_device.get();
    m_swapViews.resize(m_swapImages.size());
    for (size_t i = 0; i < m_swapImages.size(); ++i)
    {
        VkImageViewCreateInfo info{};
        info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image                           = m_swapImages[i];
        info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        info.format                          = m_swapFormat;
        info.components                      = { VK_COMPONENT_SWIZZLE_IDENTITY,
                                                  VK_COMPONENT_SWIZZLE_IDENTITY,
                                                  VK_COMPONENT_SWIZZLE_IDENTITY,
                                                  VK_COMPONENT_SWIZZLE_IDENTITY };
        info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel   = 0;
        info.subresourceRange.levelCount     = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount     = 1;

        VkImageView raw = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(m_device, &info, nullptr, &raw));
        m_swapViews[i] = VkOwned<VkImageView>{raw,
            [dev](VkImageView v){ vkDestroyImageView(dev, v, nullptr); }
        };
    }
}

void VulkanContext::createRenderPass()
{
    VkAttachmentDescription color{};
    color.format         = m_swapFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments    = &color;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dep;

    VkRenderPass raw = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(m_device, &info, nullptr, &raw));
    VkDevice dev = m_device.get();
    m_renderPass = VkOwned<VkRenderPass>{raw,
        [dev](VkRenderPass rp){ vkDestroyRenderPass(dev, rp, nullptr); }
    };
}

void VulkanContext::createFramebuffers()
{
    VkDevice dev = m_device.get();
    m_framebuffers.resize(m_swapViews.size());
    for (size_t i = 0; i < m_swapViews.size(); ++i)
    {
        VkImageView view = m_swapViews[i];
        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = m_renderPass;
        info.attachmentCount = 1;
        info.pAttachments    = &view;
        info.width           = m_extent.width;
        info.height          = m_extent.height;
        info.layers          = 1;

        VkFramebuffer raw = VK_NULL_HANDLE;
        VK_CHECK(vkCreateFramebuffer(m_device, &info, nullptr, &raw));
        m_framebuffers[i] = VkOwned<VkFramebuffer>{raw,
            [dev](VkFramebuffer fb){ vkDestroyFramebuffer(dev, fb, nullptr); }
        };
    }
}

void VulkanContext::createCommandPool()
{
    VkCommandPoolCreateInfo info{};
    info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = *m_families.graphics;

    VkCommandPool raw = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(m_device, &info, nullptr, &raw));
    VkDevice dev = m_device.get();
    m_cmdPool = VkOwned<VkCommandPool>{raw,
        [dev](VkCommandPool p){ vkDestroyCommandPool(dev, p, nullptr); }
    };
}

void VulkanContext::createCommandBuffers()
{
    VkCommandBufferAllocateInfo info{};
    info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool        = m_cmdPool;
    info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = MAX_FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &info, m_cmdBuffers.data()));
}

void VulkanContext::createSyncObjects()
{
    VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice dev = m_device.get();
    for (uint32_t i = 0; i < MAX_FRAMES; ++i)
    {
        VkSemaphore s1 = VK_NULL_HANDLE;
        VkFence     f  = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSemaphore(m_device, &si, nullptr, &s1));
        VK_CHECK(vkCreateFence    (m_device, &fi, nullptr, &f));
        m_imgReady[i] = VkOwned<VkSemaphore>{s1, [dev](VkSemaphore s){ vkDestroySemaphore(dev, s, nullptr); }};
        m_fence[i]    = VkOwned<VkFence>    {f,  [dev](VkFence     f){ vkDestroyFence    (dev, f, nullptr); }};
    }
    // m_renderDone is created per swap image in createSwapchain(), once the
    // image count is known — see the comment on its declaration.
}

void VulkanContext::createImGuiPool()
{
    // The ImGui Vulkan backend (1.92+) allocates sets with separate
    // SAMPLER + SAMPLED_IMAGE bindings, not just COMBINED_IMAGE_SAMPLER —
    // the pool must carry all three types or allocation is UB
    // (VK_ERROR_OUT_OF_POOL_MEMORY on strict implementations).
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000 },
    };
    VkDescriptorPoolCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets       = 1000;
    info.poolSizeCount = 3;
    info.pPoolSizes    = sizes;

    VkDescriptorPool raw = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(m_device, &info, nullptr, &raw));
    VkDevice dev = m_device.get();
    m_imguiPool = VkOwned<VkDescriptorPool>{raw,
        [dev](VkDescriptorPool p){ vkDestroyDescriptorPool(dev, p, nullptr); }
    };
}

// ── Swapchain recreation ──────────────────────────────────────────────
void VulkanContext::cleanupSwapchain()
{
    m_framebuffers.clear();
    m_swapViews.clear();
    m_swapImages.clear();
    m_renderDone.clear();
    m_swapchain.reset();
}

void VulkanContext::recreateSwapchain()
{
    // Only block while the window is minimised (0x0); asking first keeps a normal
    // resize from sleeping in glfwWaitEvents() until the next OS event arrives.
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    while (w == 0 || h == 0)
    {
        glfwWaitEvents();
        glfwGetFramebufferSize(m_window, &w, &h);
    }
    vkDeviceWaitIdle(m_device);
    cleanupSwapchain();
    createSwapchain();
    createImageViews();
    createFramebuffers();
}

// ── Frame loop ────────────────────────────────────────────────────────
VkCommandBuffer VulkanContext::beginFrame()
{
    VkFence fence = m_fence[m_currentFrame];
    vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);

    // Check *before* acquiring: a resize requested by the previous frame (via
    // its own VK_SUBOPTIMAL_KHR) or by the GLFW resize callback must not
    // reach vkAcquireNextImageKHR with the stale swapchain, otherwise we'd
    // signal imgReady and then abandon it unconsumed.
    //
    // Recreate and carry on with this same frame: bailing out here would starve
    // rendering for as long as resize events keep arriving (i.e. the whole drag).
    if (m_needsResize)
    {
        m_needsResize = false;
        recreateSwapchain();
    }

    VkSemaphore imgReady = m_imgReady[m_currentFrame];
    VkResult result = vkAcquireNextImageKHR(
        m_device, m_swapchain, UINT64_MAX,
        imgReady, VK_NULL_HANDLE, &m_imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // An error result — imgReady is left unsignaled per spec, safe to retry or bail.
        recreateSwapchain();
        result = vkAcquireNextImageKHR(
            m_device, m_swapchain, UINT64_MAX,
            imgReady, VK_NULL_HANDLE, &m_imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) return VK_NULL_HANDLE; // still moving: next frame
    }
    if (result == VK_SUBOPTIMAL_KHR)
        // Image was acquired (and imgReady *is* signaled) — still usable this
        // frame. Consume it normally below and recreate before the next
        // acquire instead of abandoning it unconsumed.
        m_needsResize = true;
    else
        VK_CHECK(result);

    vkResetFences(m_device, 1, &fence);

    VkCommandBuffer cmd = m_cmdBuffers[m_currentFrame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    VkClearValue clear{ .color = { { 0.0784f, 0.0784f, 0.0745f, 1.0f } } }; // CLAUDE.md #141413
    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = m_renderPass;
    rp.framebuffer       = m_framebuffers[m_imageIndex];
    rp.renderArea.offset = { 0, 0 };
    rp.renderArea.extent = m_extent;
    rp.clearValueCount   = 1;
    rp.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    return cmd;
}

void VulkanContext::endFrame()
{
    VkCommandBuffer cmd        = m_cmdBuffers[m_currentFrame];
    VkSemaphore     imgReady   = m_imgReady[m_currentFrame];
    VkSemaphore     renderDone = m_renderDone[m_imageIndex]; // indexed by swap image, not frame-in-flight
    VkFence         fence      = m_fence[m_currentFrame];

    vkCmdEndRenderPass(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &imgReady;
    submit.pWaitDstStageMask    = &waitStage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &renderDone;
    VK_CHECK(vkQueueSubmit(m_graphicsQueue, 1, &submit, fence));

    VkSwapchainKHR swapchain = m_swapchain;
    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &renderDone;
    present.swapchainCount     = 1;
    present.pSwapchains        = &swapchain;
    present.pImageIndices      = &m_imageIndex;

    VkResult result = vkQueuePresentKHR(m_presentQueue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        m_needsResize = true;
    else
        VK_CHECK(result);

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES;
}

void VulkanContext::waitIdle() const
{
    vkDeviceWaitIdle(m_device);
}
