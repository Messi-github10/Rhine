#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

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
    explicit BoundedQueue(size_t maxSize = 30) : m_maxSize(maxSize) {}

    void push(T item) {
        std::unique_lock lock(m_mutex);
        m_notFull.wait(lock, [this] {
            return m_queue.size() < m_maxSize || m_shutdown;
        });
        if (m_shutdown) return;
        m_queue.push_back(std::move(item));
        m_notEmpty.notify_one();
    }

    std::optional<T> pop() {
        std::unique_lock lock(m_mutex);
        m_notEmpty.wait(lock, [this] {
            return !m_queue.empty() || m_shutdown;
        });
        if (m_shutdown && m_queue.empty()) return std::nullopt;
        T item = std::move(m_queue.front());
        m_queue.pop_front();
        m_notFull.notify_one();
        return item;
    }

    void clear() {
        {
            std::lock_guard lock(m_mutex);
            m_queue.clear();
        }
        m_notFull.notify_all();
    }

    void shutdown() {
        {
            std::lock_guard lock(m_mutex);
            m_shutdown = true;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    size_t size() const {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }

private:
    std::deque<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    size_t m_maxSize;
    bool m_shutdown = false;
};

// Plain RGB frame — no Qt/FFmpeg dependency, used to bridge renderer → UI
struct VideoFrame {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    int stride = 0;
};
