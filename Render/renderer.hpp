#pragma once

#include "IRenderer.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

struct RendererConfig {
    BoundedQueue<AVFramePtr> *frameQueue = nullptr;
    int srcWidth = 0;
    int srcHeight = 0;
    AVPixelFormat srcFormat = AV_PIX_FMT_NONE;
    AVRational timeBase{};
    AVRational frameRate{};
};

class Renderer : public IRenderer {
public:
    explicit Renderer(const RendererConfig &config);
    ~Renderer() override;

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // IRenderer interface
    void run(FrameCallback onFrame, DoneCallback onDone = nullptr) override;
    void stop() override;
    void play() override;
    void pause() override;
    bool isPlaying() const override { return m_isPlaying.load(); }
    double currentTime() const override { return m_currentTime.load(); }
    void setDuration(double d) override { m_duration = d; }

    void resetTiming();

private:
    bool initSwsContext();

    RendererConfig m_config;
    BoundedQueue<AVFramePtr> *m_queue;

    SwsContext *m_swsCtx = nullptr;
    int m_dstWidth = 0;
    int m_dstHeight = 0;

    // Stop flag
    std::atomic<bool> m_stop{false};

    // First-frame gate: bypass pause until one frame is rendered
    std::atomic<bool> m_needsFirstFrame{true};

    // Playback state
    std::atomic<bool> m_isPlaying{false}; // start paused until user clicks Play
    std::atomic<double> m_currentTime{0.0};
    double m_duration = 0.0;

    // Pause compensation
    double m_pauseAccumulated = 0.0;
    std::mutex m_pauseMutex;
    std::condition_variable m_pauseCV;

    // Timing
    std::chrono::steady_clock::time_point m_startTime;
    int64_t m_startPts = AV_NOPTS_VALUE;
    int64_t m_lastPts = AV_NOPTS_VALUE;
};
