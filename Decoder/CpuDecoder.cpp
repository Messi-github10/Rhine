#include "CpuDecoder.hpp"
#include "Demuxer.hpp"

#include <spdlog/spdlog.h>
#include <cmath>

CpuDecoder::CpuDecoder(const DecoderConfig &config)
    : m_config(config)
    , m_queue(config.frameQueue)
{
}

CpuDecoder::~CpuDecoder() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
}

bool CpuDecoder::open() {
    // --- Format layer: open via Demuxer ---
    DemuxerConfig demuxCfg;
    demuxCfg.filePath = m_config.filePath;

    m_demuxer = std::make_unique<Demuxer>(demuxCfg);
    if (!m_demuxer->open()) return false;

    int vidx = m_demuxer->bestVideoStream();
    AVCodecParameters *codecpar = m_demuxer->codecParams(vidx);
    AVStream *stream = m_demuxer->stream(vidx);

    // --- Codec layer: open software decoder ---
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        spdlog::error("[CpuDecoder] Codec not found");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        spdlog::error("[CpuDecoder] Failed to alloc codec context");
        return false;
    }

    if (avcodec_parameters_to_context(m_codecCtx, codecpar) < 0) {
        spdlog::error("[CpuDecoder] Failed to copy codec params");
        return false;
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        spdlog::error("[CpuDecoder] Failed to open codec");
        return false;
    }

    // --- Metadata ---
    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;
    m_pixelFormat = m_codecCtx->pix_fmt;
    m_timeBase = stream->time_base;
    m_frameRate = av_guess_frame_rate(m_demuxer->formatContext(), stream, nullptr);
    m_duration = m_demuxer->duration();

    spdlog::info("[CpuDecoder] Opened: {}x{}, fps={}/{}, time_base={}/{}, duration={:.1f}s",
                 m_width, m_height,
                 m_frameRate.num, m_frameRate.den,
                 m_timeBase.num, m_timeBase.den,
                 m_duration);
    return true;
}

void CpuDecoder::seek(double targetSec, std::function<void()> onComplete) {
    m_seekTargetSec = targetSec;
    m_onSeekComplete = std::move(onComplete);
    m_seekRequested = true;
}

void CpuDecoder::doSeek() {
    int vidx = m_demuxer->bestVideoStream();
    AVStream *stream = m_demuxer->stream(vidx);

    int64_t seekTarget = av_rescale_q(
        static_cast<int64_t>(m_seekTargetSec * AV_TIME_BASE),
        AV_TIME_BASE_Q,
        stream->time_base);

    if (seekTarget < 0) seekTarget = 0;
    if (stream->duration > 0 && seekTarget > stream->duration)
        seekTarget = stream->duration;

    if (!m_demuxer->seek(m_seekTargetSec, vidx)) {
        spdlog::error("[CpuDecoder] Seek failed");
    }

    avcodec_flush_buffers(m_codecCtx);
    m_queue->clear();
    m_seekToPts = seekTarget;
    m_seekRequested = false;

    if (m_onSeekComplete) {
        m_onSeekComplete();
        m_onSeekComplete = nullptr;
    }
}

void CpuDecoder::run() {
    AVPacket *pkt = av_packet_alloc();
    AVFrame *decFrame = av_frame_alloc();

    int vidx = m_demuxer->bestVideoStream();

    while (!m_stop) {
        if (m_seekRequested.load()) {
            doSeek();
        }

        int ret = m_demuxer->readPacket(pkt);
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
            if (ret == AVERROR_EXIT) break;
            break;
        }

        if (pkt->stream_index == vidx) {
            avcodec_send_packet(m_codecCtx, pkt);
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
        }
        av_packet_unref(pkt);
    }

    m_queue->shutdown();

    av_frame_free(&decFrame);
    av_packet_free(&pkt);
}

void CpuDecoder::stop() {
    m_stop = true;
    if (m_demuxer) m_demuxer->interrupt();
}
