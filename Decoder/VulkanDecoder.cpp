#include "VulkanDecoder.hpp"
#include "Demuxer.hpp"
#include "VulkanContext.hpp"

#include <libplacebo/vulkan.h>

#include <spdlog/spdlog.h>
#include <cmath>

namespace {

// Bridge FFmpeg queue lock callbacks to libplacebo's pl_vulkan lock/unlock.
// FFmpeg calls these when submitting work to shared VkQueues.
void ffmpegLockQueue(struct AVHWDeviceContext *ctx, uint32_t qf, uint32_t qidx) {
    (void)ctx;
    auto vk = VulkanContext::instance().plVulkan();
    if (vk && vk->lock_queue) {
        vk->lock_queue(vk, qf, qidx);
    }
}

void ffmpegUnlockQueue(struct AVHWDeviceContext *ctx, uint32_t qf, uint32_t qidx) {
    (void)ctx;
    auto vk = VulkanContext::instance().plVulkan();
    if (vk && vk->unlock_queue) {
        vk->unlock_queue(vk, qf, qidx);
    }
}

} // anonymous namespace

VulkanDecoder::VulkanDecoder(const DecoderConfig &config)
    : m_config(config)
    , m_queue(config.frameQueue)
{
}

VulkanDecoder::~VulkanDecoder() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_hwDeviceRef) {
        av_buffer_unref(&m_hwDeviceRef);
    }
}

bool VulkanDecoder::open() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

    // --- 1. Format layer: open via Demuxer ---
    DemuxerConfig demuxCfg;
    demuxCfg.filePath = m_config.filePath;

    m_demuxer = std::make_unique<Demuxer>(demuxCfg);
    if (!m_demuxer->open()) return false;

    int vidx = m_demuxer->bestVideoStream();
    AVCodecParameters *codecpar = m_demuxer->codecParams(vidx);
    AVStream *stream = m_demuxer->stream(vidx);

    // --- 2. Find decoder ---
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        spdlog::error("[VulkanDecoder] Codec not found");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        spdlog::error("[VulkanDecoder] Failed to alloc codec context");
        return false;
    }

    if (avcodec_parameters_to_context(m_codecCtx, codecpar) < 0) {
        spdlog::error("[VulkanDecoder] Failed to copy codec params");
        return false;
    }

    // --- 3. Create shared Vulkan hwdevice context ---
    // Uses VulkanContext's existing instance/device so decoded frames
    // are directly accessible by libplacebo's pl_map_avframe (zero-copy).
    m_hwDeviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!m_hwDeviceRef) {
        spdlog::error("[VulkanDecoder] Failed to allocate Vulkan hwdevice context");
        return false;
    }

    auto &vkCtx = VulkanContext::instance();
    auto *hwCtx = reinterpret_cast<AVHWDeviceContext *>(m_hwDeviceRef->data);
    auto *vkDevCtx = reinterpret_cast<AVVulkanDeviceContext *>(hwCtx->hwctx);

    // Core Vulkan handles (shared with libplacebo)
    vkDevCtx->inst          = vkCtx.vkInstance();
    vkDevCtx->phys_dev      = vkCtx.physDevice();
    vkDevCtx->act_dev       = vkCtx.device();
    vkDevCtx->get_proc_addr = vkGetInstanceProcAddr;

    // Device features (copy only the top-level struct, drop pNext chain
    // owned by libplacebo — FFmpeg must not traverse foreign allocations)
    const auto *feats = vkCtx.features();
    if (feats) {
        vkDevCtx->device_features = *feats;
        vkDevCtx->device_features.pNext = nullptr;
    }

    // Device extensions (must match what libplacebo enabled)
    int extCount = 0;
    const char * const *exts = vkCtx.extensions(extCount);
    vkDevCtx->enabled_dev_extensions    = exts;
    vkDevCtx->nb_enabled_dev_extensions = extCount;

    // Queue family assignments (use same graphics queue family as libplacebo)
    uint32_t gfxQF = vkCtx.graphicsQueueFamily();
    vkDevCtx->queue_family_index      = static_cast<int>(gfxQF);
    vkDevCtx->nb_graphics_queues      = 1;
    vkDevCtx->queue_family_tx_index   = static_cast<int>(gfxQF);
    vkDevCtx->nb_tx_queues            = 1;
    vkDevCtx->queue_family_comp_index = static_cast<int>(gfxQF);
    vkDevCtx->nb_comp_queues          = 1;

    // New-style queue family list
    vkDevCtx->nb_qf = 1;
    vkDevCtx->qf[0].idx        = static_cast<int>(gfxQF);
    vkDevCtx->qf[0].num        = 1;
    vkDevCtx->qf[0].flags      = VK_QUEUE_GRAPHICS_BIT;
    vkDevCtx->qf[0].video_caps =
        static_cast<VkVideoCodecOperationFlagBitsKHR>(0);

    // Thread safety: wire libplacebo queue locks to FFmpeg
    vkDevCtx->lock_queue   = ffmpegLockQueue;
    vkDevCtx->unlock_queue = ffmpegUnlockQueue;

    if (av_hwdevice_ctx_init(m_hwDeviceRef) < 0) {
        spdlog::error("[VulkanDecoder] Failed to init Vulkan hwdevice context");
        av_buffer_unref(&m_hwDeviceRef);
        m_hwDeviceRef = nullptr;
        return false;
    }

    // --- 4. Attach hwdevice to codec context (must be BEFORE avcodec_open2) ---
    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceRef);
    if (!m_codecCtx->hw_device_ctx) {
        spdlog::error("[VulkanDecoder] Failed to ref hwdevice context");
        av_buffer_unref(&m_hwDeviceRef);
        m_hwDeviceRef = nullptr;
        return false;
    }

    // --- 5. Open codec (FFmpeg internally selects Vulkan hwaccel) ---
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        spdlog::error("[VulkanDecoder] Failed to open codec with Vulkan hwaccel");
        return false;
    }

    // --- 6. Metadata ---
    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;
    m_pixelFormat = m_codecCtx->pix_fmt;
    m_timeBase = stream->time_base;
    m_frameRate = av_guess_frame_rate(m_demuxer->formatContext(), stream, nullptr);
    m_duration = m_demuxer->duration();

    const char *hwName = "none";
    if (m_codecCtx->hw_device_ctx) {
        auto *activeDev = reinterpret_cast<AVHWDeviceContext *>(
            m_codecCtx->hw_device_ctx->data);
        hwName = av_hwdevice_get_type_name(activeDev->type);
    }

    spdlog::info("[VulkanDecoder] Opened: {}x{}, pix_fmt={}, hwaccel={}, "
                 "fps={}/{}, duration={:.1f}s",
                 m_width, m_height,
                 av_get_pix_fmt_name(m_pixelFormat),
                 hwName,
                 m_frameRate.num, m_frameRate.den,
                 m_duration);
#pragma GCC diagnostic pop
    return true;
}

void VulkanDecoder::seek(double targetSec, std::function<void()> onComplete) {
    m_seekTargetSec = targetSec;
    m_onSeekComplete = std::move(onComplete);
    m_seekRequested = true;
}

void VulkanDecoder::doSeek() {
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
        spdlog::error("[VulkanDecoder] Seek failed");
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

void VulkanDecoder::run() {
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

void VulkanDecoder::stop() {
    m_stop = true;
    if (m_demuxer) m_demuxer->interrupt();
}
