#pragma once

#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <functional>
#include <string>

struct DecoderConfig {
    std::string filePath;
    BoundedQueue<AVFramePtr> *frameQueue = nullptr;
};

class Decoder {
public:
    explicit Decoder(const DecoderConfig &config);
    ~Decoder();

    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    bool open();
    void run();  // blocking decode loop
    void stop();

    // Seek to target position (seconds). Thread-safe — sets a flag
    // processed by the decode loop. onComplete is called from decode thread
    // after the seek + flush + queue-clear completes.
    void seek(double targetSec, std::function<void()> onComplete = nullptr);

    int width() const { return m_width; }
    int height() const { return m_height; }
    double duration() const { return m_duration; }
    AVPixelFormat pixelFormat() const { return m_pixelFormat; }
    AVRational timeBase() const { return m_timeBase; }
    AVRational frameRate() const { return m_frameRate; }

private:
    void doSeek();

    DecoderConfig m_config;
    BoundedQueue<AVFramePtr> *m_queue;

    AVFormatContext *m_fmtCtx = nullptr;
    AVCodecContext *m_codecCtx = nullptr;
    int m_videoStreamIdx = -1;

    int m_width = 0;
    int m_height = 0;
    double m_duration = 0.0;
    AVPixelFormat m_pixelFormat = AV_PIX_FMT_NONE;
    AVRational m_timeBase{};
    AVRational m_frameRate{};

    // Seek state
    std::atomic<bool> m_seekRequested{false};
    double m_seekTargetSec = 0.0;
    int64_t m_seekToPts = AV_NOPTS_VALUE; // PTS to skip to (forward decode)
    std::function<void()> m_onSeekComplete;

    std::atomic<bool> m_stop{false};
};
