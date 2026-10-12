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
#include <X11/Xlib.h>
#include "frame.h"

struct _ObClient;
struct _ObFrame;

/*! The edges of a window's client area that an auto-hiding titlebar can be
  revealed from. A bitmask, so the configuration can allow several at once. */
typedef enum {
    OB_AUTOHIDE_EDGE_TOP    = 1 << 0,
    OB_AUTOHIDE_EDGE_BOTTOM = 1 << 1
} ObAutohideEdge;

void autohide_startup(gboolean reconfig);
void autohide_shutdown(gboolean reconfig);

/*! Should auto-hiding titlebars apply to this frame right now? That is, the
  feature is enabled and the client is in a state where the titlebar can be
  hidden behind its client area. */
gboolean autohide_active_for(struct _ObFrame *f);

/*! Reveal the frame's titlebar as an overlay on top of the client area, at
  the given edge of it, right now, without any delay. If the titlebar is
  already revealed at another edge it is moved to this one. */
void autohide_show(struct _ObFrame *f, ObAutohideEdge edge);

/*! The y position, relative to the frame window, of the frame's titlebar
  overlay: at the top of the client area or, if it was revealed from the
  bottom edge, along the bottom of it. */
gint autohide_title_y(struct _ObFrame *f);
/*! Hide the frame's titlebar overlay right now, if it is visible or about to
   be hidden. */
void autohide_hide(struct _ObFrame *f);

/*! The pointer has left the frame entirely. If the titlebar is revealed,
   schedule it to be hidden after the configured delay. */
void autohide_frame_leave(struct _ObFrame *f);

/*! Position and map/unmap the frame's invisible reveal strips (one along each
  edge of the client area the titlebar can be revealed from) to match its
  current state. The strips exist only while the titlebar is hidden there. */
void autohide_update_strips(struct _ObFrame *f);

/*! A crossing event happened on window `win` of the frame. If it is one of
  the frame's reveal strips, handle it (entering one reveals the titlebar) and
  return TRUE; the caller should then not process the event any further. */
gboolean autohide_strip_crossing(struct _ObFrame *f, Window win,
                                 gboolean enter);

/*! The pointer entered or left part of the frame's titlebar, which has the
  given context. While the titlebar is revealed this keeps it up (entering) or
  begins the hide delay (leaving). */
void autohide_titlebar_crossing(struct _ObFrame *f, ObFrameContext con,
                                gboolean enter);

/*! A client's state has changed (mapped, maximized, shaded, moved of desktop,
   unmanaged, ...). Hide its titlebar if it is no longer eligible. */
void autohide_client_update(struct _ObClient *c);

#endif
