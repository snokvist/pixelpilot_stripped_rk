#ifndef H265_PARSE_H
#define H265_PARSE_H

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(SstarH265Parse, sstar_h265_parse, SSTAR, H265_PARSE, GstBaseTransform)

gboolean sstar_h265_parse_register(void);

G_END_DECLS

#endif // H265_PARSE_H
