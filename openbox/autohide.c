/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   autohide.c for the Openbox window manager

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

#include "autohide.h"
#include "client.h"
#include "config.h"
#include "frame.h"
#include "framerender.h"
#include "moveresize.h"
#include "screen.h"
#include "stacking.h"
#include "window.h"
#include "openbox.h"
#include "obrender/theme.h"
#include "obt/display.h"

/* How far down from the very top edge of the client area the pointer must be
   to reveal the titlebar. Kept thin so that client content sitting just
   beneath the (hidden) titlebar remains clickable; once the titlebar is
   revealed, hovering anywhere over it keeps it up (see
   autohide_pointer_moved()). */
#define AUTOHIDE_REVEAL_STRIP 5

/* Design.
  
   Auto-hiding titlebars are hidden by default, with the client filling to the
   top edge of the frame. The titlebar can be revealed from the top edge, the
   bottom edge, or either (config_autohide_titlebar_edges); it is the same
   titlebar window either way, just anchored to the edge it was revealed from
   (frame->tb_edge, autohide_title_y()). Everything below describes the top
   edge; the bottom edge mirrors it, measured up from the bottom of the client
   area. Pointer tracking is done per-client: each client
   window has PointerMotionMask selected on it (see frame_grab_client()), so we
   are told when the pointer is over the client area and where.
  
   There are two zones at the top edge (autohide_pointer_moved()):
   1. a thin strip (AUTOHIDE_REVEAL_STRIP px) right at the very top edge --
      the pointer being here reveals the titlebar. It is deliberately thin so
      that client content sitting just beneath the hidden titlebar stays
      clickable;
   2. the titlebar band beneath it -- once the titlebar is revealed, the
      pointer being anywhere here keeps it up, so the user can move onto the
      titlebar and use its buttons.
   The titlebar is hidden again (after a short delay) when the pointer moves
   away from both zones or leaves the frame entirely. No global setup or
   polling is required.
  
   Why motion rather than Enter/Leave on the client? An Enter/Leave on the
   client window fires when the pointer crosses into/out of the whole client
   area, not just the top strip, so it cannot by itself tell us the pointer is
   in the titlebar band. A dedicated top-strip window would be needed, which
   would have to be input-only (to stay invisible); some X servers/VNC setups
   deliver no pointer events to input-only windows at all. Selecting motion on
   the client is the simplest approach that works across ordinary X servers.
  
   Testing caveat: some minimal/virtual X servers (e.g. the Xvnc used for CI
   here) do NOT deliver MotionNotify for XWarpPointer moves, deliver no
   pointer events to input-only windows, and lack a working XTEST extension.
   On such a display the reveal cannot be triggered by automation, even though
   the code is correct. Verify on a display with a working pointer-motion
   path (a normal local or full VNC X server with XTEST). */

/* Polling.

   Per-client MotionNotify is not enough on its own: toolkits that use XInput2
   for pointer input (GTK3) select XI2 motion on their window, and the X server
   then delivers the pointer motion to them as XI2 *instead of* as the core
   MotionNotify we select, so such clients never tell us where the pointer is.
   So while the pointer is over a window we also sample it with a short timer.
   It is started by an EnterNotify on any frame (core crossing events on the
   frame are unaffected) and stops itself once the pointer is not over any
   window and no titlebar is revealed, so nothing runs while idle. */
#define AUTOHIDE_POLL_INTERVAL 50

static guint poll_timer = 0;
static gint poll_x = -1, poll_y = -1;

static ObClient *autohide_client_at(gint x_root, gint y_root);

static gboolean poll_pointer(gpointer data)
{
    Window root, child;
    gint x, y, wx, wy;
    guint mask;
    gboolean keep;
    GList *it;

    if (!config_autohide_titlebar) {
        poll_timer = 0;
        return FALSE;
    }

    if (!XQueryPointer(obt_display, RootWindow(obt_display, ob_screen),
                       &root, &child, &x, &y, &wx, &wy, &mask))
    {
        poll_timer = 0;
        return FALSE;
    }

    if (x != poll_x || y != poll_y) {
        poll_x = x;
        poll_y = y;
        autohide_pointer_moved(x, y);
    }

    /* carry on while the pointer is over a window or a titlebar is up (so
       that it still gets hidden) */
    keep = autohide_client_at(x, y) != NULL;
    for (it = client_list; !keep && it; it = g_list_next(it))
        if (((ObClient*)it->data)->frame->tb_shown)
            keep = TRUE;

    if (!keep)
        poll_timer = 0;
    return keep;
}

void autohide_poll_start(void)
{
    if (poll_timer || !config_autohide_titlebar)
        return;
    poll_x = poll_y = -1;
    poll_timer = g_timeout_add(AUTOHIDE_POLL_INTERVAL, poll_pointer, NULL);
}

void autohide_startup(gboolean reconfig)
{
    (void)reconfig;
    /* the pointer may already be over a window (e.g. after a reconfigure) */
    autohide_poll_start();
}

void autohide_shutdown(gboolean reconfig)
{
    (void)reconfig;
    if (poll_timer) {
        g_source_remove(poll_timer);
        poll_timer = 0;
    }
}

gboolean autohide_active_for(ObFrame *f)
{
    ObClient *c;

    if (!config_autohide_titlebar)
        return FALSE;
    if (!f || !f->client)
        return FALSE;
    c = f->client;

    /* only decorate windows that would normally show a titlebar */
    if (!(c->decorations & OB_FRAME_DECOR_TITLEBAR) || c->undecorated)
        return FALSE;
    /* maximised windows keep the autohide titlebar too: the client still
       fills the work area (edge to edge), so the titlebar simply overlays
       the very top of it and is revealed from the top edge of the screen. */
    if (c->fullscreen || c->shaded || c->iconic)
        return FALSE;
    /* must be tall enough for the titlebar to sit on top of the client */
    if (c->area.height < ob_rr_theme->title_height)
        return FALSE;
    /* only on the current desktop (mirrors client_under_pointer) */
    if (c->desktop != screen_desktop && c->desktop != DESKTOP_ALL)
        return FALSE;

    return TRUE;
}

static gboolean tb_hide_timeout(gpointer data)
{
    ObFrame *f = data;

    f->tb_hide_timer = 0;
    autohide_hide(f);
    return FALSE; /* one-shot */
}

gint autohide_title_y(ObFrame *f)
{
    if (f->tb_edge == OB_AUTOHIDE_EDGE_BOTTOM)
        /* along the bottom of the client area */
        return f->size.top + f->client->area.height -
               ob_rr_theme->title_height;
    return 0;
}

void autohide_show(ObFrame *f, ObAutohideEdge edge)
{
    if (!autohide_active_for(f) || (f->tb_shown && f->tb_edge == edge))
        return;

    f->tb_edge = edge;
    /* the titlebar overlays an edge of the client area; maximised
       (horizontally) windows are edge to edge, so start at x=0 like the
       normal (non-autohide) titlebar does. */
    XMoveResizeWindow(obt_display, f->title,
                      (f->max_horz ? 0 : f->bwidth), autohide_title_y(f),
                      f->width, ob_rr_theme->title_height);
    /* bring the titlebar (and its button children) above the client */
    XRaiseWindow(obt_display, f->title);
    f->tb_shown = TRUE;
    if (f->tb_hide_timer) {
        g_source_remove(f->tb_hide_timer);
        f->tb_hide_timer = 0;
    }
    /* make sure the overlay is painted before it is mapped */
    f->need_render = TRUE;
    framerender_frame(f);
    XMapWindow(obt_display, f->title);
}

void autohide_hide(ObFrame *f)
{
    if (!f || !f->tb_shown)
        return;

    f->tb_shown = FALSE;
    if (f->tb_hide_timer) {
        g_source_remove(f->tb_hide_timer);
        f->tb_hide_timer = 0;
    }
    XUnmapWindow(obt_display, f->title);
}

/* The pointer has left the frame entirely (a LeaveNotify on the frame
   window). This covers the case where the pointer was in the top strip and
   then moved straight out of the window, so no "below the strip" motion event
   was seen. Begin hiding the titlebar after the configured delay. */
void autohide_frame_leave(ObFrame *f)
{
    if (!f || !f->tb_shown || f->tb_hide_timer)
        return;
    if (!autohide_active_for(f))
        return;
    f->tb_hide_timer = g_timeout_add(config_autohide_titlebar_hide_delay,
                                     tb_hide_timeout, f);
}

/* Find the topmost client (top to bottom) whose frame area contains
   (x_root, y_root). Mirrors client_under_pointer() but uses the given
   coordinates so we avoid an extra XQueryPointer round trip per motion. */
static ObClient *autohide_client_at(gint x_root, gint y_root)
{
    GList *it;

    for (it = stacking_list; it; it = g_list_next(it)) {
        if (WINDOW_IS_CLIENT(it->data)) {
            ObClient *c = WINDOW_AS_CLIENT(it->data);
            if (c->frame->visible &&
                (c->desktop == screen_desktop ||
                 c->desktop == DESKTOP_ALL) &&
                !frame_iconify_animating(c->frame) &&
                RECT_CONTAINS(c->frame->area, x_root, y_root))
            {
                return c;
            }
        }
    }
    return NULL;
}

void autohide_pointer_moved(gint x_root, gint y_root)
{
    ObClient *reveal = NULL;  /* topmost client whose reveal strip has the pointer */
    ObAutohideEdge reveal_edge = OB_AUTOHIDE_EDGE_TOP; /* ...and which edge */
    ObClient *onbar  = NULL;  /* topmost client whose (revealed) titlebar has it */
    GList *it;

    if (!config_autohide_titlebar)
        return;
    if (moveresize_in_progress)
        /* leave things alone while the user is dragging a window */
        return;

    /* The pointer is over the topmost client under it. There are two zones at
       the top edge of that client: a thin strip right at the very top edge
       (which reveals the titlebar) and, beneath it, the titlebar band (which,
       once the titlebar is up, is a "keep it shown" zone so the user can move
       the pointer onto the titlebar and use it). */
    {
        ObClient *c = autohide_client_at(x_root, y_root);
        if (c && autohide_active_for(c->frame)) {
            ObFrame *f = c->frame;
            gint th = ob_rr_theme->title_height;
            gint top = f->area.y;
            /* the bottom edge is that of the client area: that is where the
               overlay sits, and the client window is what reports motion */
            gint bottom = c->area.y + c->area.height;
            int inx = (x_root >= f->area.x &&
                       x_root < f->area.x + f->area.width);
            if (inx) {
                if ((config_autohide_titlebar_edges & OB_AUTOHIDE_EDGE_TOP) &&
                    y_root >= top && y_root < top + AUTOHIDE_REVEAL_STRIP)
                {
                    reveal = c;
                    reveal_edge = OB_AUTOHIDE_EDGE_TOP;
                }
                else if ((config_autohide_titlebar_edges &
                          OB_AUTOHIDE_EDGE_BOTTOM) &&
                         y_root < bottom &&
                         y_root >= bottom - AUTOHIDE_REVEAL_STRIP)
                {
                    reveal = c;
                    reveal_edge = OB_AUTOHIDE_EDGE_BOTTOM;
                }
                else if (f->tb_shown &&
                         (f->tb_edge & config_autohide_titlebar_edges))
                {
                    /* over the band the revealed titlebar occupies */
                    if (f->tb_edge == OB_AUTOHIDE_EDGE_BOTTOM
                        ? (y_root < bottom && y_root >= bottom - th)
                        : (y_root >= top && y_root < top + th))
                        onbar = c;
                }
            }
        }
    }

    for (it = client_list; it; it = g_list_next(it)) {
        ObClient *c = it->data;
        ObFrame *f = c->frame;

        if (!autohide_active_for(f))
            continue;

        if (c == reveal) {
            /* pointer is at the very top edge: reveal (and keep revealed) */
            autohide_show(f, reveal_edge);
            if (f->tb_hide_timer) {
                g_source_remove(f->tb_hide_timer);
                f->tb_hide_timer = 0;
            }
        } else if (c == onbar) {
            /* pointer is over the revealed titlebar: keep it up so it can be
               clicked; do nothing if it is not currently shown */
            if (f->tb_shown && f->tb_hide_timer) {
                g_source_remove(f->tb_hide_timer);
                f->tb_hide_timer = 0;
            }
        } else {
            /* pointer is away from the top edge and the titlebar: hide after
               the configured delay */
            if (f->tb_shown && !f->tb_hide_timer) {
                f->tb_hide_timer = g_timeout_add(
                                        config_autohide_titlebar_hide_delay,
                                        tb_hide_timeout, f);
            }
        }
    }
}

void autohide_client_update(ObClient *c)
{
    if (!c || !c->frame)
        return;
    if (c->frame->tb_shown && !autohide_active_for(c->frame))
        autohide_hide(c->frame);
}
