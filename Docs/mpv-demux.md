# mpv-demux.md
## 打开媒体文件（avformat_open_input）以及解析流信息（avformat_find_stream_info）
> start_open

主线程中调用start_open函数，通过mp_thread_create启动open_demux_thread
```C
static void start_open(struct MPContext *mpctx, char *url, int url_flags,
                       bool for_prefetch)
{
    cancel_open(mpctx);

    mp_assert(!mpctx->open_active);
    mp_assert(!mpctx->open_cancel);
    mp_assert(!mpctx->open_res_demuxer);
    mp_assert(!atomic_load(&mpctx->open_done));

    mpctx->open_cancel = mp_cancel_new(NULL);
    mpctx->open_url = talloc_strdup(NULL, url);
    mpctx->open_format = talloc_strdup(NULL, mpctx->opts->demuxer_name);
    mpctx->open_url_flags = url_flags;
    mpctx->open_for_prefetch = for_prefetch && mpctx->opts->demuxer_thread;
    mpctx->demuxer_changed = false;

    if (mp_thread_create(&mpctx->open_thread, open_demux_thread, mpctx)) {
        cancel_open(mpctx);
        return;
    }

    mpctx->open_active = true;
}
```

> open_demux_thread

该线程异步地打开一个新的文件并创建其demuxer
```C
static MP_THREAD_VOID open_demux_thread(void *ctx)
{
    struct MPContext *mpctx = ctx;

    mp_thread_set_name("opener");

    struct demuxer_params p = {
        .force_format = mpctx->open_format,
        .stream_flags = mpctx->open_url_flags,
        .stream_record = true,
        .is_top_level = true,
        .allow_playlist_create = mpctx->playlist->num_entries <= 1 &&
                                 !mpctx->playlist->playlist_dir,
    };
    struct demuxer *demux =
        demux_open_url(mpctx->open_url, &p, mpctx->open_cancel, mpctx->global);
    mpctx->open_res_demuxer = demux;

    if (demux) {
        MP_VERBOSE(mpctx, "Opening done: %s\n", mpctx->open_url);

        if (mpctx->open_for_prefetch && !demux->fully_read) {
            int num_streams = demux_get_num_stream(demux);
            for (int n = 0; n < num_streams; n++) {
                struct sh_stream *sh = demux_get_stream(demux, n);
                demuxer_select_track(demux, sh, MP_NOPTS_VALUE, true);
            }

            demux_set_wakeup_cb(demux, wakeup_demux, mpctx);
            demux_start_thread(demux);
            demux_start_prefetch(demux);
        }
    } else {
        MP_VERBOSE(mpctx, "Opening failed or was aborted: %s\n", mpctx->open_url);

        if (p.demuxer_failed) {
            mpctx->open_res_error = MPV_ERROR_UNKNOWN_FORMAT;
        } else {
            mpctx->open_res_error = MPV_ERROR_LOADING_FAILED;
        }
    }

    atomic_store(&mpctx->open_done, true);
    mp_wakeup_core(mpctx);
    MP_THREAD_RETURN();
}
```

> demux_open_url
```C
struct demuxer *demux_open_url(const char *url,
                               struct demuxer_params *params,
                               struct mp_cancel *cancel,
                               struct mpv_global *global)
{
    if (!params)
        return NULL;
    struct mp_cancel *priv_cancel = mp_cancel_new(NULL);
    if (cancel)
        mp_cancel_set_parent(priv_cancel, cancel);
    struct stream *s = params->external_stream;
    if (!s) {
        s = stream_create(url, STREAM_READ | params->stream_flags,
                          priv_cancel, global);
        if (s && params->init_fragment.len) {
            s = create_webshit_concat_stream(global, priv_cancel,
                                             params->init_fragment, s);
        }
    }
    if (!s) {
        talloc_free(priv_cancel);
        return NULL;
    }

    // 打开解复用器
    struct demuxer *d = demux_open(s, priv_cancel, params, global);
    if (d) {
        talloc_steal(d->in, priv_cancel);
        mp_assert(d->cancel);
    } else {
        params->demuxer_failed = true;
        if (!params->external_stream)
            free_stream(s);
        talloc_free(priv_cancel);
    }
    return d;
}
```

> demux_open
```C
static struct demuxer *demux_open(struct stream *stream,
                                  struct mp_cancel *cancel,
                                  struct demuxer_params *params,
                                  struct mpv_global *global)
{
    const int *check_levels = d_normal;
    const struct demuxer_desc *check_desc = NULL;
    struct mp_log *log = mp_log_new(NULL, global->log, "!demux");
    struct demuxer *demuxer = NULL;
    char *force_format = params ? params->force_format : NULL;

    struct parent_stream_info sinfo = {
        .seekable = stream->seekable,
        .is_network = stream->is_network,
        .is_streaming = stream->streaming,
        .stream_origin = stream->stream_origin,
        .cancel = cancel,
        .filename = talloc_strdup(NULL, stream->url),
    };

    if (!force_format)
        force_format = stream->demuxer;

    // 如果使用 -demuxer 参数， 则使用 -demuxer 参数指定的解复用器
    if (force_format && force_format[0] && !stream->is_directory) {
        check_levels = d_request;
        if (force_format[0] == '+') {
            force_format += 1;
            check_levels = d_force;
        }

        // 获取目标解复用器（遍历）
        for (int n = 0; demuxer_list[n]; n++) {
            if (strcmp(demuxer_list[n]->name, force_format) == 0) {
                check_desc = demuxer_list[n];
                break;
            }
        }
        if (!check_desc) {
            mp_err(log, "Demuxer %s does not exist.\n", force_format);
            goto done;
        }
    }

    // 逐个解复用器遍历，使用第一个能成功打开的解复用器
    for (int pass = 0; check_levels[pass] != -1; pass++) {
        enum demux_check level = check_levels[pass];
        mp_verbose(log, "Trying demuxers for level=%s.\n", d_level(level));
        for (int n = 0; demuxer_list[n]; n++) {
            const struct demuxer_desc *desc = demuxer_list[n];
            if (!check_desc || desc == check_desc) {
                // 关键是下面这个open_given_type函数
                demuxer = open_given_type(global, log, desc, stream, &sinfo,
                                          params, level);
                if (demuxer) {
                    talloc_steal(demuxer, log);
                    log = NULL;
                    goto done;
                }
            }
        }
    }

done:
    talloc_free(sinfo.filename);
    talloc_free(log);
    return demuxer;
}
```

> open_given_type
```C
static struct demuxer *open_given_type(struct mpv_global *global,
                                       struct mp_log *log,
                                       const struct demuxer_desc *desc,
                                       struct stream *stream,
                                       struct parent_stream_info *sinfo,
                                       struct demuxer_params *params,
                                       enum demux_check check)
{
    if (mp_cancel_test(sinfo->cancel))
        return NULL;

    if (params && params->depth > 10) {
        mp_err(log, "Demuxer recursion depth exceeded.\n");
        return NULL;
    }

    struct demuxer *demuxer = talloc_ptrtype(NULL, demuxer);
    struct m_config_cache *opts_cache =
        m_config_cache_alloc(demuxer, global, &demux_conf);
    struct demux_opts *opts = opts_cache->opts;
    *demuxer = (struct demuxer) {
        .desc = desc,
        .stream = stream,
        .cancel = sinfo->cancel,
        .seekable = sinfo->seekable,
        .filepos = -1,
        .global = global,
        .log = mp_log_new(demuxer, log, desc->name),
        .packet_pool = demux_packet_pool_get(global),
        .glog = log,
        .filename = talloc_strdup(demuxer, sinfo->filename),
        .is_network = sinfo->is_network,
        .is_streaming = sinfo->is_streaming,
        .stream_origin = sinfo->stream_origin,
        .access_references = opts->access_references,
        .opts = opts,
        .opts_cache = opts_cache,
        .events = DEMUX_EVENT_ALL,
        .duration = -1,
        .depth = params ? params->depth : 0,
    };

    struct demux_internal *in = demuxer->in = talloc_ptrtype(demuxer, in);
    *in = (struct demux_internal){
        .global = global,
        .log = demuxer->log,
        .packet_pool = demux_packet_pool_get(global),
        .stats = stats_ctx_create(in, global, "demuxer"),
        .can_cache = params && params->is_top_level,
        .can_record = params && params->stream_record,
        .d_thread = talloc(demuxer, struct demuxer),
        .d_user = demuxer,
        .after_seek = true, // (assumed identical to initial demuxer state)
        .after_seek_to_start = true,
        .highest_av_pts = MP_NOPTS_VALUE,
        .seeking_in_progress = MP_NOPTS_VALUE,
        .demux_ts = MP_NOPTS_VALUE,
        .owns_stream = !params->external_stream,
    };
    mp_mutex_init(&in->lock);
    mp_cond_init(&in->wakeup);

    *in->d_thread = *demuxer;

    in->d_thread->metadata = talloc_zero(in->d_thread, struct mp_tags);

    mp_dbg(log, "Trying demuxer: %s (force-level: %s)\n",
           desc->name, d_level(check));

    if (stream)
        stream_seek(stream, 0);

    in->d_thread->params = params;

    // 调用open函数，如果返回 >= 0，则表示成功打开
    int ret = demuxer->desc->open(in->d_thread, check);
    if (ret >= 0) {
        in->d_thread->params = NULL;
        if (in->d_thread->filetype)
            mp_verbose(log, "Detected file format: %s (%s)\n",
                       in->d_thread->filetype, desc->desc);
        else
            mp_verbose(log, "Detected file format: %s\n", desc->desc);
        if (!in->d_thread->seekable)
            mp_verbose(log, "Stream is not seekable.\n");
        if (!in->d_thread->seekable && opts->force_seekable) {
            mp_warn(log, "Not seekable, but enabling seeking on user request.\n");
            in->d_thread->seekable = true;
            in->d_thread->partially_seekable = true;
        }
        demux_init_cuesheet(in->d_thread);
        demux_init_ccs(demuxer, opts);
        demux_convert_tags_charset(in->d_thread);
        demux_copy(in->d_user, in->d_thread);
        in->duration = in->d_thread->duration;
        demuxer_sort_chapters(demuxer);
        in->events = DEMUX_EVENT_ALL;

        struct demuxer *sub = NULL;
        if (!(params && params->disable_timeline)) {
            struct timeline *tl = timeline_load(global, log, demuxer);
            if (tl) {
                struct demuxer_params params2 = {0};
                params2.timeline = tl;
                params2.is_top_level = params && params->is_top_level;
                params2.stream_record = params && params->stream_record;
                params2.depth = params ? params->depth + 1 : 0;
                sub =
                    open_given_type(global, log, &demuxer_desc_timeline,
                                    NULL, sinfo, &params2, DEMUX_CHECK_FORCE);
                if (sub) {
                    in->can_cache = false;
                    in->can_record = false;
                } else {
                    timeline_destroy(tl);
                }
            }
        }

        switch_to_fresh_cache_range(in);

        update_opts(demuxer);

        demux_update(demuxer, MP_NOPTS_VALUE);

        demuxer = sub ? sub : demuxer;
        return demuxer;
    }

    demuxer->stream = NULL;
    demux_free(demuxer);
    return NULL;
}
```

> 通过结构体将ffmpeg解复用器的open函数注册到mpv中
```C
const demuxer_desc_t demuxer_desc_lavf = {
    .name = "lavf",
    .desc = "libavformat",
    .read_packet = demux_lavf_read_packet,
    .open = demux_open_lavf,
    .drop_buffers = demux_drop_buffers_lavf,
    .close = demux_close_lavf,
    .seek = demux_seek_lavf,
    .switched_tracks = demux_lavf_switched_tracks,
};
```
 
> demux_open_lavf
```C
static int demux_open_lavf(demuxer_t *demuxer, enum demux_check check)
{
    AVFormatContext *avfc = NULL;
    AVDictionaryEntry *t = NULL;
    float analyze_duration = 0;
    lavf_priv_t *priv = talloc_zero(NULL, lavf_priv_t);
    AVDictionary *dopts = NULL;

    demuxer->priv = priv;
    priv->stream = demuxer->stream;

    priv->opts = mp_get_config_group(priv, demuxer->global, &demux_lavf_conf);
    struct demux_lavf_opts *lavfdopts = priv->opts;

    if (lavf_check_file(demuxer, check) < 0)
        goto fail;

    avfc = avformat_alloc_context();
    if (!avfc)
        goto fail;

    if (demuxer->opts->index_mode != 1)
        avfc->flags |= AVFMT_FLAG_IGNIDX;

    if (lavfdopts->probesize) {
        if (av_opt_set_int(avfc, "probesize", lavfdopts->probesize, 0) < 0)
            MP_ERR(demuxer, "couldn't set option probesize to %u\n",
                   lavfdopts->probesize);
    }

    if (priv->format_hack.analyzeduration)
        analyze_duration = priv->format_hack.analyzeduration;
    if (lavfdopts->analyzeduration)
        analyze_duration = lavfdopts->analyzeduration;
    if (analyze_duration > 0) {
        if (av_opt_set_int(avfc, "analyzeduration",
                           analyze_duration * AV_TIME_BASE, 0) < 0)
            MP_ERR(demuxer, "demux_lavf, couldn't set option "
                   "analyzeduration to %f\n", analyze_duration);
    }

    if (priv->format_hack.no_ext_picky) {
        bool user_set_ext_picky = false;
        for (int i = 0; lavfdopts->avopts && lavfdopts->avopts[i * 2]; i++) {
            if (bstr_startswith0(bstr0(lavfdopts->avopts[i * 2]), "extension_picky")) {
                user_set_ext_picky = true;
                break;
            }
        }
        if (!user_set_ext_picky && av_dict_set(&dopts, "extension_picky", "0", 0) >= 0)
            MP_VERBOSE(demuxer, "Option extension_picky=0 was set due to known FFmpeg bugs\n");
    }

    if ((priv->avif_flags & AVFMT_NOFILE) || priv->format_hack.no_stream) {
        mp_setup_av_network_options(&dopts, priv->avif->name,
                                    demuxer->global, demuxer->log);
        // This might be incorrect.
        demuxer->seekable = true;
    } else {
        void *buffer = av_malloc(lavfdopts->buffersize);
        if (!buffer)
            goto fail;
        priv->pb = avio_alloc_context(buffer, lavfdopts->buffersize, 0,
                                      demuxer, mp_read, NULL, mp_seek);
        if (!priv->pb) {
            av_free(buffer);
            goto fail;
        }
        priv->pb->read_seek = mp_read_seek;
        priv->pb->seekable = demuxer->seekable ? AVIO_SEEKABLE_NORMAL : 0;
        avfc->pb = priv->pb;
        if (stream_control(priv->stream, STREAM_CTRL_HAS_AVSEEK, NULL) > 0)
            demuxer->seekable = true;
        demuxer->seekable |= priv->format_hack.fully_read;
    }

    if (matches_avinputformat_name(priv, "rtsp")) {
        const char *transport = NULL;
        switch (lavfdopts->rtsp_transport) {
        case 1: transport = "udp";  break;
        case 2: transport = "tcp";  break;
        case 3: transport = "http"; break;
        case 4: transport = "udp_multicast"; break;
        }
        if (transport)
            av_dict_set(&dopts, "rtsp_transport", transport, 0);
    }

    guess_and_set_vobsub_name(demuxer, &dopts);

    avfc->interrupt_callback = (AVIOInterruptCB){
        .callback = interrupt_cb,
        .opaque = demuxer,
    };

    avfc->opaque = demuxer;
    if (demuxer->access_references) {
        priv->default_io_open = avfc->io_open;
        avfc->io_open = nested_io_open;
        priv->default_io_close2 = avfc->io_close2;
        avfc->io_close2 = nested_io_close2;
    } else {
        avfc->io_open = block_io_open;
    }

    mp_set_avdict(&dopts, lavfdopts->avopts);

    if (av_dict_copy(&priv->av_opts, dopts, 0) < 0) {
        MP_ERR(demuxer, "av_dict_copy() failed\n");
        goto fail;
    }

    if (priv->format_hack.readall_on_no_streamseek && priv->pb &&
        !priv->pb->seekable)
    {
        MP_VERBOSE(demuxer, "Non-seekable demuxer pre-read hack...\n");
        // Read incremental to avoid unnecessary large buffer sizes.
        int r = 0;
        for (int n = 16; n < 29; n++) {
            r = stream_peek(priv->stream, 1 << n);
            if (r < (1 << n))
                break;
        }
        MP_VERBOSE(demuxer, "...did read %d bytes.\n", r);
    }

    if (avformat_open_input(&avfc, priv->filename, priv->avif, &dopts) < 0) {
        MP_ERR(demuxer, "avformat_open_input() failed\n");
        goto fail;
    }

    mp_avdict_print_unset(demuxer->log, MSGL_V, dopts);
    av_dict_free(&dopts);

    priv->avfc = avfc;

    bool probeinfo = lavfdopts->probeinfo != 0;
    switch (lavfdopts->probeinfo) {
    case -2: probeinfo = priv->avfc->nb_streams == 0; break;
    case -1: probeinfo = !priv->format_hack.skipinfo; break;
    }
    if (demuxer->params && demuxer->params->skip_lavf_probing)
        probeinfo = false;
    if (probeinfo) {
        if (avformat_find_stream_info(avfc, NULL) < 0) {
            MP_ERR(demuxer, "av_find_stream_info() failed\n");
            goto fail;
        }

        MP_VERBOSE(demuxer, "avformat_find_stream_info() finished after %"PRId64
                   " bytes.\n", stream_tell(priv->stream));
    }

    for (int i = 0; i < avfc->nb_chapters; i++) {
        AVChapter *c = avfc->chapters[i];
        t = av_dict_get(c->metadata, "title", NULL, 0);
        int index = demuxer_add_chapter(demuxer, t ? t->value : "",
                                        c->start * av_q2d(c->time_base), i);
        mp_tags_move_from_av_dictionary(demuxer->chapters[index].metadata, &c->metadata);
    }

    add_new_streams(demuxer);

    mp_tags_move_from_av_dictionary(demuxer->metadata, &avfc->metadata);

    demuxer->ts_resets_possible =
        priv->avif_flags & (AVFMT_TS_DISCONT | AVFMT_NOTIMESTAMPS);

    if (avfc->start_time != AV_NOPTS_VALUE)
        demuxer->start_time = avfc->start_time / (double)AV_TIME_BASE;

    demuxer->fully_read = priv->format_hack.fully_read;

#ifdef AVFMTCTX_UNSEEKABLE
    if (avfc->ctx_flags & AVFMTCTX_UNSEEKABLE)
        demuxer->seekable = false;
#endif

    demuxer->is_network |= priv->format_hack.is_network;
    demuxer->seekable &= !priv->format_hack.no_seek;

    // We initially prefer track durations over container durations because they
    // have a higher degree of precision over the container duration which are
    // only accurate to the 6th decimal place. This is probably a lavf bug.
    double total_duration = -1;
    double av_duration = -1;
    for (int n = 0; n < priv->avfc->nb_streams; n++) {
        AVStream *st = priv->avfc->streams[n];
        if (st->duration <= 0)
            continue;
        double f_duration = st->duration * av_q2d(st->time_base);
        total_duration = MPMAX(total_duration, f_duration);
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
            st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            av_duration = MPMAX(av_duration, f_duration);
    }
    double duration = av_duration > 0 ? av_duration : total_duration;
    if (duration <= 0 && priv->avfc->duration > 0)
        duration = (double)priv->avfc->duration / AV_TIME_BASE;
    demuxer->duration = duration;

    if (demuxer->duration < 0 && priv->format_hack.no_seek_on_no_duration)
        demuxer->seekable = false;

    // In some cases, libavformat will export bogus bullshit timestamps anyway,
    // such as with mjpeg.
    if (priv->avif_flags & AVFMT_NOTIMESTAMPS) {
        MP_WARN(demuxer,
                "This format is marked by FFmpeg as having no timestamps!\n"
                "FFmpeg will likely make up its own broken timestamps. For\n"
                "video streams you can correct this with:\n"
                "    --no-correct-pts --container-fps-override=VALUE\n"
                "with VALUE being the real framerate of the stream. You can\n"
                "expect seeking and buffering estimation to be generally\n"
                "broken as well.\n");
    }

    if (demuxer->fully_read) {
        demux_close_stream(demuxer);
        if (priv->own_stream)
            free_stream(priv->stream);
        priv->own_stream = false;
        priv->stream = NULL;
    }

    if (priv->stream) {
        const char *sname = priv->stream->info->name;
        priv->is_dvd_bd = strcmp(sname, "dvdnav") == 0 ||
                          strcmp(sname, "ifo_dvdnav") == 0 ||
                          strcmp(sname, "bd") == 0 ||
                          strcmp(sname, "bdnav") == 0 ||
                          strcmp(sname, "bdmv/bluray") == 0;
    }

    return 0;

fail:
    if (!priv->avfc)
        avformat_free_context(avfc);
    av_dict_free(&dopts);

    return -1;
}
```
> avformat_alloc_context():

分配并初始化一个 AVFormatContext 结构体。这个结构体是 libavformat 库的核心数据结构，用于存储整个媒体文件/流的格式信息、各个音视频流的信息以及解复用过程中的状态。

> av_opt_set_int():

这不是直接的解复用函数，而是 libavutil 提供的选项设置函数。在此函数中用于向 AVFormatContext (avfc) 设置一些探测和分析相关的参数，如 probesize 和 analyzeduration。这些参数会影响后续 avformat_open_input 和 avformat_find_stream_info 的行为。

> avformat_open_input():

这是最核心的函数之一。它根据传入的 AVFormatContext、文件名(priv->filename) 和推测的输入格式 (priv->avif) 来打开并初始化输入媒体文件或流。它会读取文件头信息，并填充 AVFormatContext 中的相关字段，为后续的解复用做准备。这个调用会触发之前设置的各种选项（包括通过 dopts 传递的用户自定义选项）生效。

> avformat_find_stream_info():

可选但重要的函数。它会读取并解码一部分媒体数据（受 analyzeduration 和其他因素限制），以获取每个音视频流的详细编解码参数（codec parameters），如分辨率、帧率、采样率、比特率等。这些信息对于后续解码至关重要。

> av_dict_set() / av_dict_copy() / av_dict_free() / av_dict_get():

这些是 libavutil 提供的字典操作函数。在此函数中用于构建、复制和传递选项给 libavformat（例如 dopts），以及读取 FFmpeg 内部设置的选项（例如 av_dict_get 用于获取章节标题）。它们不是直接的解复用功能函数，但用于配置和交互。

> avformat_free_context():

在初始化失败时，用于释放之前通过 avformat_alloc_context 分配的 AVFormatContext 及其内部可能分配的相关资源。

> 总结：demux_open_lavf 函数围绕着 libavformat 的 AVFormatContext，主要通过 avformat_alloc_context 创建上下文，avformat_open_input 打开输入源，以及可选的 avformat_find_stream_info 获取流详细信息，这三个函数构成了与 FFmpeg 核心解复用逻辑交互的基础。同时，利用 AVDictionary 系列函数进行选项配置。

## 按帧读取压缩数据包（av_read_frame）
> demux_start_thread

demux_start_thread用于启动解复用线程demux_thread
```C
void demux_start_thread(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    mp_assert(demuxer == in->d_user);

    if (!in->threading) {
        in->threading = true;
        if (mp_thread_create(&in->thread, demux_thread, in))
            in->threading = false;
    }
}
```

> demux_thread

该线程中有一个循环，不断调用thread_work进行解复用工作
```C
static MP_THREAD_VOID demux_thread(void *pctx)
{
    struct demux_internal *in = pctx;
    mp_thread_set_name("demux");
    mp_mutex_lock(&in->lock);

    stats_register_thread_cputime(in->stats, "thread");

    while (!in->thread_terminate) {
        if (thread_work(in))
            continue;
        mp_cond_signal(&in->wakeup);
        mp_cond_timedwait_until(&in->wakeup, &in->lock, in->next_cache_update);
    }

    if (in->shutdown_async) {
        mp_mutex_unlock(&in->lock);
        demux_shutdown(in);
        mp_mutex_lock(&in->lock);
        in->shutdown_async = false;
        if (in->wakeup_cb)
            in->wakeup_cb(in->wakeup_cb_ctx);
    }

    stats_unregister_thread(in->stats, "thread");

    mp_mutex_unlock(&in->lock);
    MP_THREAD_RETURN();
}
```

> thread_work

thread_work函数是demux_thread的主体，负责解复用工作
其中read_packet函数调用av_read_frame进行解复用
```C
static bool thread_work(struct demux_internal *in)
{
    struct demux_opts *opts = in->d_user->opts;
    size_t old_max_bytes = opts->max_bytes;
    size_t old_max_bytes_bw = opts->max_bytes_bw;
    if (m_config_cache_update(in->d_user->opts_cache)) {
        update_opts(in->d_user);
        if (opts->max_bytes + opts->max_bytes_bw < old_max_bytes + old_max_bytes_bw)
            demux_packet_pool_clear(in->packet_pool);
    }
    if (in->tracks_switched) {
        execute_trackswitch(in);
        return true;
    }
    if (in->need_back_seek) {
        perform_backward_seek(in);
        return true;
    }
    if (in->back_any_need_recheck) {
        check_backward_seek(in);
        return true;
    }
    if (in->seeking) {
        execute_seek(in);
        return true;
    }
    if (read_packet(in))
        return true; // read_packet unlocked, so recheck conditions
    if (mp_time_ns() >= in->next_cache_update) {
        update_cache(in);
        return true;
    }
    return false;
}
```

> read_packet

通过demux->desc->read_packet读取压缩数据包
```C
static bool read_packet(struct demux_internal *in)
{
    bool was_reading = in->reading;
    in->reading = false;

    if (!was_reading || in->blocked || demux_cancel_test(in->d_thread))
        return false;

    // Check if we need to read a new packet. We do this if all queues are below
    // the minimum, or if a stream explicitly needs new packets. Also includes
    // safe-guards against packet queue overflow.
    bool read_more = false, prefetch_more = false, refresh_more = false;
    uint64_t total_fw_bytes = 0;
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;
        if (ds->eager) {
            read_more |= !ds->reader_head;
            if (in->back_demuxing)
                read_more |= ds->back_restarting || ds->back_resuming;
        } else {
            if (lazy_stream_needs_wait(ds)) {
                read_more = true;
            } else {
                mark_stream_eof(ds); // let playback continue
            }
        }
        refresh_more |= ds->refreshing;
        if (ds->eager && ds->queue->last_ts != MP_NOPTS_VALUE &&
            in->min_secs > 0 && ds->base_ts != MP_NOPTS_VALUE &&
            ds->queue->last_ts >= ds->base_ts &&
            !in->back_demuxing)
        {
            if (ds->queue->last_ts - ds->base_ts <= in->hyst_secs)
                in->hyst_active = false;
            if (!in->hyst_active)
                prefetch_more |= ds->queue->last_ts - ds->base_ts < in->min_secs;
        }
        total_fw_bytes += get_forward_buffered_bytes(ds);
    }

    MP_TRACE(in, "bytes=%zd, read_more=%d prefetch_more=%d, refresh_more=%d\n",
             (size_t)total_fw_bytes, read_more, prefetch_more, refresh_more);
    if (total_fw_bytes >= in->max_bytes) {
        // if we hit the limit just by prefetching, simply stop prefetching
        if (!read_more) {
            in->hyst_active = !!in->hyst_secs;
            return false;
        }
        if (!in->warned_queue_overflow) {
            in->warned_queue_overflow = true;
            MP_WARN(in, "Too many packets in the demuxer packet queues:\n");
            for (int n = 0; n < in->num_streams; n++) {
                struct demux_stream *ds = in->streams[n]->ds;
                if (ds->selected) {
                    size_t num_pkts = 0;
                    for (struct demux_packet *dp = ds->reader_head;
                         dp; dp = dp->next)
                        num_pkts++;
                    uint64_t fw_bytes = get_forward_buffered_bytes(ds);
                    MP_WARN(in, "  %s/%d: %zd packets, %zd bytes%s%s\n",
                            stream_type_name(ds->type), n,
                            num_pkts, (size_t)fw_bytes,
                            ds->eager ? "" : " (lazy)",
                            ds->refreshing ? " (refreshing)" : "");
                }
            }
            if (in->back_demuxing)
                MP_ERR(in, "Backward playback is likely stuck/broken now.\n");
        }
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            if (!ds->reader_head)
                mark_stream_eof(ds);
        }
        return false;
    }

    if (!read_more && !prefetch_more && !refresh_more) {
        in->hyst_active = !!in->hyst_secs;
        return false;
    }

    if (in->after_seek_to_start) {
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            in->current_range->streams[n]->is_bof =
                ds->selected && !ds->refreshing;
        }
    }

    // Actually read a packet. Drop the lock while doing so, because waiting
    // for disk or network I/O can take time.
    in->reading = true;
    in->after_seek = false;
    in->after_seek_to_start = false;
    mp_mutex_unlock(&in->lock);

    struct demuxer *demux = in->d_thread;
    struct demux_packet *pkt = NULL;

    bool eof = true;
    if (demux->desc->read_packet && !demux_cancel_test(demux))
        eof = !demux->desc->read_packet(demux, &pkt);

    mp_mutex_lock(&in->lock);
    update_cache(in);

    if (pkt) {
        mp_assert(pkt->stream >= 0 && pkt->stream < in->num_streams);
        add_packet_locked(in->streams[pkt->stream], pkt);
    }

    if (!in->seeking) {
        if (eof) {
            for (int n = 0; n < in->num_streams; n++)
                mark_stream_eof(in->streams[n]->ds);
            // If we had EOF previously, then don't wakeup (avoids wakeup loop)
            if (!in->eof) {
                if (in->wakeup_cb)
                    in->wakeup_cb(in->wakeup_cb_ctx);
                mp_cond_signal(&in->wakeup);
                MP_VERBOSE(in, "EOF reached.\n");
            }
        }
        in->eof = eof;
        in->reading = !eof;
    }
    return true;
}
```

> 将FFmpeg的demux_lavf_read_packet注册到MPV中

```C
typedef struct demuxer_desc {
    const char *name;      // Demuxer name, used with -demuxer switch
    const char *desc;      // Displayed to user

    // If non-NULL, these are added to the global option list.
    const struct m_sub_options *options;

    // Return 0 on success, otherwise -1
    int (*open)(struct demuxer *demuxer, enum demux_check check);
    // The following functions are all optional
    // Try to read a packet. Return false on EOF. If true is returned, the
    // demuxer may set *pkt to a new packet (the reference goes to the caller).
    // If *pkt is NULL (the value when this function is called), the call
    // will be repeated.
    bool (*read_packet)(struct demuxer *demuxer, struct demux_packet **pkt);
    void (*drop_buffers)(struct demuxer *demuxer);
    void (*close)(struct demuxer *demuxer);
    void (*seek)(struct demuxer *demuxer, double rel_seek_secs, int flags);
    void (*switched_tracks)(struct demuxer *demuxer);
    // See timeline.c
    void (*load_timeline)(struct timeline *tl);
} demuxer_desc_t;
```
```C
const demuxer_desc_t demuxer_desc_lavf = {
    .name = "lavf",
    .desc = "libavformat",
    .read_packet = demux_lavf_read_packet,
    .open = demux_open_lavf,
    .drop_buffers = demux_drop_buffers_lavf,
    .close = demux_close_lavf,
    .seek = demux_seek_lavf,
    .switched_tracks = demux_lavf_switched_tracks,
};
```

> demux_lavf_read_packet

```C
static bool demux_lavf_read_packet(struct demuxer *demux,
                                   struct demux_packet **mp_pkt)
{
    lavf_priv_t *priv = demux->priv;

    AVPacket *pkt = av_packet_alloc();
    MP_HANDLE_OOM(pkt);

    // 核心函数：av_read_frame -> 得到AVPacket
    int r = av_read_frame(priv->avfc, pkt);
    update_read_stats(demux);
    if (r < 0) {
        av_packet_free(&pkt);
        if (r == AVERROR_EOF)
            return false;
        MP_WARN(demux, "error reading packet: %s.\n", av_err2str(r));
        if (priv->retry_counter >= 10) {
            MP_ERR(demux, "...treating it as fatal error.\n");
            return false;
        }
        priv->retry_counter += 1;
        return true;
    }
    priv->retry_counter = 0;

    add_new_streams(demux);
    update_metadata(demux);

    mp_assert(pkt->stream_index >= 0 && pkt->stream_index < priv->num_streams);
    struct stream_info *info = priv->streams[pkt->stream_index];
    struct sh_stream *stream = info->sh;
    AVStream *st = priv->avfc->streams[pkt->stream_index];

    if (!demux_stream_is_selected(stream)) {
        av_packet_free(&pkt);
        return true; // don't signal EOF if skipping a packet
    }

    // Never send additional frames for streams that are a single frame.
    if (stream->image && priv->format_hack.first_frame_only && pkt->pos != 0) {
        av_packet_free(&pkt);
        return true;
    }

    struct demux_packet *dp = new_demux_packet_from_avpacket(demux->packet_pool, pkt);
    if (!dp) {
        av_packet_free(&pkt);
        return true;
    }

    if (priv->pcm_seek_hack == st && !priv->pcm_seek_hack_packet_size)
        priv->pcm_seek_hack_packet_size = pkt->size;

    dp->pts = mp_pts_from_av(pkt->pts, &st->time_base);
    dp->dts = mp_pts_from_av(pkt->dts, &st->time_base);
    dp->duration = pkt->duration * av_q2d(st->time_base);
    dp->pos = pkt->pos;
    dp->keyframe = pkt->flags & AV_PKT_FLAG_KEY;
    dp->is_wrapped_avframe = st->codecpar->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME;
    if (dp->is_wrapped_avframe) {
        mp_require(dp->buffer);
        const AVFrame *frame = (AVFrame *)dp->buffer;
        dp->keyframe |= frame->flags & AV_FRAME_FLAG_KEY;
    }
    av_packet_free(&pkt);

    if (priv->format_hack.clear_filepos)
        dp->pos = -1;

    dp->stream = stream->index;

    if (priv->linearize_ts) {
        dp->pts = MP_ADD_PTS(dp->pts, info->ts_offset);
        dp->dts = MP_ADD_PTS(dp->dts, info->ts_offset);

        double pts = MP_PTS_OR_DEF(dp->pts, dp->dts);
        if (pts != MP_NOPTS_VALUE) {
            if (dp->keyframe) {
                if (pts < info->highest_pts) {
                    MP_WARN(demux, "Linearizing discontinuity: %f -> %f\n",
                            pts, info->highest_pts);
                    // Note: introduces a small discontinuity by a frame size.
                    double diff = info->highest_pts - pts;
                    dp->pts = MP_ADD_PTS(dp->pts, diff);
                    dp->dts = MP_ADD_PTS(dp->dts, diff);
                    pts += diff;
                    info->ts_offset += diff;
                    priv->any_ts_fixed = true;
                }
                info->last_key_pts = pts;
            }
            info->highest_pts = MP_PTS_MAX(info->highest_pts, pts);
        }
    }

    if (st->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) {
        st->event_flags = 0;
        struct mp_tags *tags = talloc_zero(NULL, struct mp_tags);
        mp_tags_move_from_av_dictionary(tags, &st->metadata);
        double pts = MP_PTS_OR_DEF(dp->pts, dp->dts);
        demux_stream_tags_changed(demux, stream, tags, pts);
    }

    *mp_pkt = dp;
    return true;
}
```

## 关键帧级别的定位（av_seek_frame）
> demux_seek

```C
int demux_seek(demuxer_t *demuxer, double seek_pts, int flags)
{
    struct demux_internal *in = demuxer->in;
    mp_assert(demuxer == in->d_user);

    mp_mutex_lock(&in->lock);

    if (!(flags & SEEK_FACTOR))
        seek_pts = MP_ADD_PTS(seek_pts, -in->ts_offset);

    int res = queue_seek(in, seek_pts, flags, true);

    mp_cond_signal(&in->wakeup);
    mp_mutex_unlock(&in->lock);

    return res;
}
```

> queue_seek

```C
static bool queue_seek(struct demux_internal *in, double seek_pts, int flags,
                       bool clear_back_state)
{
    if (seek_pts == MP_NOPTS_VALUE)
        return false;

    MP_VERBOSE(in, "queuing seek to %f%s\n", seek_pts,
               in->seeking ? " (cascade)" : "");

    bool require_cache = flags & SEEK_CACHED;
    flags &= ~(unsigned)SEEK_CACHED;

    bool set_backwards = flags & SEEK_SATAN;
    flags &= ~(unsigned)SEEK_SATAN;

    bool block = flags & SEEK_BLOCK;
    flags &= ~(unsigned)SEEK_BLOCK;

    struct demux_cached_range *cache_target =
        find_cache_seek_range(in, seek_pts, flags);

    if (!cache_target) {
        if (require_cache) {
            MP_VERBOSE(in, "Cached seek not possible.\n");
            return false;
        }
        if (!in->d_thread->seekable) {
            MP_WARN(in, "Cannot seek in this file.\n");
            return false;
        }
    }

    in->eof = false;
    in->reading = false;
    in->back_demuxing = set_backwards;

    clear_reader_state(in, clear_back_state);

    in->blocked = block;

    if (cache_target) {
        execute_cache_seek(in, cache_target, seek_pts, flags);
    } else {
        switch_to_fresh_cache_range(in);

        in->seeking = true;
        in->seek_flags = flags;
        in->seek_pts = seek_pts;
    }

    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;

        if (in->back_demuxing) {
            if (ds->back_seek_pos == MP_NOPTS_VALUE)
                ds->back_seek_pos = seek_pts;
            // Process possibly cached packets.
            back_demux_see_packets(in->streams[n]->ds);
        }

        wakeup_ds(ds);
    }

    if (!in->threading && in->seeking)
        execute_seek(in);

    return true;
}
```

> execute_seek

```C
static void execute_seek(struct demux_internal *in)
{
    int flags = in->seek_flags;
    double pts = in->seek_pts;
    in->eof = false;
    in->seeking = false;
    in->seeking_in_progress = pts;
    in->demux_ts = MP_NOPTS_VALUE;
    in->low_level_seeks += 1;
    in->after_seek = true;
    in->after_seek_to_start =
        !(flags & (SEEK_FORWARD | SEEK_FACTOR)) &&
        pts <= in->d_thread->start_time;

    for (int n = 0; n < in->num_streams; n++)
        in->streams[n]->ds->queue->last_pos_fixup = -1;

    if (in->recorder)
        mp_recorder_mark_discontinuity(in->recorder);

    mp_mutex_unlock(&in->lock);

    MP_VERBOSE(in, "execute seek (to %f flags %d)\n", pts, flags);

    if (in->d_thread->desc->seek)
        in->d_thread->desc->seek(in->d_thread, pts, flags);

    MP_VERBOSE(in, "seek done\n");

    mp_mutex_lock(&in->lock);

    in->seeking_in_progress = MP_NOPTS_VALUE;
}
```

> 将FFmpeg的seek函数注册到MPV中

```C
typedef struct demuxer_desc {
    const char *name;      // Demuxer name, used with -demuxer switch
    const char *desc;      // Displayed to user

    // If non-NULL, these are added to the global option list.
    const struct m_sub_options *options;

    // Return 0 on success, otherwise -1
    int (*open)(struct demuxer *demuxer, enum demux_check check);
    // The following functions are all optional
    // Try to read a packet. Return false on EOF. If true is returned, the
    // demuxer may set *pkt to a new packet (the reference goes to the caller).
    // If *pkt is NULL (the value when this function is called), the call
    // will be repeated.
    bool (*read_packet)(struct demuxer *demuxer, struct demux_packet **pkt);
    void (*drop_buffers)(struct demuxer *demuxer);
    void (*close)(struct demuxer *demuxer);
    void (*seek)(struct demuxer *demuxer, double rel_seek_secs, int flags);
    void (*switched_tracks)(struct demuxer *demuxer);
    // See timeline.c
    void (*load_timeline)(struct timeline *tl);
} demuxer_desc_t;
```
```C
const demuxer_desc_t demuxer_desc_lavf = {
    .name = "lavf",
    .desc = "libavformat",
    .read_packet = demux_lavf_read_packet,
    .open = demux_open_lavf,
    .drop_buffers = demux_drop_buffers_lavf,
    .close = demux_close_lavf,
    .seek = demux_seek_lavf,
    .switched_tracks = demux_lavf_switched_tracks,
};
```

> demux_seek_lavf

```C
static void demux_seek_lavf(demuxer_t *demuxer, double seek_pts, int flags)
{
    lavf_priv_t *priv = demuxer->priv;
    int avsflags = 0;
    int64_t seek_pts_av = 0;
    int seek_stream = -1;

    if (priv->any_ts_fixed)  {
        // helpful message to piss of users
        MP_WARN(demuxer, "Some timestamps returned by the demuxer were linearized. "
                         "A low level seek was requested; this won't work due to "
                         "restrictions in libavformat's API. You may have more "
                         "luck by enabling or enlarging the mpv cache.\n");
    }

    if (priv->linearize_ts < 0)
        priv->linearize_ts = 0;

    if (!(flags & SEEK_FORWARD))
        avsflags = AVSEEK_FLAG_BACKWARD;

    if (flags & SEEK_FACTOR) {
        struct stream *s = priv->stream;
        int64_t end = s ? stream_get_size(s) : -1;
        if (end > 0 && demuxer->ts_resets_possible &&
            !(priv->avif_flags & AVFMT_NO_BYTE_SEEK))
        {
            avsflags |= AVSEEK_FLAG_BYTE;
            seek_pts_av = end * seek_pts;
        } else if (priv->avfc->duration != 0 &&
                   priv->avfc->duration != AV_NOPTS_VALUE)
        {
            seek_pts_av = seek_pts * priv->avfc->duration;
        }
    } else {
        if (!(flags & SEEK_FORWARD))
            seek_pts -= priv->seek_delay;
        seek_pts_av = seek_pts * AV_TIME_BASE;
    }

    // Hack to make wav seeking "deterministic". Without this, features like
    // backward playback won't work.
    if (priv->pcm_seek_hack && !priv->pcm_seek_hack_packet_size) {
        // This might for example be the initial seek. Fuck it up like the
        // bullshit it is.
        AVPacket *pkt = av_packet_alloc();
        MP_HANDLE_OOM(pkt);
        if (av_read_frame(priv->avfc, pkt) >= 0)
            priv->pcm_seek_hack_packet_size = pkt->size;
        av_packet_free(&pkt);
        add_new_streams(demuxer);
    }
    if (priv->pcm_seek_hack && priv->pcm_seek_hack_packet_size &&
        !(avsflags & AVSEEK_FLAG_BYTE))
    {
        int samples = priv->pcm_seek_hack_packet_size /
                      priv->pcm_seek_hack->codecpar->block_align;
        if (samples > 0) {
            MP_VERBOSE(demuxer, "using bullshit libavformat PCM seek hack\n");
            double pts = seek_pts_av / (double)AV_TIME_BASE;
            seek_pts_av = pts / av_q2d(priv->pcm_seek_hack->time_base);
            int64_t align = seek_pts_av % samples;
            seek_pts_av -= align;
            seek_stream = priv->pcm_seek_hack->index;
        }
    }

    int r = av_seek_frame(priv->avfc, seek_stream, seek_pts_av, avsflags);
    if (r < 0 && (avsflags & AVSEEK_FLAG_BACKWARD)) {
        // When seeking before the beginning of the file, and seeking fails,
        // try again without the backwards flag to make it seek to the
        // beginning.
        avsflags &= ~AVSEEK_FLAG_BACKWARD;
        r = av_seek_frame(priv->avfc, seek_stream, seek_pts_av, avsflags);
    }

    if (r < 0) {
        char buf[180];
        av_strerror(r, buf, sizeof(buf));
        MP_VERBOSE(demuxer, "Seek failed (%s)\n", buf);
    }

    update_read_stats(demuxer);
}
```

> Seek flags: 

> AVSEEK_FLAG_BACKWARD 1

向后/向低时间戳方向寻找关键帧。这是最常用的标志，也是精准Seek的起点，确保你跳转到的位置不高于目标时间戳（即目标位置之前），这对于视频播放至关重要，因为解码总是需要一个起始的关键帧（I帧）。

> AVSEEK_FLAG_BYTE 2

基于字节位置进行定位。此时 timestamp 参数被解释为文件中的字节偏移量，而不是时间戳。需要注意的是，并非所有解复用器都支持这种方式。

> AVSEEK_FLAG_ANY 4

允许跳转到任意帧，不限于关键帧。这可以实现非常精准的定位，但可能会导致解码器从非关键帧开始解码，从而产生花屏（马赛克）或画面不完整等问题，需谨慎使用。

> AVSEEK_FLAG_FRAME 8

基于帧序号进行定位。此时 timestamp 参数被解释为特定流（由 stream_index 指定）中的帧索引。同样，支持程度取决于具体的解复用器。

## 