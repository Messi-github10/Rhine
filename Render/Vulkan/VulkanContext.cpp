#include "VulkanContext.hpp"
#include "Logger.hpp"

// XCB types required by vulkan_xcb.h
#include <xcb/xcb.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_wayland.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/log.h>
#include <libplacebo/swapchain.h>

#include <QGuiApplication>
#include <QWindow>
#include <QtGui/qpa/qplatformnativeinterface.h>

#include <spdlog/spdlog.h>
#include <chrono>
#include <string>

namespace {

void plLogCallback(void*, enum pl_log_level level, const char* msg) {
    auto* logger = spdlog::default_logger_raw();
    switch (level) {
        case PL_LOG_FATAL:
        case PL_LOG_ERR:   logger->log(spdlog::level::err, "[libplacebo] {}", msg); break;
        case PL_LOG_WARN:  logger->log(spdlog::level::warn, "[libplacebo] {}", msg); break;
        case PL_LOG_INFO:  logger->log(spdlog::level::info, "[libplacebo] {}", msg); break;
        default: break; // debug/trace: skip
    }
}

} // anonymous namespace

VulkanContext& VulkanContext::instance() {
    static VulkanContext ctx;
    return ctx;
}

VulkanContext::~VulkanContext() {
    destroy();
}

bool VulkanContext::init() {
    if (m_vk) return true;

    // Detect Qt platform for Vulkan surface extension
    std::string platform = QGuiApplication::platformName().toStdString();
    const char *surfaceExt = nullptr;

    if (platform == "xcb") {
        surfaceExt = VK_KHR_XCB_SURFACE_EXTENSION_NAME;
        m_isXcb = true;
        spdlog::info("[VulkanContext] Platform: XCB");
    } else if (platform.rfind("wayland", 0) == 0) {
        surfaceExt = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
        m_isXcb = false;
        spdlog::info("[VulkanContext] Platform: Wayland");
    } else {
        spdlog::warn("[VulkanContext] Unknown platform '{}', surface creation disabled",
                     platform);
    }

    // Create libplacebo log
    struct pl_log_params logParams = pl_log_default_params;
    logParams.log_cb = plLogCallback;
    logParams.log_level = PL_LOG_INFO;
    m_log = pl_log_create_362(PL_API_VER, &logParams);

    // Instance params — enable the platform surface extension
    struct pl_vk_inst_params instParams = pl_vk_inst_default_params;
    instParams.debug = true;
    if (surfaceExt) {
        instParams.extensions = &surfaceExt;
        instParams.num_extensions = 1;
    }

    // Vulkan params
    struct pl_vulkan_params vkParams = pl_vulkan_default_params;
    vkParams.instance_params = &instParams;

    m_vk = pl_vulkan_create(m_log, &vkParams);

    if (!m_vk) {
        spdlog::error("[VulkanContext] Failed to create Vulkan device");
        pl_log_destroy(&m_log);
        return false;
    }

    spdlog::info("[VulkanContext] Vulkan initialized successfully");
    return true;
}

void VulkanContext::destroy() {
    auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };

    if (m_vk) {
        auto t0 = std::chrono::steady_clock::now();
        pl_vulkan_destroy(&m_vk);
        m_vk = nullptr;
        spdlog::info("[VulkanContext] pl_vulkan_destroy took {}ms", ms(std::chrono::steady_clock::now() - t0));
    }
    if (m_log) {
        auto t0 = std::chrono::steady_clock::now();
        pl_log_destroy(&m_log);
        m_log = nullptr;
        spdlog::info("[VulkanContext] pl_log_destroy took {}ms", ms(std::chrono::steady_clock::now() - t0));
    }
}

VkInstance VulkanContext::vkInstance() const {
    return m_vk ? m_vk->instance : VK_NULL_HANDLE;
}

pl_gpu VulkanContext::gpu() const {
    return m_vk ? m_vk->gpu : nullptr;
}

VkSurfaceKHR VulkanContext::createSurface(QWindow *window) const {
    if (!m_vk || !window) return VK_NULL_HANDLE;

    VkInstance instance = m_vk->instance;
    auto *native = QGuiApplication::platformNativeInterface();

    if (m_isXcb) {
        auto *connection = static_cast<xcb_connection_t *>(
            native->nativeResourceForIntegration("connection"));
        xcb_window_t xcbWin = static_cast<xcb_window_t>(window->winId());

        if (!connection) {
            spdlog::error("[VulkanContext] Failed to get xcb_connection_t");
            return VK_NULL_HANDLE;
        }

        VkXcbSurfaceCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        createInfo.connection = connection;
        createInfo.window = xcbWin;

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult result = vkCreateXcbSurfaceKHR(instance, &createInfo, nullptr, &surface);
        if (result != VK_SUCCESS) {
            spdlog::error("[VulkanContext] vkCreateXcbSurfaceKHR failed: {}",
                          static_cast<int>(result));
            return VK_NULL_HANDLE;
        }
        spdlog::info("[VulkanContext] Created XCB surface {}x{}",
                     window->width(), window->height());
        return surface;
    }

    // Wayland
    auto *display = static_cast<wl_display *>(
        native->nativeResourceForIntegration("display"));
    auto *surface_ptr = static_cast<wl_surface *>(
        native->nativeResourceForWindow("surface", window));

    if (!display || !surface_ptr) {
        spdlog::error("[VulkanContext] Failed to get Wayland display/surface");
        return VK_NULL_HANDLE;
    }

    VkWaylandSurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    createInfo.display = display;
    createInfo.surface = surface_ptr;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult result = vkCreateWaylandSurfaceKHR(instance, &createInfo, nullptr, &surface);
    if (result != VK_SUCCESS) {
        spdlog::error("[VulkanContext] vkCreateWaylandSurfaceKHR failed: {}",
                      static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    spdlog::info("[VulkanContext] Created Wayland surface {}x{}",
                 window->width(), window->height());
    return surface;
}

pl_swapchain VulkanContext::createSwapchain(VkSurfaceKHR surface, int /*width*/, int /*height*/) {
    if (!m_vk || !surface) return nullptr;

    struct pl_vulkan_swapchain_params swParams = {
        .surface = surface,
        .present_mode = VK_PRESENT_MODE_FIFO_KHR,
        .swapchain_depth = 3,
    };

    return pl_vulkan_create_swapchain(m_vk, &swParams);
}

VkPhysicalDevice VulkanContext::physDevice() const {
    return m_vk ? m_vk->phys_device : VK_NULL_HANDLE;
}

VkDevice VulkanContext::device() const {
    return m_vk ? m_vk->device : VK_NULL_HANDLE;
}

uint32_t VulkanContext::graphicsQueueFamily() const {
    return m_vk ? m_vk->queue_graphics.index : UINT32_MAX;
}

const char * const *VulkanContext::extensions(int &count) const {
    if (m_vk) {
        count = m_vk->num_extensions;
        return m_vk->extensions;
    }
    count = 0;
    return nullptr;
}

const VkPhysicalDeviceFeatures2 *VulkanContext::features() const {
    return m_vk ? m_vk->features : nullptr;
}
