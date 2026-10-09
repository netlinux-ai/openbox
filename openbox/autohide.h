/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   autohide.h for the Openbox window manager

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
 */

#ifndef __autohide_h
#define __autohide_h

#include <glib.h>

struct _ObClient;
struct _ObFrame;

void autohide_startup(gboolean reconfig);
void autohide_shutdown(gboolean reconfig);

/*! Should auto-hiding titlebars apply to this frame right now? That is, the
  feature is enabled and the client is in a state where the titlebar can be
  hidden behind its client area. */
gboolean autohide_active_for(struct _ObFrame *f);

/*! Reveal the frame's titlebar as an overlay on top of the client area,
  right now, without any delay. */
void autohide_show(struct _ObFrame *f);
/*! Hide the frame's titlebar overlay right now, if it is visible or about to
   be hidden. */
void autohide_hide(struct _ObFrame *f);

/*! The pointer has left the frame entirely. If the titlebar is revealed,
   schedule it to be hidden after the configured delay. */
void autohide_frame_leave(struct _ObFrame *f);

/*! The pointer has moved to (x_root, y_root). Decide which window, if any,
  should have its titlebar revealed, and schedule auto-hiding for the others. */
void autohide_pointer_moved(gint x_root, gint y_root);

/*! A client's state has changed (mapped, maximized, shaded, moved of desktop,
   unmanaged, ...). Hide its titlebar if it is no longer eligible. */
void autohide_client_update(struct _ObClient *c);

/* TEMPORARY diagnostic logging (remove before final). */
void ahlog_ext(const char *fmt, ...);

#endif
