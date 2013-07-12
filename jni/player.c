#include <player.h>
#include <android/log.h>
#include <android/bitmap.h>
#define LOG_TAG "player"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)

JavaVM *g_jvm = NULL;
jclass mClass = NULL;
jobject mObject = NULL;
jmethodID refresh = NULL;
int registerCallBackRes = -1;

const int MSG_REFRESH = 1;
const int MSG_EXIT = 2;	
AVFrame *pFrameRGB;
uint8_t *buffer;
int frequency = 44100;

static void fill_bitmap(AndroidBitmapInfo*  info, void *pixels, AVFrame *pFrame)
{
    uint8_t *frameLine;

    int  yy;
    for (yy = 0; yy < info->height; yy++) {
        uint8_t*  line = (uint8_t*)pixels;
        frameLine = (uint8_t *)pFrame->data[0] + (yy * pFrame->linesize[0]);
        int xx;
        for (xx = 0; xx < info->width; xx++) {
            int out_offset = xx * 4;
            int in_offset = xx * 3;
            line[out_offset] = frameLine[in_offset];
            line[out_offset+1] = frameLine[in_offset+1];
            line[out_offset+2] = frameLine[in_offset+2];
            line[out_offset+3] = 0;
        }
        pixels = (char*)pixels + info->stride;
    }
}
static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}
/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        //SDL_CondSignal(is->continue_read_thread);
		pthread_cond_signal(&is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    if (is->paused) {
        is->frame_timer += av_gettime() / 1000000.0 + is->vidclk.pts_drift - is->vidclk.pts;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;
    //SDL_LockMutex(q->mutex);
	pthread_mutex_lock(&q->mutex);
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
           // SDL_CondWait(q->cond, q->mutex);
		   pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    //SDL_UnlockMutex(q->mutex);
	pthread_mutex_unlock(&q->mutex);
    return ret;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;

    pkt1 = av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    /* XXX: should duplicate packet data in DV case */
    //SDL_CondSignal(q->cond);
	pthread_cond_signal(&q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    /* duplicate the packet */
    if (pkt != &flush_pkt && av_dup_packet(pkt) < 0)
        return -1;

    //SDL_LockMutex(q->mutex);
	pthread_mutex_lock(&q->mutex);
    ret = packet_queue_put_private(q, pkt);
    //SDL_UnlockMutex(q->mutex);
	pthread_mutex_unlock(&q->mutex);

    if (pkt != &flush_pkt && ret < 0)
        av_free_packet(pkt);

    return ret;
}

/* packet queue handling */
static void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    //q->mutex = SDL_CreateMutex();
    //q->cond = SDL_CreateCond();
	pthread_mutex_init(&q->mutex, NULL);
	pthread_mutex_init(&q->cond, NULL);
    q->abort_request = 1;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

    //SDL_LockMutex(q->mutex);
	pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    //SDL_UnlockMutex(q->mutex);
	pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_abort(PacketQueue *q)
{
    //SDL_LockMutex(q->mutex);
	pthread_mutex_lock(&q->mutex);
    q->abort_request = 1;
    //SDL_CondSignal(q->cond);
	pthread_cond_signal(&q->cond);
    //SDL_UnlockMutex(q->mutex);
	pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    //SDL_LockMutex(q->mutex);
	pthread_mutex_lock(&q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    //SDL_UnlockMutex(q->mutex);
	pthread_mutex_unlock(&q->mutex);
}

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    avctx = ic->streams[stream_index]->codec;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        packet_queue_abort(&is->audioq);

        //SDL_CloseAudio();

        packet_queue_flush(&is->audioq);
        av_free_packet(&is->audio_pkt);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;
        av_frame_free(&is->frame);

        //if (is->rdft) {
        //    av_rdft_end(is->rdft);
        //    av_freep(&is->rdft_data);
        //    is->rdft = NULL;
        //    is->rdft_bits = 0;
       // }
#if CONFIG_AVFILTER
        //avfilter_graph_free(&is->agraph);
#endif
        break;
    case AVMEDIA_TYPE_VIDEO:
        packet_queue_abort(&is->videoq);

        /* note: we also signal this mutex to make sure we deblock the
           video thread in all cases */
        //SDL_LockMutex(is->pictq_mutex);
        //SDL_CondSignal(is->pictq_cond);
        //SDL_UnlockMutex(is->pictq_mutex);
		pthread_mutex_lock(&is->pictq_mutex);
		pthread_cond_signal(&is->pictq_cond);
		pthread_mutex_unlock(&is->pictq_mutex);
		
        //SDL_WaitThread(is->video_tid, NULL);
		pthread_cond_wait(&is->video_tid, NULL);
        packet_queue_flush(&is->videoq);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        packet_queue_abort(&is->subtitleq);

        /* note: we also signal this mutex to make sure we deblock the
           video thread in all cases */
        //SDL_LockMutex(is->subpq_mutex);
		pthread_mutex_lock(&is->subpq_mutex);
        is->subtitle_stream_changed = 1;

        // SDL_CondSignal(is->subpq_cond);
        // SDL_UnlockMutex(is->subpq_mutex);
		pthread_cond_signal(&is->subpq_cond);
		pthread_mutex_unlock(&is->subpq_mutex);
		
        //SDL_WaitThread(is->subtitle_tid, NULL);
		pthread_cond_wait(&is->subtitle_tid, NULL);
        packet_queue_flush(&is->subtitleq);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    avcodec_close(avctx);
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static int is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->filename, "rtp:", 4)
                 || !strncmp(s->filename, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, int64_t pos, int serial)
{
    VideoPicture *vp;
	//AVPicture *pict;
    /* wait until we have space to put a new picture */
    //SDL_LockMutex(is->pictq_mutex);
	pthread_mutex_lock(&is->pictq_mutex);
    while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE - 1 && !is->videoq.abort_request) {
        //SDL_CondWait(is->pictq_cond, is->pictq_mutex);
		LOGI("### picture queue is full");
		pthread_cond_wait(&is->pictq_cond, &is->pictq_mutex);
    }
    //SDL_UnlockMutex(is->pictq_mutex);
	pthread_mutex_unlock(&is->pictq_mutex);
    vp = &is->pictq[is->pictq_windex];
    vp->sar = src_frame->sample_aspect_ratio;
    sws_scale(is->img_convert_ctx, src_frame->data, src_frame->linesize, 0, src_frame->height, pFrameRGB->data, pFrameRGB->linesize);
	//LOGI("### 222 src_frame->width = %d, pict->data = %p, pict.linesize = %d", src_frame->width, pFrameRGB->width,pFrameRGB->linesize[0]);
	
	vp->pict = pFrameRGB;
	vp->pts = pts;
	vp->pos = pos;
	vp->serial = serial;
	if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
		is->pictq_windex = 0;
	//SDL_LockMutex(is->pictq_mutex);
	pthread_mutex_lock(&is->pictq_mutex);
	is->pictq_size++;
	//SDL_UnlockMutex(is->pictq_mutex);
	pthread_mutex_unlock(&is->pictq_mutex);
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame, AVPacket *pkt, int *serial)
{
    int got_picture;

    if (packet_queue_get(&is->videoq, pkt, 1, serial) < 0) {
		LOGE("### packet_queue_get aborted");
        return -1;
	}

    if (pkt->data == flush_pkt.data) {
        avcodec_flush_buffers(is->video_st->codec);
        //SDL_LockMutex(is->pictq_mutex);
		pthread_mutex_lock(&is->pictq_mutex);
        // Make sure there are no long delay timers (ideally we should just flush the queue but that's harder)
        while (is->pictq_size && !is->videoq.abort_request) {
            //SDL_CondWait(is->pictq_cond, is->pictq_mutex);
			LOGI("### get_video_frame pthread_cond_wait");
			pthread_cond_wait(&is->pictq_cond, &is->pictq_mutex);
        }
        is->video_current_pos = -1;
        is->frame_last_pts = AV_NOPTS_VALUE;
        is->frame_last_duration = 0;
        is->frame_timer = (double)av_gettime() / 1000000.0;
        is->frame_last_dropped_pts = AV_NOPTS_VALUE;
        //SDL_UnlockMutex(is->pictq_mutex);
		pthread_mutex_unlock(&is->pictq_mutex);
        return 0;
    }

    if(avcodec_decode_video2(is->video_st->codec, frame, &got_picture, pkt) < 0) {
		LOGE("### avcodec_decode_video2 fail !");
        return 0;
	}

    if (got_picture) {
		//LOGI("### got_picture  linesize = %d, width = %d", frame->linesize[0], frame->width);
        int ret = 1;
        double dpts = NAN;
        if (decoder_reorder_pts == -1) {
            frame->pts = av_frame_get_best_effort_timestamp(frame);
        } else if (decoder_reorder_pts) {
            frame->pts = frame->pkt_pts;
        } else {
            frame->pts = frame->pkt_dts;
        }
        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);
//LOGI("### dpts = %d, frame->sample_aspect_ratio  = %d", dpts, frame->sample_aspect_ratio);
        if (framedrop>0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            //SDL_LockMutex(is->pictq_mutex);
			//LOGI("### framedrop = %d",framedrop); exec
			pthread_mutex_lock(&is->pictq_mutex);
            if (is->frame_last_pts != AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE) {
                double clockdiff = get_clock(&is->vidclk) - get_master_clock(is);
                double ptsdiff = dpts - is->frame_last_pts;
                if (!isnan(clockdiff) && fabs(clockdiff) < AV_NOSYNC_THRESHOLD &&
                    !isnan(ptsdiff) && ptsdiff > 0 && ptsdiff < AV_NOSYNC_THRESHOLD &&
                    clockdiff + ptsdiff - is->frame_last_filter_delay < 0 &&
                    is->videoq.nb_packets) {
                    is->frame_last_dropped_pos = pkt->pos;
                    is->frame_last_dropped_pts = dpts;
                    is->frame_last_dropped_serial = *serial;
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    ret = 0;
					LOGI("### is->frame_last_dropped_pos = %d, is->frame_last_dropped_pts = %d, is->frame_last_dropped_serial = %d", is->frame_last_dropped_pos, is->frame_last_dropped_pts, is->frame_last_dropped_serial);
                }
				//LOGI("### clockdiff = %d, ptsdiff = %d",clockdiff, ptsdiff);
            }
            //SDL_UnlockMutex(is->pictq_mutex);
			pthread_mutex_unlock(&is->pictq_mutex);
        }
        return ret;
    }
    return 0;
}

static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = FFMIN(FFMAX(wanted_nb_samples, min_nb_samples), max_nb_samples);
                }
				LOGI("### diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is)
{
    AVPacket *pkt_temp = &is->audio_pkt_temp;
    AVPacket *pkt = &is->audio_pkt;
    AVCodecContext *dec = is->audio_st->codec;
    int len1, data_size, resampled_data_size;
    int64_t dec_channel_layout;
    int got_frame;
    av_unused double audio_clock0;
    int new_packet = 0;
    int flush_complete = 0;
    int wanted_nb_samples;
    AVRational tb;
    int ret;
    int reconfigure;
    for (;;) {
        /* NOTE: the audio packet can contain several frames */
        while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet) || is->audio_buf_frames_pending) {
            if (!is->frame) {
                if (!(is->frame = avcodec_alloc_frame()))
                    return AVERROR(ENOMEM);
            } else {
                av_frame_unref(is->frame);
                avcodec_get_frame_defaults(is->frame);
            }
            if (is->audioq.serial != is->audio_pkt_temp_serial)
                break;

            if (is->paused)
                return -1;

            if (!is->audio_buf_frames_pending) {
                if (flush_complete) {
                    break;
				}	
                new_packet = 0;
                len1 = avcodec_decode_audio4(dec, is->frame, &got_frame, pkt_temp);
                if (len1 < 0) {
                    /* if error, we skip the frame */
                    pkt_temp->size = 0;
                    break;
                }

                pkt_temp->data += len1;
                pkt_temp->size -= len1;

                if (!got_frame) {
                    /* stop sending empty packets if the decoder is finished */
                    if (!pkt_temp->data && dec->codec->capabilities & CODEC_CAP_DELAY)
                        flush_complete = 1;
                    continue;
                }

                tb = (AVRational){1, is->frame->sample_rate};
                if (is->frame->pts != AV_NOPTS_VALUE)
                    is->frame->pts = av_rescale_q(is->frame->pts, dec->time_base, tb);
                if (is->frame->pts == AV_NOPTS_VALUE && pkt_temp->pts != AV_NOPTS_VALUE)
                    is->frame->pts = av_rescale_q(pkt_temp->pts, is->audio_st->time_base, tb);
                if (pkt_temp->pts != AV_NOPTS_VALUE)
                    pkt_temp->pts += (double) is->frame->nb_samples / is->frame->sample_rate / av_q2d(is->audio_st->time_base);
            }
            data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(is->frame),
                                                   is->frame->nb_samples,
                                                   is->frame->format, 1);
            dec_channel_layout =
                (is->frame->channel_layout && av_frame_get_channels(is->frame) == av_get_channel_layout_nb_channels(is->frame->channel_layout)) ?
                is->frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(is->frame));
            wanted_nb_samples = synchronize_audio(is, is->frame->nb_samples);

            if (is->frame->format        != is->audio_src.fmt            ||
                dec_channel_layout       != is->audio_src.channel_layout ||
                is->frame->sample_rate   != is->audio_src.freq           ||
                (wanted_nb_samples       != is->frame->nb_samples && !is->swr_ctx)) {
                swr_free(&is->swr_ctx);
                is->swr_ctx = swr_alloc_set_opts(NULL,
                                                 is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                                 dec_channel_layout,           is->frame->format, is->frame->sample_rate,
                                                 0, NULL);
                if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
					LOGE("### Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                            is->frame->sample_rate, av_get_sample_fmt_name(is->frame->format), av_frame_get_channels(is->frame),
                            is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
                    break;
                }
                is->audio_src.channel_layout = dec_channel_layout;
                is->audio_src.channels       = av_frame_get_channels(is->frame);
                is->audio_src.freq = is->frame->sample_rate;
                is->audio_src.fmt = is->frame->format;
            }

            if (is->swr_ctx) {
                const uint8_t **in = (const uint8_t **)is->frame->extended_data;
                uint8_t **out = &is->audio_buf1;
                int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / is->frame->sample_rate + 256;
				//LOGI("### channels = %d, out_count = %d, fmt = %d",is->audio_tgt.channels, out_count, is->audio_tgt.fmt);
                int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
                int len2;
                if (out_size < 0) {
                    //av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
					LOGE("### av_samples_get_buffer_size() failed.");
                    break;
                }
                if (wanted_nb_samples != is->frame->nb_samples) {
                    if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - is->frame->nb_samples) * is->audio_tgt.freq / is->frame->sample_rate,
                                                wanted_nb_samples * is->audio_tgt.freq / is->frame->sample_rate) < 0) {
                        //av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
						LOGE("### swr_set_compensation() failed.");
                        break;
                    }
                }
                av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
                if (!is->audio_buf1)
                    return AVERROR(ENOMEM);
                len2 = swr_convert(is->swr_ctx, out, out_count, in, is->frame->nb_samples);
                if (len2 < 0) {
                    //av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
					LOGE("### swr_convert() failed.");
                    break;
                }
                if (len2 == out_count) {
                    //av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
					LOGE("### audio buffer is probably too small.");
                    swr_init(is->swr_ctx);
                }
                is->audio_buf = is->audio_buf1;
                resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
            } else {
                is->audio_buf = is->frame->data[0];
                resampled_data_size = data_size;
            }

            audio_clock0 = is->audio_clock;
            /* update the audio clock with the pts */
            if (is->frame->pts != AV_NOPTS_VALUE) {
                is->audio_clock = is->frame->pts * av_q2d(tb) + (double) is->frame->nb_samples / is->frame->sample_rate;
                is->audio_clock_serial = is->audio_pkt_temp_serial;
            }
#ifdef DEBUG
            {
                static double last_clock;
				LOGI("### audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
                       is->audio_clock - last_clock,
                       is->audio_clock, audio_clock0");
                last_clock = is->audio_clock;
            }
#endif
            return resampled_data_size;
        }

        /* free the current packet */
        if (pkt->data)
            av_free_packet(pkt);
        memset(pkt_temp, 0, sizeof(*pkt_temp));
        if (is->audioq.abort_request) {
            return -1;
        }

        if (is->audioq.nb_packets == 0) {
            //SDL_CondSignal(is->continue_read_thread);
			pthread_cond_signal(&is->continue_read_thread);
		}
        /* read next packet */
        if ((new_packet = packet_queue_get(&is->audioq, pkt, 1, &is->audio_pkt_temp_serial)) < 0) {
            return -1;
		}
        if (pkt->data == flush_pkt.data) {
            avcodec_flush_buffers(dec);
            flush_complete = 0;
            is->audio_buf_frames_pending = 0;
        }
        *pkt_temp = *pkt;
    }
}

void *video_thread(void *arg) {
	JNIEnv *env;
	if((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
		LOGE("### start video thead error!");
		return ((void *)-1);
	}
	AVPacket pkt = { 0 };
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    int ret;
    int serial = 0;
    for (;;) {
        while (is->paused && !is->videoq.abort_request) {
			usleep(10000);
			LOGI("video_thread --- is->paused = %d, is->videoq.abort_request = %d",is->paused, is->videoq.abort_request);
		}
        avcodec_get_frame_defaults(frame);
        av_free_packet(&pkt);
        ret = get_video_frame(is, frame, &pkt, &serial);
		//LOGI("get_video_frame ret =  %d",ret);
        if (ret < 0)
            goto the_end;
        if (!ret) {
            continue;
		}
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(is->video_st->time_base);
        ret = queue_picture(is, frame, pts, pkt.pos, serial);
        //av_frame_unref(frame);
        if (ret < 0)
            goto the_end;
    }
 the_end:
    avcodec_flush_buffers(is->video_st->codec);
    av_free_packet(&pkt);
    av_frame_free(&frame);
	LOGI("video_thread end.");
	if((*g_jvm)->DetachCurrentThread(g_jvm) != JNI_OK) {
		LOGE("### detach video thread error");
	}
	pthread_exit(0);
    return 0;
}

void *audio_thread(void *arg) {
	JNIEnv* env; 
	if((*g_jvm)->AttachCurrentThread(g_jvm, (void**)&env, NULL) != JNI_OK) 
	{
		LOGE("%s: AttachCurrentThread() failed", __FUNCTION__);
		return ((void *)-1);;
	}
	VideoState *is = (VideoState*)arg;
	int remain, audio_size;//remain 解码出的音频缓冲区剩余的数据长度
	int pcmBufferLen;//音频数据写入的缓冲区的长度
	jclass audio_track_cls = (*env)->FindClass(env,"android/media/AudioTrack");
	jmethodID min_buff_size_id = (*env)->GetStaticMethodID(
										 env,
										 audio_track_cls,
										"getMinBufferSize",
										"(III)I");
	int buffer_size = (*env)->CallStaticIntMethod(env,audio_track_cls,min_buff_size_id, 		frequency,
			    12,			/*CHANNEL_IN_STEREO*/
				2);         /*ENCODING_PCM_16BIT*/
	LOGI("buffer_size=%i",buffer_size);	
	pcmBufferLen = buffer_size;//AVCODEC_MAX_AUDIO_FRAME_SIZE * 3/2;
	jbyteArray buffer = (*env)->NewByteArray(env,pcmBufferLen);
	jmethodID constructor_id = (*env)->GetMethodID(env,audio_track_cls, "<init>",
			"(IIIIII)V");
	jobject audio_track = (*env)->NewObject(env,audio_track_cls,
			constructor_id,
			3, 			  /*AudioManager.STREAM_MUSIC*/
			frequency,        /*sampleRateInHz*/
			12,			  /*CHANNEL_IN_STEREO*/
			2,			  /*ENCODING_PCM_16BIT*/
			buffer_size*10,  /*bufferSizeInBytes*/
			1			  /*AudioTrack.MODE_STREAM*/
	);	
	//setvolume
	jmethodID setStereoVolume = (*env)->GetMethodID(env,audio_track_cls,"setStereoVolume","(FF)I");
	(*env)->CallIntMethod(env,audio_track,setStereoVolume,1.0,1.0);
	//play
    jmethodID method_play = (*env)->GetMethodID(env,audio_track_cls, "play",
			"()V");
    (*env)->CallVoidMethod(env,audio_track, method_play);
    //write
    jmethodID method_write = (*env)->GetMethodID(env,audio_track_cls,"write","([BII)I");
	//release
	jmethodID method_release = (*env)->GetMethodID(env,audio_track_cls,"release","()V");
	//double ref_clock, sync_threshold, diff;
	double pts;
	//LOGI("pcmBufferLen = %d, AVCODEC_MAX_AUDIO_FRAME_SIZE = %d", pcmBufferLen, AVCODEC_MAX_AUDIO_FRAME_SIZE);
	 int frame_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, 1, is->audio_tgt.fmt, 1);
	while(!is->paused) {
		if(is->audio_buf_index >= is->audio_buf_size) {//audio_buf中的数据已经转移完毕了
		    audio_size = audio_decode_frame(is);
			//LOGI("### audio size = %d, frame_size = %d", audio_size, frame_size);
		    if (audio_size <= 0) {
				is->audio_buf  = is->silence_buf;
                is->audio_buf_size = sizeof(is->silence_buf) / frame_size * frame_size;
		    }else {
				//if (is->show_mode != SHOW_MODE_VIDEO)
                 //  update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
				is->audio_buf_size = audio_size;
		    } 
		    //每次解码出音频之后，就把音频的索引audio_buf_index值0 从头开始索引
		    is->audio_buf_index = 0;	
		}
		//剩余的数据长度超过音频数据写入的缓冲区的长度
		remain = is->audio_buf_size - is->audio_buf_index;
		if(remain > pcmBufferLen) {
		  remain = pcmBufferLen;
		}
		(*env)->SetByteArrayRegion(env,buffer, 0, remain, (jbyte *)is->audio_buf);
		(*env)->CallIntMethod(env,audio_track,method_write,buffer,0,remain);
		is->audio_buf_index += remain;	
	}
	(*env)->CallVoidMethod(env,audio_track, method_release);
	//if(debug) LOGI("### decode audio thread exit.");
	if((*g_jvm)->DetachCurrentThread(g_jvm) != JNI_OK) {
		LOGE("### detach audio thread error");
	} 
	pthread_exit(0);
	((void *)0);
}


static int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = frequency;//spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  wanted_nb_channels;
    return 1024;//spec.size;
}

/* open a given stream. Return 0 if OK audio 1  video 0 */ 
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret;
    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return -1;
	}
    avctx = ic->streams[stream_index]->codec;

    codec = avcodec_find_decoder(avctx->codec_id);
    switch(avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO   : 
			is->last_audio_stream    = stream_index; 
			forced_codec_name =    audio_codec_name; 
			break;
        case AVMEDIA_TYPE_SUBTITLE: 
			is->last_subtitle_stream = stream_index; 
			forced_codec_name = subtitle_codec_name; 
			break;
        case AVMEDIA_TYPE_VIDEO   : 
			is->last_video_stream    = stream_index; 
			forced_codec_name =    video_codec_name;
			break;
    }
    if (forced_codec_name){
        codec = avcodec_find_decoder_by_name(forced_codec_name);
		LOGI("### forced_codec_name = %s", forced_codec_name);
	}
    if (!codec) {
        if (forced_codec_name) {
			//av_log(NULL, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forced_codec_name);
			LOGE("### No codec could be found with name %s", forced_codec_name);
		} else {
			//av_log(NULL, AV_LOG_WARNING,"No codec could be found with id %d\n", avctx->codec_id);
			LOGE("### No codec could be found with id %d\n", avctx->codec_id);
		}
        return -1;
    }
    avctx->codec_id = codec->id;
    avctx->workaround_bugs   = workaround_bugs;
    avctx->lowres            = lowres;
    if(avctx->lowres > codec->max_lowres){
        //av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
		LOGE("### The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
        avctx->lowres= codec->max_lowres;
    }
    avctx->idct_algo         = idct;
    avctx->error_concealment = error_concealment;
    if(avctx->lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
    if (fast)   avctx->flags2 |= CODEC_FLAG2_FAST;
    if(codec->capabilities & CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
	/*
    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (avctx->lowres)
        av_dict_set(&opts, "lowres", av_asprintf("%d", avctx->lowres), AV_DICT_DONT_STRDUP_VAL);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
       av_dict_set(&opts, "refcounted_frames", "1", 0);
	   LOGE("### refcounted_frames");
	}*/
    if (avcodec_open2(avctx, codec, NULL) < 0) {
		LOGE("### avcodec_open2 error !");
        return -1;
	}
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        //av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		LOGE("### Option %s not found.", t->key);
        return AVERROR_OPTION_NOT_FOUND;
    }
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
	
    case AVMEDIA_TYPE_AUDIO:
        sample_rate    = avctx->sample_rate;
        nb_channels    = avctx->channels;
        channel_layout = avctx->channel_layout;
//LOGE("### audio info sample_rate = %d, nb_channels = %d, channel_layout = %d", sample_rate,nb_channels,channel_layout);
        /* prepare audio output */
        if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0) {
			LOGE("### audio_open error! ret = %d", ret);
            return ret;
		}
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio fifo fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = 2.0 * is->audio_hw_buf_size / av_samples_get_buffer_size(NULL, is->audio_tgt.channels, is->audio_tgt.freq, is->audio_tgt.fmt, 1);
        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        memset(&is->audio_pkt_temp, 0, sizeof(is->audio_pkt_temp));

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        packet_queue_start(&is->audioq);
        //SDL_PauseAudio(0);
		frequency = sample_rate;
		pthread_t audio;
		pthread_create(&audio, NULL, &audio_thread, is);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];
		//LOGI("### 1111 avctx->width = %d, avctx->height = %d", avctx->width, avctx->height);
		is->img_convert_ctx = sws_getCachedContext(NULL,
		  avctx->width, avctx->height, avctx->pix_fmt,
		  avctx->width, avctx->height,
		  PIX_FMT_RGB24, SWS_BICUBIC,
		  NULL, NULL, NULL);
		pFrameRGB=avcodec_alloc_frame();		   
		int numBytes;
		numBytes=avpicture_get_size(PIX_FMT_RGB24, avctx->width, avctx->height);
		buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));
		avpicture_fill((AVPicture *)pFrameRGB, buffer, PIX_FMT_RGB24, avctx->width, avctx->height);
        packet_queue_start(&is->videoq);
        //is->video_tid = SDL_CreateThread(video_thread, is);
		pthread_t videoThread;
		is->video_tid = pthread_create(&videoThread, NULL, &video_thread, is);
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];
        packet_queue_start(&is->subtitleq);
        //is->subtitle_tid = SDL_CreateThread(subtitle_thread, is);
        break;
    default:
        break;
    }
    return 0;
}

void *read_thread(void *arg) {
	JNIEnv *env;
	if((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
		LOGE("### start decode thead error");
		return ((void *)-1);
	}
	struct timespec outtime;
	VideoState *is =  (VideoState*)arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int eof = 0;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    AVDictionary **opts;
    int orig_nb_streams;
	//SDL_mutex *wait_mutex = SDL_CreateMutex();
	pthread_mutex_t wait_mutex;
	pthread_mutex_init(&wait_mutex, NULL);
	
	memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    ic = avformat_alloc_context();
    //ic->interrupt_callback.callback = decode_interrupt_cb;
    //ic->interrupt_callback.opaque = is;
    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
	if (err < 0) {
        //print_error(is->filename, err);
		LOGE("### avformat_open_input error code = %d", err);
        ret = -1;
        goto fail;
    }
	if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        //av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		LOGE("### av_dict_get Option %s  not found.", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    is->ic = ic;
	if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;
    //opts = setup_find_stream_info_opts(ic, codec_opts);
    orig_nb_streams = ic->nb_streams;

    err = avformat_find_stream_info(ic, opts);
    if (err < 0) {
        //av_log(NULL, AV_LOG_WARNING,"%s: could not find codec parameters\n", is->filename);
		LOGE("### %s: could not find codec parameters.", is->filename);
        ret = -1;
        goto fail;
    }
    //for (i = 0; i < orig_nb_streams; i++) {
	    //av_dict_free(&opts[i]);
		//LOGI("### &opts['%d'] = %d", i, opts[i]);
	//}
    //av_freep(&opts);
    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use url_feof() to test for the end
    if (seek_by_bytes < 0)
        seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /// add the stream start time 
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            //av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n", is->filename, (double)timestamp / AV_TIME_BASE);
			LOGE("### %s: could not seek to position %0.3f\n", is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }
	
	is->realtime = is_realtime(ic);
    for (i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = AVDISCARD_ALL;
    if (!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                wanted_stream[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                wanted_stream[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                wanted_stream[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);
    if (show_status) {
        av_dump_format(ic, 0, is->filename, 0);
    }
    is->show_mode = show_mode;
    // open the streams 
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }
    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }
	LOGE("### is->video_stream = %d, is->audio_stream =%d\n", is->video_stream, is->audio_stream );
    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "%s: could not open codecs\n", is->filename);
		LOGE("### %s: could not open codecs\n", is->filename);
        ret = -1;
        goto fail;
    }
    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;
    for (;;) {
	    if (is->abort_request)
            break;
		if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            //SDL_Delay(10);
			usleep(10000);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                //av_log(NULL, AV_LOG_ERROR,"%s: error while seeking\n", is->ic->filename);
				LOGE("### error while seeking\n", is->ic->filename);
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->extclk, NAN, 0);
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }
		if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy;
                if ((ret = av_copy_packet(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
            }
            is->queue_attachments_req = 0;
        }
        /* if the queue are full, no need to read more */
        if (infinite_buffer<1 &&
				(is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
				|| 
				(
					(is->audioq.nb_packets > MIN_FRAMES
					|| is->audio_stream < 0
					|| is->audioq.abort_request)
					&& (is->videoq.nb_packets > MIN_FRAMES || is->video_stream < 0 || is->videoq.abort_request
                    || (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))
					//&& (is->subtitleq.nb_packets > MIN_FRAMES || is->subtitle_stream < 0 || is->subtitleq.abort_request)
				)
			)
		) {
			LOGI("### is->audioq.size = %d, is->videoq.size = %d, is->subtitleq.size = %d, is->audioq.nb_packets =%d", is->audioq.size, is->videoq.size , is->subtitleq.size, is->audioq.nb_packets );
			LOGI("### aa is->audioq.nb_packets = %d, is->audio_stream = %d, is->audioq.abort_request = %d ", is->audioq.nb_packets, is->audio_stream, is->audioq.abort_request);
			LOGI("### bb is->videoq.nb_packets = %d, is->video_stream = %d, is->videoq.abort_request = %d ", is->videoq.nb_packets, is->video_stream, is->videoq.abort_request);
			
			//LOGI("### is->subtitleq.nb_packets = %d, is->subtitle_stream = %d, is->subtitleq.abort_request = %d ", is->subtitleq.nb_packets, is->subtitle_stream, is->subtitleq.abort_request);
            /* wait 10 ms */
            //SDL_LockMutex(wait_mutex);
			pthread_mutex_lock(&wait_mutex);
			outtime.tv_sec=time(NULL)+1;
			outtime.tv_nsec=0;
            //SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
			pthread_cond_timedwait(&is->continue_read_thread, &wait_mutex, &outtime);
            //SDL_UnlockMutex(wait_mutex);
			pthread_mutex_unlock(&wait_mutex);
			//LOGE("### 999 is->videoq.size = %d",is->videoq.size);	
			usleep(10000);
            continue;
        }
	
		if (eof) {
            if (is->video_stream >= 0) {
                av_init_packet(pkt);
                pkt->data = NULL;
                pkt->size = 0;
                pkt->stream_index = is->video_stream;
                packet_queue_put(&is->videoq, pkt);
            }
            if (is->audio_stream >= 0 &&
                is->audio_st->codec->codec->capabilities & CODEC_CAP_DELAY) {
                av_init_packet(pkt);
                pkt->data = NULL;
                pkt->size = 0;
                pkt->stream_index = is->audio_stream;
                packet_queue_put(&is->audioq, pkt);
            }
            //SDL_Delay(10);
			usleep(10000);
            if (is->audioq.size + is->videoq.size + is->subtitleq.size == 0) {
                if (loop != 1 && (!loop || --loop)) {
                    stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
                } else if (autoexit) {
                    ret = AVERROR_EOF;
                    goto fail;
                }
            }
            eof=0;
            continue;
        }
		ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || url_feof(ic->pb)) {
                eof = 1;
			}
            if (ic->pb && ic->pb->error)
                break;
            //SDL_LockMutex(wait_mutex);
			pthread_mutex_lock(&wait_mutex);
            //SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
			outtime.tv_sec=time(NULL)+1;
			outtime.tv_nsec=0;
			pthread_cond_timedwait(&is->continue_read_thread, &wait_mutex, &outtime);
            //SDL_UnlockMutex(wait_mutex);
			pthread_mutex_unlock(&wait_mutex);
            continue;
        }
		/* check if packet is in play range specified by user, then queue, otherwise discard */
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                (pkt->pts - ic->streams[pkt->stream_index]->start_time) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double)duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            //packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_free_packet(pkt);
        }
	}
	/* wait until the end */
    while (!is->abort_request) {
        //SDL_Delay(100);
		LOGE("### 100");	
		usleep(100);
    }
	LOGE("### 1001");	
    ret = 0;	
	fail:
    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);
    if (is->ic) {
        avformat_close_input(&is->ic);
    }

    if (ret != 0) {
       // SDL_Event event;
        //event.type = FF_QUIT_EVENT;
        //event.user.data1 = is;
        //SDL_PushEvent(&event);
    }
    //SDL_DestroyMutex(wait_mutex);
	pthread_mutex_destroy(&wait_mutex);
	if((*g_jvm)->DetachCurrentThread(g_jvm) != JNI_OK) {
		LOGE("### detach decode thread error");
	}
	pthread_exit(0);
    return ((void *)0);
}

static VideoState *stream_open(const char *filename, AVInputFormat *iformat)
{
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    av_strlcpy(is->filename, filename, sizeof(is->filename));
    is->iformat = iformat;
    /* start video display */
    //is->pictq_mutex = SDL_CreateMutex();
    //is->pictq_cond  = SDL_CreateCond();
	pthread_mutex_init(&is->pictq_mutex, NULL);
	pthread_mutex_init(&is->pictq_cond, NULL);

   // is->subpq_mutex = SDL_CreateMutex();
    //is->subpq_cond  = SDL_CreateCond();
	pthread_mutex_init(&is->subpq_mutex, NULL);
	pthread_mutex_init(&is->subpq_cond, NULL);
	
    packet_queue_init(&is->videoq);
    packet_queue_init(&is->audioq);
    packet_queue_init(&is->subtitleq);

    //is->continue_read_thread = SDL_CreateCond();
	pthread_mutex_init(&is->continue_read_thread, NULL);
	
    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    is->audio_last_serial = -1;
    is->av_sync_type = av_sync_type;
    // is->read_tid     = SDL_CreateThread(read_thread, is);
    pthread_t readThread;
	is->read_tid = pthread_create(&readThread, NULL, &read_thread, is);
	//LOGI("### create readThread is->read_tid  = %d",is->read_tid);
    if (is->read_tid != 0) {
        av_free(is);
		LOGI("### create readThread error!");
        return NULL;
    }
	LOGI("### create readThread success!");
    return is;
}


jint openVideoFile(JNIEnv *env, jclass clazz,jstring name){
	(*env)->GetJavaVM(env, &g_jvm);
    avcodec_register_all();
#if CONFIG_AVDEVICE
    //avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    //avfilter_register_all();
#endif
    av_register_all();
    avformat_network_init();
	av_init_packet(&flush_pkt);
	flush_pkt.data = (uint8_t *)&flush_pkt;
	input_filename =(const char *) (*env)->GetStringUTFChars(env, name, NULL);
	LOGI("input filename = %s",input_filename);
	is = stream_open(input_filename, file_iformat);
	if (!is) {
        //av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
		LOGI("### Failed to initialize VideoState!");
		return -1;
    }
	return 0;
}


int display(JNIEnv * env, jobject this, jstring bitmap){
	AndroidBitmapInfo  info;
	void*              pixels;
	int ret;
    if ((ret = AndroidBitmap_getInfo(env, bitmap, &info)) < 0) {
        LOGE("AndroidBitmap_getInfo() failed ! error=%d", ret);
        return -1;//bitmap_getinfo_error;
	}
	VideoPicture *vp;
	double actual_delay, delay, sync_threshold, ref_clock, diff;
	while(!is->abort_request) {// && is->video_st
		if ((ret = AndroidBitmap_lockPixels(env, bitmap, &pixels)) < 0) {
				LOGE("AndroidBitmap_lockPixels() failed ! error=%d", ret);
		}
		if(is->pictq_size == 0) {
			usleep(5000);
			//LOGI("no image, wait.");
		} else {
			// 取出图像
			vp = &is->pictq[is->pictq_rindex];
			//LOGI("### 333 vp->pict->width = %d", vp->pict->linesize[0]);
			/*is->video_current_pts = vp->pts;
			is->video_current_pts_time = av_gettime();
			delay = vp->pts - is->frame_last_pts;
			LOGE(1, "is->video_current_pts = %d, delay = %d",is->video_current_pts,delay);
			if (delay <= 0 || delay >= 1.0) {
				delay = is->frame_last_delay;
			}
			is->frame_last_delay = delay;
			is->frame_last_pts = vp->pts;
			is->frame_timer += delay;
			actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
			if(is->av_sync_type != AV_SYNC_VIDEO_MASTER) {
				ref_clock = get_master_clock(is);
				diff = vp->pts - ref_clock;
				sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay :	AV_SYNC_THRESHOLD;
				if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
					if(diff <= -sync_threshold) {
						delay = 0;
					} else if(diff >= sync_threshold) {
						delay = 2 * delay;
					}
				}
			}
			if (actual_delay < 0.010) {
			  actual_delay = 0.010;
			}*/
			//LOGE(10, "### refresh delay =  %d",(int)(actual_delay * 1000 + 0.5));
			//usleep(10000*(int)(actual_delay * 1000 + 0.5));
			fill_bitmap(&info, pixels, vp->pict);
			AndroidBitmap_unlockPixels(env, bitmap);
			if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
				is->pictq_rindex = 0;
			}
			pthread_mutex_lock(&is->pictq_mutex);
			is->pictq_size--;
			pthread_cond_signal(&is->pictq_cond);
			pthread_mutex_unlock(&is->pictq_mutex);
			if(mClass == NULL || mObject == NULL || refresh == NULL) {
				registerCallBackRes = registerCallBack(env);
				LOGI("registerCallBack == %d", registerCallBackRes);	
				if(registerCallBackRes != 0) {
					//is->quit = 0;				
					continue;
				}
			}
			(*env)->CallVoidMethod(env, mObject, refresh, MSG_REFRESH);
			
		}
	}
	return 0;
}

int registerCallBack(JNIEnv *env) {
	if(mClass == NULL) {
		mClass = (*env)->FindClass(env, "com/sky/drovik/player/media/MovieView");
		if(mClass == NULL){
			return -1;
		}
		LOGI("register local class OK.");
	}
	if (mObject == NULL) {
		if (GetProviderInstance(env, mClass) != 1) {
			//(*env)->DeleteLocalRef(env, mClass);
			return -1;
		}
		LOGI("register local object OK.");
	}
	if(refresh == NULL) {
		refresh = (*env)->GetMethodID(env, mClass, "callBackRefresh","(I)V");
		if(refresh == NULL) {
			//(*env)->DeleteLocalRef(env, mClass);
			//(*env)->DeleteLocalRef(env, mObject);
			return -3;
		}
	}
	return 0;
}

int GetProviderInstance(JNIEnv *env,jclass obj_class) {
	jmethodID construction_id = (*env)->GetMethodID(env, obj_class,	"<init>", "()V");
	if (construction_id == 0) {
		return -1;
	}
	mObject = (*env)->NewObject(env, obj_class, construction_id);
	if (mObject == NULL) {
		return -2;
	}
	return 1;
}