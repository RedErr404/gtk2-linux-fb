#ifndef __GTKINTL_H__
#define __GTKINTL_H__

#include <glib/gi18n-lib.h>
#include <glib/gquark.h>

#ifdef ENABLE_NLS
#define P_(String) dgettext(GETTEXT_PACKAGE "-properties",String)
#else 
#define P_(String) (String)
#endif

/* not really I18N-related, but also a string marker macro */
#define g_intern_static_string(string) g_intern_static_string (string)

#endif
