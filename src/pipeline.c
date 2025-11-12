#include "pipeline.h"

#include "logging.h"
#include "h265_depay.h"
#include "h265_parse.h"

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstutils.h>

#include <string.h>

#define CHECK_ELEM(elem, name)                                                                      \
    do {                                                                                            \
        if ((elem) == NULL) {                                                                       \
            LOGE("Failed to create GStreamer element '%s'", (name));                               \
            goto fail;                                                                              \
        }                                                                                           \
    } while (0)

static void cleanup_pipeline(PipelineState *ps);

static void ensure_gst_initialized(void) {
    static gsize once_init = 0;
    if (g_once_init_enter(&once_init)) {
        gst_init(NULL, NULL);
        g_once_init_leave(&once_init, 1);
    }
}

static GstElement *create_udp_app_source(const AppCfg *cfg, UdpReceiver **receiver_out) {
    if (cfg == NULL || receiver_out == NULL) {
        return NULL;
    }

    GstElement *appsrc_elem = gst_element_factory_make("appsrc", "udp_appsrc");
    if (appsrc_elem == NULL) {
        LOGE("Failed to create GStreamer element 'appsrc'");
        return NULL;
    }

    GstCaps *caps = gst_caps_new_simple("application/x-rtp",
                                        "media", G_TYPE_STRING, "video",
                                        "encoding-name", G_TYPE_STRING, "H265",
                                        "payload", G_TYPE_INT, cfg->vid_pt,
                                        "clock-rate", G_TYPE_INT, 90000,
                                        NULL);
    if (caps == NULL) {
        LOGE("Failed to allocate RTP caps for appsrc");
        gst_object_unref(appsrc_elem);
        return NULL;
    }

    g_object_set(appsrc_elem,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "stream-type", GST_APP_STREAM_TYPE_STREAM,
                 "do-timestamp", TRUE,
                 "max-bytes", (guint64)(4 * 1024 * 1024),
                 NULL);

    GstAppSrc *appsrc = GST_APP_SRC(appsrc_elem);
    gst_app_src_set_caps(appsrc, caps);
    gst_caps_unref(caps);

    UdpReceiver *receiver = udp_receiver_create(cfg->udp_port, cfg->vid_pt, appsrc);
    if (receiver == NULL) {
        LOGE("Failed to create UDP receiver");
        gst_object_unref(appsrc_elem);
        return NULL;
    }

    *receiver_out = receiver;
    return appsrc_elem;
}

static gpointer appsink_thread_func(gpointer data) {
    PipelineState *ps = (PipelineState *)data;
    GstAppSink *appsink = ps->appsink != NULL ? GST_APP_SINK(ps->appsink) : NULL;
    if (appsink == NULL) {
        g_mutex_lock(&ps->lock);
        ps->appsink_thread_running = FALSE;
        g_mutex_unlock(&ps->lock);
        return NULL;
    }

    g_mutex_lock(&ps->lock);
    ps->appsink_thread_running = TRUE;
    g_mutex_unlock(&ps->lock);

    size_t max_packet = video_decoder_max_packet_size(ps->decoder_initialized ? ps->decoder : NULL);
    if (max_packet == 0) {
        max_packet = 1024 * 1024;
    }

    while (TRUE) {
        g_mutex_lock(&ps->lock);
        gboolean stop = ps->stop_requested;
        gboolean decoder_running = ps->decoder_running;
        g_mutex_unlock(&ps->lock);

        if (stop || !decoder_running) {
            break;
        }

        GstSample *sample = gst_app_sink_try_pull_sample(appsink, 100 * GST_MSECOND);
        if (sample == NULL) {
            continue;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstClockTime pts = GST_CLOCK_TIME_NONE;
        if (buffer != NULL) {
            pts = GST_BUFFER_PTS(buffer);
            if (!GST_CLOCK_TIME_IS_VALID(pts)) {
                pts = GST_BUFFER_DTS(buffer);
            }
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                if (map.size > 0 && map.size <= max_packet) {
                    gboolean corrupted = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED);
                    gboolean discont = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT);
                    if (discont) {
                        corrupted = TRUE;
                    }
                    g_mutex_lock(&ps->recorder_lock);
                    VideoRecorder *recorder = ps->recorder;
                    if (recorder != NULL) {
                        video_recorder_handle_sample(recorder, sample, buffer, map.data, map.size);
                    }
                    g_mutex_unlock(&ps->recorder_lock);

                    if (video_decoder_feed(ps->decoder, map.data, map.size, pts, corrupted, discont) != 0) {
                        LOGV("Video decoder feed busy; retrying");
                    }
                }
                gst_buffer_unmap(buffer, &map);
            }
        }

        gst_sample_unref(sample);
    }

    if (ps->decoder != NULL) {
        video_decoder_send_eos(ps->decoder);
    }

    g_mutex_lock(&ps->lock);
    ps->appsink_thread_running = FALSE;
    g_mutex_unlock(&ps->lock);
    return NULL;
}

static gpointer bus_thread_func(gpointer data) {
    PipelineState *ps = (PipelineState *)data;
    GstBus *bus = gst_element_get_bus(ps->pipeline);
    if (bus == NULL) {
        LOGE("Pipeline bus unavailable");
        g_mutex_lock(&ps->lock);
        ps->encountered_error = TRUE;
        ps->bus_thread_running = FALSE;
        g_cond_signal(&ps->cond);
        g_mutex_unlock(&ps->lock);
        return NULL;
    }

    g_mutex_lock(&ps->lock);
    ps->bus_thread_running = TRUE;
    g_mutex_unlock(&ps->lock);

    gboolean running = TRUE;
    while (running) {
        GstMessage *msg = gst_bus_timed_pop(bus, GST_MSECOND * 100);
        if (msg == NULL) {
            g_mutex_lock(&ps->lock);
            running = ps->stop_requested ? FALSE : TRUE;
            g_mutex_unlock(&ps->lock);
            continue;
        }

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            LOGE("Pipeline error: %s", err != NULL ? err->message : "unknown");
            if (dbg != NULL) {
                LOGV("Pipeline debug info: %s", dbg);
            }
            g_clear_error(&err);
            g_free(dbg);
            g_mutex_lock(&ps->lock);
            ps->encountered_error = TRUE;
            ps->stop_requested = TRUE;
            g_mutex_unlock(&ps->lock);
            running = FALSE;
            break;
        }
        case GST_MESSAGE_EOS:
            LOGI("Pipeline received EOS");
            g_mutex_lock(&ps->lock);
            ps->stop_requested = TRUE;
            g_mutex_unlock(&ps->lock);
            running = FALSE;
            break;
        default:
            break;
        }

        gst_message_unref(msg);
    }

    gst_object_unref(bus);
    g_mutex_lock(&ps->lock);
    ps->bus_thread_running = FALSE;
    g_cond_signal(&ps->cond);
    g_mutex_unlock(&ps->lock);
    return NULL;
}

int pipeline_start(const AppCfg *cfg, const ModesetResult *ms, int drm_fd, PipelineState *ps) {
    if (cfg == NULL || ms == NULL || ps == NULL) {
        return -1;
    }

    if (ps->state != PIPELINE_STOPPED) {
        LOGW("pipeline_start: refused (state=%d)", ps->state);
        return -1;
    }

    ensure_gst_initialized();

    if (!ps->initialized) {
        g_mutex_init(&ps->lock);
        g_mutex_init(&ps->recorder_lock);
        g_cond_init(&ps->cond);
        ps->initialized = TRUE;
    }

    ps->cfg = cfg;
    ps->pipeline = NULL;
    ps->appsink = NULL;
    ps->udp_receiver = NULL;
    ps->bus_thread = NULL;
    ps->appsink_thread = NULL;
    ps->bus_thread_running = FALSE;
    ps->appsink_thread_running = FALSE;
    ps->stop_requested = FALSE;
    ps->encountered_error = FALSE;
    GstElement *pipeline = gst_pipeline_new("pixelpilot_stripped_rk");
    CHECK_ELEM(pipeline, "pipeline");

    UdpReceiver *receiver = NULL;
    GstElement *appsrc = create_udp_app_source(cfg, &receiver);
    if (appsrc == NULL) {
        goto fail;
    }

    if (!sstar_h265_depay_register()) {
        LOGE("Failed to register sstarh265depay element");
        goto fail;
    }

    if (!sstar_h265_parse_register()) {
        LOGE("Failed to register sstarh265parse element");
        goto fail;
    }

    GstElement *queue = gst_element_factory_make("queue", "udp_queue");
    GstElement *depay = gst_element_factory_make("sstarh265depay", "video_depay");
    GstElement *parser = gst_element_factory_make("sstarh265parse", "video_parser");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "video_capsfilter");
    GstElement *appsink = gst_element_factory_make("appsink", "video_sink");

    CHECK_ELEM(queue, "queue");
    CHECK_ELEM(depay, "sstarh265depay");
    CHECK_ELEM(parser, "sstarh265parse");
    CHECK_ELEM(capsfilter, "capsfilter");
    CHECK_ELEM(appsink, "appsink");

    if (cfg->vid_pt >= -1 && cfg->vid_pt <= 127) {
        g_object_set(depay, "payload-type", cfg->vid_pt, NULL);
    }
    g_object_set(depay, "emit-partial-au", TRUE, NULL);

    guint max_buffers = (cfg->appsink_max_buffers > 0) ? (guint)cfg->appsink_max_buffers : 4u;
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink), max_buffers);
    gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);
    g_object_set(appsink, "sync", FALSE, NULL);

    GstCaps *raw_caps = gst_caps_new_simple("video/x-h265",
                                            "stream-format", G_TYPE_STRING, "byte-stream",
                                            "alignment", G_TYPE_STRING, "au",
                                            NULL);
    if (raw_caps == NULL) {
        LOGE("Failed to allocate caps for byte-stream enforcement");
        goto fail;
    }
    g_object_set(capsfilter, "caps", raw_caps, NULL);
    gst_app_sink_set_caps(GST_APP_SINK(appsink), raw_caps);
    gst_caps_unref(raw_caps);

    g_object_set(queue,
                 "leaky", 2,
                 "max-size-time", (guint64)0,
                 "max-size-bytes", (guint64)0,
                 "max-size-buffers", 16,
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline), appsrc, queue, depay, parser, capsfilter, appsink, NULL);
    if (!gst_element_link_many(appsrc, queue, depay, parser, capsfilter, appsink, NULL)) {
        LOGE("Failed to link UDP pipeline");
        goto fail;
    }

    ps->pipeline = pipeline;
    ps->appsink = appsink;
    ps->udp_receiver = receiver;

    if (udp_receiver_start(ps->udp_receiver) != 0) {
        LOGE("Failed to start UDP receiver");
        goto fail;
    }

    GstStateChangeReturn ret = gst_element_set_state(ps->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOGE("Failed to set pipeline to PLAYING");
        goto fail;
    }
    if (ret == GST_STATE_CHANGE_ASYNC) {
        GstState state = GST_STATE_NULL;
        GstState pending = GST_STATE_NULL;
        ret = gst_element_get_state(ps->pipeline, &state, &pending, GST_SECOND);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            LOGE("Pipeline state change failed during async wait");
            goto fail;
        }
    }

    if (ps->decoder == NULL) {
        ps->decoder = video_decoder_new();
        if (ps->decoder == NULL) {
            LOGE("Failed to allocate video decoder");
            goto fail;
        }
    }

    if (video_decoder_init(ps->decoder, cfg, ms, drm_fd) != 0) {
        LOGE("Failed to initialise video decoder");
        goto fail;
    }
    ps->decoder_initialized = TRUE;

    if (video_decoder_start(ps->decoder) != 0) {
        LOGE("Failed to start video decoder");
        goto fail;
    }
    ps->decoder_running = TRUE;

    ps->appsink_thread = g_thread_new("appsink-thread", appsink_thread_func, ps);
    if (ps->appsink_thread == NULL) {
        LOGE("Failed to create appsink thread");
        goto fail;
    }

    ps->bus_thread = g_thread_new("pipeline-bus", bus_thread_func, ps);
    if (ps->bus_thread == NULL) {
        LOGE("Failed to create pipeline bus thread");
        goto fail;
    }

    ps->state = PIPELINE_RUNNING;
    return 0;

fail:
    cleanup_pipeline(ps);
    return -1;
}

static void stop_appsink_thread(PipelineState *ps) {
    if (ps->appsink_thread != NULL) {
        g_thread_join(ps->appsink_thread);
        ps->appsink_thread = NULL;
    }
}

static void stop_bus_thread(PipelineState *ps, int wait_ms_total) {
    if (ps->bus_thread == NULL) {
        return;
    }

    gint64 deadline = g_get_monotonic_time() + (gint64)wait_ms_total * G_TIME_SPAN_MILLISECOND;
    g_mutex_lock(&ps->lock);
    while (ps->bus_thread_running) {
        if (!g_cond_wait_until(&ps->cond, &ps->lock, deadline)) {
            break;
        }
    }
    g_mutex_unlock(&ps->lock);

    g_thread_join(ps->bus_thread);
    ps->bus_thread = NULL;
}

void pipeline_stop(PipelineState *ps, int wait_ms_total) {
    if (ps == NULL) {
        return;
    }

    g_mutex_lock(&ps->lock);
    if (ps->state == PIPELINE_STOPPED) {
        g_mutex_unlock(&ps->lock);
        return;
    }
    ps->state = PIPELINE_STOPPING;
    ps->stop_requested = TRUE;
    g_mutex_unlock(&ps->lock);

    if (ps->pipeline != NULL) {
        gst_element_send_event(ps->pipeline, gst_event_new_eos());
        gst_element_set_state(ps->pipeline, GST_STATE_NULL);
    }

    if (ps->udp_receiver != NULL) {
        udp_receiver_stop(ps->udp_receiver);
    }

    stop_appsink_thread(ps);
    stop_bus_thread(ps, wait_ms_total);

    cleanup_pipeline(ps);
    ps->state = PIPELINE_STOPPED;
}

static void cleanup_pipeline(PipelineState *ps) {
    if (ps == NULL) {
        return;
    }

    if (ps->appsink_thread != NULL) {
        g_thread_join(ps->appsink_thread);
        ps->appsink_thread = NULL;
    }
    if (ps->bus_thread != NULL) {
        g_thread_join(ps->bus_thread);
        ps->bus_thread = NULL;
    }

    if (ps->udp_receiver != NULL) {
        udp_receiver_destroy(ps->udp_receiver);
        ps->udp_receiver = NULL;
    }

    if (ps->pipeline != NULL) {
        gst_element_set_state(ps->pipeline, GST_STATE_NULL);
        gst_object_unref(ps->pipeline);
        ps->pipeline = NULL;
    }

    ps->appsink = NULL;

    if (ps->decoder_running) {
        video_decoder_stop(ps->decoder);
        ps->decoder_running = FALSE;
    }
    if (ps->decoder_initialized) {
        video_decoder_deinit(ps->decoder);
        ps->decoder_initialized = FALSE;
    }
    if (ps->decoder != NULL) {
        video_decoder_free(ps->decoder);
        ps->decoder = NULL;
    }

    g_mutex_lock(&ps->recorder_lock);
    VideoRecorder *rec = ps->recorder;
    ps->recorder = NULL;
    g_mutex_unlock(&ps->recorder_lock);
    if (rec != NULL) {
        video_recorder_free(rec);
    }
}

void pipeline_poll_child(PipelineState *ps) {
    if (ps == NULL) {
        return;
    }
    if (ps->bus_thread == NULL) {
        return;
    }

    g_mutex_lock(&ps->lock);
    gboolean running = ps->bus_thread_running;
    gboolean had_error = ps->encountered_error;
    g_mutex_unlock(&ps->lock);

    if (!running) {
        g_thread_join(ps->bus_thread);
        ps->bus_thread = NULL;
        cleanup_pipeline(ps);
        ps->state = PIPELINE_STOPPED;
        if (had_error) {
            LOGI("Pipeline exited due to error");
        } else {
            LOGI("Pipeline exited cleanly");
        }
    }
}

int pipeline_enable_recording(PipelineState *ps, const RecordCfg *cfg) {
    if (ps == NULL || cfg == NULL) {
        return -1;
    }
    if (cfg->output_path[0] == '\0') {
        return -1;
    }

    RecordCfg local_cfg = *cfg;
    local_cfg.enable = 1;

    VideoRecorder *rec = video_recorder_new(&local_cfg);
    if (rec == NULL) {
        return -1;
    }

    g_mutex_lock(&ps->recorder_lock);
    if (ps->recorder != NULL) {
        g_mutex_unlock(&ps->recorder_lock);
        video_recorder_free(rec);
        return 0;
    }
    ps->recorder = rec;
    g_mutex_unlock(&ps->recorder_lock);
    return 0;
}

void pipeline_disable_recording(PipelineState *ps) {
    if (ps == NULL) {
        return;
    }

    g_mutex_lock(&ps->recorder_lock);
    VideoRecorder *rec = ps->recorder;
    ps->recorder = NULL;
    g_mutex_unlock(&ps->recorder_lock);

    if (rec != NULL) {
        video_recorder_free(rec);
    }
}

int pipeline_get_recording_stats(const PipelineState *ps, PipelineRecordingStats *stats) {
    if (ps == NULL || stats == NULL) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));

    g_mutex_lock((GMutex *)&ps->recorder_lock);
    VideoRecorder *rec = ps->recorder;
    if (rec != NULL) {
        VideoRecorderStats vr_stats;
        video_recorder_get_stats(rec, &vr_stats);
        stats->active = vr_stats.active ? TRUE : FALSE;
        stats->bytes_written = vr_stats.bytes_written;
        stats->elapsed_ns = vr_stats.elapsed_ns;
        stats->media_duration_ns = vr_stats.media_duration_ns;
        g_strlcpy(stats->output_path, vr_stats.output_path, sizeof(stats->output_path));
    }
    g_mutex_unlock((GMutex *)&ps->recorder_lock);
    return 0;
}
