#pragma once

#include "IDecoder.hpp"

#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <atomic>
#include <memory>

class Demuxer;

/// Vulkan hardware-accelerated decoder.
/// Shares the Vulkan instance/device from VulkanContext with FFmpeg
/// via AV_HWDEVICE_TYPE_VULKAN for zero-copy GPU decode → render.
class VulkanDecoder : public IDecoder {
public:
    explicit VulkanDecoder(const DecoderConfig &config);
    ~VulkanDecoder() override;

    VulkanDecoder(const VulkanDecoder &) = delete;
    VulkanDecoder &operator=(const VulkanDecoder &) = delete;

    // IDecoder interface
    bool open() override;
    void run() override;
    void stop() override;
    void seek(double targetSec,
              std::function<void()> onComplete = nullptr) override;

    int width() const override { return m_width; }
    int height() const override { return m_height; }
    double duration() const override { return m_duration; }
    AVPixelFormat pixelFormat() const override { return m_pixelFormat; }
    AVRational timeBase() const override { return m_timeBase; }
    AVRational frameRate() const override { return m_frameRate; }

private:
    void doSeek();

    DecoderConfig m_config;
    BoundedQueue<AVFramePtr> *m_queue;
    std::unique_ptr<Demuxer> m_demuxer;

    AVCodecContext *m_codecCtx = nullptr;
    AVBufferRef *m_hwDeviceRef = nullptr;

    int m_width = 0;
    int m_height = 0;
    double m_duration = 0.0;
    AVPixelFormat m_pixelFormat = AV_PIX_FMT_NONE;
    AVRational m_timeBase{};
    AVRational m_frameRate{};

    // Seek state
    std::atomic<bool> m_seekRequested{false};
    double m_seekTargetSec = 0.0;
    int64_t m_seekToPts = AV_NOPTS_VALUE;
    std::function<void()> m_onSeekComplete;

    std::atomic<bool> m_stop{false};
};
