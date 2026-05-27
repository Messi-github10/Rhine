#pragma once

#include <vulkan/vulkan.h>

// Forward declarations for libplacebo C types
struct pl_log_t;
typedef const struct pl_log_t *pl_log;

struct pl_gpu_t;
typedef const struct pl_gpu_t *pl_gpu;

struct pl_vulkan_t;
typedef const struct pl_vulkan_t *pl_vulkan;

struct pl_swapchain_t;
typedef const struct pl_swapchain_t *pl_swapchain;

class QWindow;

class VulkanContext {
public:
    static VulkanContext& instance();

    // Initialize libplacebo Vulkan backend. Creates VkInstance and VkDevice
    // internally via pl_vulkan_create(). Enables platform surface extension
    // based on the current Qt platform (XCB or Wayland).
    bool init();

    // Destroy all Vulkan resources.
    void destroy();

    bool isValid() const { return m_vk != nullptr; }

    // libplacebo abstractions
    pl_log plLog() const { return m_log; }
    pl_gpu gpu() const;

    // Underlying Vulkan handles
    pl_vulkan plVulkan() const { return m_vk; }
    VkInstance vkInstance() const;
    VkPhysicalDevice physDevice() const;
    VkDevice device() const;
    uint32_t graphicsQueueFamily() const;

    // Device extensions enabled by libplacebo at device creation
    const char * const *extensions(int &count) const;

    // Device features enabled at device creation
    const VkPhysicalDeviceFeatures2 *features() const;

    // Create a VkSurfaceKHR from a Qt window (XCB or Wayland)
    VkSurfaceKHR createSurface(QWindow *window) const;

    // Create a swapchain for a given window surface
    pl_swapchain createSwapchain(VkSurfaceKHR surface, int width, int height);

private:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    pl_log m_log = nullptr;
    pl_vulkan m_vk = nullptr;
    bool m_isXcb = false;
};
