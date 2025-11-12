#ifndef H265_PARSE_H
#define H265_PARSE_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

typedef struct _SstarH265Parse SstarH265Parse;
typedef struct _SstarH265ParseClass SstarH265ParseClass;

GType sstar_h265_parse_get_type(void);

#define SSTAR_TYPE_H265_PARSE (sstar_h265_parse_get_type())
#define SSTAR_H265_PARSE(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), SSTAR_TYPE_H265_PARSE, SstarH265Parse))
#define SSTAR_H265_PARSE_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), SSTAR_TYPE_H265_PARSE, SstarH265ParseClass))
#define SSTAR_IS_H265_PARSE(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), SSTAR_TYPE_H265_PARSE))
#define SSTAR_IS_H265_PARSE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), SSTAR_TYPE_H265_PARSE))

gboolean sstar_h265_parse_register(void);

G_END_DECLS

#endif // H265_PARSE_H
