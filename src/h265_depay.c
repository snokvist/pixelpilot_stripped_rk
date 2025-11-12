// SPDX-License-Identifier: MIT

#include "h265_depay.h"

GST_DEBUG_CATEGORY_STATIC(sstar_h265_depay_debug);
#define GST_CAT_DEFAULT sstar_h265_depay_debug

#define RTP_VERSION 2
#define RTP_MIN_HEADER 12
#define H265_AP_NAL_TYPE 48
#define H265_FU_NAL_TYPE 49
#define RTP_CLOCK_RATE 90000

struct _SstarH265Depay {
    GstElement parent;

    GstPad *sinkpad;
    GstPad *srcpad;

    gint payload_type;

    GByteArray *current_au;
    GByteArray *current_fu;
    gboolean au_corrupted;

    gboolean have_au;
    gboolean have_last_ts;
    gboolean have_base_ts;

    guint32 current_timestamp;
    guint64 au_timestamp_ext;
    guint64 last_ts_ext;
    guint64 base_ts_ext;

    gboolean emit_partial_au;
};

struct _SstarH265DepayClass {
    GstElementClass parent_class;
};

G_DEFINE_TYPE(SstarH265Depay, sstar_h265_depay, GST_TYPE_ELEMENT)

enum {
    PROP_0,
    PROP_PAYLOAD_TYPE,
    PROP_EMIT_PARTIAL_AU,
};

static const guint8 kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

static void sstar_h265_depay_reset_state(SstarH265Depay *self);
static GstFlowReturn sstar_h265_depay_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer);
static gboolean sstar_h265_depay_sink_event(GstPad *pad, GstObject *parent, GstEvent *event);
static GstCaps *sstar_h265_depay_build_src_caps(void);
static void sstar_h265_depay_mark_corruption(SstarH265Depay *self);

static inline guint64 extend_timestamp(SstarH265Depay *self, guint32 ts) {
    if (!self->have_last_ts) {
        self->last_ts_ext = ts;
        self->have_last_ts = TRUE;
        return self->last_ts_ext;
    }

    guint32 prev = (guint32)(self->last_ts_ext & 0xffffffffu);
    guint64 base = self->last_ts_ext & 0xffffffff00000000ull;
    guint64 candidate = base | ts;
    if (ts < prev && (prev - ts) > 0x80000000u) {
        candidate += (1ull << 32);
    } else if (ts > prev && (ts - prev) > 0x80000000u) {
        if (candidate >= (1ull << 32)) {
            candidate -= (1ull << 32);
        }
    }
    self->last_ts_ext = candidate;
    return candidate;
}

static gboolean ensure_current_au(SstarH265Depay *self, guint64 ts_ext) {
    if (!self->have_au) {
        if (self->current_au != NULL) {
            g_byte_array_free(self->current_au, TRUE);
        }
        self->current_au = g_byte_array_sized_new(4096);
        self->au_timestamp_ext = ts_ext;
        self->au_corrupted = FALSE;
        self->have_au = TRUE;
    }
    return self->current_au != NULL;
}

static void drop_current_fu(SstarH265Depay *self) {
    if (self->current_fu != NULL) {
        g_byte_array_free(self->current_fu, TRUE);
        self->current_fu = NULL;
    }
}

static gboolean append_nal(SstarH265Depay *self, const guint8 *data, gsize size) {
    if (self->current_au == NULL) {
        return FALSE;
    }
    if (size == 0) {
        return TRUE;
    }
    g_byte_array_append(self->current_au, kStartCode, sizeof(kStartCode));
    g_byte_array_append(self->current_au, data, (guint)size);
    return TRUE;
}

static gboolean handle_single_nal(SstarH265Depay *self, const guint8 *payload, gsize len) {
    drop_current_fu(self);
    if (len < 2) {
        sstar_h265_depay_mark_corruption(self);
        return FALSE;
    }
    if (self->current_au == NULL) {
        return FALSE;
    }
    return append_nal(self, payload, len);
}

static gboolean handle_ap(SstarH265Depay *self, const guint8 *payload, gsize len) {
    drop_current_fu(self);
    if (len <= 2) {
        sstar_h265_depay_mark_corruption(self);
        return FALSE;
    }
    gsize offset = 2;
    while (offset + 2 <= len) {
        guint16 nal_size = (guint16)((payload[offset] << 8) | payload[offset + 1]);
        offset += 2;
        if (offset + nal_size > len) {
            sstar_h265_depay_mark_corruption(self);
            return FALSE;
        }
        if (nal_size > 0) {
            if (!append_nal(self, payload + offset, nal_size)) {
                return FALSE;
            }
        }
        offset += nal_size;
    }
    return TRUE;
}

static gboolean handle_fu(SstarH265Depay *self, const guint8 *payload, gsize len) {
    if (len < 3) {
        sstar_h265_depay_mark_corruption(self);
        return FALSE;
    }

    guint8 fu_header = payload[2];
    gboolean start = (fu_header & 0x80u) != 0;
    gboolean end = (fu_header & 0x40u) != 0;
    guint8 nal_type = fu_header & 0x3fu;

    guint16 indicator = (guint16)((payload[0] << 8) | payload[1]);
    guint16 reconstructed = (indicator & 0x81ffu) | ((guint16)nal_type << 9);

    gsize offset = 3;

    if (start) {
        drop_current_fu(self);
        self->current_fu = g_byte_array_sized_new((guint)(len + 4));
        if (self->current_fu == NULL) {
            return FALSE;
        }
        guint8 header[2] = {(guint8)(reconstructed >> 8), (guint8)(reconstructed & 0xff)};
        g_byte_array_append(self->current_fu, kStartCode, sizeof(kStartCode));
        g_byte_array_append(self->current_fu, header, sizeof(header));
    } else if (self->current_fu == NULL) {
        sstar_h265_depay_mark_corruption(self);
        return FALSE;
    }

    if (len > offset) {
        g_byte_array_append(self->current_fu, payload + offset, (guint)(len - offset));
    }

    if (end) {
        if (self->current_fu == NULL) {
            sstar_h265_depay_mark_corruption(self);
            return FALSE;
        }
        if (self->current_au == NULL) {
            sstar_h265_depay_mark_corruption(self);
            return FALSE;
        }
        g_byte_array_append(self->current_au, self->current_fu->data, self->current_fu->len);
        drop_current_fu(self);
    }

    return TRUE;
}

static gboolean depayload_nalu(SstarH265Depay *self, const guint8 *payload, gsize len) {
    if (self->current_au == NULL) {
        return FALSE;
    }
    if (len < 2) {
        sstar_h265_depay_mark_corruption(self);
        return FALSE;
    }

    guint16 nal_header = (guint16)((payload[0] << 8) | payload[1]);
    guint8 nal_type = (nal_header >> 9) & 0x3f;

    switch (nal_type) {
    case H265_AP_NAL_TYPE:
        return handle_ap(self, payload, len);
    case H265_FU_NAL_TYPE:
        return handle_fu(self, payload, len);
    default:
        return handle_single_nal(self, payload, len);
    }
}

static GstFlowReturn finish_current_au(SstarH265Depay *self, gboolean drop) {
    if (!self->have_au) {
        drop_current_fu(self);
        if (self->current_au != NULL) {
            g_byte_array_free(self->current_au, TRUE);
            self->current_au = NULL;
        }
        self->au_corrupted = FALSE;
        return GST_FLOW_OK;
    }

    gboolean corrupted = drop || self->au_corrupted;

    if (self->current_fu != NULL) {
        sstar_h265_depay_mark_corruption(self);
        drop = TRUE;
        corrupted = TRUE;
        drop_current_fu(self);
    }

    self->have_au = FALSE;

    gboolean should_drop = (drop && !self->emit_partial_au) || self->current_au == NULL || self->current_au->len == 0;

    if (should_drop) {
        self->au_corrupted = FALSE;
        if (self->current_au != NULL) {
            g_byte_array_free(self->current_au, TRUE);
            self->current_au = NULL;
        }
        return GST_FLOW_OK;
    }

    gsize size = self->current_au->len;
    guint8 *data = g_byte_array_free(self->current_au, FALSE);
    self->current_au = NULL;

    GstBuffer *out = gst_buffer_new_wrapped_full(0, data, size, 0, size, data, g_free);
    if (out == NULL) {
        g_free(data);
        return GST_FLOW_ERROR;
    }

    if (!self->have_base_ts) {
        self->base_ts_ext = self->au_timestamp_ext;
        self->have_base_ts = TRUE;
    }

    guint64 pts_offset = self->au_timestamp_ext - self->base_ts_ext;
    GstClockTime pts = gst_util_uint64_scale(pts_offset, GST_SECOND, RTP_CLOCK_RATE);
    GST_BUFFER_PTS(out) = pts;
    GST_BUFFER_DTS(out) = pts;

    if (corrupted) {
        GST_BUFFER_FLAG_SET(out, GST_BUFFER_FLAG_CORRUPTED);
        GST_BUFFER_FLAG_SET(out, GST_BUFFER_FLAG_DISCONT);
    }

    GstFlowReturn ret = gst_pad_push(self->srcpad, out);
    self->au_corrupted = FALSE;
    return ret;
}

static gboolean parse_rtp_payload(GstMapInfo *map,
                                  gint expect_pt,
                                  guint8 **payload_out,
                                  gsize *payload_len_out,
                                  guint32 *timestamp_out,
                                  gboolean *marker_out,
                                  guint16 *seq_out) {
    if (map->size < RTP_MIN_HEADER) {
        return FALSE;
    }

    const guint8 *data = map->data;
    guint8 vpxcc = data[0];
    guint8 version = vpxcc >> 6;
    guint8 padding = (vpxcc >> 5) & 0x01u;
    guint8 extension = (vpxcc >> 4) & 0x01u;
    guint8 cc = vpxcc & 0x0fu;

    if (version != RTP_VERSION) {
        return FALSE;
    }

    guint8 mpt = data[1];
    gboolean marker = (mpt & 0x80u) != 0;
    guint8 payload_type = mpt & 0x7fu;
    guint16 seq = ((guint16)data[2] << 8) | (guint16)data[3];

    if (expect_pt >= 0 && payload_type != (guint8)expect_pt) {
        return FALSE;
    }

    guint32 timestamp = ((guint32)data[4] << 24) | ((guint32)data[5] << 16) | ((guint32)data[6] << 8) | (guint32)data[7];

    gsize offset = RTP_MIN_HEADER + ((gsize)cc * 4u);
    if (map->size < offset) {
        return FALSE;
    }

    if (extension) {
        if (map->size < offset + 4) {
            return FALSE;
        }
        guint16 ext_len = ((guint16)data[offset + 2] << 8) | (guint16)data[offset + 3];
        offset += 4;
        gsize ext_bytes = (gsize)ext_len * 4u;
        if (map->size < offset + ext_bytes) {
            return FALSE;
        }
        offset += ext_bytes;
    }

    if (map->size < offset) {
        return FALSE;
    }

    gsize payload_len = map->size - offset;
    if (padding) {
        if (payload_len == 0) {
            return FALSE;
        }
        guint8 pad = data[map->size - 1];
        if (pad > payload_len) {
            return FALSE;
        }
        payload_len -= pad;
    }

    if (payload_len == 0) {
        return FALSE;
    }

    *payload_out = (guint8 *)(data + offset);
    *payload_len_out = payload_len;
    *timestamp_out = timestamp;
    *marker_out = marker;
    if (seq_out != NULL) {
        *seq_out = seq;
    }
    return TRUE;
}

static GstFlowReturn sstar_h265_depay_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer) {
    SstarH265Depay *self = SSTAR_H265_DEPAY(parent);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }

    guint8 *payload = NULL;
    gsize payload_len = 0;
    guint32 timestamp = 0;
    gboolean marker = FALSE;
    guint16 seq = 0;

    GstFlowReturn ret = GST_FLOW_OK;

    if (!parse_rtp_payload(&map,
                           self->payload_type,
                           &payload,
                           &payload_len,
                           &timestamp,
                           &marker,
                           &seq)) {
        if (self->have_au) {
            sstar_h265_depay_mark_corruption(self);
        }
        goto done;
    }

    (void)seq;

    guint64 ts_ext = extend_timestamp(self, timestamp);

    if (self->have_au && timestamp != self->current_timestamp) {
        self->au_corrupted = TRUE;
        ret = finish_current_au(self, TRUE);
        if (ret != GST_FLOW_OK) {
            goto done;
        }
    }

    if (!ensure_current_au(self, ts_ext)) {
        ret = GST_FLOW_ERROR;
        goto done;
    }

    self->current_timestamp = timestamp;

    if (!depayload_nalu(self, payload, payload_len)) {
        self->au_corrupted = TRUE;
    }

    if (marker) {
        ret = finish_current_au(self, self->au_corrupted);
    }

done:
    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);
    return ret;
}

static GstCaps *sstar_h265_depay_build_src_caps(void) {
    return gst_caps_new_simple("video/x-h265",
                               "stream-format", G_TYPE_STRING, "byte-stream",
                               "alignment", G_TYPE_STRING, "au",
                               NULL);
}

static gboolean sstar_h265_depay_sink_event(GstPad *pad, GstObject *parent, GstEvent *event) {
    SstarH265Depay *self = SSTAR_H265_DEPAY(parent);
    gboolean forward = TRUE;

    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_START:
        forward = gst_pad_push_event(self->srcpad, event);
        break;
    case GST_EVENT_FLUSH_STOP:
        sstar_h265_depay_reset_state(self);
        forward = gst_pad_push_event(self->srcpad, event);
        break;
    case GST_EVENT_EOS:
        finish_current_au(self, self->au_corrupted);
        forward = gst_pad_push_event(self->srcpad, event);
        break;
    case GST_EVENT_CAPS: {
        gst_event_unref(event);
        GstCaps *outcaps = sstar_h265_depay_build_src_caps();
        if (outcaps == NULL) {
            forward = FALSE;
            break;
        }
        GstEvent *caps_event = gst_event_new_caps(outcaps);
        gst_caps_unref(outcaps);
        if (caps_event == NULL) {
            forward = FALSE;
            break;
        }
        forward = gst_pad_push_event(self->srcpad, caps_event);
        break;
    }
    default:
        forward = gst_pad_push_event(self->srcpad, event);
        break;
    }

    return forward;
}

static void sstar_h265_depay_finalize(GObject *object) {
    SstarH265Depay *self = SSTAR_H265_DEPAY(object);
    sstar_h265_depay_reset_state(self);
    G_OBJECT_CLASS(sstar_h265_depay_parent_class)->finalize(object);
}

static void sstar_h265_depay_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    SstarH265Depay *self = SSTAR_H265_DEPAY(object);
    switch (prop_id) {
    case PROP_PAYLOAD_TYPE:
        self->payload_type = g_value_get_int(value);
        break;
    case PROP_EMIT_PARTIAL_AU:
        self->emit_partial_au = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void sstar_h265_depay_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    SstarH265Depay *self = SSTAR_H265_DEPAY(object);
    switch (prop_id) {
    case PROP_PAYLOAD_TYPE:
        g_value_set_int(value, self->payload_type);
        break;
    case PROP_EMIT_PARTIAL_AU:
        g_value_set_boolean(value, self->emit_partial_au);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void sstar_h265_depay_reset_state(SstarH265Depay *self) {
    drop_current_fu(self);
    if (self->current_au != NULL) {
        g_byte_array_free(self->current_au, TRUE);
        self->current_au = NULL;
    }
    self->au_corrupted = FALSE;
    self->have_au = FALSE;
    self->have_last_ts = FALSE;
    self->have_base_ts = FALSE;
    self->current_timestamp = 0;
    self->au_timestamp_ext = 0;
    self->last_ts_ext = 0;
    self->base_ts_ext = 0;
}

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE("sink",
                            GST_PAD_SINK,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("application/x-rtp, media=(string)video, encoding-name=(string)H265, clock-rate=(int)90000"));

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE("src",
                            GST_PAD_SRC,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-h265, stream-format=(string)byte-stream, alignment=(string)au"));

static void sstar_h265_depay_class_init(SstarH265DepayClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->finalize = sstar_h265_depay_finalize;
    gobject_class->set_property = sstar_h265_depay_set_property;
    gobject_class->get_property = sstar_h265_depay_get_property;

    g_object_class_install_property(gobject_class,
                                    PROP_PAYLOAD_TYPE,
                                    g_param_spec_int("payload-type",
                                                     "Payload Type",
                                                     "RTP payload type to accept (-1 to accept any)",
                                                     -1,
                                                     127,
                                                     96,
                                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(gobject_class,
                                    PROP_EMIT_PARTIAL_AU,
                                    g_param_spec_boolean("emit-partial-au",
                                                         "Emit Partial Access Units",
                                                         "Forward corrupted access units with DISCONT/CORRUPTED flags",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    gst_element_class_set_static_metadata(element_class,
                                          "SStar H.265 depayloader",
                                          "Codec/Depayloader/Network",
                                          "Minimal RTP H.265 depayload element",
                                          "PixelPilot Project");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);
}

static void sstar_h265_depay_init(SstarH265Depay *self) {
    self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
    gst_pad_set_chain_function(self->sinkpad, GST_DEBUG_FUNCPTR(sstar_h265_depay_chain));
    gst_pad_set_event_function(self->sinkpad, GST_DEBUG_FUNCPTR(sstar_h265_depay_sink_event));
    gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);

    self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
    gst_pad_use_fixed_caps(self->srcpad);
    gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

    self->payload_type = 96;
    self->current_au = NULL;
    self->current_fu = NULL;
    self->au_corrupted = FALSE;
    self->have_au = FALSE;
    self->have_last_ts = FALSE;
    self->have_base_ts = FALSE;
    self->current_timestamp = 0;
    self->au_timestamp_ext = 0;
    self->last_ts_ext = 0;
    self->base_ts_ext = 0;
    self->emit_partial_au = FALSE;
}

gboolean sstar_h265_depay_register(void) {
    static gsize once_init = 0;
    static gboolean registered = FALSE;

    if (g_once_init_enter(&once_init)) {
        GST_DEBUG_CATEGORY_INIT(sstar_h265_depay_debug, "sstarh265depay", 0, "SStar H.265 depayloader");
        registered = gst_element_register(NULL, "sstarh265depay", GST_RANK_PRIMARY + 10, SSTAR_TYPE_H265_DEPAY);
        g_once_init_leave(&once_init, 1);
    }

    return registered;
}

static void sstar_h265_depay_mark_corruption(SstarH265Depay *self) {
    if (self != NULL) {
        self->au_corrupted = TRUE;
    }
}
