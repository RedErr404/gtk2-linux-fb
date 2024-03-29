/* GTK - The GIMP Toolkit
 * gtkprintbackendcups.h: Default implementation of GtkPrintBackend 
 * for the Common Unix Print System (CUPS)
 * Copyright (C) 2003, Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>

#include <config.h>
#include <cups/cups.h>
#include <cups/language.h>
#include <cups/http.h>
#include <cups/ipp.h>
#include <errno.h>
#include <cairo.h>
#include <cairo-pdf.h>
#include <cairo-ps.h>

#include <glib/gi18n-lib.h>
#include <gmodule.h>

#include <gtk/gtkprintoperation.h>
#include <gtk/gtkprintsettings.h>
#include <gtk/gtkprintbackend.h>
#include <gtk/gtkprinter.h>
#include <gtk/gtkprinter-private.h>

#include "gtkprintbackendcups.h"
#include "gtkprintercups.h"

#include "gtkcupsutils.h"
#include "gtkdebug.h"


typedef struct _GtkPrintBackendCupsClass GtkPrintBackendCupsClass;

#define GTK_PRINT_BACKEND_CUPS_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_PRINT_BACKEND_CUPS, GtkPrintBackendCupsClass))
#define GTK_IS_PRINT_BACKEND_CUPS_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_PRINT_BACKEND_CUPS))
#define GTK_PRINT_BACKEND_CUPS_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_PRINT_BACKEND_CUPS, GtkPrintBackendCupsClass))

#define _CUPS_MAX_ATTEMPTS 10 
#define _CUPS_MAX_CHUNK_SIZE 8192

/* define this to see warnings about ignored ppd options */
#undef PRINT_IGNORED_OPTIONS

#define _CUPS_MAP_ATTR_INT(attr, v, a) {if (!g_ascii_strcasecmp (attr->name, (a))) v = attr->values[0].integer;}
#define _CUPS_MAP_ATTR_STR(attr, v, a) {if (!g_ascii_strcasecmp (attr->name, (a))) v = attr->values[0].string.text;}

static GType print_backend_cups_type = 0;

typedef void (* GtkPrintCupsResponseCallbackFunc) (GtkPrintBackend *print_backend,
                                                   GtkCupsResult   *result, 
                                                   gpointer         user_data);

typedef enum 
{
  DISPATCH_SETUP,
  DISPATCH_REQUEST,
  DISPATCH_SEND,
  DISPATCH_CHECK,
  DISPATCH_READ,
  DISPATCH_ERROR
} GtkPrintCupsDispatchState;

typedef struct 
{
  GSource source;

  http_t *http;
  GtkCupsRequest *request;
  GPollFD *data_poll;
  GtkPrintBackendCups *backend;

} GtkPrintCupsDispatchWatch;

struct _GtkPrintBackendCupsClass
{
  GtkPrintBackendClass parent_class;
};

struct _GtkPrintBackendCups
{
  GtkPrintBackend parent_instance;

  char *default_printer;
  
  guint list_printers_poll;
  guint list_printers_pending : 1;
  guint got_default_printer   : 1;
};

static GObjectClass *backend_parent_class;

static void                 gtk_print_backend_cups_class_init      (GtkPrintBackendCupsClass          *class);
static void                 gtk_print_backend_cups_init            (GtkPrintBackendCups               *impl);
static void                 gtk_print_backend_cups_finalize        (GObject                           *object);
static void                 gtk_print_backend_cups_dispose         (GObject                           *object);
static void                 cups_get_printer_list                  (GtkPrintBackend                   *print_backend);
static void                 cups_request_execute                   (GtkPrintBackendCups               *print_backend,
								    GtkCupsRequest                    *request,
								    GtkPrintCupsResponseCallbackFunc   callback,
								    gpointer                           user_data,
								    GDestroyNotify                     notify);
static void                 cups_printer_get_settings_from_options (GtkPrinter                        *printer,
								    GtkPrinterOptionSet               *options,
								    GtkPrintSettings                  *settings);
static gboolean             cups_printer_mark_conflicts            (GtkPrinter                        *printer,
								    GtkPrinterOptionSet               *options);
static GtkPrinterOptionSet *cups_printer_get_options               (GtkPrinter                        *printer,
								    GtkPrintSettings                  *settings,
								    GtkPageSetup                      *page_setup,
                                                                    GtkPrintCapabilities               capabilities);
static void                 cups_printer_prepare_for_print         (GtkPrinter                        *printer,
								    GtkPrintJob                       *print_job,
								    GtkPrintSettings                  *settings,
								    GtkPageSetup                      *page_setup);
static GList *              cups_printer_list_papers               (GtkPrinter                        *printer);
static void                 cups_printer_request_details           (GtkPrinter                        *printer);
static void                 cups_request_default_printer           (GtkPrintBackendCups               *print_backend);
static void                 cups_request_ppd                       (GtkPrinter                        *printer);
static void                 cups_printer_get_hard_margins          (GtkPrinter                        *printer,
								    double                            *top,
								    double                            *bottom,
								    double                            *left,
								    double                            *right);
static GtkPrintCapabilities cups_printer_get_capabilities          (GtkPrinter                        *printer);
static void                 set_option_from_settings               (GtkPrinterOption                  *option,
								    GtkPrintSettings                  *setting);
static void                 cups_begin_polling_info                (GtkPrintBackendCups               *print_backend,
								    GtkPrintJob                       *job,
								    int                                job_id);
static gboolean             cups_job_info_poll_timeout             (gpointer                           user_data);
static void                 gtk_print_backend_cups_print_stream    (GtkPrintBackend                   *backend,
								    GtkPrintJob                       *job,
								    GIOChannel                        *data_io,
								    GtkPrintJobCompleteFunc            callback,
								    gpointer                           user_data,
								    GDestroyNotify                     dnotify);
static cairo_surface_t *    cups_printer_create_cairo_surface      (GtkPrinter                        *printer,
								    GtkPrintSettings                  *settings,
								    gdouble                            width,
								    gdouble                            height,
								    GIOChannel                        *cache_io);


static void
gtk_print_backend_cups_register_type (GTypeModule *module)
{

}

G_MODULE_EXPORT void 
pb_module_init (GTypeModule *module)
{

}

G_MODULE_EXPORT void 
pb_module_exit (void)
{

}
  
G_MODULE_EXPORT GtkPrintBackend * 
pb_module_create (void)
{
  return gtk_print_backend_cups_new ();
}

/*
 * GtkPrintBackendCups
 */
GType
gtk_print_backend_cups_get_type (void)
{
  return print_backend_cups_type;
}

/**
 * gtk_print_backend_cups_new:
 *
 * Creates a new #GtkPrintBackendCups object. #GtkPrintBackendCups
 * implements the #GtkPrintBackend interface with direct access to
 * the filesystem using Unix/Linux API calls
 *
 * Return value: the new #GtkPrintBackendCups object
 */
GtkPrintBackend *
gtk_print_backend_cups_new (void)
{
  GTK_NOTE (PRINTING,
            g_print ("CUPS Backend: Creating a new CUPS print backend object\n"));

  return g_object_new (GTK_TYPE_PRINT_BACKEND_CUPS, NULL);
}

static void
gtk_print_backend_cups_class_init (GtkPrintBackendCupsClass *class)
{

}

static cairo_status_t
_cairo_write_to_cups (void                *closure,
                      const unsigned char *data,
                      unsigned int         length)
{

  return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *
cups_printer_create_cairo_surface (GtkPrinter       *printer,
				   GtkPrintSettings *settings,
				   gdouble           width, 
				   gdouble           height,
				   GIOChannel       *cache_io)
{
  cairo_surface_t *surface; 
 
  /* TODO: check if it is a ps or pdf printer */
  
  surface = cairo_ps_surface_create_for_stream  (_cairo_write_to_cups, cache_io, width, height);

  /* TODO: DPI from settings object? */
  cairo_surface_set_fallback_resolution (surface, 300, 300);

  return surface;
}

typedef struct {
  GtkPrintJobCompleteFunc callback;
  GtkPrintJob *job;
  gpointer user_data;
  GDestroyNotify dnotify;
} CupsPrintStreamData;

static void
cups_free_print_stream_data (CupsPrintStreamData *data)
{
  GTK_NOTE (PRINTING,
            g_print ("CUPS Backend: %s\n", G_STRFUNC));

  if (data->dnotify)
    data->dnotify (data->user_data);
  g_object_unref (data->job);
  g_free (data);
}

static void
cups_print_cb (GtkPrintBackendCups *print_backend,
               GtkCupsResult       *result,
               gpointer             user_data)
{
 
}

static void
add_cups_options (const gchar *key,
		  const gchar *value,
		  gpointer     user_data)
{

}

static void
gtk_print_backend_cups_print_stream (GtkPrintBackend         *print_backend,
                                     GtkPrintJob             *job,
				     GIOChannel              *data_io,
				     GtkPrintJobCompleteFunc  callback,
				     gpointer                 user_data,
				     GDestroyNotify           dnotify)
{

}


static void
gtk_print_backend_cups_init (GtkPrintBackendCups *backend_cups)
{

}

static void
gtk_print_backend_cups_finalize (GObject *object)
{

}

static void
gtk_print_backend_cups_dispose (GObject *object)
{

}


static gboolean
cups_dispatch_watch_check (GSource *source)
{

  return FALSE;
}

static gboolean
cups_dispatch_watch_prepare (GSource *source,
			     gint    *timeout_)
{
  GtkPrintCupsDispatchWatch *dispatch;

  dispatch = (GtkPrintCupsDispatchWatch *) source;

  GTK_NOTE (PRINTING,
            g_print ("CUPS Backend: %s <source %p>\n", G_STRFUNC, source));

  *timeout_ = -1;
  
  return gtk_cups_request_read_write (dispatch->request);
}

static gboolean
cups_dispatch_watch_dispatch (GSource     *source,
			      GSourceFunc  callback,
			      gpointer     user_data)
{

    
  return FALSE;
}

static void
cups_dispatch_watch_finalize (GSource *source)
{

}

static GSourceFuncs _cups_dispatch_watch_funcs = {
  cups_dispatch_watch_prepare,
  cups_dispatch_watch_check,
  cups_dispatch_watch_dispatch,
  cups_dispatch_watch_finalize
};


static void
cups_request_execute (GtkPrintBackendCups              *print_backend,
                      GtkCupsRequest                   *request,
                      GtkPrintCupsResponseCallbackFunc  callback,
                      gpointer                          user_data,
                      GDestroyNotify                    notify)
{

}

#if 0
static void
cups_request_printer_info_cb (GtkPrintBackendCups *backend,
                              GtkCupsResult       *result,
                              gpointer             user_data)
{

}

static void
cups_request_printer_info (GtkPrintBackendCups *print_backend,
                           const gchar         *printer_name)
{

}
#endif

typedef struct {
  GtkPrintBackendCups *print_backend;
  GtkPrintJob *job;
  int job_id;
  int counter;
} CupsJobPollData;

static void
job_object_died	(gpointer  user_data,
		 GObject  *where_the_object_was)
{

}

static void
cups_job_poll_data_free (CupsJobPollData *data)
{

}

static void
cups_request_job_info_cb (GtkPrintBackendCups *print_backend,
			  GtkCupsResult       *result,
			  gpointer             user_data)
{

}

static void
cups_request_job_info (CupsJobPollData *data)
{

}

static gboolean
cups_job_info_poll_timeout (gpointer user_data)
{

  
  return FALSE;
}

static void
cups_begin_polling_info (GtkPrintBackendCups *print_backend,
			 GtkPrintJob         *job,
			 gint                 job_id)
{

}

static void
mark_printer_inactive (GtkPrinter      *printer, 
                       GtkPrintBackend *backend)
{

}

static gint
find_printer (GtkPrinter  *printer, 
	      const gchar *find_name)
{
  const gchar *printer_name;

  printer_name = gtk_printer_get_name (printer);
  return g_ascii_strcasecmp (printer_name, find_name);
}

static void
cups_request_printer_list_cb (GtkPrintBackendCups *cups_backend,
                              GtkCupsResult       *result,
                              gpointer             user_data)
{

}

static gboolean
cups_request_printer_list (GtkPrintBackendCups *cups_backend)
{

  return TRUE;
}

static void
cups_get_printer_list (GtkPrintBackend *backend)
{

}

typedef struct {
  GtkPrinterCups *printer;
  GIOChannel *ppd_io;
  http_t *http;
} GetPPDData;

static void
get_ppd_data_free (GetPPDData *data)
{

}

static void
cups_request_ppd_cb (GtkPrintBackendCups *print_backend,
                     GtkCupsResult       *result,
                     GetPPDData          *data)
{

}

static void
cups_request_ppd (GtkPrinter *printer)
{

}


static void
cups_request_default_printer_cb (GtkPrintBackendCups *print_backend,
				 GtkCupsResult       *result,
				 gpointer             user_data)
{

}

static void
cups_request_default_printer (GtkPrintBackendCups *print_backend)
{

}

static void
cups_printer_request_details (GtkPrinter *printer)
{

}

static char *
ppd_text_to_utf8 (ppd_file_t *ppd_file, 
		  const char *text)
{

  return NULL;
}

/* TODO: Add more translations for common settings here */

static const struct {
  const char *keyword;
  const char *translation;
} cups_option_translations[] = {
  { "Duplex", N_("Two Sided") },
  { "MediaType", N_("Paper Type") },
  { "InputSlot", N_("Paper Source") },
  { "OutputBin", N_("Output Tray") },
};


static const struct {
  const char *keyword;
  const char *choice;
  const char *translation;
} cups_choice_translations[] = {
  { "Duplex", "None", N_("One Sided") },
  { "InputSlot", "Auto", N_("Auto Select") },
  { "InputSlot", "AutoSelect", N_("Auto Select") },
  { "InputSlot", "Default", N_("Printer Default") },
  { "InputSlot", "None", N_("Printer Default") },
  { "InputSlot", "PrinterDefault", N_("Printer Default") },
  { "InputSlot", "Unspecified", N_("Auto Select") },
};

static const struct {
  const char *ppd_keyword;
  const char *name;
} option_names[] = {
  {"Duplex", "gtk-duplex" },
  {"MediaType", "gtk-paper-type"},
  {"InputSlot", "gtk-paper-source"},
  {"OutputBin", "gtk-output-tray"},
};

/* keep sorted when changing */
static const char *color_option_whitelist[] = {
  "BRColorEnhancement",
  "BRColorMatching",
  "BRColorMatching",
  "BRColorMode",
  "BRGammaValue",
  "BRImprovedGray",
  "BlackSubstitution",
  "ColorModel",
  "HPCMYKInks",
  "HPCSGraphics",
  "HPCSImages",
  "HPCSText",
  "HPColorSmart",
  "RPSBlackMode",
  "RPSBlackOverPrint",
  "Rcmyksimulation",
};

/* keep sorted when changing */
static const char *color_group_whitelist[] = {
  "ColorPage",
  "FPColorWise1",
  "FPColorWise2",
  "FPColorWise3",
  "FPColorWise4",
  "FPColorWise5",
  "HPColorOptionsPanel",
};
  
/* keep sorted when changing */
static const char *image_quality_option_whitelist[] = {
  "BRDocument",
  "BRHalfTonePattern",
  "BRNormalPrt",
  "BRPrintQuality",
  "BitsPerPixel",
  "Darkness",
  "Dithering",
  "EconoMode",
  "Economode",
  "HPEconoMode",
  "HPEdgeControl",
  "HPGraphicsHalftone",
  "HPHalftone",
  "HPLJDensity",
  "HPPhotoHalftone",
  "OutputMode",
  "REt",
  "RPSBitsPerPixel",
  "RPSDitherType",
  "Resolution",
  "ScreenLock",
  "Smoothing",
  "TonerSaveMode",
  "UCRGCRForImage",
};

/* keep sorted when changing */
static const char *image_quality_group_whitelist[] = {
  "FPImageQuality1",
  "FPImageQuality2",
  "FPImageQuality3",
  "ImageQualityPage",
};

/* keep sorted when changing */
static const char * finishing_option_whitelist[] = {
  "BindColor",
  "BindEdge",
  "BindType",
  "BindWhen",
  "Booklet",
  "FoldType",
  "FoldWhen",
  "HPStaplerOptions",
  "Jog",
  "Slipsheet",
  "Sorter",
  "StapleLocation",
  "StapleOrientation",
  "StapleWhen",
  "StapleX",
  "StapleY",
};

/* keep sorted when changing */
static const char *finishing_group_whitelist[] = {
  "FPFinishing1",
  "FPFinishing2",
  "FPFinishing3",
  "FPFinishing4",
  "FinishingPage",
  "HPFinishingPanel",
};

/* keep sorted when changing */
static const char *cups_option_blacklist[] = {
  "Collate",
  "Copies", 
  "OutputOrder",
  "PageRegion",
  "PageSize",
};

static char *
get_option_text (ppd_file_t   *ppd_file, 
		 ppd_option_t *option)
{
  int i;
  char *utf8;
  
  for (i = 0; i < G_N_ELEMENTS (cups_option_translations); i++)
    {
      if (strcmp (cups_option_translations[i].keyword, option->keyword) == 0)
	return g_strdup (_(cups_option_translations[i].translation));
    }

  utf8 = ppd_text_to_utf8 (ppd_file, option->text);

  /* Some ppd files have spaces in the text before the colon */
  g_strchomp (utf8);
  
  return utf8;
}

static char *
get_choice_text (ppd_file_t   *ppd_file, 
		 ppd_choice_t *choice)
{
  int i;
  ppd_option_t *option = choice->option;
  const char *keyword = option->keyword;
  
  for (i = 0; i < G_N_ELEMENTS (cups_choice_translations); i++)
    {
      if (strcmp (cups_choice_translations[i].keyword, keyword) == 0 &&
	  strcmp (cups_choice_translations[i].choice, choice->choice) == 0)
	return g_strdup (_(cups_choice_translations[i].translation));
    }
  return ppd_text_to_utf8 (ppd_file, choice->text);
}

static gboolean
group_has_option (ppd_group_t  *group, 
		  ppd_option_t *option)
{

  return FALSE;
}

static void
set_option_off (GtkPrinterOption *option)
{

}

static gboolean
value_is_off (const char *value)
{
  return  (strcasecmp (value, "None") == 0 ||
	   strcasecmp (value, "Off") == 0 ||
	   strcasecmp (value, "False") == 0);
}

static char *
ppd_group_name (ppd_group_t *group)
{
#if CUPS_VERSION_MAJOR > 1 || (CUPS_VERSION_MAJOR == 1 && CUPS_VERSION_MINOR > 1) || (CUPS_VERSION_MAJOR == 1 && CUPS_VERSION_MINOR == 1 && CUPS_VERSION_PATCH >= 18) 
  return group->name;
#else
  return group->text;
#endif
}

static int
available_choices (ppd_file_t     *ppd,
		   ppd_option_t   *option,
		   ppd_choice_t ***available,
		   gboolean        keep_if_only_one_option)
{

  return 0;
}

static GtkPrinterOption *
create_pickone_option (ppd_file_t   *ppd_file,
		       ppd_option_t *ppd_option,
		       const gchar  *gtk_name)
{
  GtkPrinterOption *option;
  ppd_choice_t **available;
  char *label;
  int n_choices;
  int i;
#ifdef HAVE_CUPS_API_1_2
  ppd_coption_t *coption;
#endif

  g_assert (ppd_option->ui == PPD_UI_PICKONE);
  
  option = NULL;

  n_choices = available_choices (ppd_file, ppd_option, &available, g_str_has_prefix (gtk_name, "gtk-"));
  if (n_choices > 0)
    {
      
      /* right now only support one parameter per custom option 
       * if more than one print warning and only offer the default choices
       */

      label = get_option_text (ppd_file, ppd_option);

#ifdef HAVE_CUPS_API_1_2
      coption = ppdFindCustomOption (ppd_file, ppd_option->keyword);

      if (coption)
        {
	  ppd_cparam_t *cparam;

          cparam = ppdFirstCustomParam (coption);

          if (ppdNextCustomParam (coption) == NULL)
	    {
              switch (cparam->type)
	        {
                case PPD_CUSTOM_INT:
		  option = gtk_printer_option_new (gtk_name, label,
				         GTK_PRINTER_OPTION_TYPE_PICKONE_INT);
		  break;
                case PPD_CUSTOM_PASSCODE:
		  option = gtk_printer_option_new (gtk_name, label,
				         GTK_PRINTER_OPTION_TYPE_PICKONE_PASSCODE);
		  break;
                case PPD_CUSTOM_PASSWORD:
		    option = gtk_printer_option_new (gtk_name, label,
				         GTK_PRINTER_OPTION_TYPE_PICKONE_PASSWORD);
		  break;
               case PPD_CUSTOM_REAL:
		    option = gtk_printer_option_new (gtk_name, label,
				         GTK_PRINTER_OPTION_TYPE_PICKONE_REAL);
		  break;
                case PPD_CUSTOM_STRING:
		  option = gtk_printer_option_new (gtk_name, label,
				         GTK_PRINTER_OPTION_TYPE_PICKONE_STRING);
		  break;
#ifdef PRINT_IGNORED_OPTIONS
                case PPD_CUSTOM_POINTS: 
		  g_warning ("CUPS Backend: PPD Custom Points Option not supported");
		  break;
                case PPD_CUSTOM_CURVE:
                  g_warning ("CUPS Backend: PPD Custom Curve Option not supported");
		  break;
                case PPD_CUSTOM_INVCURVE: 	
		  g_warning ("CUPS Backend: PPD Custom Inverse Curve Option not supported");
		  break;
#endif
                default: 
                  break;
		}
	    }
#ifdef PRINT_IGNORED_OPTIONS
	  else
	    g_warning ("CUPS Backend: Multi-parameter PPD Custom Option not supported");
#endif
	}
#endif /* HAVE_CUPS_API_1_2 */

      if (!option)
        option = gtk_printer_option_new (gtk_name, label,
				         GTK_PRINTER_OPTION_TYPE_PICKONE);
      g_free (label);
      
      gtk_printer_option_allocate_choices (option, n_choices);
      for (i = 0; i < n_choices; i++)
	{
	  if (available[i] == NULL)
	    {
	      /* This was auto-added */
	      option->choices[i] = g_strdup ("gtk-ignore-value");
	      option->choices_display[i] = g_strdup (_("Printer Default"));
	    }
	  else
	    {
	      option->choices[i] = g_strdup (available[i]->choice);
	      option->choices_display[i] = get_choice_text (ppd_file, available[i]);
	    }
	}
      gtk_printer_option_set (option, ppd_option->defchoice);
    }
#ifdef PRINT_IGNORED_OPTIONS
  else
    g_warning ("CUPS Backend: Ignoring pickone %s\n", ppd_option->text);
#endif
  g_free (available);

  return option;
}

static GtkPrinterOption *
create_boolean_option (ppd_file_t   *ppd_file,
		       ppd_option_t *ppd_option,
		       const gchar  *gtk_name)
{
  GtkPrinterOption *option;
  ppd_choice_t **available;
  char *label;
  int n_choices;

  g_assert (ppd_option->ui == PPD_UI_BOOLEAN);
  
  option = NULL;

  n_choices = available_choices (ppd_file, ppd_option, &available, g_str_has_prefix (gtk_name, "gtk-"));
  if (n_choices == 2)
    {
      label = get_option_text (ppd_file, ppd_option);
      option = gtk_printer_option_new (gtk_name, label,
				       GTK_PRINTER_OPTION_TYPE_BOOLEAN);
      g_free (label);
      
      gtk_printer_option_allocate_choices (option, 2);
      option->choices[0] = g_strdup ("True");
      option->choices_display[0] = g_strdup ("True");
      option->choices[1] = g_strdup ("False");
      option->choices_display[1] = g_strdup ("False");
      
      gtk_printer_option_set (option, ppd_option->defchoice);
    }
#ifdef PRINT_IGNORED_OPTIONS
  else
    g_warning ("CUPS Backend: Ignoring boolean %s\n", ppd_option->text);
#endif
  g_free (available);

  return option;
}

static gchar *
get_option_name (const gchar *keyword)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (option_names); i++)
    if (strcmp (option_names[i].ppd_keyword, keyword) == 0)
      return g_strdup (option_names[i].name);

  return g_strdup_printf ("cups-%s", keyword);
}

static int
strptr_cmp (const void *a, 
	    const void *b)
{
  char **aa = (char **)a;
  char **bb = (char **)b;
  return strcmp (*aa, *bb);
}


static gboolean
string_in_table (gchar       *str, 
		 const gchar *table[], 
		 gint         table_len)
{
  return bsearch (&str, table, table_len, sizeof (char *), (void *)strptr_cmp) != NULL;
}

#define STRING_IN_TABLE(_str, _table) (string_in_table (_str, _table, G_N_ELEMENTS (_table)))

static void
handle_option (GtkPrinterOptionSet *set,
	       ppd_file_t          *ppd_file,
	       ppd_option_t        *ppd_option,
	       ppd_group_t         *toplevel_group,
	       GtkPrintSettings    *settings)
{
  GtkPrinterOption *option;
  char *name;

  if (STRING_IN_TABLE (ppd_option->keyword, cups_option_blacklist))
    return;

  name = get_option_name (ppd_option->keyword);

  option = NULL;
  if (ppd_option->ui == PPD_UI_PICKONE)
    {
      option = create_pickone_option (ppd_file, ppd_option, name);
    }
  else if (ppd_option->ui == PPD_UI_BOOLEAN)
    {
      option = create_boolean_option (ppd_file, ppd_option, name);
    }
#ifdef PRINT_IGNORED_OPTIONS
  else
    g_warning ("CUPS Backend: Ignoring pickmany setting %s\n", ppd_option->text);
#endif  
  
  if (option)
    {
      char *name;

      name = ppd_group_name (toplevel_group);
      if (STRING_IN_TABLE (name,
			   color_group_whitelist) ||
	  STRING_IN_TABLE (ppd_option->keyword,
			   color_option_whitelist))
	{
	  option->group = g_strdup ("ColorPage");
	}
      else if (STRING_IN_TABLE (name,
				image_quality_group_whitelist) ||
	       STRING_IN_TABLE (ppd_option->keyword,
				image_quality_option_whitelist))
	{
	  option->group = g_strdup ("ImageQualityPage");
	}
      else if (STRING_IN_TABLE (name,
				finishing_group_whitelist) ||
	       STRING_IN_TABLE (ppd_option->keyword,
				finishing_option_whitelist))
	{
	  option->group = g_strdup ("FinishingPage");
	}
      else
	{
	  option->group = g_strdup (toplevel_group->text);
	}

      set_option_from_settings (option, settings);
      
      gtk_printer_option_set_add (set, option);
    }
  
  g_free (name);
}

static void
handle_group (GtkPrinterOptionSet *set,
	      ppd_file_t          *ppd_file,
	      ppd_group_t         *group,
	      ppd_group_t         *toplevel_group,
	      GtkPrintSettings    *settings)
{
  gint i;
  gchar *name;
  
  /* Ignore installable options */
  name = ppd_group_name (toplevel_group);
  if (strcmp (name, "InstallableOptions") == 0)
    return;
  
  for (i = 0; i < group->num_options; i++)
    handle_option (set, ppd_file, &group->options[i], toplevel_group, settings);

  for (i = 0; i < group->num_subgroups; i++)
    handle_group (set, ppd_file, &group->subgroups[i], toplevel_group, settings);

}

static GtkPrinterOptionSet *
cups_printer_get_options (GtkPrinter           *printer,
			  GtkPrintSettings     *settings,
			  GtkPageSetup         *page_setup,
			  GtkPrintCapabilities  capabilities)
{
  GtkPrinterOptionSet *set;
  GtkPrinterOption *option;
  ppd_file_t *ppd_file;
  int i;
  char *print_at[] = { "now", "at", "on-hold" };
  char *n_up[] = {"1", "2", "4", "6", "9", "16" };
  char *prio[] = {"100", "80", "50", "30" };
  char *prio_display[] = {N_("Urgent"), N_("High"), N_("Medium"), N_("Low") };
  char *cover[] = {"none", "classified", "confidential", "secret", "standard", "topsecret", "unclassified" };
  char *cover_display[] = {N_("None"), N_("Classified"), N_("Confidential"), N_("Secret"), N_("Standard"), N_("Top Secret"), N_("Unclassified"),};


  set = gtk_printer_option_set_new ();

  /* Cups specific, non-ppd related settings */

  option = gtk_printer_option_new ("gtk-n-up", "Pages Per Sheet", GTK_PRINTER_OPTION_TYPE_PICKONE);
  gtk_printer_option_choices_from_array (option, G_N_ELEMENTS (n_up),
					 n_up, n_up);
  gtk_printer_option_set (option, "1");
  set_option_from_settings (option, settings);
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);

  for (i = 0; i < G_N_ELEMENTS(prio_display); i++)
    prio_display[i] = _(prio_display[i]);
  
  option = gtk_printer_option_new ("gtk-job-prio", "Job Priority", GTK_PRINTER_OPTION_TYPE_PICKONE);
  gtk_printer_option_choices_from_array (option, G_N_ELEMENTS (prio),
					 prio, prio_display);
  gtk_printer_option_set (option, "50");
  set_option_from_settings (option, settings);
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);

  option = gtk_printer_option_new ("gtk-billing-info", "Billing Info", GTK_PRINTER_OPTION_TYPE_STRING);
  gtk_printer_option_set (option, "");
  set_option_from_settings (option, settings);
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);

  for (i = 0; i < G_N_ELEMENTS(cover_display); i++)
    cover_display[i] = _(cover_display[i]);
  
  option = gtk_printer_option_new ("gtk-cover-before", "Before", GTK_PRINTER_OPTION_TYPE_PICKONE);
  gtk_printer_option_choices_from_array (option, G_N_ELEMENTS (cover),
					 cover, cover_display);
  gtk_printer_option_set (option, "none");
  set_option_from_settings (option, settings);
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);

  option = gtk_printer_option_new ("gtk-cover-after", "After", GTK_PRINTER_OPTION_TYPE_PICKONE);
  gtk_printer_option_choices_from_array (option, G_N_ELEMENTS (cover),
					 cover, cover_display);
  gtk_printer_option_set (option, "none");
  set_option_from_settings (option, settings);
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);

  option = gtk_printer_option_new ("gtk-print-time", "Print at", GTK_PRINTER_OPTION_TYPE_PICKONE);
  gtk_printer_option_choices_from_array (option, G_N_ELEMENTS (print_at),
					 print_at, print_at);
  gtk_printer_option_set (option, "now");
  set_option_from_settings (option, settings);
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);
  
  option = gtk_printer_option_new ("gtk-print-time-text", "Print at time", GTK_PRINTER_OPTION_TYPE_STRING);
  gtk_printer_option_set (option, "");
  set_option_from_settings (option, settings);
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);
  
  /* Printer (ppd) specific settings */
  ppd_file = gtk_printer_cups_get_ppd (GTK_PRINTER_CUPS (printer));
  if (ppd_file)
    {
      GtkPaperSize *paper_size;
      ppd_option_t *option;
      const gchar  *ppd_name;

      ppdMarkDefaults (ppd_file);

      paper_size = gtk_page_setup_get_paper_size (page_setup);

      option = ppdFindOption (ppd_file, "PageSize");
      ppd_name = gtk_paper_size_get_ppd_name (paper_size);
      
      if (ppd_name)
	strncpy (option->defchoice, ppd_name, PPD_MAX_NAME);
      else
        {
          gchar *custom_name;

	  custom_name = g_strdup_printf ("Custom.%2fx%.2f",
					 gtk_paper_size_get_width (paper_size, GTK_UNIT_POINTS),
					 gtk_paper_size_get_height (paper_size, GTK_UNIT_POINTS));
          strncpy (option->defchoice, custom_name, PPD_MAX_NAME);
          g_free (custom_name);
        }

      for (i = 0; i < ppd_file->num_groups; i++)
        handle_group (set, ppd_file, &ppd_file->groups[i], &ppd_file->groups[i], settings);
    }

  return set;
}


static void
mark_option_from_set (GtkPrinterOptionSet *set,
		      ppd_file_t          *ppd_file,
		      ppd_option_t        *ppd_option)
{
  GtkPrinterOption *option;
  char *name = get_option_name (ppd_option->keyword);

  option = gtk_printer_option_set_lookup (set, name);

  if (option)
    ppdMarkOption (ppd_file, ppd_option->keyword, option->value);
  
  g_free (name);
}


static void
mark_group_from_set (GtkPrinterOptionSet *set,
		     ppd_file_t          *ppd_file,
		     ppd_group_t         *group)
{
  int i;

  for (i = 0; i < group->num_options; i++)
    mark_option_from_set (set, ppd_file, &group->options[i]);

  for (i = 0; i < group->num_subgroups; i++)
    mark_group_from_set (set, ppd_file, &group->subgroups[i]);
}

static void
set_conflicts_from_option (GtkPrinterOptionSet *set,
			   ppd_file_t          *ppd_file,
			   ppd_option_t        *ppd_option)
{
  GtkPrinterOption *option;
  char *name;

  if (ppd_option->conflicted)
    {
      name = get_option_name (ppd_option->keyword);
      option = gtk_printer_option_set_lookup (set, name);

      if (option)
	gtk_printer_option_set_has_conflict (option, TRUE);
#ifdef PRINT_IGNORED_OPTIONS
      else
	g_warning ("CUPS Backend: Ignoring conflict for option %s", ppd_option->keyword);
#endif
      
      g_free (name);
    }
}

static void
set_conflicts_from_group (GtkPrinterOptionSet *set,
			  ppd_file_t          *ppd_file,
			  ppd_group_t         *group)
{
  int i;

  for (i = 0; i < group->num_options; i++)
    set_conflicts_from_option (set, ppd_file, &group->options[i]);

  for (i = 0; i < group->num_subgroups; i++)
    set_conflicts_from_group (set, ppd_file, &group->subgroups[i]);
}

static gboolean
cups_printer_mark_conflicts (GtkPrinter          *printer,
			     GtkPrinterOptionSet *options)
{
  ppd_file_t *ppd_file;
  int num_conflicts;
  int i;
 
  ppd_file = gtk_printer_cups_get_ppd (GTK_PRINTER_CUPS (printer));

  if (ppd_file == NULL)
    return FALSE;

  ppdMarkDefaults (ppd_file);

  for (i = 0; i < ppd_file->num_groups; i++)
    mark_group_from_set (options, ppd_file, &ppd_file->groups[i]);

  num_conflicts = ppdConflicts (ppd_file);

  if (num_conflicts > 0)
    {
      for (i = 0; i < ppd_file->num_groups; i++)
	set_conflicts_from_group (options, ppd_file, &ppd_file->groups[i]);
    }
 
  return num_conflicts > 0;
}

struct OptionData {
  GtkPrinter *printer;
  GtkPrinterOptionSet *options;
  GtkPrintSettings *settings;
  ppd_file_t *ppd_file;
};

typedef struct {
  const char *cups;
  const char *standard;
} NameMapping;

static void
map_settings_to_option (GtkPrinterOption  *option,
			const NameMapping  table[],
			gint               n_elements,
			GtkPrintSettings  *settings,
			const gchar       *standard_name,
			const gchar       *cups_name)
{
  int i;
  char *name;
  const char *cups_value;
  const char *standard_value;

  /* If the cups-specific setting is set, always use that */
  name = g_strdup_printf ("cups-%s", cups_name);
  cups_value = gtk_print_settings_get (settings, name);
  g_free (name);
  
  if (cups_value != NULL) 
    {
      gtk_printer_option_set (option, cups_value);
      return;
    }

  /* Otherwise we try to convert from the general setting */
  standard_value = gtk_print_settings_get (settings, standard_name);
  if (standard_value == NULL)
    return;

  for (i = 0; i < n_elements; i++)
    {
      if (table[i].cups == NULL && table[i].standard == NULL)
	{
	  gtk_printer_option_set (option, standard_value);
	  break;
	}
      else if (table[i].cups == NULL &&
	       strcmp (table[i].standard, standard_value) == 0)
	{
	  set_option_off (option);
	  break;
	}
      else if (strcmp (table[i].standard, standard_value) == 0)
	{
	  gtk_printer_option_set (option, table[i].cups);
	  break;
	}
    }
}

static void
map_option_to_settings (const gchar       *value,
			const NameMapping  table[],
			gint               n_elements,
			GtkPrintSettings  *settings,
			const gchar       *standard_name,
			const gchar       *cups_name)
{
  int i;
  char *name;

  for (i = 0; i < n_elements; i++)
    {
      if (table[i].cups == NULL && table[i].standard == NULL)
	{
	  gtk_print_settings_set (settings,
				  standard_name,
				  value);
	  break;
	}
      else if (table[i].cups == NULL && table[i].standard != NULL)
	{
	  if (value_is_off (value))
	    {
	      gtk_print_settings_set (settings,
				      standard_name,
				      table[i].standard);
	      break;
	    }
	}
      else if (strcmp (table[i].cups, value) == 0)
	{
	  gtk_print_settings_set (settings,
				  standard_name,
				  table[i].standard);
	  break;
	}
    }

  /* Always set the corresponding cups-specific setting */
  name = g_strdup_printf ("cups-%s", cups_name);
  gtk_print_settings_set (settings, name, value);
  g_free (name);
}


static const NameMapping paper_source_map[] = {
  { "Lower", "lower"},
  { "Middle", "middle"},
  { "Upper", "upper"},
  { "Rear", "rear"},
  { "Envelope", "envelope"},
  { "Cassette", "cassette"},
  { "LargeCapacity", "large-capacity"},
  { "AnySmallFormat", "small-format"},
  { "AnyLargeFormat", "large-format"},
  { NULL, NULL}
};

static const NameMapping output_tray_map[] = {
  { "Upper", "upper"},
  { "Lower", "lower"},
  { "Rear", "rear"},
  { NULL, NULL}
};

static const NameMapping duplex_map[] = {
  { "DuplexTumble", "vertical" },
  { "DuplexNoTumble", "horizontal" },
  { NULL, "simplex" }
};

static const NameMapping output_mode_map[] = {
  { "Standard", "normal" },
  { "Normal", "normal" },
  { "Draft", "draft" },
  { "Fast", "draft" },
};

static const NameMapping media_type_map[] = {
  { "Transparency", "transparency"},
  { "Standard", "stationery"},
  { NULL, NULL}
};

static const NameMapping all_map[] = {
  { NULL, NULL}
};


static void
set_option_from_settings (GtkPrinterOption *option,
			  GtkPrintSettings *settings)
{
  const char *cups_value;
  char *value;
  
  if (settings == NULL)
    return;

  if (strcmp (option->name, "gtk-paper-source") == 0)
    map_settings_to_option (option, paper_source_map, G_N_ELEMENTS (paper_source_map),
			     settings, GTK_PRINT_SETTINGS_DEFAULT_SOURCE, "InputSlot");
  else if (strcmp (option->name, "gtk-output-tray") == 0)
    map_settings_to_option (option, output_tray_map, G_N_ELEMENTS (output_tray_map),
			    settings, GTK_PRINT_SETTINGS_OUTPUT_BIN, "OutputBin");
  else if (strcmp (option->name, "gtk-duplex") == 0)
    map_settings_to_option (option, duplex_map, G_N_ELEMENTS (duplex_map),
			    settings, GTK_PRINT_SETTINGS_DUPLEX, "Duplex");
  else if (strcmp (option->name, "cups-OutputMode") == 0)
    map_settings_to_option (option, output_mode_map, G_N_ELEMENTS (output_mode_map),
			    settings, GTK_PRINT_SETTINGS_QUALITY, "OutputMode");
  else if (strcmp (option->name, "cups-Resolution") == 0)
    {
      cups_value = gtk_print_settings_get (settings, option->name);
      if (cups_value)
	gtk_printer_option_set (option, cups_value);
      else
	{
	  int res = gtk_print_settings_get_resolution (settings);
	  if (res != 0)
	    {
	      value = g_strdup_printf ("%ddpi", res);
	      gtk_printer_option_set (option, value);
	      g_free (value);
	    }
	}
    }
  else if (strcmp (option->name, "gtk-paper-type") == 0)
    map_settings_to_option (option, media_type_map, G_N_ELEMENTS (media_type_map),
			    settings, GTK_PRINT_SETTINGS_MEDIA_TYPE, "MediaType");
  else if (strcmp (option->name, "gtk-n-up") == 0)
    {
      map_settings_to_option (option, all_map, G_N_ELEMENTS (all_map),
			      settings, GTK_PRINT_SETTINGS_NUMBER_UP, "number-up");
    }
  else if (strcmp (option->name, "gtk-billing-info") == 0)
    {
      cups_value = gtk_print_settings_get (settings, "cups-job-billing");
      if (cups_value)
	gtk_printer_option_set (option, cups_value);
    } 
  else if (strcmp (option->name, "gtk-job-prio") == 0)
    {
      cups_value = gtk_print_settings_get (settings, "cups-job-priority");
      if (cups_value)
	gtk_printer_option_set (option, cups_value);
    } 
  else if (strcmp (option->name, "gtk-cover-before") == 0)
    {
      cups_value = gtk_print_settings_get (settings, "cover-before");
      if (cups_value)
	gtk_printer_option_set (option, cups_value);
    } 
  else if (strcmp (option->name, "gtk-cover-after") == 0)
    {
      cups_value = gtk_print_settings_get (settings, "cover-after");
      if (cups_value)
	gtk_printer_option_set (option, cups_value);
    } 
  else if (strcmp (option->name, "gtk-print-time") == 0)
    {
      cups_value = gtk_print_settings_get (settings, "print-at");
      if (cups_value)
	gtk_printer_option_set (option, cups_value);
    } 
  else if (strcmp (option->name, "gtk-print-time-text") == 0)
    {
      cups_value = gtk_print_settings_get (settings, "print-at-time");
      if (cups_value)
	gtk_printer_option_set (option, cups_value);
    } 
  else if (g_str_has_prefix (option->name, "cups-"))
    {
      cups_value = gtk_print_settings_get (settings, option->name);
      if (cups_value)
	gtk_printer_option_set (option, cups_value);
    } 
}

static void
foreach_option_get_settings (GtkPrinterOption *option,
			     gpointer          user_data)
{
  struct OptionData *data = user_data;
  GtkPrintSettings *settings = data->settings;
  const char *value;

  value = option->value;

  if (strcmp (option->name, "gtk-paper-source") == 0)
    map_option_to_settings (value, paper_source_map, G_N_ELEMENTS (paper_source_map),
			    settings, GTK_PRINT_SETTINGS_DEFAULT_SOURCE, "InputSlot");
  else if (strcmp (option->name, "gtk-output-tray") == 0)
    map_option_to_settings (value, output_tray_map, G_N_ELEMENTS (output_tray_map),
			    settings, GTK_PRINT_SETTINGS_OUTPUT_BIN, "OutputBin");
  else if (strcmp (option->name, "gtk-duplex") == 0)
    map_option_to_settings (value, duplex_map, G_N_ELEMENTS (duplex_map),
			    settings, GTK_PRINT_SETTINGS_DUPLEX, "Duplex");
  else if (strcmp (option->name, "cups-OutputMode") == 0)
    map_option_to_settings (value, output_mode_map, G_N_ELEMENTS (output_mode_map),
			    settings, GTK_PRINT_SETTINGS_QUALITY, "OutputMode");
  else if (strcmp (option->name, "cups-Resolution") == 0)
    {
      int res = atoi (value);
      /* TODO: What if resolution is on XXXxYYYdpi form? */
      if (res != 0)
	gtk_print_settings_set_resolution (settings, res);
      gtk_print_settings_set (settings, option->name, value);
    }
  else if (strcmp (option->name, "gtk-paper-type") == 0)
    map_option_to_settings (value, media_type_map, G_N_ELEMENTS (media_type_map),
			    settings, GTK_PRINT_SETTINGS_MEDIA_TYPE, "MediaType");
  else if (strcmp (option->name, "gtk-n-up") == 0)
    map_option_to_settings (value, all_map, G_N_ELEMENTS (all_map),
			    settings, GTK_PRINT_SETTINGS_NUMBER_UP, "number-up");
  else if (strcmp (option->name, "gtk-billing-info") == 0 && strlen (value) > 0)
    gtk_print_settings_set (settings, "cups-job-billing", value);
  else if (strcmp (option->name, "gtk-job-prio") == 0)
    gtk_print_settings_set (settings, "cups-job-priority", value);
  else if (strcmp (option->name, "gtk-cover-before") == 0)
    gtk_print_settings_set (settings, "cover-before", value);
  else if (strcmp (option->name, "gtk-cover-after") == 0)
    gtk_print_settings_set (settings, "cover-after", value);
  else if (strcmp (option->name, "gtk-print-time") == 0)
    gtk_print_settings_set (settings, "print-at", value);
  else if (strcmp (option->name, "gtk-print-time-text") == 0)
    gtk_print_settings_set (settings, "print-at-time", value);
  else if (g_str_has_prefix (option->name, "cups-"))
    gtk_print_settings_set (settings, option->name, value);
}

static void
cups_printer_get_settings_from_options (GtkPrinter          *printer,
					GtkPrinterOptionSet *options,
					GtkPrintSettings    *settings)
{
  struct OptionData data;
  const char *print_at, *print_at_time;

  data.printer = printer;
  data.options = options;
  data.settings = settings;
  data.ppd_file = gtk_printer_cups_get_ppd (GTK_PRINTER_CUPS (printer));
 
  if (data.ppd_file != NULL)
    {
      GtkPrinterOption *cover_before, *cover_after;
      
      gtk_printer_option_set_foreach (options, foreach_option_get_settings, &data);

      cover_before = gtk_printer_option_set_lookup (options, "gtk-cover-before");
      cover_after = gtk_printer_option_set_lookup (options, "gtk-cover-after");
      if (cover_before && cover_after)
	{
	  char *value = g_strdup_printf ("%s,%s", cover_before->value, cover_after->value);
	  gtk_print_settings_set (settings, "cups-job-sheets", value);
	  g_free (value);
	}

      print_at = gtk_print_settings_get (settings, "print-at");
      print_at_time = gtk_print_settings_get (settings, "print-at-time");
      if (strcmp (print_at, "at") == 0)
	gtk_print_settings_set (settings, "cups-job-hold-until", print_at_time);
      else if (strcmp (print_at, "on-hold") == 0)
	gtk_print_settings_set (settings, "cups-job-hold-until", "indefinite");
    }
}

static void
cups_printer_prepare_for_print (GtkPrinter       *printer,
				GtkPrintJob      *print_job,
				GtkPrintSettings *settings,
				GtkPageSetup     *page_setup)
{
  GtkPageSet page_set;
  GtkPaperSize *paper_size;
  const char *ppd_paper_name;
  double scale;

  print_job->print_pages = gtk_print_settings_get_print_pages (settings);
  print_job->page_ranges = NULL;
  print_job->num_page_ranges = 0;
  
  if (print_job->print_pages == GTK_PRINT_PAGES_RANGES)
    print_job->page_ranges =
      gtk_print_settings_get_page_ranges (settings,
					  &print_job->num_page_ranges);
  
  if (gtk_print_settings_get_collate (settings))
    gtk_print_settings_set (settings, "cups-Collate", "True");
  print_job->collate = FALSE;

  if (gtk_print_settings_get_reverse (settings))
    gtk_print_settings_set (settings, "cups-OutputOrder", "Reverse");
  print_job->reverse = FALSE;

  if (gtk_print_settings_get_n_copies (settings) > 1)
    gtk_print_settings_set_int (settings, "cups-copies",
				gtk_print_settings_get_n_copies (settings));
  print_job->num_copies = 1;

  scale = gtk_print_settings_get_scale (settings);
  print_job->scale = 1.0;
  if (scale != 100.0)
    print_job->scale = scale/100.0;

  page_set = gtk_print_settings_get_page_set (settings);
  if (page_set == GTK_PAGE_SET_EVEN)
    gtk_print_settings_set (settings, "cups-page-set", "even");
  else if (page_set == GTK_PAGE_SET_ODD)
    gtk_print_settings_set (settings, "cups-page-set", "odd");
  print_job->page_set = GTK_PAGE_SET_ALL;

  paper_size = gtk_page_setup_get_paper_size (page_setup);
  ppd_paper_name = gtk_paper_size_get_ppd_name (paper_size);
  if (ppd_paper_name != NULL)
    gtk_print_settings_set (settings, "cups-PageSize", ppd_paper_name);
  else
    {
      char *custom_name = g_strdup_printf ("Custom.%2fx%.2f",
					   gtk_paper_size_get_width (paper_size, GTK_UNIT_POINTS),
					   gtk_paper_size_get_height (paper_size, GTK_UNIT_POINTS));
      gtk_print_settings_set (settings, "cups-PageSize", custom_name);
      g_free (custom_name);
    }

  print_job->rotate_to_orientation = TRUE;
}

static GList *
cups_printer_list_papers (GtkPrinter *printer)
{
  ppd_file_t *ppd_file;
  ppd_size_t *size;
  char *display_name;
  GtkPageSetup *page_setup;
  GtkPaperSize *paper_size;
  ppd_option_t *option;
  ppd_choice_t *choice;
  GList *l;
  int i;

  ppd_file = gtk_printer_cups_get_ppd (GTK_PRINTER_CUPS (printer));
  if (ppd_file == NULL)
    return NULL;

  l = NULL;
  
  for (i = 0; i < ppd_file->num_sizes; i++)
    {
      size = &ppd_file->sizes[i];

      display_name = NULL;
      option = ppdFindOption (ppd_file, "PageSize");
      if (option)
	{
	  choice = ppdFindChoice (option, size->name);
	  if (choice)
	    display_name = ppd_text_to_utf8 (ppd_file, choice->text);
	}
      if (display_name == NULL)
	display_name = g_strdup (size->name);

      page_setup = gtk_page_setup_new ();
      paper_size = gtk_paper_size_new_from_ppd (size->name,
						display_name,
						size->width,
						size->length);
      gtk_page_setup_set_paper_size (page_setup, paper_size);
      gtk_paper_size_free (paper_size);

      gtk_page_setup_set_top_margin (page_setup, size->length - size->top, GTK_UNIT_POINTS);
      gtk_page_setup_set_bottom_margin (page_setup, size->bottom, GTK_UNIT_POINTS);
      gtk_page_setup_set_left_margin (page_setup, size->left, GTK_UNIT_POINTS);
      gtk_page_setup_set_right_margin (page_setup, size->width - size->right, GTK_UNIT_POINTS);
	
      g_free (display_name);

      l = g_list_prepend (l, page_setup);
    }

  return g_list_reverse (l);
}

static void
cups_printer_get_hard_margins (GtkPrinter *printer,
			       gdouble    *top,
			       gdouble    *bottom,
			       gdouble    *left,
			       gdouble    *right)
{
  ppd_file_t *ppd_file;

  ppd_file = gtk_printer_cups_get_ppd (GTK_PRINTER_CUPS (printer));
  if (ppd_file == NULL)
    return;

  *left = ppd_file->custom_margins[0];
  *bottom = ppd_file->custom_margins[1];
  *right = ppd_file->custom_margins[2];
  *top = ppd_file->custom_margins[3];
}

static GtkPrintCapabilities
cups_printer_get_capabilities (GtkPrinter *printer)
{
  return
    GTK_PRINT_CAPABILITY_COPIES |
    GTK_PRINT_CAPABILITY_COLLATE |
    GTK_PRINT_CAPABILITY_REVERSE;
}
