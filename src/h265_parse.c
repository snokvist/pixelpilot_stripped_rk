// SPDX-License-Identifier: MIT

#include "h265_parse.h"

#include <dlfcn.h>

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>

GST_DEBUG_CATEGORY_STATIC(sstar_h265_parse_debug);
#define GST_CAT_DEFAULT sstar_h265_parse_debug

struct _SstarH265Parse {
    GstBaseTransform parent;
    GstCaps *configured_caps;
};

struct _SstarH265ParseClass {
    GstBaseTransformClass parent_class;
};

G_DEFINE_TYPE(SstarH265Parse, sstar_h265_parse, GST_TYPE_BASE_TRANSFORM)

typedef void (*PassthroughHelper)(GstBaseTransformClass *, gboolean);

static PassthroughHelper resolve_passthrough_helper(void) {
    static gsize once_init = 0;
    static PassthroughHelper helper = NULL;

    if (g_once_init_enter(&once_init)) {
        void *symbol = NULL;
#ifdef RTLD_DEFAULT
        symbol = dlsym(RTLD_DEFAULT, "gst_base_transform_class_set_passthrough_on_same_caps");
#else
        void *handle = dlopen(NULL, RTLD_LAZY);
        if (handle != NULL) {
            symbol = dlsym(handle, "gst_base_transform_class_set_passthrough_on_same_caps");
        }
#endif
        helper = (PassthroughHelper)symbol;
        g_once_init_leave(&once_init, 1);
    }

    return helper;
}

static GstCaps *sstar_h265_parse_transform_caps(GstBaseTransform *base,
                                                GstPadDirection direction,
                                                GstCaps *caps,
                                                GstCaps *filter) {
    if (caps == NULL) {
        return gst_caps_new_empty();
    }

    GstCaps *result = gst_caps_new_empty();
    guint size = gst_caps_get_size(caps);
    for (guint i = 0; i < size; ++i) {
        const GstStructure *in_s = gst_caps_get_structure(caps, i);
        if (in_s == NULL) {
            continue;
        }
        GstStructure *out_s = gst_structure_copy(in_s);
        const gchar *alignment = gst_structure_get_string(in_s, "alignment");
        if (alignment == NULL) {
            alignment = "au";
        }
        gst_structure_set(out_s, "stream-format", G_TYPE_STRING, "byte-stream", NULL);
        gst_structure_set(out_s, "alignment", G_TYPE_STRING, alignment, NULL);
        gst_caps_append_structure(result, out_s);
    }

    if (filter != NULL && !gst_caps_is_any(filter)) {
        GstCaps *filtered = gst_caps_intersect_full(result, filter, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(result);
        result = filtered;
    }

    return result;
}

static gboolean sstar_h265_parse_set_caps(GstBaseTransform *base, GstCaps *incaps, GstCaps *outcaps) {
    SstarH265Parse *self = SSTAR_H265_PARSE(base);

    if (self->configured_caps != NULL) {
        gst_caps_unref(self->configured_caps);
        self->configured_caps = NULL;
    }

    if (outcaps != NULL) {
        self->configured_caps = gst_caps_copy(outcaps);
    }

    return TRUE;
}

static GstFlowReturn sstar_h265_parse_transform_ip(GstBaseTransform *base, GstBuffer *buffer) {
    if (buffer == NULL) {
        return GST_FLOW_OK;
    }
    return GST_FLOW_OK;
}

static void sstar_h265_parse_finalize(GObject *object) {
    SstarH265Parse *self = SSTAR_H265_PARSE(object);
    if (self->configured_caps != NULL) {
        gst_caps_unref(self->configured_caps);
        self->configured_caps = NULL;
    }

    G_OBJECT_CLASS(sstar_h265_parse_parent_class)->finalize(object);
}

static GstStaticPadTemplate sstar_h265_parse_sink_template =
    GST_STATIC_PAD_TEMPLATE("sink",
                            GST_PAD_SINK,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-h265, stream-format=(string)byte-stream, alignment=(string){ au, nal }"));

static GstStaticPadTemplate sstar_h265_parse_src_template =
    GST_STATIC_PAD_TEMPLATE("src",
                            GST_PAD_SRC,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-h265, stream-format=(string)byte-stream, alignment=(string){ au, nal }"));

static void sstar_h265_parse_class_init(SstarH265ParseClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *base_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->finalize = sstar_h265_parse_finalize;

    gst_element_class_set_static_metadata(element_class,
                                          "SStar H.265 parser",
                                          "Codec/Parser/Video",
                                          "Lightweight in-place H.265 parser",
                                          "PixelPilot Project");

    gst_element_class_add_static_pad_template(element_class, &sstar_h265_parse_sink_template);
    gst_element_class_add_static_pad_template(element_class, &sstar_h265_parse_src_template);

    base_class->transform_caps = sstar_h265_parse_transform_caps;
    base_class->set_caps = sstar_h265_parse_set_caps;
    base_class->transform_ip = sstar_h265_parse_transform_ip;
    PassthroughHelper helper = resolve_passthrough_helper();
    if (helper != NULL) {
        helper(base_class, TRUE);
    } else {
        base_class->passthrough_on_same_caps = TRUE;
    }
}

static void sstar_h265_parse_init(SstarH265Parse *self) {
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_qos_enabled(GST_BASE_TRANSFORM(self), FALSE);
    self->configured_caps = NULL;
}

gboolean sstar_h265_parse_register(void) {
    static gsize once_init = 0;
    static gboolean registered = FALSE;

    if (g_once_init_enter(&once_init)) {
        GST_DEBUG_CATEGORY_INIT(sstar_h265_parse_debug, "sstarh265parse", 0, "SStar H.265 parser");
        registered = gst_element_register(NULL, "sstarh265parse", GST_RANK_PRIMARY + 10, SSTAR_TYPE_H265_PARSE);
        g_once_init_leave(&once_init, 1);
    }

    return registered;
}
