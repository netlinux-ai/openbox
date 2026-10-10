/* tilt.c - interactive 3D tilt of a window, for a Composite/GL compositor

   Openbox does not draw anything in 3D itself. These actions publish a
   per-window transform in the _METISSE_TRANSFORM property of the client's
   frame window; a separate compositor (metisse/composite) reads it and draws
   the window rotated in perspective.

   The property is INTEGER, format 32, in thousandths:
       rx ry rz scale opacity
   (rotation about X, Y, Z in degrees, uniform scale, opacity 0..1).

   Tilt    interactive: dragging rotates the window (horizontal motion about
           the Y axis, vertical about the X axis). Escape cancels.
   Untilt  removes the transform, so the window is drawn flat again.
*/

#include "openbox/actions.h"
#include "openbox/client.h"
#include "openbox/frame.h"
#include "openbox/grab.h"
#include "openbox/misc.h"
#include "obt/display.h"
#include "obt/keyboard.h"
#include "obt/xml.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <glib.h>

#define N_VALS 5
#define MAX_TILT 80.0 /* degrees; beyond this a window is seen edge-on */

typedef struct {
    gdouble sensitivity; /* degrees of rotation per pixel of pointer motion */
} Options;

static ObClient *tilt_client = NULL;
static Window tilt_win = None;
static gboolean tilt_grabbed = FALSE;
static gint start_x, start_y;
static gint tilt_button;
static glong start_vals[N_VALS]; /* the property when the drag began */

static gpointer setup_func(xmlNodePtr node,
                           ObActionsIPreFunc *pre,
                           ObActionsIInputFunc *input,
                           ObActionsICancelFunc *cancel,
                           ObActionsIPostFunc *post);
static void free_func(gpointer options);
static gboolean run_func(ObActionsData *data, gpointer options);
static gboolean i_input_func(guint initial_state, XEvent *e, ObtIC *ic,
                             gpointer options, gboolean *used);
static void i_cancel_func(gpointer options);
static void i_post_func(gpointer options);
static gboolean run_untilt(ObActionsData *data, gpointer options);

static Atom transform_atom(void)
{
    static Atom a = None;
    if (a == None)
        a = XInternAtom(obt_display, "_METISSE_TRANSFORM", False);
    return a;
}

/* Read the transform into vals; missing values default to the identity. */
static void get_transform(Window win, glong vals[N_VALS])
{
    Atom type;
    gint fmt;
    gulong n, after, i;
    guchar *data = NULL;

    vals[0] = vals[1] = vals[2] = 0;
    vals[3] = vals[4] = 1000;
    if (XGetWindowProperty(obt_display, win, transform_atom(), 0, N_VALS,
                           False, XA_INTEGER, &type, &fmt, &n, &after,
                           &data) == Success && data)
    {
        if (type == XA_INTEGER && fmt == 32)
            for (i = 0; i < n && i < N_VALS; ++i)
                vals[i] = (gint32)((glong*)data)[i];
        XFree(data);
    }
}

static void set_transform(Window win, const glong vals[N_VALS])
{
    glong out[N_VALS];
    gint i;

    for (i = 0; i < N_VALS; ++i)
        out[i] = vals[i];
    XChangeProperty(obt_display, win, transform_atom(), XA_INTEGER, 32,
                    PropModeReplace, (guchar*)out, N_VALS);
}

static void client_dest(ObClient *client, gpointer data)
{
    if (client == tilt_client) {
        tilt_client = NULL;
        tilt_win = None;
        actions_interactive_cancel_act();
    }
}

void action_tilt_startup(void)
{
    actions_register_i("Tilt", setup_func, free_func, run_func);
    actions_register("Untilt", NULL, NULL, run_untilt);
    client_add_destroy_notify(client_dest, NULL);
}

static gpointer setup_func(xmlNodePtr node,
                           ObActionsIPreFunc *pre,
                           ObActionsIInputFunc *input,
                           ObActionsICancelFunc *cancel,
                           ObActionsIPostFunc *post)
{
    xmlNodePtr n;
    Options *o = g_slice_new0(Options);

    o->sensitivity = 0.35;
    if ((n = obt_xml_find_node(node, "sensitivity"))) {
        gchar *s = obt_xml_node_string(n);
        o->sensitivity = g_ascii_strtod(s, NULL);
        g_free(s);
        if (o->sensitivity <= 0.0)
            o->sensitivity = 0.35;
    }

    *input = i_input_func;
    *cancel = i_cancel_func;
    *post = i_post_func;
    return o;
}

static void free_func(gpointer options)
{
    g_slice_free(Options, options);
}

/* Returns TRUE: the drag continues interactively in i_input_func. */
static gboolean run_func(ObActionsData *data, gpointer options)
{
    /* needs a window and a held button to drag with */
    if (!data->client || !data->button)
        return FALSE;
    if (!grab_pointer(FALSE, FALSE, OB_CURSOR_MOVE))
        return FALSE;

    tilt_grabbed = TRUE;
    tilt_client = data->client;
    tilt_win = data->client->frame->window;
    tilt_button = data->button;
    start_x = data->x;
    start_y = data->y;
    get_transform(tilt_win, start_vals);
    return TRUE;
}

static gboolean i_input_func(guint initial_state, XEvent *e, ObtIC *ic,
                             gpointer options, gboolean *used)
{
    Options *o = options;

    if (!tilt_client)
        return FALSE;

    if (e->type == MotionNotify) {
        glong vals[N_VALS];
        gdouble rx, ry;
        gint i;

        for (i = 0; i < N_VALS; ++i)
            vals[i] = start_vals[i];
        /* dragging right turns the window about the vertical axis; dragging
           up tips its top edge away */
        ry = start_vals[1] / 1000.0 + (e->xmotion.x_root - start_x) *
            o->sensitivity;
        rx = start_vals[0] / 1000.0 + (e->xmotion.y_root - start_y) *
            o->sensitivity;
        vals[0] = (glong)(CLAMP(rx, -MAX_TILT, MAX_TILT) * 1000.0);
        vals[1] = (glong)(CLAMP(ry, -MAX_TILT, MAX_TILT) * 1000.0);
        set_transform(tilt_win, vals);
        *used = TRUE;
    }
    else if (e->type == ButtonRelease) {
        if (e->xbutton.button == (guint)tilt_button) {
            *used = TRUE;
            return FALSE; /* done: the tilt stays */
        }
    }
    else if (e->type == KeyPress) {
        KeySym sym = obt_keyboard_keypress_to_keysym(e);
        if (sym == XK_Escape) {
            set_transform(tilt_win, start_vals);
            *used = TRUE;
            return FALSE;
        }
    }
    return TRUE;
}

static void i_cancel_func(gpointer options)
{
    if (tilt_win != None)
        set_transform(tilt_win, start_vals);
}

static void i_post_func(gpointer options)
{
    if (tilt_grabbed) {
        ungrab_pointer();
        tilt_grabbed = FALSE;
    }
    tilt_client = NULL;
    tilt_win = None;
}

static gboolean run_untilt(ObActionsData *data, gpointer options)
{
    if (data->client)
        XDeleteProperty(obt_display, data->client->frame->window,
                        transform_atom());
    return FALSE;
}
