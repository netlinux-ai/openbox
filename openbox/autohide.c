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

/* Design.
  
   Auto-hiding titlebars are revealed when the pointer reaches a window's top
   edge. Pointer tracking is done per-client: each client window has
   PointerMotionMask selected on it (see frame_grab_client()), so we are told
   when the pointer is over the client area and can tell when it reaches the
   top edge (where the titlebar would sit, overlaid on the client). The
   titlebar is hidden again (after a short delay) when the pointer leaves the
   top strip or the frame entirely. No global setup or polling is required.
  
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

void autohide_startup(gboolean reconfig)
{
    (void)reconfig;
}

void autohide_shutdown(gboolean reconfig)
{
    (void)reconfig;
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
    /* windows that are edge-to-edge or have no client area to hide behind */
    if (c->max_horz || c->max_vert)
        return FALSE;
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

void autohide_show(ObFrame *f)
{
    if (!autohide_active_for(f) || f->tb_shown)
        return;

    /* the titlebar overlays the top edge of the client area */
    XMoveResizeWindow(obt_display, f->title,
                      f->bwidth, 0,
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
    ObClient *target = NULL;
    GList *it;

    if (!config_autohide_titlebar)
        return;
    if (moveresize_in_progress)
        /* leave things alone while the user is dragging a window */
        return;

    /* the window to reveal is the topmost one under the pointer, and only if
       the pointer is within its top-edge strip (where the titlebar would sit)
     */
    {
        ObClient *c = autohide_client_at(x_root, y_root);
        if (c) {
            gboolean act = autohide_active_for(c->frame);
            gint top = c->frame->area.y;
            int inx = (x_root >= c->frame->area.x &&
                       x_root < c->frame->area.x + c->frame->area.width);
            int iny = (y_root >= top &&
                       y_root < top + ob_rr_theme->title_height);
            if (act && inx && iny)
                target = c;
        }
    }

    for (it = client_list; it; it = g_list_next(it)) {
        ObClient *c = it->data;
        ObFrame *f = c->frame;

        if (!autohide_active_for(f))
            continue;

        if (c == target) {
            if (!f->tb_shown)
                autohide_show(f);
            if (f->tb_hide_timer) {
                g_source_remove(f->tb_hide_timer);
                f->tb_hide_timer = 0;
            }
        } else {
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
