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
#include "focus.h"
#include "frame.h"
#include "framerender.h"
#include "moveresize.h"
#include "screen.h"
#include "stacking.h"
#include "window.h"
#include "openbox.h"
#include "obrender/theme.h"
#include "obt/display.h"

/* Thickness, in pixels, of the invisible reveal strips along the edges of the
   client area (see autohide_update_strips()). Kept thin so that client content
   sitting beneath them stays usable. */
#define AUTOHIDE_REVEAL_STRIP 2

/* Design.

   Auto-hiding titlebars are hidden by default, with the client filling to the
   edge of the frame. The titlebar can be revealed from the top edge, the
   bottom edge, or either (config_autohide_titlebar_edges); it is the same
   titlebar window either way, just anchored to the edge it was revealed from
   (frame->tb_edge, autohide_title_y()).

   The pointer is tracked with core crossing events on windows that openbox
   itself owns, so no client can interfere with it:
   1. the pointer leaving the focused window through an allowed edge: the
      LeaveNotify on the frame carries the pointer's new position, which says
      which edge it went out through (autohide_frame_exit()). This needs no
      extra window, steals no clicks, and is not fooled by a fast flick, since
      the server reports where the pointer landed;
   2. where the pointer cannot leave because the edge of the window is the edge
      of the screen (maximised windows): a thin InputOnly "reveal strip"
      window along that edge, mapped only while the titlebar is hidden there.
      The pointer is clamped at the screen edge, so it always lands in it
      (autohide_strip_crossing());
   3. the revealed titlebar itself (and its buttons): the pointer being over it
      keeps it up, and leaving it begins the hide delay
      (autohide_titlebar_crossing()). The frame being left entirely does the
      same (autohide_frame_leave()).
   The titlebar is hidden again once the pointer has been away for the
   configured delay. Nothing polls and nothing runs while the pointer is idle.

   Why not watch pointer motion on the client window? Toolkits that use
   XInput2 for pointer input (GTK3) select XI2 motion on their window, and the
   X server then delivers the motion to them as XI2 *instead of* as the core
   MotionNotify we would select, so such clients would never reveal their
   titlebar. Selecting XI2 ourselves is no answer either: it would in turn
   starve core-event clients (xterm) of motion. Windows we own avoid both. The
   cost is that the strips cover the outermost AUTOHIDE_REVEAL_STRIP pixels of
   the client area while the titlebar is hidden.

   Testing caveat: some minimal/virtual X servers (e.g. Xvnc) deliver no
   pointer events to input-only windows, so the reveal cannot be triggered
   there. Verify on a normal X server (Xephyr is fine). */

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
    autohide_update_strips(f);
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
    autohide_update_strips(f);
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

/* Position and map/unmap one reveal strip */
static void update_strip(Window strip, gboolean want,
                         gint x, gint y, gint w, gint h)
{
    if (want) {
        XMoveResizeWindow(obt_display, strip, x, y, MAX(1, w), MAX(1, h));
        /* above the client window, which is a sibling of it in the frame */
        XRaiseWindow(obt_display, strip);
        XMapWindow(obt_display, strip);
    }
    else
        XUnmapWindow(obt_display, strip);
}

void autohide_update_strips(ObFrame *f)
{
    gboolean active;
    gboolean top_edge, bottom_edge;
    const Rect *screen;

    if (!f || !f->client)
        return;
    active = autohide_active_for(f);

    /* Only windows whose edge is the edge of the screen need a strip: the
       pointer cannot leave through it, so autohide_frame_exit() never fires
       there. Everywhere else the pointer leaving the window does the job. */
    screen = screen_physical_area_all_monitors();
    top_edge = f->area.y <= screen->y;
    bottom_edge = f->area.y + f->area.height >= screen->y + screen->height;

    /* along the top: from the frame's own top edge (so the thin border above a
       non-maximised client reveals too) down into the client area */
    update_strip(f->autohide_strip_top,
                 active && top_edge &&
                 (config_autohide_titlebar_edges & OB_AUTOHIDE_EDGE_TOP) &&
                 !(f->tb_shown && f->tb_edge == OB_AUTOHIDE_EDGE_TOP),
                 0, 0, f->width, f->size.top + AUTOHIDE_REVEAL_STRIP);

    /* along the bottom of the client area, where the overlay would sit, and
       on down through whatever border/handle is below it to the frame's edge:
       this strip is only used where that is the edge of the screen, so the
       pointer clamps on the very last row, which may not be client area */
    update_strip(f->autohide_strip_bottom,
                 active && bottom_edge &&
                 (config_autohide_titlebar_edges & OB_AUTOHIDE_EDGE_BOTTOM) &&
                 !(f->tb_shown && f->tb_edge == OB_AUTOHIDE_EDGE_BOTTOM),
                 f->size.left,
                 f->size.top + f->client->area.height - AUTOHIDE_REVEAL_STRIP,
                 f->client->area.width, AUTOHIDE_REVEAL_STRIP + f->size.bottom);
}

void autohide_frame_exit(ObFrame *f, gint x_root, gint y_root)
{
    gint ax, ay, aw, ah, dx, above, below;
    ObAutohideEdge edge;

    /* only the focused window: otherwise every window the pointer crosses on
       its way somewhere else would flash its titlebar */
    if (!f || !f->client || f->client != focus_client)
        return;
    if (moveresize_in_progress || !autohide_active_for(f))
        return;

    ax = f->area.x;
    ay = f->area.y;
    aw = f->area.width;
    ah = f->area.height;

    /* how far outside the frame, on each side, the pointer has ended up. The
       frame may also have been left because another window opened over it, in
       which case the pointer is still inside its area and none of these is
       positive. */
    dx = x_root < ax ? ax - x_root :
         x_root >= ax + aw ? x_root - (ax + aw) + 1 : 0;
    /* The outermost row of the frame counts as outside too. A window whose
       edge is the edge of the screen gets its LeaveNotify with the pointer
       still on that last row (some servers, Xephyr among them, hit-test the
       pointer one pixel past where they report it, so the pointer has left
       the window while still appearing to be on its last row). A genuine exit
       always lands further out than this, so it costs nothing elsewhere. */
    above = ay - y_root + 1;
    below = y_root - (ay + ah) + 2;

    /* it must have gone out mostly through the top or the bottom, not the
       side (a diagonal exit near a corner counts as whichever is further) */
    if (above > 0 && above > dx)
        edge = OB_AUTOHIDE_EDGE_TOP;
    else if (below > 0 && below > dx)
        edge = OB_AUTOHIDE_EDGE_BOTTOM;
    else
        return;

    if (!(config_autohide_titlebar_edges & edge))
        return;

    autohide_show(f, edge);
    /* the pointer is not on the titlebar, so give it the hide delay to get
       there (entering it cancels this) */
    autohide_frame_leave(f);
}

gboolean autohide_strip_crossing(ObFrame *f, Window win, gboolean enter)
{
    ObAutohideEdge edge;

    if (win == f->autohide_strip_top)
        edge = OB_AUTOHIDE_EDGE_TOP;
    else if (win == f->autohide_strip_bottom)
        edge = OB_AUTOHIDE_EDGE_BOTTOM;
    else
        return FALSE;

    /* leave windows alone while the user is dragging one */
    if (enter && !moveresize_in_progress)
        autohide_show(f, edge);
    return TRUE;
}

void autohide_titlebar_crossing(ObFrame *f, ObFrameContext con,
                                gboolean enter)
{
    switch (con) {
    case OB_FRAME_CONTEXT_TITLEBAR:
    case OB_FRAME_CONTEXT_TLCORNER:
    case OB_FRAME_CONTEXT_TRCORNER:
    case OB_FRAME_CONTEXT_MAXIMIZE:
    case OB_FRAME_CONTEXT_ALLDESKTOPS:
    case OB_FRAME_CONTEXT_SHADE:
    case OB_FRAME_CONTEXT_ICONIFY:
    case OB_FRAME_CONTEXT_ICON:
    case OB_FRAME_CONTEXT_CLOSE:
    case OB_FRAME_CONTEXT_OPENBOX_CONFIG:
    case OB_FRAME_CONTEXT_LAYER:
        break;
    default:
        return;
    }
    if (!f->tb_shown)
        return;

    if (enter) {
        /* the pointer is on the revealed titlebar: keep it up */
        if (f->tb_hide_timer) {
            g_source_remove(f->tb_hide_timer);
            f->tb_hide_timer = 0;
        }
    }
    else
        autohide_frame_leave(f);
}

void autohide_client_update(ObClient *c)
{
    if (!c || !c->frame)
        return;
    if (c->frame->tb_shown && !autohide_active_for(c->frame))
        autohide_hide(c->frame);
    autohide_update_strips(c->frame);
}
