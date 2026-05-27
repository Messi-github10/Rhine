#include "VulkanRenderer.hpp"
#include "VulkanContext.hpp"

#include <libplacebo/renderer.h>
#include <libplacebo/gpu.h>
#include <libplacebo/swapchain.h>

// Include libav.h first with PL_LIBAV_IMPLEMENTATION=0 to suppress impl warning
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>
#include <vector>

static double ptsToSec(int64_t pts, AVRational tb) {
    return static_cast<double>(pts) * tb.num / tb.den;
}

VulkanRenderer::VulkanRenderer(const Config &config)
    : m_config(config)
    , m_queue(config.frameQueue)
{
}

VulkanRenderer::~VulkanRenderer() {
    stop();
}

bool VulkanRenderer::initSwapchain() {
    m_gpu = VulkanContext::instance().gpu();
    if (!m_gpu) {
        spdlog::error("[VulkanRenderer] No GPU available");
        return false;
    }

    // Create renderer
    m_plRenderer = pl_renderer_create(VulkanContext::instance().plLog(), m_gpu);
    if (!m_plRenderer) {
        spdlog::error("[VulkanRenderer] Failed to create pl_renderer");
        return false;
    }

    // Create swapchain
    if (m_config.surface) {
        m_swapchain = VulkanContext::instance().createSwapchain(
            m_config.surface, m_config.windowWidth, m_config.windowHeight);

        if (!m_swapchain) {
            spdlog::error("[VulkanRenderer] Failed to create swapchain");
            pl_renderer_destroy(&m_plRenderer);
            return false;
        }

        // Set initial size
        m_swWidth = m_config.windowWidth;
        m_swHeight = m_config.windowHeight;
        if (m_swWidth > 0 && m_swHeight > 0) {
            int w = m_swWidth;
            int h = m_swHeight;
            pl_swapchain_resize(m_swapchain, &w, &h);
            spdlog::info("[VulkanRenderer] Swapchain resized to {}x{} (requested {}x{})",
                         w, h, m_swWidth, m_swHeight);
        }
    } else {
        spdlog::error("[VulkanRenderer] No surface provided");
        pl_renderer_destroy(&m_plRenderer);
        return false;
    }

    spdlog::info("[VulkanRenderer] Swapchain initialized");
    return true;
}

void VulkanRenderer::handleResize() {
    int newW = m_resizeWidth.load(std::memory_order_acquire);
    int newH = m_resizeHeight.load(std::memory_order_acquire);
    if (newW <= 0 || newH <= 0) return;

    int w = newW, h = newH;
    if (pl_swapchain_resize(m_swapchain, &w, &h)) {
        m_swWidth = w;
        m_swHeight = h;
        // Clear after successful resize so next signal pair is detected
        m_resizeWidth.store(0, std::memory_order_release);
        m_resizeHeight.store(0, std::memory_order_release);
        spdlog::info("[VulkanRenderer] Resized to {}x{}", w, h);
    } else {
        spdlog::warn("[VulkanRenderer] Resize to {}x{} failed", newW, newH);
    }
}

void VulkanRenderer::run(FrameCallback /*onFrame*/, DoneCallback onDone) {
    if (!initSwapchain()) {
        if (onDone) onDone();
        return;
    }

    // Timing state
    std::chrono::steady_clock::time_point startTime;
    int64_t startPts = AV_NOPTS_VALUE;
    int64_t lastPts = AV_NOPTS_VALUE;
    double pauseAccum = 0.0;
    bool needsFirstFrame = true;

    // Texture array for pl_map_avframe (4 textures for up to 4 planes)
    pl_tex gpuTex[4] = {nullptr, nullptr, nullptr, nullptr};

    while (!m_stop) {
        // --- Pause gate ---
        if (!m_isPlaying && !needsFirstFrame) {
            auto pauseStart = std::chrono::steady_clock::now();
            {
                std::unique_lock lock(m_pauseMutex);
                m_pauseCV.wait(lock, [this] {
                    return m_isPlaying.load() || m_stop.load();
                });
            }
            auto pauseEnd = std::chrono::steady_clock::now();
            pauseAccum +=
                std::chrono::duration<double>(pauseEnd - pauseStart).count();
            if (m_stop) break;
        }

        // Pop frame from queue
        auto optFrame = m_queue->pop();
        if (!optFrame) break;

        AVFrame *frame = optFrame->get();

        // --- PTS Timing (before rendering, so frame is fresh) ---
        if (frame->pts != AV_NOPTS_VALUE) {
            if (lastPts != AV_NOPTS_VALUE) {
                int64_t ptsDiff = frame->pts - lastPts;
                if (ptsDiff < 0 || ptsToSec(ptsDiff, m_config.timeBase) > 2.0) {
                    startPts = AV_NOPTS_VALUE;
                    pauseAccum = 0.0;
                    needsFirstFrame = true;
                }
            }
            lastPts = frame->pts;

            if (startPts == AV_NOPTS_VALUE) {
                startPts = frame->pts;
                startTime = std::chrono::steady_clock::now();
                pauseAccum = 0.0;
            } else {
                double elapsed = ptsToSec(frame->pts - startPts, m_config.timeBase);
                auto targetTime = startTime +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(elapsed + pauseAccum));
                std::unique_lock lock(m_pauseMutex);
                m_pauseCV.wait_until(lock, targetTime, [this] { return m_stop.load(); });
                if (m_stop) break;
            }
            m_currentTime = ptsToSec(frame->pts, m_config.timeBase);
        }

        // --- Handle resize ---
        handleResize();

        // --- Map AVFrame to GPU ---
        struct pl_frame plFrame;
        memset(&plFrame, 0, sizeof(plFrame));
        pl_frame_from_avframe(&plFrame, frame);

        if (!pl_map_avframe(m_gpu, &plFrame, gpuTex, frame)) {
            spdlog::warn("[VulkanRenderer] Failed to map AVFrame");
            continue;
        }

        // --- Start swapchain frame ---
        struct pl_swapchain_frame swFrame;
        if (!pl_swapchain_start_frame(m_swapchain, &swFrame)) {
            // Window hidden or other transient issue
            pl_unmap_avframe(m_gpu, &plFrame);
            if (m_stop) break;
            continue;
        }

        // --- Build render target from swapchain fbo ---
        struct pl_frame target;
        pl_frame_from_swapchain(&target, &swFrame);
        // Use source dimensions for crop (video aspect ratio preserved by renderer)
        target.crop.x0 = 0;
        target.crop.y0 = 0;
        target.crop.x1 = static_cast<float>(m_config.srcWidth);
        target.crop.y1 = static_cast<float>(m_config.srcHeight);

        // --- Render ---
        struct pl_render_params renderParams = pl_render_default_params;
        if (!pl_render_image(m_plRenderer, &plFrame, &target, &renderParams)) {
            spdlog::error("[VulkanRenderer] Render failed");
            pl_swapchain_submit_frame(m_swapchain); // still need to submit to keep lockstep
            pl_swapchain_swap_buffers(m_swapchain);
            pl_unmap_avframe(m_gpu, &plFrame);
            continue;
        }

        // --- Submit and present ---
        if (!pl_swapchain_submit_frame(m_swapchain)) {
            spdlog::error("[VulkanRenderer] Submit failed");
            pl_unmap_avframe(m_gpu, &plFrame);
            break;
        }

        pl_swapchain_swap_buffers(m_swapchain);

        // --- Unmap AVFrame ---
        pl_unmap_avframe(m_gpu, &plFrame);

        needsFirstFrame = false;
    }

    // Cleanup
    {
        using clock = std::chrono::steady_clock;
        auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        auto t0 = clock::now();
        if (m_swapchain) {
            pl_swapchain_destroy(&m_swapchain);
        }
        auto t1 = clock::now();
        if (m_plRenderer) {
            pl_renderer_destroy(&m_plRenderer);
        }
        auto t2 = clock::now();
        spdlog::info("[VulkanRenderer] cleanup: swDestroy={}ms rendererDestroy={}ms",
                     ms(t1-t0), ms(t2-t1));
    }

    m_isPlaying = false;
    if (onDone) onDone();
}

void VulkanRenderer::stop() {
    m_stop = true;
    m_pauseCV.notify_one();
}

void VulkanRenderer::play() {
    m_isPlaying = true;
    m_pauseCV.notify_one();
}

void VulkanRenderer::pause() {
    m_isPlaying = false;
}

bool VulkanRenderer::isPlaying() const {
    return m_isPlaying.load();
}

double VulkanRenderer::currentTime() const {
    return m_currentTime.load();
}

void VulkanRenderer::resize(int width, int height) {
    if (width > 0) m_resizeWidth.store(width, std::memory_order_release);
    if (height > 0) m_resizeHeight.store(height, std::memory_order_release);
}
