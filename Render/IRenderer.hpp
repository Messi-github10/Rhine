#pragma once

#include "FrameQueue.hpp"

#include <cstdint>
#include <functional>

class IRenderer {
public:
    using FrameCallback = std::function<void(VideoFrame)>;
    using DoneCallback = std::function<void()>;

    virtual ~IRenderer() = default;

    // Blocking render loop. Calls onFrame for each frame, onDone at end.
    virtual void run(FrameCallback onFrame, DoneCallback onDone) = 0;

    virtual void stop() = 0;

    // Playback control (thread-safe)
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual bool isPlaying() const = 0;

    // Progress (thread-safe, polled from any thread)
    virtual double currentTime() const = 0;
    virtual void setDuration(double d) = 0;

    // Window resize notification (thread-safe, from main thread)
    virtual void resize(int /*width*/, int /*height*/) {}
};
