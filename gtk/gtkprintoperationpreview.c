/* GTK - The GIMP Toolkit
 * gtkprintoperationpreview.c: Abstract print preview interface
 * Copyright (C) 2006, Red Hat, Inc.
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

#include "config.h"

#include "gtkprintoperationpreview.h"
#include "gtkmarshalers.h"
#include "gtkintl.h"
#include "gtkalias.h"


static void gtk_print_operation_preview_base_init (gpointer g_iface);

GType
gtk_print_operation_preview_get_type (void)
{
  static GType print_operation_preview_type = 0;

  if (!print_operation_preview_type)
    {
      const GTypeInfo print_operation_preview_info =
      {
        sizeof (GtkPrintOperationPreviewIface), /* class_size */
	gtk_print_operation_preview_base_init,   /* base_init */
	NULL,		/* base_finalize */
	NULL,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      print_operation_preview_type =
	g_type_register_static (G_TYPE_INTERFACE, g_intern_static_string("GtkPrintOperationPreview"),
				&print_operation_preview_info, 0);

      g_type_interface_add_prerequisite (print_operation_preview_type, G_TYPE_OBJECT);
    }

  return print_operation_preview_type;
}

static void
gtk_print_operation_preview_base_init (gpointer g_iface)
{
  static gboolean initialized = FALSE;

  if (!initialized)
    {
      g_signal_new (g_intern_static_string("ready"),
		    GTK_TYPE_PRINT_OPERATION_PREVIEW,
		    G_SIGNAL_RUN_LAST,
		    G_STRUCT_OFFSET (GtkPrintOperationPreviewIface, ready),
		    NULL, NULL,
		    g_cclosure_marshal_VOID__OBJECT,
		    G_TYPE_NONE, 1,
		    GTK_TYPE_PRINT_CONTEXT);

      g_signal_new (g_intern_static_string("got-page-size"),
		    GTK_TYPE_PRINT_OPERATION_PREVIEW,
		    G_SIGNAL_RUN_LAST,
		    G_STRUCT_OFFSET (GtkPrintOperationPreviewIface, got_page_size),
		    NULL, NULL,
		    _gtk_marshal_VOID__OBJECT_OBJECT,
		    G_TYPE_NONE, 2,
		    GTK_TYPE_PRINT_CONTEXT,
		    GTK_TYPE_PAGE_SETUP);

      initialized = TRUE;
    }
}

/**
 * gtk_print_operation_preview_render_page:
 * @preview: a #GtkPrintOperationPreview
 * @page_nr: the page to render
 *
 * Renders a page to the preview, using the print context that
 * was passed to the GtkPrintOperation::preview handler together
 * with @preview.
 *
 * Note that this function requires a suitable cairo context to 
 * be associated with the print context. 
 *
 * Since: 2.10 
 */
void    
gtk_print_operation_preview_render_page (GtkPrintOperationPreview *preview,
					 gint			   page_nr)
{
  g_return_if_fail (GTK_IS_PRINT_OPERATION_PREVIEW (preview));

  GTK_PRINT_OPERATION_PREVIEW_GET_IFACE (preview)->render_page (preview,
								page_nr);
}

/**
 * gtk_print_operation_preview_end_preview:
 * @preview: a #GtkPrintOperationPreview
 *
 * Ends a preview. 
 *
 * This function must be called to finish a custom print preview.
 *
 * Since: 2.10
 */
void
gtk_print_operation_preview_end_preview (GtkPrintOperationPreview *preview)
{
  g_return_if_fail (GTK_IS_PRINT_OPERATION_PREVIEW (preview));

  GTK_PRINT_OPERATION_PREVIEW_GET_IFACE (preview)->end_preview (preview);
}

/**
 * gtk_print_operation_preview_is_selected:
 * @preview: a #GtkPrintOperationPreview
 * @page_nr: a page number
 *
 * Returns whether the given page is included in the set of pages that
 * have been selected for printing.
 * 
 * Returns: %TRUE if the page has been selected for printing
 *
 * Since: 2.10
 */
gboolean
gtk_print_operation_preview_is_selected (GtkPrintOperationPreview *preview,
					 gint                      page_nr)
{
  g_return_val_if_fail (GTK_IS_PRINT_OPERATION_PREVIEW (preview), FALSE);

  return GTK_PRINT_OPERATION_PREVIEW_GET_IFACE (preview)->is_selected (preview, page_nr);
}


#define __GTK_PRINT_OPERATION_PREVIEW_C__
#include "gtkaliasdef.c"
