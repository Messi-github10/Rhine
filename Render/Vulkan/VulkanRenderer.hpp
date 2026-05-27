#pragma once

#include "IRenderer.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

// libplacebo C types
struct pl_gpu_t;
typedef const struct pl_gpu_t *pl_gpu;
struct pl_renderer_t;
typedef struct pl_renderer_t *pl_renderer;
struct pl_swapchain_t;
typedef const struct pl_swapchain_t *pl_swapchain;

#include <vulkan/vulkan.h>

extern "C" {
#include <libavutil/rational.h>
}

class VulkanRenderer : public IRenderer {
public:
    struct Config {
        BoundedQueue<AVFramePtr> *frameQueue = nullptr;
        int srcWidth = 0;
        int srcHeight = 0;
        AVRational timeBase{};
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        int windowWidth = 0;
        int windowHeight = 0;
    };

    explicit VulkanRenderer(const Config &config);
    ~VulkanRenderer() override;

    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;

    // IRenderer interface
    void run(FrameCallback onFrame, DoneCallback onDone) override;
    void stop() override;
    void play() override;
    void pause() override;
    bool isPlaying() const override;
    double currentTime() const override;
    void setDuration(double d) override { m_duration = d; }

    // Thread-safe resize request from main thread
    void resize(int width, int height);

private:
    bool initSwapchain();
    void handleResize();

    Config m_config;
    BoundedQueue<AVFramePtr> *m_queue;

    // libplacebo objects
    pl_gpu m_gpu = nullptr;
    pl_renderer m_plRenderer = nullptr;
    pl_swapchain m_swapchain = nullptr;

    // Current swapchain extent
    int m_swWidth = 0;
    int m_swHeight = 0;

    // Pending resize from main thread
    std::atomic<int> m_resizeWidth{0};
    std::atomic<int> m_resizeHeight{0};

    // State
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_isPlaying{false};
    std::atomic<double> m_currentTime{0.0};
    double m_duration = 0.0;

    // Pause
    std::mutex m_pauseMutex;
    std::condition_variable m_pauseCV;
};
