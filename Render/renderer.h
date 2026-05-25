#pragma once

#include "framequeue.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

struct RendererConfig {
    BoundedQueue<AVFramePtr> *frameQueue = nullptr;
    int srcWidth = 0;
    int srcHeight = 0;
    AVPixelFormat srcFormat = AV_PIX_FMT_NONE;
    AVRational timeBase{};
    AVRational frameRate{};
};

class Renderer {
public:
    using FrameCallback = std::function<void(VideoFrame)>;
    using DoneCallback = std::function<void()>;

    explicit Renderer(const RendererConfig &config);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // Blocking render loop. Calls onFrame for each RGB frame, onDone at end.
    void run(FrameCallback onFrame, DoneCallback onDone = nullptr);
    void stop();

    // Playback control (thread-safe)
    void play();
    void pause();
    void resetTiming();
    bool isPlaying() const { return m_isPlaying.load(); }

    // Progress (thread-safe, polled from any thread)
    double currentTime() const { return m_currentTime.load(); }
    void setDuration(double d) { m_duration = d; }

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
