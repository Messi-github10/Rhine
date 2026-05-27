#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

#include <atomic>
#include <string>

struct DemuxerConfig {
    std::string filePath;
};

/// Demuxer wraps FFmpeg format-layer operations:
///   - avformat_open_input / avformat_find_stream_info / av_find_best_stream
///   - av_read_frame
///   - av_seek_frame
///
/// Uses AVIOInterruptCallback so that blocking I/O (read, seek) can be
/// aborted immediately via interrupt(), avoiding slow shutdown.
class Demuxer {
public:
    explicit Demuxer(const DemuxerConfig &config);
    ~Demuxer();

    Demuxer(const Demuxer &) = delete;
    Demuxer &operator=(const Demuxer &) = delete;

    /// Open the file, probe stream info, locate the best video stream.
    /// Returns true on success.
    bool open();

    /// Close and release all resources.
    void close();

    /// Signal blocking FFmpeg calls (read, seek) to abort immediately.
    /// Thread-safe — call from any thread.
    void interrupt() { m_interrupted.store(true, std::memory_order_release); }

    /// Read the next compressed packet from the container.
    /// Returns 0 on success, AVERROR_EXIT on interrupt,
    /// AVERROR_EOF on end, or another negative error code.
    int readPacket(AVPacket *pkt);

    /// Seek to targetSec (seconds) on the given stream.
    /// Uses AVSEEK_FLAG_BACKWARD — lands on the nearest keyframe before target.
    bool seek(double targetSec, int streamIdx);

    // --- Stream info accessors (used by Decoder for codec init) ---

    int bestVideoStream() const { return m_videoStreamIdx; }

    AVStream *stream(int idx) const {
        return m_fmtCtx ? m_fmtCtx->streams[idx] : nullptr;
    }

    AVCodecParameters *codecParams(int idx) const {
        auto *st = stream(idx);
        return st ? st->codecpar : nullptr;
    }

    AVRational timeBase(int idx) const {
        auto *st = stream(idx);
        return st ? st->time_base : AVRational{0, 0};
    }

    AVFormatContext *formatContext() const { return m_fmtCtx; }

    double duration() const { return m_duration; }
    bool isOpen() const { return m_fmtCtx != nullptr; }

private:
    static int interruptCallback(void *opaque);

    DemuxerConfig m_config;
    AVFormatContext *m_fmtCtx = nullptr;
    int m_videoStreamIdx = -1;
    double m_duration = 0.0;
    std::atomic<bool> m_interrupted{false};
};
