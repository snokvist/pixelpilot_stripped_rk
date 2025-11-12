#ifndef H265_DEPAY_H
#define H265_DEPAY_H

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _SstarH265Depay SstarH265Depay;
typedef struct _SstarH265DepayClass SstarH265DepayClass;

GType sstar_h265_depay_get_type(void);

#define SSTAR_TYPE_H265_DEPAY (sstar_h265_depay_get_type())
#define SSTAR_H265_DEPAY(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), SSTAR_TYPE_H265_DEPAY, SstarH265Depay))
#define SSTAR_H265_DEPAY_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), SSTAR_TYPE_H265_DEPAY, SstarH265DepayClass))
#define SSTAR_IS_H265_DEPAY(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), SSTAR_TYPE_H265_DEPAY))
#define SSTAR_IS_H265_DEPAY_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), SSTAR_TYPE_H265_DEPAY))

gboolean sstar_h265_depay_register(void);

G_END_DECLS

#endif // H265_DEPAY_H
