#ifndef PIPELINE_H
#define PIPELINE_H

#include <glib.h>
#include <gst/gst.h>

#include "config.h"
#include "drm_modeset.h"
#include "udp_receiver.h"
#include "video_decoder.h"
#include "video_recorder.h"

typedef enum {
    PIPELINE_STOPPED = 0,
    PIPELINE_RUNNING = 1,
    PIPELINE_STOPPING = 2
} PipelineStateEnum;

typedef struct {
    PipelineStateEnum state;
    GstElement *pipeline;
    GstElement *appsink;
    UdpReceiver *udp_receiver;
    GThread *bus_thread;
    GThread *appsink_thread;
    GMutex lock;
    GCond cond;
    gboolean initialized;
    gboolean bus_thread_running;
    gboolean stop_requested;
    gboolean encountered_error;

    VideoDecoder *decoder;
    gboolean decoder_initialized;
    gboolean decoder_running;

    gboolean appsink_thread_running;

    VideoRecorder *recorder;
    GMutex recorder_lock;

    const AppCfg *cfg;

    gint queue_overruns;
    gint queue_underruns;
} PipelineState;

typedef struct PipelineRecordingStats {
    gboolean active;
    guint64 bytes_written;
    guint64 elapsed_ns;
    guint64 media_duration_ns;
    char output_path[PATH_MAX];
} PipelineRecordingStats;

int pipeline_start(const AppCfg *cfg, const ModesetResult *ms, int drm_fd, PipelineState *ps);
void pipeline_stop(PipelineState *ps, int wait_ms_total);
void pipeline_poll_child(PipelineState *ps);
int pipeline_enable_recording(PipelineState *ps, const RecordCfg *cfg);
void pipeline_disable_recording(PipelineState *ps);
int pipeline_get_recording_stats(const PipelineState *ps, PipelineRecordingStats *stats);

#endif // PIPELINE_H
