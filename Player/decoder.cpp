#include "decoder.h"

#include <cstdio>
#include <cmath>

Decoder::Decoder(const DecoderConfig &config)
    : m_config(config)
    , m_queue(config.frameQueue)
{
}

Decoder::~Decoder() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
    }
}

bool Decoder::open() {
    if (avformat_open_input(&m_fmtCtx, m_config.filePath.c_str(), nullptr, nullptr) < 0) {
        std::fprintf(stderr, "[Decoder] Failed to open: %s\n", m_config.filePath.c_str());
        return false;
    }

    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        std::fprintf(stderr, "[Decoder] Failed to find stream info\n");
        return false;
    }

    m_videoStreamIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStreamIdx < 0) {
        std::fprintf(stderr, "[Decoder] No video stream found\n");
        return false;
    }

    AVStream *stream = m_fmtCtx->streams[m_videoStreamIdx];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        std::fprintf(stderr, "[Decoder] Codec not found\n");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        std::fprintf(stderr, "[Decoder] Failed to alloc codec context\n");
        return false;
    }

    if (avcodec_parameters_to_context(m_codecCtx, stream->codecpar) < 0) {
        std::fprintf(stderr, "[Decoder] Failed to copy codec params\n");
        return false;
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::fprintf(stderr, "[Decoder] Failed to open codec\n");
        return false;
    }

    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;
    m_pixelFormat = m_codecCtx->pix_fmt;
    m_timeBase = stream->time_base;
    m_frameRate = av_guess_frame_rate(m_fmtCtx, stream, nullptr);

    // Duration in seconds
    if (stream->duration > 0)
        m_duration = stream->duration * av_q2d(stream->time_base);
    else if (m_fmtCtx->duration > 0)
        m_duration = (double)m_fmtCtx->duration / AV_TIME_BASE;

    std::fprintf(stderr, "[Decoder] Opened: %dx%d, fps=%d/%d, time_base=%d/%d, duration=%.1fs\n",
                 m_width, m_height,
                 m_frameRate.num, m_frameRate.den,
                 m_timeBase.num, m_timeBase.den,
                 m_duration);
    return true;
}

void Decoder::seek(double targetSec, std::function<void()> onComplete) {
    m_seekTargetSec = targetSec;
    m_onSeekComplete = std::move(onComplete);
    m_seekRequested = true;
}

void Decoder::doSeek() {
    AVStream *stream = m_fmtCtx->streams[m_videoStreamIdx];

    // Convert target seconds → stream time_base PTS
    int64_t seekTarget = av_rescale_q(
        (int64_t)(m_seekTargetSec * AV_TIME_BASE),
        AV_TIME_BASE_Q,
        stream->time_base);

    // Clamp to valid range
    if (seekTarget < 0) seekTarget = 0;
    if (stream->duration > 0 && seekTarget > stream->duration)
        seekTarget = stream->duration;

    // Seek to nearest keyframe BEFORE target
    int ret = av_seek_frame(m_fmtCtx, m_videoStreamIdx,
                            seekTarget, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        std::fprintf(stderr, "[Decoder] Seek failed: %d\n", ret);
    }

    // Flush codec internal buffers
    avcodec_flush_buffers(m_codecCtx);

    // Discard all queued frames from old position
    m_queue->clear();

    // Mark PTS to skip to (forward-decode approach)
    m_seekToPts = seekTarget;

    m_seekRequested = false;

    // Notify that seek is complete (unpause renderer)
    if (m_onSeekComplete) {
        m_onSeekComplete();
        m_onSeekComplete = nullptr;
    }
}

void Decoder::run() {
    AVPacket *pkt = av_packet_alloc();
    AVFrame *decFrame = av_frame_alloc();

    while (!m_stop) {
        // --- Check for seek request ---
        if (m_seekRequested.load()) {
            doSeek();
        }

        int ret = av_read_frame(m_fmtCtx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // flush decoder
                avcodec_send_packet(m_codecCtx, nullptr);
                while (avcodec_receive_frame(m_codecCtx, decFrame) == 0) {
                    if (m_seekToPts != AV_NOPTS_VALUE && decFrame->pts < m_seekToPts) {
                        av_frame_unref(decFrame);
                        continue;
                    }
                    m_seekToPts = AV_NOPTS_VALUE;
                    AVFrame *clone = av_frame_clone(decFrame);
                    if (clone) {
                        m_queue->push(AVFramePtr(clone));
                    }
                }
                break;
            }
            if (ret == AVERROR(EAGAIN)) continue;
            break;
        }

        if (pkt->stream_index == m_videoStreamIdx) {
            avcodec_send_packet(m_codecCtx, pkt);
            while (avcodec_receive_frame(m_codecCtx, decFrame) == 0) {
                // Forward-decode: skip frames before seek target PTS
                if (m_seekToPts != AV_NOPTS_VALUE && decFrame->pts < m_seekToPts) {
                    av_frame_unref(decFrame);
                    continue;
                }
                m_seekToPts = AV_NOPTS_VALUE;  // reached target

                AVFrame *clone = av_frame_clone(decFrame);
                if (clone) {
                    m_queue->push(AVFramePtr(clone));
                }
            }
        }
        av_packet_unref(pkt);
    }

    m_queue->shutdown();

    av_frame_free(&decFrame);
    av_packet_free(&pkt);
}

void Decoder::stop() {
    m_stop = true;
}
