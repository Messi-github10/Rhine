#include "Demuxer.hpp"

#include <spdlog/spdlog.h>
#include <cmath>

int Demuxer::interruptCallback(void *opaque) {
    auto *self = static_cast<Demuxer *>(opaque);
    return self->m_interrupted.load(std::memory_order_acquire) ? 1 : 0;
}

Demuxer::Demuxer(const DemuxerConfig &config)
    : m_config(config)
{
}

Demuxer::~Demuxer() {
    close();
}

bool Demuxer::open() {
    m_interrupted.store(false, std::memory_order_release);

    if (avformat_open_input(&m_fmtCtx, m_config.filePath.c_str(), nullptr, nullptr) < 0) {
        spdlog::error("[Demuxer] Failed to open: {}", m_config.filePath);
        return false;
    }

    // Register interrupt callback so blocking I/O can be aborted on shutdown
    m_fmtCtx->interrupt_callback = {interruptCallback, this};

    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        spdlog::error("[Demuxer] Failed to find stream info");
        close();
        return false;
    }

    m_videoStreamIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStreamIdx < 0) {
        spdlog::error("[Demuxer] No video stream found");
        close();
        return false;
    }

    // Duration in seconds
    AVStream *vst = m_fmtCtx->streams[m_videoStreamIdx];
    if (vst->duration > 0)
        m_duration = vst->duration * av_q2d(vst->time_base);
    else if (m_fmtCtx->duration > 0)
        m_duration = static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE;

    spdlog::info("[Demuxer] Opened: {}x{}, time_base={}/{}, duration={:.1f}s",
                 codecParams(m_videoStreamIdx)->width,
                 codecParams(m_videoStreamIdx)->height,
                 vst->time_base.num, vst->time_base.den,
                 m_duration);
    return true;
}

void Demuxer::close() {
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_videoStreamIdx = -1;
    m_duration = 0.0;
}

int Demuxer::readPacket(AVPacket *pkt) {
    if (!m_fmtCtx) return AVERROR(EINVAL);
    return av_read_frame(m_fmtCtx, pkt);
}

bool Demuxer::seek(double targetSec, int streamIdx) {
    if (!m_fmtCtx) return false;

    AVStream *vst = m_fmtCtx->streams[streamIdx];

    // Convert target seconds → stream time_base PTS
    int64_t seekTarget = av_rescale_q(
        static_cast<int64_t>(targetSec * AV_TIME_BASE),
        AV_TIME_BASE_Q,
        vst->time_base);

    // Clamp to valid range
    if (seekTarget < 0) seekTarget = 0;
    if (vst->duration > 0 && seekTarget > vst->duration)
        seekTarget = vst->duration;

    // Seek to nearest keyframe BEFORE target
    int ret = av_seek_frame(m_fmtCtx, streamIdx, seekTarget, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        spdlog::error("[Demuxer] Seek failed: {}", ret);
        return false;
    }

    return true;
}
