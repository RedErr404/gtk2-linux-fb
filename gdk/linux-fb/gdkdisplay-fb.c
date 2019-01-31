/* GDK - The GIMP Drawing Kit
 * Copyright (C) 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Modified by the GTK+ Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GTK+ Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GTK+ at ftp://ftp.gtk.org/pub/gtk/. 
 */

#include <config.h>
#include "gdk.h"
#include "gdkprivate-fb.h"
#include "gdkalias.h"

GdkDisplay *
gdk_display_open (const gchar *display_name)
{
  if (gdk_display == NULL || _gdk_display != NULL)
    return NULL; /* single display only */

  _gdk_display = g_object_new (GDK_TYPE_DISPLAY, NULL);
  _gdk_screen = g_object_new (GDK_TYPE_SCREEN, NULL);

  _gdk_visual_init ();
  gdk_screen_set_default_colormap (_gdk_screen,
                                   gdk_screen_get_system_colormap (_gdk_screen));
  _gdk_windowing_window_init ();
  _gdk_windowing_image_init ();
  _gdk_events_init ();
  _gdk_input_init ();
  _gdk_dnd_init ();

  g_signal_emit_by_name (gdk_display_manager_get (),
			 "display_opened", _gdk_display);

  return _gdk_display;
}

G_CONST_RETURN gchar *
gdk_display_get_name (GdkDisplay *display)
{
  return gdk_get_display_arg_name ();
}

int
gdk_display_get_n_screens (GdkDisplay *display)
{
  return 1;
}

GdkScreen *
gdk_display_get_screen (GdkDisplay *display,
			gint        screen_num)
{
  return _gdk_screen;
}

GdkScreen *
gdk_display_get_default_screen (GdkDisplay *display)
{
  return _gdk_screen;
}

void
gdk_display_sync (GdkDisplay *display)
{
}

void
gdk_display_flush (GdkDisplay * display)
{
}

gboolean
gdk_display_supports_cursor_alpha (GdkDisplay *display)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), FALSE);
  return TRUE;
}

gboolean
gdk_display_supports_cursor_color (GdkDisplay *display)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), FALSE);
  return TRUE;
}

gboolean
gdk_display_supports_clipboard_persistence (GdkDisplay *display)
{
  g_warning("gdk_display_supports_clipboard_persistence Unimplemented function \n");
  return FALSE;
}

void
gdk_display_get_maximal_cursor_size (GdkDisplay *display,
                                     guint       *width,
                                     guint       *height)
{
  g_return_if_fail (GDK_IS_DISPLAY (display));

  /* Cursor sizes in DirectFB can be large (4095x4095), but we limit this to
     128x128 for max compatibility with the x11 backend. */
  *width  = 128;
  *height = 128;
}

gboolean
gdk_display_supports_selection_notification (GdkDisplay *display)
{
  return FALSE;
}

gboolean gdk_display_request_selection_notification  (GdkDisplay *display,
                                                      GdkAtom     selection)

{
  g_warning("gdk_display_request_selection_notification Unimplemented function \n");
  return FALSE;
}

void
gdk_display_warp_pointer (GdkDisplay *display,
                          GdkScreen  *screen,
                          gint        x,
                          gint        y)
{
  g_warning ("gdk_display_warp_pointer() not implemented.\n");
}

void
gdk_display_store_clipboard  (GdkDisplay *display,
						     GdkWindow  *clipboard_window,
						     guint32     time_,
						     GdkAtom    *targets,
						     gint        n_targets)
{

  g_warning("gdk_display_store_clipboard Unimplemented function \n");

}


#define __GDK_DISPLAY_FB_C__
#include "gdkaliasdef.c"

