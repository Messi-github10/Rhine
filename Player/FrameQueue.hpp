#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <semaphore>
#include <vector>

#include <moodycamel/blockingconcurrentqueue.h>

extern "C" {
#include <libavutil/frame.h>
}

// Shared FFmpeg frame types
struct AVFrameDeleter {
    void operator()(AVFrame *f) const { av_frame_free(&f); }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t maxSize = 30)
        : m_freeSlots(static_cast<std::ptrdiff_t>(maxSize)) {}

    void push(T item) {
        // Wait for a free slot (bounded backpressure)
        while (!m_shutdown.load(std::memory_order_acquire)) {
            if (m_freeSlots.try_acquire_for(std::chrono::milliseconds(10))) {
                if (m_shutdown.load(std::memory_order_acquire)) {
                    m_freeSlots.release();
                    return;
                }
                m_queue.enqueue(std::move(item));
                return;
            }
        }
    }

    std::optional<T> pop() {
        T item;
        while (!m_shutdown.load(std::memory_order_acquire)) {
            if (m_queue.wait_dequeue_timed(item, std::chrono::milliseconds(10))) {
                m_freeSlots.release();
                return item;
            }
        }
        // Shutdown: drain remaining items before returning nullopt
        if (m_queue.try_dequeue(item)) {
            m_freeSlots.release();
            return item;
        }
        return std::nullopt;
    }

    void clear() {
        T item;
        while (m_queue.try_dequeue(item)) {
            m_freeSlots.release();
        }
    }

    void shutdown() {
        m_shutdown.store(true, std::memory_order_release);
    }

private:
    moodycamel::BlockingConcurrentQueue<T> m_queue;
    std::counting_semaphore<> m_freeSlots;
    std::atomic<bool> m_shutdown = false;
};

// Plain RGB frame — no Qt/FFmpeg dependency, used to bridge renderer → UI
struct VideoFrame {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    int stride = 0;
};