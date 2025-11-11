#ifndef H265_DEPAY_H
#define H265_DEPAY_H

#include <gst/gst.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(SstarH265Depay, sstar_h265_depay, SSTAR, H265_DEPAY, GstElement)

gboolean sstar_h265_depay_register(void);

G_END_DECLS

#endif // H265_DEPAY_H
