#pragma once

#include "renderer/vk_owned.hpp"
#include <vulkan/vulkan.h>
#include <array>
#include <format>
#include <optional>
#include <stdexcept>
#include <vector>

struct GLFWwindow;

// ── Error helper ─────────────────────────────────────────────────────
#define VK_CHECK(call)                                                       \
    do {                                                                     \
        if (VkResult _r = (call); _r != VK_SUCCESS)                          \
            throw std::runtime_error(                                       \
                std::format("[Vulkan] {} failed ({})", #call, (int32_t)_r)); \
    } while (0)

struct QueueFamilies
{
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    bool complete() const { return graphics && present; }
};

// Classic render-pass/framebuffer Vulkan setup (portable across GPUs/drivers
// that don't support VK_KHR_dynamic_rendering), with careful frame-in-flight
// vs. per-swap-image sync object indexing (see m_renderDone below).
class VulkanContext
{
public:
    explicit VulkanContext(GLFWwindow* window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Returns a ready command buffer, or VK_NULL_HANDLE when the swapchain
    // was just recreated (skip this frame, call again next iteration).
    VkCommandBuffer beginFrame();
    void            endFrame();
    void            waitIdle() const;

    // Called by the GLFW framebuffer-resize callback.
    void onResize() { m_needsResize = true; }

    // ── Accessors (for ImGui init) ────────────────────────────────────
    VkInstance          instance()       const { return m_instance; }
    VkPhysicalDevice    physDevice()     const { return m_physDevice; }
    VkDevice            device()         const { return m_device; }
    VkQueue             graphicsQueue()  const { return m_graphicsQueue; }
    uint32_t            graphicsFamily() const { return *m_families.graphics; }
    VkRenderPass        renderPass()     const { return m_renderPass; }
    VkDescriptorPool    imguiPool()      const { return m_imguiPool; }
    uint32_t            minImageCount()  const { return m_minImageCount; }
    uint32_t            imageCount()     const { return (uint32_t)m_swapImages.size(); }
    VkExtent2D          extent()         const { return m_extent; }

private:
    void createInstance();
    void createDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createDevice();
    void createSwapchain();
    void createImageViews();
    void createRenderPass();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void createImGuiPool();

    void recreateSwapchain();
    void cleanupSwapchain();

    QueueFamilies      queryFamilies(VkPhysicalDevice dev) const;
    bool               isSuitable(VkPhysicalDevice dev)    const;
    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>&) const;
    VkPresentModeKHR   choosePresentMode(const std::vector<VkPresentModeKHR>&)     const;
    VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR&)               const;

    GLFWwindow* m_window;

    // Members are declared outermost-first so that reverse-order destruction
    // automatically matches the required Vulkan teardown sequence.

    VkOwned<VkInstance>               m_instance;
    VkOwned<VkSurfaceKHR>             m_surface;
    VkOwned<VkDebugUtilsMessengerEXT> m_debugMessenger;

    VkPhysicalDevice  m_physDevice    = VK_NULL_HANDLE; // not owned
    VkOwned<VkDevice> m_device;
    VkQueue           m_graphicsQueue = VK_NULL_HANDLE; // not owned
    VkQueue           m_presentQueue  = VK_NULL_HANDLE; // not owned
    QueueFamilies     m_families;

    VkOwned<VkRenderPass> m_renderPass;

    VkOwned<VkSwapchainKHR>             m_swapchain;
    std::vector<VkImage>                m_swapImages; // not owned — belong to swapchain
    VkFormat                            m_swapFormat    = VK_FORMAT_UNDEFINED;
    VkExtent2D                          m_extent        = {};
    uint32_t                            m_minImageCount = 2;
    std::vector<VkOwned<VkImageView>>   m_swapViews;
    std::vector<VkOwned<VkFramebuffer>> m_framebuffers;

    VkOwned<VkCommandPool> m_cmdPool;

    static constexpr uint32_t MAX_FRAMES = 2;
    std::array<VkCommandBuffer, MAX_FRAMES> m_cmdBuffers = {}; // freed with pool

    // m_imgReady/m_fence are indexed by frame-in-flight (m_currentFrame).
    // m_renderDone must instead be indexed by swap image (m_imageIndex):
    // when swapImages.size() > MAX_FRAMES, a frame-in-flight-indexed
    // "render done" semaphore can still be in use by a previous present
    // of the same image when the next frame tries to re-signal it.
    std::array<VkOwned<VkSemaphore>, MAX_FRAMES> m_imgReady = {};
    std::vector<VkOwned<VkSemaphore>>            m_renderDone;
    std::array<VkOwned<VkFence>, MAX_FRAMES>     m_fence = {};

    VkOwned<VkDescriptorPool> m_imguiPool;

    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex   = 0;
    bool     m_needsResize  = false;
};
