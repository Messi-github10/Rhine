# mpv-decoder.md
## 提供MPV与FFmpeg之间的通用转换函数（av_common）

> mp_set_avctx_codec_headers

将 mpv 的编码参数（extradata、时间基等）填入AVCodecContext，供解码器使用

```C
int mp_set_avctx_codec_headers(AVCodecContext *avctx, const struct mp_codec_params *c)
{
    enum AVMediaType codec_type = avctx->codec_type;
    enum AVCodecID codec_id = avctx->codec_id;
    
    // 将mpv的编解码参数结构转换为FFmpeg的AVCodecParameters格式
    AVCodecParameters *avp = mp_codec_params_to_av(c);
    if (!avp)
        return -1;

    // 使用FFmpeg API将参数应用到解码器上下文
    int r = avcodec_parameters_to_context(avctx, avp) < 0 ? -1 : 0;
    
    avcodec_parameters_free(&avp);

    if (avctx->codec_type != AVMEDIA_TYPE_UNKNOWN)
        avctx->codec_type = codec_type;
    if (avctx->codec_id != AV_CODEC_ID_NONE)
        avctx->codec_id = codec_id;
    
    return r;  // 返回操作结果：0表示成功，-1表示失败
}
```

---

<br/>

> mp_set_av_packet

将 mpv 内部的 demux_packet 转换为 FFmpeg 的 AVPacket，包括时间戳、标志等

```C
void mp_set_av_packet(AVPacket *dst, struct demux_packet *mpkt, AVRational *tb)
{
    dst->side_data = NULL;
    dst->side_data_elems = 0;
    dst->buf = NULL;
    av_packet_unref(dst);

    // 设置packet的数据指针和大小
    dst->data = mpkt ? mpkt->buffer : NULL;
    dst->size = mpkt ? mpkt->len : 0;

    // 如果是关键帧，设置AV_PKT_FLAG_KEY标志
    if (mpkt && mpkt->keyframe)
        dst->flags |= AV_PKT_FLAG_KEY;

    // 如果mpkt包含原始的FFmpeg AVPacket，则复制其侧数据和标志
    if (mpkt && mpkt->avpacket) {
        dst->side_data = mpkt->avpacket->side_data;
        dst->side_data_elems = mpkt->avpacket->side_data_elems;
        if (dst->data == mpkt->avpacket->data)
            dst->buf = mpkt->avpacket->buf;
        dst->flags |= mpkt->avpacket->flags;
    }


    if (mpkt && tb && tb->num > 0 && tb->den > 0)
        dst->duration = mpkt->duration / av_q2d(*tb);

    dst->pts = mp_pts_to_av(mpkt ? mpkt->pts : MP_NOPTS_VALUE, tb);  // 转换显示时间戳
    dst->dts = mp_pts_to_av(mpkt ? mpkt->dts : MP_NOPTS_VALUE, tb);  // 转换解码时间戳
}
```

> mp_pts_to_av 和 mp_pts_from_av

* AVRational *tb是一个时间基，例如时间基 {1, 1000} 表示每个时间单位代表 0.001 秒（1毫秒）
* mp_pts_to_av中除法运算的意义：mp_pts / av_q2d(b) = 秒数 ÷ (每个时间单位的秒数) = 时间单位数量
* mp_pts_from_av中乘法运算的意义：av_pts * av_q2d(b) = 时间单位数量 乘以 每个时间单位的秒数 = 秒数

```C
int64_t mp_pts_to_av(double mp_pts, AVRational *tb)
{
    AVRational b = get_def_tb(tb);
    return mp_pts == MP_NOPTS_VALUE ? AV_NOPTS_VALUE : llrint(mp_pts / av_q2d(b));
}

double mp_pts_from_av(int64_t av_pts, AVRational *tb)
{
    AVRational b = get_def_tb(tb);
    return av_pts == AV_NOPTS_VALUE ? MP_NOPTS_VALUE : av_pts * av_q2d(b);
}
```

---

<br/>

## 视频解码器核心逻辑（vd_lavc）
### 初始化解码器
> create

创建一个视频解码器实例

```C
static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct mp_filter *vd = mp_filter_create(parent, &vd_lavc_filter);
    if (!vd)
        return NULL;

    mp_filter_add_pin(vd, MP_PIN_IN, "in");
    mp_filter_add_pin(vd, MP_PIN_OUT, "out");

    vd->log = mp_log_new(vd, parent->log, NULL);

    vd_ffmpeg_ctx *ctx = vd->priv;
    ctx->log = vd->log;
    ctx->opts_cache = m_config_cache_alloc(ctx, vd->global, &vd_lavc_conf);
    ctx->opts = ctx->opts_cache->opts;
    ctx->hwdec_opts_cache = m_config_cache_alloc(ctx, vd->global, &hwdec_conf);
    ctx->hwdec_opts = ctx->hwdec_opts_cache->opts;

    // 保存解码参数
    ctx->codec = codec;
    ctx->decoder = talloc_strdup(ctx, decoder);
    
    // 创建硬件解码软件池和创建直接渲染池
    ctx->hwdec_swpool = mp_image_pool_new(ctx);
    ctx->dr_pool = mp_image_pool_new(ctx);

    ctx->public.f = vd;
    ctx->public.control = control;

    mp_mutex_init(&ctx->dr_lock);

    // hwdec/DR
    struct mp_stream_info *info = mp_filter_find_stream_info(vd);
    if (info) {
        ctx->hwdec_devs = info->hwdec_devs;     // 硬件解码设备
        ctx->vo = info->dr_vo;
    }

    reinit(vd);     // 初始化解码器

    if (!ctx->avctx) {
        talloc_free(vd);
        return NULL;
    }

    codec->codec_desc = ctx->avctx->codec_descriptor->long_name;

    return &ctx->public;
}
```

---

<br/>

> reinit

初始化解码器

```C
static void reinit(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    uninit_avctx(vd);

    TA_FREEP(&ctx->attempted_hwdecs);
    ctx->num_attempted_hwdecs = 0;
    ctx->hwdec_notified = false;

    // 选择并设置要使用的硬件解码器
    select_and_set_hwdec(vd);

    bool use_hwdec = ctx->use_hwdec;

    // 初始化 AVCodec 上下文
    init_avctx(vd);

    // 如果使用硬件解码但初始化失败，尝试回退到其他解码器
    if (!ctx->avctx && use_hwdec) {
        do {
            force_fallback(vd);
        } while (!ctx->avctx);
    }

    ctx->wait_for_keyframe = ctx->use_hwdec ? HWDEC_WAIT_KEYFRAME_COUNT : 0;
}
```

---

<br/>

> select_and_set_hwdec

选择并设置要使用的硬件解码器

```C
static void select_and_set_hwdec(struct mp_filter *vd)
{
    // 获取解码器上下文
    vd_ffmpeg_ctx *ctx = vd->priv;

    // 获取当前视频流的编解码器名称
    const char *codec = ctx->codec->codec;

    m_config_cache_update(ctx->hwdec_opts_cache);

    // 获取所有可用的编解码方法
    struct hwdec_info *hwdecs = NULL;
    int num_hwdecs = 0;
    add_all_hwdec_methods(&hwdecs, &num_hwdecs);    // 枚举所有支持的硬件解码方案

    char **hwdec_api = ctx->hwdec_opts->hwdec_api;
    for (int i = 0; hwdec_api && hwdec_api[i]; i++) {
        bstr opt = bstr0(hwdec_api[i]);

        bool hwdec_requested = !bstr_equals0(opt, "no");
        bool hwdec_auto_safe = bstr_equals0(opt, "auto") ||
                            bstr_equals0(opt, "auto-safe") ||
                            bstr_equals0(opt, "auto-copy") ||
                            bstr_equals0(opt, "auto-copy-safe") ||
                            bstr_equals0(opt, "yes") ||
                            bstr_equals0(opt, "");
        bool hwdec_auto_unsafe = bstr_equals0(opt, "auto-unsafe");
        bool hwdec_auto_copy = bstr_equals0(opt, "auto-copy") ||
                            bstr_equals0(opt, "auto-copy-safe") ||
                            bstr_equals0(opt, "auto-copy-unsafe");
        bool hwdec_auto = hwdec_auto_unsafe || hwdec_auto_copy || hwdec_auto_safe;

        if (!hwdec_requested) {
            MP_VERBOSE(vd, "No hardware decoding requested.\n");
            break;
        } else if (!hwdec_codec_allowed(vd, codec)) {
            MP_VERBOSE(vd, "Not trying to use hardware decoding: codec %s is not "
                    "on whitelist.\n", codec);
            break;
        } else {
            bool hwdec_name_supported = false;

            // 遍历所有可用的硬件解码方法
            for (int n = 0; n < num_hwdecs; n++) {
                struct hwdec_info *hwdec = &hwdecs[n];  // 这里的hwdec指向之前保存的数组元素

                if (!hwdec_auto && !(bstr_equals0(opt, hwdec->method_name) ||
                                    bstr_equals0(opt, hwdec->name)))
                    continue;
                hwdec_name_supported = true;

                bool already_attempted = false;
                for (int j = 0; j < ctx->num_attempted_hwdecs; j++) {
                    if (bstr_equals0(ctx->attempted_hwdecs[j], hwdec->name)) {
                        MP_DBG(vd, "Skipping previously attempted hwdec: %s\n",
                               hwdec->name);
                        already_attempted = true;
                        break;
                    }
                }
                if (already_attempted)
                    continue;

                // 检查硬件解码器支持的编解码器是否与当前视频匹配
                // 将硬件解码器的AVCodecID转换为MPV的编解码器名称
                const char *hw_codec = mp_codec_from_av_codec_id(hwdec->codec->id);

                // 进行字符串比较匹配
                // 仅仅编解码器名称匹配是不够的（h264、h265、av1...）
                if (!hw_codec || strcmp(hw_codec, codec) != 0)
                    continue;

                if (hwdec_auto_safe && !(hwdec->flags & HWDEC_FLAG_WHITELIST))
                    continue;

                MP_VERBOSE(vd, "Looking at hwdec %s...\n", hwdec->name);

                MP_TARRAY_APPEND(ctx, ctx->attempted_hwdecs,
                                 ctx->num_attempted_hwdecs,
                                 bstrdup(ctx, bstr0(hwdec->name)));

                if (hwdec_auto_copy && !hwdec->copying) {
                    MP_VERBOSE(vd, "Not using this for auto-copy.\n");
                    continue;
                }

                if (hwdec->lavc_device) {
                    // 尝试从 VO 层获取已创建的设备
                    // 只有该函数返回设备，才说明真正创建解码器成功
                    ctx->hwdec_dev = hwdec_create_dev(vd, hwdec, hwdec_auto);
                    if (!ctx->hwdec_dev) {
                        MP_VERBOSE(vd, "Could not create device.\n");
                        continue;
                    }

                    const struct hwcontext_fns *fns =
                                hwdec_get_hwcontext_fns(hwdec->lavc_device);
                    if (fns && fns->is_emulated && fns->is_emulated(ctx->hwdec_dev)) {
                        if (hwdec_auto) {
                            MP_VERBOSE(vd, "Not using emulated API.\n");
                            av_buffer_unref(&ctx->hwdec_dev);
                            continue;
                        }
                        MP_WARN(vd, "Using emulated hardware decoding API.\n");
                    }
                } else if (!hwdec->copying) {
                    if (ctx->hwdec_devs) {
                        struct hwdec_imgfmt_request params = {
                            .imgfmt = pixfmt2imgfmt(hwdec->pix_fmt),
                            .probing = hwdec_auto,
                        };
                        hwdec_devices_request_for_img_fmt(
                            ctx->hwdec_devs, &params);
                    }
                }

                ctx->use_hwdec = true;
                ctx->hwdec = *hwdec;    // 整个结构体赋值，包括.codec字段
                break;
            }
            if (ctx->use_hwdec)
                break;
            else if (!hwdec_auto && !hwdec_name_supported)
                MP_WARN(vd, "Unsupported hwdec: %.*s\n", BSTR_P(opt));
        }
    }
    talloc_free(hwdecs);


    if (ctx->use_hwdec) {
        MP_VERBOSE(vd, "Trying hardware decoding via %s.\n", ctx->hwdec.name);
        if (strcmp(ctx->decoder, ctx->hwdec.codec->name) != 0)
            MP_VERBOSE(vd, "Using underlying hw-decoder '%s'\n",
                       ctx->hwdec.codec->name);
    } else {
        if (ctx->hwdec_opts->software_fallback == INT_MAX) {
            MP_WARN(ctx, "Software decoding fallback is disabled.\n");
            ctx->force_eof = true;
        } else {    // 回退使用软件解码
            MP_VERBOSE(vd, "Using software decoding.\n");
        }
    }
}
```

---

<br/>

> add_all_hwdec_methods

```C
static void add_all_hwdec_methods(struct hwdec_info **infos, int *num_infos)
{
    const AVCodec *codec = NULL;
    void *iter = NULL;
    while (1) {
        // 获取下一个编解码器
        codec = av_codec_iterate(&iter);
        if (!codec)
            break;

        // av_codec_is_decoder用于检查给定的AVCodec是否代表一个解码器，而不是编码器
        // 只关心视频解码器
        if (codec->type != AVMEDIA_TYPE_VIDEO || !av_codec_is_decoder(codec))
            continue;

        struct hwdec_info info_template = {
            .pix_fmt = AV_PIX_FMT_NONE,
            .codec = codec,     // 这里设置了codec字段
        };

        const char *wrapper = NULL;
        if (codec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID))
            wrapper = codec->wrapper_name;

        bool found_any = false;

        // 遍历所有可用的硬件配置
        for (int n = 0; ; n++) {
            // avcodec_get_hw_config 的作用：查询一个指定的编解码器（Decoder/Encoder），看它支持哪些具体的硬件加速方式
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, n);
            if (!cfg)
                break;

            // 使用外部硬件方法还是内部硬件方法
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) ||
                (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            {
                struct hwdec_info info = info_template;
                info.lavc_device = cfg->device_type;
                info.pix_fmt = cfg->pix_fmt;

                const char *name = av_hwdevice_get_type_name(cfg->device_type);
                mp_assert(name);
                if (strcmp(name, "cuda") == 0 && !wrapper)
                    name = "nvdec";

                snprintf(info.method_name, sizeof(info.method_name), "%s", name);

                if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) {
                    info.use_hw_frames = true;  // 使用hw_frames_ctx方式
                } else {
                    info.use_hw_device = true;  // 使用hw_device_ctx方式
                }

                add_hwdec_item(infos, num_infos, info);

                info.copying = true;
                if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
                    info.use_hw_frames = false;
                    info.use_hw_device = true;
                }
                add_hwdec_item(infos, num_infos, info);

                found_any = true;
            } else if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL) {
                struct hwdec_info info = info_template;
                info.pix_fmt = cfg->pix_fmt;

                const char *name = wrapper;
                if (!name)
                    name = av_get_pix_fmt_name(info.pix_fmt);
                mp_assert(name);

                snprintf(info.method_name, sizeof(info.method_name), "%s", name);

                add_hwdec_item(infos, num_infos, info);

                info.copying = true;
                info.pix_fmt = AV_PIX_FMT_NONE;
                add_hwdec_item(infos, num_infos, info);

                found_any = true;
            }
        }

        if (!found_any && wrapper) {
            struct hwdec_info info = info_template;
            info.copying = true;

            snprintf(info.method_name, sizeof(info.method_name), "%s", wrapper);
            add_hwdec_item(infos, num_infos, info);
        }
    }

    qsort(*infos, *num_infos, sizeof(struct hwdec_info), hwdec_compare);
}
```

> 外部硬件方法与内部硬件方法

- **外部硬件方法**：解码器只负责解码，但需要**你（调用者）**来准备和管理硬件设备与内存。（绝对的主流和首选）
- **内部硬件方法**：解码器内部自己管理一切硬件资源，**你不需要也不应该插手**。（特定平台[嵌入式]的补充）

---

**外部硬件方法**

对应代码中检查的标志位：
`AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX` 或 `AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX`。

**通俗理解**：
解码器像一个**租客**，它只会使用硬件解码核心，但不会去创建硬件上下文或分配硬件内存。**房东**需要事先把硬件设备（`AVHWDeviceContext`）和硬件帧池（`AVHWFramesContext`）准备好，然后告诉解码器：“用这个硬件去解码，结果放到这块硬件内存里。”

**工作流程**：

1.  **你创建**硬件设备上下文（例如 CUDA, VAAPI, DXVA2）。
2.  **你创建**硬件帧上下文，并从中分配硬件帧缓冲区。
3.  **你把**这两个上下文“安装”到解码器（`AVCodecContext`）的 `hw_device_ctx` 或 `hw_frames_ctx` 字段中。
4.  **解码器**在解码时，直接使用你提供的硬件资源，并将解码后的数据 (`AVFrame`) 放入指定的硬件缓冲区。

**特点**：
-   **需要你主动管理**：代码更复杂，但控制力强。
-   **结果留在硬件内存**：`AVFrame` 里的 `data` 指针指向的是 GPU 内存地址。如果要拿到 CPU 里使用，必须手动调用 `av_hwframe_transfer_data()` 拷回来。
-   **典型代表**：CUDA/NVDEC, VAAPI, DXVA2, D3D11VA, Videotoolbox 等主流跨平台方案。

---

2.  内部硬件方法
对应代码中检查的标志位：
`AV_CODEC_HW_CONFIG_METHOD_INTERNAL`。

**通俗理解**：
解码器像一个**全包式管家**。它内部自己会去找硬件、初始化设备、分配内存。你**完全不需要**知道硬件细节，甚至不用关心是不是硬件加速了。解码器给你吐出的数据，默认就是 CPU 能直接读的普通内存。

**工作流程**：

1.  **你创建**解码器，像往常一样打开它。
2.  **解码器内部**默默检测硬件、初始化、进行硬件解码，并自动将结果从硬件内存拷贝到软件内存。
3.  **你接收**解码后的 `AVFrame`，直接就拿到了 CPU 里的普通像素数据（比如 YUV420P）。

**特点**：
-   **对你透明**：使用简单，和软解的代码几乎一样。
-   **结果在CPU内存**：解码器内部帮你做了内存拷贝，你用起来像软解一样方便。
-   **典型代表**：主要用于嵌入式或移动平台。
    -   **Android MediaCodec**：通过 FFmpeg 的 `mediacodec` wrapper 实现，内部管理 Java 层的硬件解码器。
    -   **某些专用芯片**：比如树莓派的 MMAL，或者海思、安霸等厂商的专有解码 API。

---

**核心区别对比表**

| 特性 | 外部硬件方法 | 内部硬件方法 |
| :--- | :--- | :--- |
| **资源管理 (Context)** | 由 **你（调用者）** 创建和管理 | 由 **解码器内部** 自己管理 |
| **数据输出位置** | **GPU/硬件** 显存中 | **CPU** 内存中（内部已拷贝） |
| **使用复杂度** | **高** (需要初始化多个上下文) | **低** (和软解几乎一样) |
| **控制灵活性** | **高** (可以控制硬件帧池、零拷贝等) | **低** (只能接受其内部行为) |
| **典型应用场景** | 桌面/服务器GPU (NVIDIA, Intel, AMD) | 移动平台 (Android MediaCodec) |
| **对应 FFmpeg 配置** | `hwupload`, `hwdownload` 滤镜 | 直接使用，无需额外配置 |

---

**总结**：这个区别定义了**由谁来做硬件管理的脏活累活**。外部方法交给你，调用复杂但灵活；内部方法自己全包，用起来简单但限制多。

---

<br/>

> 硬件加速的**三层蛋糕**模型

![alt text](deepseek_mermaid_20260424_92cba9.png)

---

<br/>

> hw_device_ctx方式和hw_frames_ctx方式

`hw_device_ctx` 和 `hw_frames_ctx` 的核心区别在于**控制权和自由度**。

-   **`hw_device_ctx`（硬件设备上下文）**：你告诉 FFmpeg **“用哪块显卡”**，至于怎么分配内存、用什么格式，全权交给 FFmpeg 内部自动处理。这种方式代码量最少，适合大多数场景。
-   **`hw_frames_ctx`（硬件帧上下文）**：你不仅告诉 FFmpeg 用哪块显卡，还亲自规定 **“内存池有多大、像素格式是什么”**。这种方式提供了更精细的控制权，比如实现零拷贝渲染或与外部图形引擎交互。

---

1. hw_device_ctx：全自动模式

这是 FFmpeg 官方推荐的“懒人模式”，也是你之前看到的代码中重点处理的类型。

当你在 `AVCodecContext` 中设置了 `hw_device_ctx` 后，FFmpeg 内部会在 `get_format` 回调中**自动创建**对应的 `hw_frames_ctx`。开发者甚至不需要手动调用 `av_hwframe_ctx_alloc` 和 `av_hwframe_ctx_init`。

*   **使用时机**：在调用 `avcodec_open2()` **之前**设置。
*   **特点**：使用简单，FFmpeg 全自动管理。但由于是内部自动生成的，外部无法获取这个帧池的指针，无法直接操作其中的帧。

2. hw_frames_ctx：手动挡模式

这是更“硬核”的控制模式。你需要手动创建、配置并初始化 `AVHWFramesContext`，然后在 `get_format` 回调中把它赋给 `AVCodecContext`。

*   **使用时机**：在 `get_format` 回调函数**内部**设置。
*   **特点**：虽然代码更复杂，但它赋予了你非常关键的能力：

    1.  **外部渲染交互**：你可以拿到帧池指针，直接把解码后的 GPU 内存交给渲染管线做纹理映射，实现零拷贝渲染。
    2.  **资源控制**：你可以决定硬件帧池的大小，或者要求输出特定的像素格式。

---


| 特性 | `hw_device_ctx` (自动挡) | `hw_frames_ctx` (手动挡) |
| :--- | :--- | :--- |
| **控制层级** | 设备层 (Device) | 帧内存池层 (Frame Pool) |
| **设置时机** | `avcodec_open2()` **之前** | `get_format` 回调 **内部** |
| **内存管理方** | **FFmpeg 内部**自动分配管理 | **用户代码**手动创建分配 |
| **代码复杂度** | **低** (省心) | **高** (强大) |
| **典型场景** | 常规转码、通用播放器 | **零拷贝渲染**、与图形引擎集成 |

---

<br/>

> hwdec_create_dev

该函数的作用: 根据一个硬件解码方法（hwdec）的记录，创建并返回一个 FFmpeg 的硬件设备上下文（AVBufferRef *，内部是 AVHWDeviceContext）。

```C
static AVBufferRef *hwdec_create_dev(struct mp_filter *vd,
                                     struct hwdec_info *hwdec,
                                     bool autoprobe)
{
    vd_ffmpeg_ctx *ctx = vd->priv;  // 在 create() 函数中已经被正确初始化，特别是 ctx->hwdec_devs 从 VO 层获取了硬件设备管理器
    mp_assert(hwdec->lavc_device);

    if (hwdec->copying) {   // 拷贝模式
        const struct hwcontext_fns *fns =
            hwdec_get_hwcontext_fns(hwdec->lavc_device);
        if (fns && fns->create_dev) {
            struct hwcontext_create_dev_params params = {
                .probing = autoprobe,
            };
            return fns->create_dev(vd->global, vd->log, &params);
        } else {
            AVBufferRef* ref = NULL;
            av_hwdevice_ctx_create(&ref, hwdec->lavc_device, NULL, NULL, 0);
            return ref;
        }
    } else if (ctx->hwdec_devs) {   // 零拷贝模式（共享模式）

        // 第一步：将 FFmpeg 的像素格式（AVPixelFormat）转换为 mpv 内部使用的格式 ID
        // 例如：AV_PIX_FMT_NV12 -> IMGFMT_NV12
        int imgfmt = pixfmt2imgfmt(hwdec->pix_fmt);
        
        // 准备一个请求参数结构体
        struct hwdec_imgfmt_request params = {
            .imgfmt = imgfmt,       // 我需要的像素格式
            .probing = autoprobe,   // 是否处于探测模式
        };

        // 第二步：向设备管理器"下订单"——确保有一个能输出这种格式的硬件设备可用
        // 这个函数内部逻辑：
        //   - 遍历现有设备，看有没有已经能输出 imgfmt 的设备
        //   - 如果有：什么也不做，直接返回
        //   - 如果没有：调用相应的创建函数，新建一个设备并加入到管理器中
        // 执行完这个函数后，可以确定：设备管理器里肯定有一个能干活儿的设备了

        // 找到设备后，ctx->hwdec_devs 本身没有变化，但设备池（hwdec_devs 管理的内部数组）可能会增加一个新创建出来的设备
        hwdec_devices_request_for_img_fmt(ctx->hwdec_devs, &params);

        // 第三步：从设备管理器中"精确领取"我们需要的那个设备
        // 为什么要精确？因为可能有多个设备都能处理同一种 imgfmt
        // 例如：NVIDIA 的 CUDA 和 Intel 的 VAAPI 都能输出 NV12 格式
        // 我们需要通过 lavc_device（硬件类型）来区分到底要哪个
        // 参数说明：
        //   - imgfmt：像素格式，用于粗筛
        //   - hwdec->lavc_device：硬件类型（如 AV_HWDEVICE_TYPE_CUDA），用于精确匹配

        // add_all_hwdec_methods 函数中: info.lavc_device = cfg->device_type;
        const struct mp_hwdec_ctx *hw_ctx =
            hwdec_devices_get_by_imgfmt_and_type(ctx->hwdec_devs, imgfmt,
                                                 hwdec->lavc_device);

        // 第四步：如果找到了，就返回这个设备引用的拷贝
        // av_buffer_ref() 会增加引用计数，调用者用完后需要 av_buffer_unref() 释放
        // 注意：这里不转移所有权，只是"借"用，设备管理器仍然保有原始引用
        if (hw_ctx && hw_ctx->av_device_ref)
            return av_buffer_ref(hw_ctx->av_device_ref);
    }

    return NULL;
}
```

![alt text](deepseek_mermaid_20260424_0f3dbe.png)

---

<br/>

> init_avctx

创建并配置好一个 FFmpeg 的 AVCodecContext

```C
static void init_avctx(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    struct vd_lavc_params *lavc_param = ctx->opts;
    struct mp_codec_params *c = ctx->codec;

    m_config_cache_update(ctx->opts_cache);

    mp_assert(!ctx->avctx);

    const AVCodec *lavc_codec = NULL;

    if (ctx->use_hwdec) {   // 硬解模式：使用 hwdec 中已经选好的解码器
        lavc_codec = ctx->hwdec.codec;  // 该值在select_and_set_hwdec中设置
    } else {     // 软解模式：通过名字查找解码器
        lavc_codec = avcodec_find_decoder_by_name(ctx->decoder);
    }
    if (!lavc_codec)
        return;

    const AVCodecDescriptor *desc = avcodec_descriptor_get(lavc_codec->id);
    ctx->intra_only = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);

    ctx->codec_timebase = mp_get_codec_timebase(ctx->codec);

    ctx->hwdec_failed = false;
    ctx->hwdec_request_reinit = false;

    // 存放解码器上下文
    ctx->avctx = avcodec_alloc_context3(lavc_codec);
    
    AVCodecContext *avctx = ctx->avctx;
    if (!ctx->avctx)
        goto error;
    avctx->codec_type = AVMEDIA_TYPE_VIDEO;
    avctx->codec_id = lavc_codec->id;
    avctx->pkt_timebase = ctx->codec_timebase;

    // 存放解码后的帧
    ctx->pic = av_frame_alloc();
    if (!ctx->pic)
        goto error;

    // 存放输入的压缩数据包
    ctx->avpkt = av_packet_alloc();
    if (!ctx->avpkt)
        goto error;

    int threads = lavc_param->threads;
    if (ctx->use_hwdec) {
        // 设置解码器回调的上下文
        avctx->opaque = vd;

        // 忽略级别
        avctx->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
        if (!lavc_param->check_hw_profile)
            avctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;

#ifdef AV_HWACCEL_FLAG_UNSAFE_OUTPUT
        avctx->hwaccel_flags |= AV_HWACCEL_FLAG_UNSAFE_OUTPUT;
#endif

        // 设置硬件设备
        if (ctx->hwdec.use_hw_device) {
            if (ctx->hwdec_dev)
                avctx->hw_device_ctx = av_buffer_ref(ctx->hwdec_dev);   // 硬件设备上下文设置时机   
            if (!avctx->hw_device_ctx)  // 硬件帧上下文设置将在后续的回调函数get_format_hwdec中设置
                goto error;
        }
        if (ctx->hwdec.use_hw_frames) {
            if (!ctx->hwdec_dev)
                goto error;
        }

        // 设置像素格式查询回调（硬件格式协商）
        if (ctx->hwdec.pix_fmt != AV_PIX_FMT_NONE)
            avctx->get_format = get_format_hwdec;

        // Some APIs benefit from this, for others it's additional bloat.
        if (ctx->hwdec.copying)
            ctx->max_delay_queue = HWDEC_DELAY_QUEUE_COUNT;
        ctx->hw_probing = true;

        threads = ctx->hwdec_opts->hwdec_threads;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(62, 11, 100)
        // Vulkan threading was not safe before 62.11.100
        bstr hwdec_name = bstr0(ctx->hwdec.name);
        if (bstr_endswith0(hwdec_name, "vulkan") || bstr_endswith0(hwdec_name, "vulkan-copy"))
            threads = 1;
#endif
    }

    mp_set_avcodec_threads(vd->log, avctx, threads);

    if (!ctx->use_hwdec && ctx->vo && lavc_param->dr) {
        avctx->opaque = vd;
        avctx->get_buffer2 = get_buffer2_direct;
    }

    avctx->flags |= lavc_param->bitexact ? AV_CODEC_FLAG_BITEXACT : 0;
    avctx->flags2 |= lavc_param->fast ? AV_CODEC_FLAG2_FAST : 0;

    if (lavc_param->show_all)
        avctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;

    avctx->skip_loop_filter = lavc_param->skip_loop_filter;
    avctx->skip_idct = lavc_param->skip_idct;
    avctx->skip_frame = lavc_param->skip_frame;
    avctx->apply_cropping = lavc_param->apply_cropping;

    if (lavc_codec->id == AV_CODEC_ID_H264 && lavc_param->old_x264)
        av_opt_set(avctx, "x264_build", "150", AV_OPT_SEARCH_CHILDREN);

    switch(ctx->opts->film_grain) {
    case 0: /*CPU*/
        // default lavc flags handle film grain within the decoder.
        break;
    case 1: /*GPU*/
        if (!ctx->vo ||
            (ctx->vo && !(ctx->vo->driver->caps & VO_CAP_FILM_GRAIN))) {
            MP_MSG(vd, ctx->vo ? MSGL_WARN : MSGL_V,
                   "GPU film grain requested, but VO %s, expect wrong output.\n",
                   ctx->vo ?
                   "does not support applying film grain" :
                   "is not available at decoder initialization to verify support");
        }

        avctx->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
        break;
    default:
        if (ctx->vo && (ctx->vo->driver->caps & VO_CAP_FILM_GRAIN))
            avctx->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;

        break;
    }

    mp_set_avopts(vd->log, avctx, lavc_param->avopts);

    ctx->skip_frame = avctx->skip_frame;

    if (mp_set_avctx_codec_headers(avctx, c) < 0) {
        MP_ERR(vd, "Could not set codec parameters.\n");
        goto error;
    }

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (avctx->width > 8192 || avctx->height > 8192) {
        MP_ERR(vd, "Frame size too big %" PRIu32 "x%" PRIu32 ".\n", avctx->width, avctx->height);
        goto error;
    }
#endif

    // 打开解码器
    if (avcodec_open2(avctx, lavc_codec, NULL) < 0)
        goto error;

    if (lavc_codec->id == AV_CODEC_ID_H264 && c->first_packet) {
        mp_set_av_packet(ctx->avpkt, c->first_packet, &ctx->codec_timebase);
        avcodec_send_packet(avctx, ctx->avpkt);
        avcodec_receive_frame(avctx, ctx->pic);
        av_frame_unref(ctx->pic);
        avcodec_flush_buffers(ctx->avctx);
    }

    return;

error:
    MP_ERR(vd, "Could not open codec.\n");
    uninit_avctx(vd);
}
```

---

<br/>

> 硬件设备模式和硬件帧模式在MPV中都需要调用get_format

```C
static enum AVPixelFormat get_format_hwdec(struct AVCodecContext *avctx,
                                           const enum AVPixelFormat *fmt)
{
    struct mp_filter *vd = avctx->opaque;
    vd_ffmpeg_ctx *ctx = vd->priv;

    MP_VERBOSE(vd, "Pixel formats supported by decoder:");
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++)
        MP_VERBOSE(vd, " %s", av_get_pix_fmt_name(fmt[i]));
    MP_VERBOSE(vd, "\n");

    const char *profile = avcodec_profile_name(avctx->codec_id, avctx->profile);
    MP_VERBOSE(vd, "Codec profile: %s (0x%x)\n", profile ? profile : "unknown",
               avctx->profile);

    mp_assert(ctx->use_hwdec);

    ctx->hwdec_request_reinit |= ctx->hwdec_failed;
    ctx->hwdec_failed = false;

    enum AVPixelFormat select = AV_PIX_FMT_NONE;

    // 查找匹配的像素格式
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++) {
        if (ctx->hwdec.pix_fmt == fmt[i]) {
            // 硬件设备模式和硬件帧模式的关键区别在此
            if (init_generic_hwaccel(avctx, fmt[i]) < 0)
                break;
            select = fmt[i];
            break;
        }
    }

    if (select == AV_PIX_FMT_NONE)
        ctx->hwdec_failed = true;

    const char *name = av_get_pix_fmt_name(select);
    MP_VERBOSE(vd, "Requesting pixfmt '%s' from decoder.\n", name ? name : "-");
    return select;
}
```

> init_generic_hwaccel

```C
static int init_generic_hwaccel(struct AVCodecContext *avctx, enum AVPixelFormat hw_fmt)
{
    struct mp_filter *vd = avctx->opaque;
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVBufferRef *new_frames_ctx = NULL;

    // 只有 use_hw_frames 模式才执行主要逻辑
    if (!ctx->hwdec.use_hw_frames)
        return 0;

    if (!ctx->hwdec_dev) {
        MP_ERR(ctx, "Missing device context.\n");
        goto error;
    }

    // 获取帧池参数模板
    // avcodec_get_hw_frames_parameters 的作用：询问 FFmpeg，对于这个解码器（avctx）、使用这个硬件设备（ctx->hwdec_dev）、输出这个像素格式（hw_fmt），需要什么样的帧池
    if (avcodec_get_hw_frames_parameters(avctx,
                                ctx->hwdec_dev, hw_fmt, &new_frames_ctx) < 0)
    {
        MP_VERBOSE(ctx, "Hardware decoding of this stream is unsupported?\n");
        goto error;
    }

    // mpv 允许用户覆盖默认配置
    AVHWFramesContext *new_fctx = (void *)new_frames_ctx->data;

    if (ctx->hwdec_opts->hwdec_image_format)
        new_fctx->sw_format = imgfmt2pixfmt(ctx->hwdec_opts->hwdec_image_format);

    // 1 surface is already included by libavcodec. The field is 0 if the
    // hwaccel supports dynamic surface allocation.
    if (new_fctx->initial_pool_size)
        new_fctx->initial_pool_size += ctx->hwdec_opts->hwdec_extra_frames - 1;

    // 硬件特定的微调
    const struct hwcontext_fns *fns =
        hwdec_get_hwcontext_fns(new_fctx->device_ctx->type);

    if (fns && fns->refine_hwframes)
        fns->refine_hwframes(new_frames_ctx);

    // 帧池缓存与复用
    if (ctx->cached_hw_frames_ctx) {
        AVHWFramesContext *old_fctx = (void *)ctx->cached_hw_frames_ctx->data;

        if (new_fctx->format            != old_fctx->format ||
            new_fctx->sw_format         != old_fctx->sw_format ||
            new_fctx->width             != old_fctx->width ||
            new_fctx->height            != old_fctx->height ||
            new_fctx->initial_pool_size != old_fctx->initial_pool_size)
            av_buffer_unref(&ctx->cached_hw_frames_ctx);
    }

    if (!ctx->cached_hw_frames_ctx) {
        if (av_hwframe_ctx_init(new_frames_ctx) < 0) {
            MP_ERR(ctx, "Failed to allocate hw frames.\n");
            goto error;
        }

        ctx->cached_hw_frames_ctx = new_frames_ctx;
        new_frames_ctx = NULL;
    }

    // 安装到解码器
    avctx->hw_frames_ctx = av_buffer_ref(ctx->cached_hw_frames_ctx);
    if (!avctx->hw_frames_ctx)
        goto error;

    av_buffer_unref(&new_frames_ctx);
    return 0;

error:
    av_buffer_unref(&new_frames_ctx);
    av_buffer_unref(&ctx->cached_hw_frames_ctx);
    return -1;
}
```


### 解码循环

> 核心调用链

filter_graph_process() -> mp_filter_run() -> vd_lavc_process() -> lavc_process() -> send_packet() 和 receive_frame()

> vd_lavc_process
```C
static void vd_lavc_process(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    lavc_process(vd, &ctx->state, send_packet, receive_frame);
}
```

---

<br/>

> lavc_process

```C
void lavc_process(struct mp_filter *f, struct lavc_state *state,
                  int (*send)(struct mp_filter *f, struct demux_packet *pkt),
                  int (*receive)(struct mp_filter *f, struct mp_frame *res))
{
    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    struct mp_frame frame = {0};
    int ret_recv = receive(f, &frame);
    if (frame.type) {
        state->eof_returned = false;
        mp_pin_in_write(f->ppins[1], frame);
    } else if (ret_recv == AVERROR_EOF) {
        if (!state->eof_returned)
            mp_pin_in_write(f->ppins[1], MP_EOF_FRAME);
        state->eof_returned = true;
        state->packets_sent = false;
    } else if (ret_recv == AVERROR(EAGAIN)) {
        // Need to feed a packet.
        frame = mp_pin_out_read(f->ppins[0]);
        struct demux_packet *pkt = NULL;
        if (frame.type == MP_FRAME_PACKET) {
            pkt = frame.data;
        } else if (frame.type != MP_FRAME_EOF) {
            if (frame.type) {
                MP_ERR(f, "unexpected frame type\n");
                mp_frame_unref(&frame);
                mp_filter_internal_mark_failed(f);
            }
            return;
        } else if (!state->packets_sent) {
            // EOF only; just return it, without requiring send/receive to
            // pass it through properly.
            mp_pin_in_write(f->ppins[1], MP_EOF_FRAME);
            return;
        }
        int ret_send = send(f, pkt);
        if (ret_send == AVERROR(EAGAIN)) {
            // Should never happen, but can happen with broken decoders.
            MP_WARN(f, "could not consume packet\n");
            mp_pin_out_unread(f->ppins[0], frame);
            mp_filter_wakeup(f);
            return;
        }
        state->packets_sent = true;
        demux_packet_pool_push(f->packet_pool, pkt);
        mp_filter_internal_mark_progress(f);
    } else {
        // Decoding error, or hwdec fallback recovery. Just try again.
        mp_filter_internal_mark_progress(f);
    }
}
```

---

<br/>

#### 发送压缩数据包并解压

> send_packet

```C
static int send_packet(struct mp_filter *vd, struct demux_packet *pkt)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVCodecContext *avctx = ctx->avctx;

    if (ctx->num_requeue_packets && ctx->requeue_packets[0] != pkt)
        return AVERROR(EAGAIN); // cannot consume the packet

    if (ctx->hwdec_failed)
        return AVERROR(EAGAIN);

    if (!ctx->avctx)
        return AVERROR_EOF;

    prepare_decoding(vd);

    // 关键帧等待逻辑
    // 某些情况下（如 seek 后、硬解初始化后），解码器需要从一个关键帧开始才能正确解码。这个逻辑会丢弃所有非关键帧，直到遇到关键帧
    if (ctx->wait_for_keyframe > 0 && pkt) {
        if (!pkt->keyframe && !ctx->intra_only) {
            MP_DBG(vd, "Waiting for keyframe after reinit (dropping frame).\n");
            ctx->wait_for_keyframe--;
            return 0;
        }
        ctx->wait_for_keyframe = 0;
    }

    if (avctx->skip_frame == AVDISCARD_ALL)
        return 0;

    // 将 mpv 的 demux_packet 转换为 FFmpeg 的 AVPacket，同时设置正确的时间基
    mp_set_av_packet(ctx->avpkt, pkt, &ctx->codec_timebase);

    // 将包送入 FFmpeg 解码器
    // avcodec_send_packet() 只是把压缩数据包推入解码器
    int ret = avcodec_send_packet(avctx, pkt ? ctx->avpkt : NULL);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return ret;

    // 硬件探测模式的特殊处理
    // 这是 mpv 硬件解码探测机制的关键，保存前 32 个包的副本到 sent_packets 数组中，如果硬件解码处理失败，可以回退为软件解码
    if (ctx->hw_probing && ctx->num_sent_packets < 32 &&
        ctx->hwdec_opts->software_fallback <= 32)
    {
        pkt = pkt ? demux_copy_packet(vd->packet_pool, pkt) : NULL;
        MP_TARRAY_APPEND(ctx, ctx->sent_packets, ctx->num_sent_packets, pkt);
    }

    if (ret < 0)
        handle_err(vd);
    return ret;
}
```

---

</br>

#### 接受解码帧

> receive_frame

```C
static int receive_frame(struct mp_filter *vd, struct mp_frame *out_frame)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    // 解码帧
    int ret = decode_frame(vd);

    if (ctx->hwdec_failed) {
        struct demux_packet **pkts = ctx->sent_packets;
        int num_pkts = ctx->num_sent_packets;
        ctx->sent_packets = NULL;
        ctx->num_sent_packets = 0;

        do {
            force_fallback(vd);
        } while (!ctx->avctx);

        ctx->requeue_packets = pkts;
        ctx->num_requeue_packets = num_pkts;

        return 0;
    }

    if (ret == AVERROR(EAGAIN) && ctx->num_requeue_packets)
        return 0;

    if (ctx->num_delay_queue <= ctx->max_delay_queue && ret != AVERROR_EOF)
        return ret;

    if (!ctx->num_delay_queue)
        return ret;

    struct mp_image *res = ctx->delay_queue[0];
    MP_TARRAY_REMOVE_AT(ctx->delay_queue, ctx->num_delay_queue, 0);

    res = res ? mp_img_swap_to_native(res) : NULL;
    if (!res)
        return AVERROR_UNKNOWN;

    if (ctx->use_hwdec && ctx->hwdec.copying && res->hwctx) {
        struct mp_image *sw = mp_image_hw_download(res, ctx->hwdec_swpool);
        mp_image_unrefp(&res);
        res = sw;
        if (!res) {
            MP_ERR(vd, "Could not copy back hardware decoded frame.\n");
            ctx->hwdec_fail_count = INT_MAX - 1;
            handle_err(vd);
            return AVERROR_UNKNOWN;
        }
    }

    if (!ctx->hwdec_notified) {
        if (ctx->use_hwdec) {
            MP_INFO(vd, "Using hardware decoding (%s).\n",
                    ctx->hwdec.method_name);
        } else {
            MP_VERBOSE(vd, "Using software decoding.\n");
        }
        ctx->hwdec_notified = true;
    }

    if (ctx->hw_probing) {
        for (int n = 0; n < ctx->num_sent_packets; n++)
            demux_packet_pool_push(vd->packet_pool, ctx->sent_packets[n]);
        ctx->num_sent_packets = 0;
        ctx->hw_probing = false;
    }

    *out_frame = MAKE_FRAME(MP_FRAME_VIDEO, res);
    return 0;
}
```

> decode_frame

```C
static int decode_frame(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVCodecContext *avctx = ctx->avctx;

    if (!avctx || ctx->force_eof)
        return AVERROR_EOF;

    prepare_decoding(vd);

    if (ctx->num_requeue_packets)
        send_queued_packet(vd);

    // 从解码器取出解码后的帧
    int ret = avcodec_receive_frame(avctx, ctx->pic);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            if (!ctx->num_delay_queue)
                reset_avctx(vd);
        } else if (ret == AVERROR(EAGAIN)) {
            // just retry after caller writes a packet
        } else {
            handle_err(vd);
        }
        return ret;
    }

    mp_codec_info_from_av(avctx, ctx->codec);

    mp_assert(ctx->pic->buf[0]);

    // 将 FFmpeg 的 AVFrame（解码后的原始帧）转换为 mpv 内部使用的 mp_image 结构
    struct mp_image *mpi = mp_image_from_av_frame(ctx->pic);
    if (!mpi) {
        av_frame_unref(ctx->pic);
        return ret;
    }

    if (mpi->imgfmt == IMGFMT_CUDA && !mpi->planes[0]) {
        MP_ERR(vd, "CUDA frame without data. This is a FFmpeg bug.\n");
        talloc_free(mpi);
        handle_err(vd);
        return AVERROR_BUG;
    }

    ctx->hwdec_fail_count = 0;

    mpi->pts = mp_pts_from_av(ctx->pic->pts, &ctx->codec_timebase);
    mpi->dts = mp_pts_from_av(ctx->pic->pkt_dts, &ctx->codec_timebase);
    mpi->pkt_duration = mp_pts_from_av(ctx->pic->duration, &ctx->codec_timebase);

    av_frame_unref(ctx->pic);

    MP_TARRAY_APPEND(ctx, ctx->delay_queue, ctx->num_delay_queue, mpi);
    return ret;
}
```