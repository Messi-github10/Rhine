#pragma once

#include "FrameQueue.hpp"

#include <functional>
#include <string>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

struct DecoderConfig {
    std::string filePath;
    BoundedQueue<AVFramePtr> *frameQueue = nullptr;
};

/// Abstract decoder interface.
/// Implementations provide CPU software decoding (CpuDecoder) or
/// Vulkan hardware-accelerated decoding (VulkanDecoder).
class IDecoder {
public:
    virtual ~IDecoder() = default;

    /// Open the file, probe stream info, initialize codec.
    /// Returns true on success.
    virtual bool open() = 0;

    /// Blocking decode loop. Pushes AVFramePtr into the configured queue.
    /// Returns when EOF is reached or stop() is called.
    virtual void run() = 0;

    /// Signal the decode loop to exit. Thread-safe.
    virtual void stop() = 0;

    /// Request a seek to targetSec (seconds). Thread-safe.
    /// onComplete is called from the decode thread after the seek completes.
    virtual void seek(double targetSec,
                      std::function<void()> onComplete = nullptr) = 0;

    // --- Stream metadata (valid after open()) ---
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual double duration() const = 0;
    virtual AVPixelFormat pixelFormat() const = 0;
    virtual AVRational timeBase() const = 0;
    virtual AVRational frameRate() const = 0;
};
