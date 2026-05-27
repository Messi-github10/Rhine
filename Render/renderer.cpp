#include "renderer.hpp"

#include <cstdio>
#include <thread>

Renderer::Renderer(const RendererConfig &config)
    : m_config(config)
    , m_queue(config.frameQueue)
{
}

Renderer::~Renderer() {
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
    }
}

bool Renderer::initSwsContext() {
    m_swsCtx = sws_getContext(
        m_config.srcWidth, m_config.srcHeight, m_config.srcFormat,
        m_config.srcWidth, m_config.srcHeight, AV_PIX_FMT_RGB32,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!m_swsCtx) {
        std::fprintf(stderr, "[Renderer] Failed to create sws context\n");
        return false;
    }

    m_dstWidth = m_config.srcWidth;
    m_dstHeight = m_config.srcHeight;
    return true;
}

static double ptsToSec(int64_t pts, AVRational tb) {
    return static_cast<double>(pts) * tb.num / tb.den;
}

void Renderer::play() {
    m_isPlaying = true;
    m_pauseCV.notify_one();
}

void Renderer::pause() {
    m_isPlaying = false;
}

void Renderer::resetTiming() {
    m_startPts = AV_NOPTS_VALUE;
    m_pauseAccumulated = 0.0;
}

void Renderer::run(FrameCallback onFrame, DoneCallback onDone) {
    if (!initSwsContext()) {
        if (onDone) onDone();
        return;
    }

    const int rgbStride = m_dstWidth * 4;
    std::vector<uint8_t> rgbBuffer(rgbStride * m_dstHeight);

    while (!m_stop) {
        // --- Pause gate (bypassed for first frame) ---
        if (!m_isPlaying && !m_needsFirstFrame.load()) {
            auto pauseStart = std::chrono::steady_clock::now();
            {
                std::unique_lock lock(m_pauseMutex);
                m_pauseCV.wait(lock, [this] { return m_isPlaying.load() || m_stop.load(); });
            }
            auto pauseEnd = std::chrono::steady_clock::now();
            m_pauseAccumulated +=
                std::chrono::duration<double>(pauseEnd - pauseStart).count();
            if (m_stop) break;
        }

        auto optFrame = m_queue->pop();
        if (!optFrame) {
            break; // queue shut down
        }

        AVFrame *frame = optFrame->get();

        // sws_scale: YUV → RGB32
        uint8_t *dstData[4] = {rgbBuffer.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {rgbStride, 0, 0, 0};
        sws_scale(m_swsCtx,
                  frame->data, frame->linesize, 0, m_config.srcHeight,
                  dstData, dstStride);

        // Timing control (compensates for accumulated pause duration)
        if (frame->pts != AV_NOPTS_VALUE) {
            // Detect PTS discontinuity (seek happened) — auto-reset timing
            if (m_lastPts != AV_NOPTS_VALUE) {
                int64_t ptsDiff = frame->pts - m_lastPts;
                // Backward jump or forward jump > 2 seconds = seek
                if (ptsDiff < 0 || ptsToSec(ptsDiff, m_config.timeBase) > 2.0) {
                    std::fprintf(stderr, "[Renderer] PTS jump detected, resetting timing (%.2fs)\n",
                                 ptsToSec(ptsDiff, m_config.timeBase));
                    m_startPts = AV_NOPTS_VALUE;
                    m_pauseAccumulated = 0.0;
                    m_needsFirstFrame = true;  // render first post-seek frame immediately
                }
            }
            m_lastPts = frame->pts;

            if (m_startPts == AV_NOPTS_VALUE) {
                m_startPts = frame->pts;
                m_startTime = std::chrono::steady_clock::now();
                m_pauseAccumulated = 0.0;
            } else {
                double elapsed = ptsToSec(frame->pts - m_startPts, m_config.timeBase);
                auto targetTime = m_startTime +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(elapsed + m_pauseAccumulated));
                std::unique_lock lock(m_pauseMutex);
                m_pauseCV.wait_until(lock, targetTime, [this] { return m_stop.load(); });
                if (m_stop) break;
            }
            m_currentTime = ptsToSec(frame->pts, m_config.timeBase);
        }

        // Build frame and hand off
        VideoFrame vf;
        vf.data = rgbBuffer;
        vf.width = m_dstWidth;
        vf.height = m_dstHeight;
        vf.stride = rgbStride;
        onFrame(std::move(vf));

        // First frame rendered — re-engage pause gate
        m_needsFirstFrame = false;
    }

    m_isPlaying = false;
    if (onDone) onDone();
}

void Renderer::stop() {
    m_stop = true;
    m_pauseCV.notify_one(); // wake up if paused
}
