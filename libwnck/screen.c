/* screen object */

/*
 * Copyright (C) 2001 Havoc Pennington
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "screen.h"
#include "window.h"
#include "workspace.h"
#include "application.h"
#include "xutils.h"
#include "private.h"
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <string.h>
#include <stdlib.h>

static WnckScreen** screens = NULL;

struct _WnckScreenPrivate
{
  int number;
  Window xroot;
  Screen *xscreen;
  int width;
  int height;
  /* in map order */
  GList *mapped_windows;
  /* in stacking order */
  GList *stacked_windows;
  /* in 0-to-N order */
  GList *workspaces;
  
  WnckWindow *active_window;
  WnckWorkspace *active_workspace;

  Pixmap bg_pixmap;
  
  guint update_handler;  

#ifdef HAVE_STARTUP_NOTIFICATION
  SnDisplay *sn_display;
#endif
  
  guint showing_desktop : 1;
  
  /* if you add flags, be sure to set them
   * when we create the screen so we get an initial update
   */
  guint need_update_stack_list : 1;
  guint need_update_workspace_list : 1;
  guint need_update_active_workspace : 1;
  guint need_update_active_window : 1;
  guint need_update_workspace_names : 1;
  guint need_update_bg_pixmap : 1;
  guint need_update_showing_desktop : 1;
};

enum {
  ACTIVE_WINDOW_CHANGED,
  ACTIVE_WORKSPACE_CHANGED,
  WINDOW_STACKING_CHANGED,
  WINDOW_OPENED,
  WINDOW_CLOSED,
  WORKSPACE_CREATED,
  WORKSPACE_DESTROYED,
  APPLICATION_OPENED,
  APPLICATION_CLOSED,
  BACKGROUND_CHANGED,
  SHOWING_DESKTOP_CHANGED,
  LAST_SIGNAL
};

static void wnck_screen_init        (WnckScreen      *screen);
static void wnck_screen_class_init  (WnckScreenClass *klass);
static void wnck_screen_finalize    (GObject         *object);

static void update_client_list      (WnckScreen      *screen);
static void update_workspace_list   (WnckScreen      *screen);
static void update_active_workspace (WnckScreen      *screen);
static void update_active_window    (WnckScreen      *screen);
static void update_workspace_names  (WnckScreen      *screen);
static void update_showing_desktop  (WnckScreen      *screen);

static void queue_update            (WnckScreen      *screen);
static void unqueue_update          (WnckScreen      *screen);              
static void do_update_now           (WnckScreen      *screen);

static void emit_active_window_changed    (WnckScreen      *screen);
static void emit_active_workspace_changed (WnckScreen      *screen);
static void emit_window_stacking_changed  (WnckScreen      *screen);
static void emit_window_opened            (WnckScreen      *screen,
                                           WnckWindow      *window);
static void emit_window_closed            (WnckScreen      *screen,
                                           WnckWindow      *window);
static void emit_workspace_created        (WnckScreen      *screen,
                                           WnckWorkspace   *space);
static void emit_workspace_destroyed      (WnckScreen      *screen,
                                           WnckWorkspace   *space);
static void emit_application_opened       (WnckScreen      *screen,
                                           WnckApplication *app);
static void emit_application_closed       (WnckScreen      *screen,
                                           WnckApplication *app);
static void emit_background_changed       (WnckScreen      *screen);
static void emit_showing_desktop_changed  (WnckScreen      *screen);

static gpointer parent_class;
static guint signals[LAST_SIGNAL] = { 0 };

GType
wnck_screen_get_type (void)
{
  static GType object_type = 0;

  g_type_init ();
  
  if (!object_type)
    {
      static const GTypeInfo object_info =
      {
        sizeof (WnckScreenClass),
        (GBaseInitFunc) NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc) wnck_screen_class_init,
        NULL,           /* class_finalize */
        NULL,           /* class_data */
        sizeof (WnckScreen),
        0,              /* n_preallocs */
        (GInstanceInitFunc) wnck_screen_init,
      };
      
      object_type = g_type_register_static (G_TYPE_OBJECT,
                                            "WnckScreen",
                                            &object_info, 0);
    }
  
  return object_type;
}

static void
wnck_screen_init (WnckScreen *screen)
{  
  screen->priv = g_new0 (WnckScreenPrivate, 1);

}

static void
wnck_screen_class_init (WnckScreenClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  _wnck_init ();
  
  parent_class = g_type_class_peek_parent (klass);
  
  object_class->finalize = wnck_screen_finalize;

  signals[ACTIVE_WINDOW_CHANGED] =
    g_signal_new ("active_window_changed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, active_window_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[ACTIVE_WORKSPACE_CHANGED] =
    g_signal_new ("active_workspace_changed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, active_workspace_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
  
  signals[WINDOW_STACKING_CHANGED] =
    g_signal_new ("window_stacking_changed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, window_stacking_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[WINDOW_OPENED] =
    g_signal_new ("window_opened",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, window_opened),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1, WNCK_TYPE_WINDOW);

  signals[WINDOW_CLOSED] =
    g_signal_new ("window_closed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, window_closed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1, WNCK_TYPE_WINDOW);
  
  signals[WORKSPACE_CREATED] =
    g_signal_new ("workspace_created",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, workspace_created),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1, WNCK_TYPE_WORKSPACE);
  
  signals[WORKSPACE_DESTROYED] =
    g_signal_new ("workspace_destroyed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, workspace_destroyed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1, WNCK_TYPE_WORKSPACE);

  signals[APPLICATION_OPENED] =
    g_signal_new ("application_opened",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, application_opened),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1, WNCK_TYPE_APPLICATION);

  signals[APPLICATION_CLOSED] =
    g_signal_new ("application_closed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, application_closed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1, WNCK_TYPE_APPLICATION);

  signals[BACKGROUND_CHANGED] =
    g_signal_new ("background_changed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (WnckScreenClass, background_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
  signals[SHOWING_DESKTOP_CHANGED] =
    g_signal_new ("showing_desktop_changed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
#if 0
                  /* FIXME when we can break ABI add this */
                  G_STRUCT_OFFSET (WnckScreenClass, showing_desktop_changed),
#else
                  0,
#endif
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}

static void
wnck_screen_finalize (GObject *object)
{
  WnckScreen *screen;

  screen = WNCK_SCREEN (object);
  
  unqueue_update (screen);

  /* FIXME this isn't right, we need to destroy the items
   * in the list. But it doesn't matter at the
   * moment since we never finalize screens anyway.
   */
  g_list_free (screen->priv->mapped_windows);
  g_list_free (screen->priv->stacked_windows);
  g_list_free (screen->priv->workspaces);

  screens[screen->priv->number] = NULL;

#ifdef HAVE_STARTUP_NOTIFICATION
  sn_display_unref (screen->priv->sn_display);
#endif
  
  g_free (screen->priv);
  
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

#ifdef HAVE_STARTUP_NOTIFICATION
static void
sn_error_trap_push (SnDisplay *display,
                    Display   *xdisplay)
{
  gdk_error_trap_push ();
}

static void
sn_error_trap_pop (SnDisplay *display,
                   Display   *xdisplay)
{
  gdk_error_trap_pop ();
}
#endif /* HAVE_STARTUP_NOTIFICATION */

static void
wnck_screen_construct (WnckScreen *screen,
                       int         number)
{
  /* Create the initial state of the screen. */
  screen->priv->xroot = RootWindow (gdk_display, number);
  screen->priv->xscreen = ScreenOfDisplay (gdk_display, number);
  screen->priv->number = number;

#ifdef HAVE_STARTUP_NOTIFICATION
  screen->priv->sn_display = sn_display_new (gdk_display,
                                             sn_error_trap_push,
                                             sn_error_trap_pop);
#endif
  
  screen->priv->bg_pixmap = None;
  
  _wnck_select_input (screen->priv->xroot,
                      PropertyChangeMask);

  screen->priv->width = WidthOfScreen (screen->priv->xscreen);
  screen->priv->height = HeightOfScreen (screen->priv->xscreen);

  screen->priv->need_update_workspace_list = TRUE;
  screen->priv->need_update_stack_list = TRUE;
  screen->priv->need_update_active_workspace = TRUE;
  screen->priv->need_update_active_window = TRUE;
  screen->priv->need_update_workspace_names = TRUE;
  screen->priv->need_update_bg_pixmap = TRUE;
  screen->priv->need_update_showing_desktop = TRUE;
  
  queue_update (screen);
}

/**
 * wnck_screen_get:
 * @index: screen number, starting from 0, as for Xlib
 * 
 * Gets the #WnckScreen for a given screen on the default display.
 * Creates the #WnckScreen if necessary.
 * 
 * Return value: the #WnckScreen for screen @index
 **/
WnckScreen*
wnck_screen_get (int index)
{
  g_return_val_if_fail (gdk_display != NULL, NULL);
  g_return_val_if_fail (index < ScreenCount (gdk_display), NULL);
  
  if (screens == NULL)
    {
      screens = g_new0 (WnckScreen*, ScreenCount (gdk_display));
      _wnck_event_filter_init ();
    }
  
  if (screens[index] == NULL)
    {
      screens[index] = g_object_new (WNCK_TYPE_SCREEN, NULL);
      
      wnck_screen_construct (screens[index], index);
    }

  return screens[index];
}

WnckScreen*
_wnck_screen_get_existing (int number)
{
  g_return_val_if_fail (gdk_display != NULL, NULL);
  g_return_val_if_fail (number < ScreenCount (gdk_display), NULL);

  if (screens != NULL)
    return screens[number];
  else
    return NULL;
}

WnckScreen*
wnck_screen_get_default (void)
{
  int default_screen;

  default_screen = DefaultScreen (gdk_display);

  return wnck_screen_get (default_screen);
}

/**
 * wnck_screen_get_for_root:
 * @root_window_id: an Xlib window ID
 * 
 * Gets the #WnckScreen for the root window at @root_window_id, or
 * returns %NULL if no #WnckScreen exists for this root window. Never
 * creates a new #WnckScreen, unlike wnck_screen_get().
 * 
 * Return value: #WnckScreen or %NULL
 **/
WnckScreen*
wnck_screen_get_for_root (gulong root_window_id)
{
  int i;
  
  if (screens == NULL)
    return NULL;

  i = 0;
  while (i < ScreenCount (gdk_display))
    {
      if (screens[i] != NULL && screens[i]->priv->xroot == root_window_id)
        return screens[i];
      
      ++i;
    }

  return NULL;
}

/**
 * wnck_screen_get_workspace:
 * @screen: a #WnckScreen
 * @number: a workspace index
 * 
 * Gets the workspace numbered @number for screen @screen, or returns %NULL if
 * no such workspace exists.
 * 
 * Return value: the workspace, or %NULL
 **/
WnckWorkspace*
wnck_screen_get_workspace (WnckScreen *screen,
			   int         number)
{
  GList *list;
  
  /* We trust this function with property-provided numbers, it
   * must reliably return NULL on bad data
   */
  list = g_list_nth (screen->priv->workspaces, number);

  if (list == NULL)
    return NULL;
  
  return WNCK_WORKSPACE (list->data);
}

/**
 * wnck_screen_get_active_workspace:
 * @screen: a #WnckScreen 
 * 
 * Gets the active workspace. May return %NULL sometimes,
 * if we are in a weird state due to the asynchronous
 * nature of our interaction with the window manager.
 * 
 * Return value: active workspace or %NULL
 **/
WnckWorkspace*
wnck_screen_get_active_workspace (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), NULL);

  return screen->priv->active_workspace;
}

/**
 * wnck_screen_get_active_window:
 * @screen: a #WnckScreen
 * 
 * Gets the active window. May return %NULL sometimes,
 * since not all window managers guarantee that a window
 * is always active.
 * 
 * Return value: active window or %NULL
 **/
WnckWindow*
wnck_screen_get_active_window (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), NULL);

  return screen->priv->active_window;
}

/**
 * wnck_screen_get_windows:
 * @screen: a #WnckScreen
 * 
 * Gets the screen's list of windows. The list is not copied
 * and should not be freed. The list is not in a defined order,
 * but should be "stable" (windows shouldn't reorder themselves in
 * it). (However, the stability of the list is established by
 * the window manager, so don't blame libwnck if it breaks down.)
 * 
 * Return value: reference to list of windows
 **/
GList*
wnck_screen_get_windows (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), NULL);

  return screen->priv->mapped_windows;
}

/**
 * wnck_screen_get_windows_stacked:
 * @screen: a #WnckScreen
 * 
 * Gets the screen's list of windows in bottom-to-top order.
 * The list is not copied and should not be freed.
 * 
 * Return value: reference to list of windows in stacking order
 **/
GList*
wnck_screen_get_windows_stacked (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), NULL);

  return screen->priv->stacked_windows;
}

/**
 * _wnck_screen_get_gdk_screen:
 * @screen: a #WnckScreen
 * 
 * Obtains the #GdkScreen referring to the same screen
 * as @screen.
 * 
 * Return value: a #GdkScreen
 **/
GdkScreen *
_wnck_screen_get_gdk_screen (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), NULL);

  return gdk_display_get_screen (gdk_display_get_default (),
				 screen->priv->number);
}

/**
 * wnck_screen_force_update:
 * @screen: a #WnckScreen
 * 
 * Synchronously and immediately update the window list.  This is
 * usually a bad idea for both performance and correctness reasons (to
 * get things right, you need to write model-view code that tracks
 * changes, not get a static list of open windows).
 * 
 **/
void
wnck_screen_force_update (WnckScreen *screen)
{
  g_return_if_fail (WNCK_IS_SCREEN (screen));

  do_update_now (screen);
}

/**
 * wnck_screen_get_workspace_count:
 * @screen: a #WnckScreen
 * 
 * Gets the number of workspaces.
 * 
 * Return value: number of workspaces
 **/
int
wnck_screen_get_workspace_count (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), 0);

  return g_list_length (screen->priv->workspaces);
}

/**
 * wnck_screen_change_workspace_count:
 * @screen: a #WnckScreen
 * @count: requested count
 * 
 * Asks the window manager to change the number of workspaces.
 **/
void
wnck_screen_change_workspace_count (WnckScreen *screen,
                                    int         count)
{
  XEvent xev;
  
  g_return_if_fail (WNCK_IS_SCREEN (screen));
  g_return_if_fail (count >= 1);
  
  xev.xclient.type = ClientMessage;
  xev.xclient.serial = 0;
  xev.xclient.window = screen->priv->xroot;
  xev.xclient.send_event = True;
  xev.xclient.display = DisplayOfScreen (screen->priv->xscreen);
  xev.xclient.message_type = _wnck_atom_get ("_NET_NUMBER_OF_DESKTOPS");
  xev.xclient.format = 32;
  xev.xclient.data.l[0] = count;
  
  XSendEvent (DisplayOfScreen (screen->priv->xscreen),
              screen->priv->xroot,
              False,
              SubstructureRedirectMask | SubstructureNotifyMask,
              &xev);
}

void
_wnck_screen_process_property_notify (WnckScreen *screen,
                                      XEvent     *xevent)
{
  /* most frequently-changed properties first */
  if (xevent->xproperty.atom ==
      _wnck_atom_get ("_NET_ACTIVE_WINDOW"))
    {
      screen->priv->need_update_active_window = TRUE;
      queue_update (screen);
    }
  else if (xevent->xproperty.atom ==
           _wnck_atom_get ("_NET_CURRENT_DESKTOP"))
    {
      screen->priv->need_update_active_workspace = TRUE;
      queue_update (screen);
    }
  else if (xevent->xproperty.atom ==
           _wnck_atom_get ("_NET_CLIENT_LIST_STACKING") ||
           xevent->xproperty.atom ==
           _wnck_atom_get ("_NET_CLIENT_LIST"))
    {
      screen->priv->need_update_stack_list = TRUE;
      queue_update (screen);
    }
  else if (xevent->xproperty.atom ==
           _wnck_atom_get ("_NET_NUMBER_OF_DESKTOPS"))
    {
      screen->priv->need_update_workspace_list = TRUE;
      queue_update (screen);
    }
  else if (xevent->xproperty.atom ==
           _wnck_atom_get ("_NET_DESKTOP_NAMES"))
    {
      screen->priv->need_update_workspace_names = TRUE;
      queue_update (screen);
    }
  else if (xevent->xproperty.atom ==
           _wnck_atom_get ("_XROOTPMAP_ID"))
    {
      screen->priv->need_update_bg_pixmap = TRUE;
      queue_update (screen);
    }
  else if (xevent->xproperty.atom ==
           _wnck_atom_get ("_NET_SHOWING_DESKTOP"))
    {
      screen->priv->need_update_showing_desktop = TRUE;
      queue_update (screen);
    }
}

static gboolean
lists_equal (GList *a,
             GList *b)
{
  GList *a_iter;
  GList *b_iter;

  a_iter = a;
  b_iter = b;

  while (a_iter && b_iter)
    {
      if (a_iter->data != b_iter->data)
        return FALSE;
      
      a_iter = a_iter->next;
      b_iter = b_iter->next;
    }

  if (a_iter || b_iter)
    return FALSE;

  return TRUE;
}

static int
wincmp (const void *a,
        const void *b)
{
  const Window *aw = a;
  const Window *bw = b;

  if (*aw < *bw)
    return -1;
  else if (*aw > *bw)
    return 1;
  else
    return 0;
}

static gboolean
arrays_contain_same_windows (Window *a,
                             int     a_len,
                             Window *b,
                             int     b_len)
{
  Window *a_tmp;
  Window *b_tmp;
  gboolean result;

  if (a_len != b_len)
    return FALSE;

  if (a_len == 0 ||
      b_len == 0)
    return FALSE; /* one was nonzero */
  
  a_tmp = g_new (Window, a_len);
  b_tmp = g_new (Window, b_len);

  memcpy (a_tmp, a, a_len * sizeof (Window));
  memcpy (b_tmp, b, b_len * sizeof (Window));

  qsort (a_tmp, a_len, sizeof (Window), wincmp);
  qsort (b_tmp, b_len, sizeof (Window), wincmp);

  result = memcmp (a_tmp, b_tmp, sizeof (Window) * a_len) == 0;

  g_free (a_tmp);
  g_free (b_tmp);
  
  return result;
}

static void
update_client_list (WnckScreen *screen)
{
  /* stacking order */
  Window *stack = NULL;
  int stack_length = 0;
  /* mapping order */
  Window *mapping = NULL;
  int mapping_length = 0;
  GList *new_stack_list;
  GList *new_list;
  GList *created;
  GList *closed;
  GList *created_apps;
  GList *closed_apps;
  GList *tmp;
  int i;
  GHashTable *new_hash;
  static int reentrancy_guard = 0;
  gboolean active_changed;
  gboolean stack_changed;
  gboolean list_changed;
  
  g_return_if_fail (reentrancy_guard == 0);
  
  if (!screen->priv->need_update_stack_list)
    return;

  ++reentrancy_guard;
  
  screen->priv->need_update_stack_list = FALSE;
  
  _wnck_get_window_list (screen->priv->xroot,
                         _wnck_atom_get ("_NET_CLIENT_LIST_STACKING"),
                         &stack,
                         &stack_length);

  _wnck_get_window_list (screen->priv->xroot,
                         _wnck_atom_get ("_NET_CLIENT_LIST"),
                         &mapping,
                         &mapping_length);

  if (!arrays_contain_same_windows (stack, stack_length,
                                    mapping, mapping_length))
    {
      /* Don't update until we're in a consistent state */
      g_free (stack);
      g_free (mapping);
      --reentrancy_guard;
      return;
    }
  
  created = NULL;
  closed = NULL;
  created_apps = NULL;
  closed_apps = NULL;

  new_hash = g_hash_table_new (NULL, NULL);
  
  new_stack_list = NULL;
  i = 0;
  while (i < stack_length)
    {
      WnckWindow *window;

      window = wnck_window_get (stack[i]);

      if (window == NULL)
        {
          Window leader;
          WnckApplication *app;
          
          window = _wnck_window_create (stack[i], screen);
          created = g_list_prepend (created, window);

          leader = wnck_window_get_group_leader (window);
          
          app = wnck_application_get (leader);
          if (app == NULL)
            {
              app = _wnck_application_create (leader, screen);
              created_apps = g_list_prepend (created_apps, app);
            }
          
          _wnck_application_add_window (app, window);
        }

      new_stack_list = g_list_prepend (new_stack_list, window);

      g_hash_table_insert (new_hash, window, window);
      
      ++i;
    }
      
  /* put list back in order */
  new_stack_list = g_list_reverse (new_stack_list);

  /* Now we need to find windows in the old list that aren't
   * in this new list
   */
  tmp = screen->priv->stacked_windows;
  while (tmp != NULL)
    {
      WnckWindow *window = tmp->data;

      if (g_hash_table_lookup (new_hash, window) == NULL)
        {
          WnckApplication *app;
          
          closed = g_list_prepend (closed, window);

          app = wnck_window_get_application (window);

          _wnck_application_remove_window (app, window);

          if (wnck_application_get_windows (app) == NULL)
            closed_apps = g_list_prepend (closed_apps, app);
        }
      
      tmp = tmp->next;
    }

  g_hash_table_destroy (new_hash);

  /* Now get the mapping in list form */
  new_list = NULL;
  i = 0;
  while (i < mapping_length)
    {
      WnckWindow *window;

      window = wnck_window_get (mapping[i]);

      g_assert (window != NULL);

      new_list = g_list_prepend (new_list, window);
      
      ++i;
    }
  
  g_free (stack);
  g_free (mapping);
      
  /* put list back in order */
  new_list = g_list_reverse (new_list);
  
  /* Now new_stack_list becomes screen->priv->stack_windows, new_list
   * becomes screen->priv->mapped_windows, and we emit the opened/closed
   * signals as appropriate
   */

  stack_changed = !lists_equal (screen->priv->stacked_windows, new_stack_list);
  list_changed = !lists_equal (screen->priv->mapped_windows, new_list);

  if (!(stack_changed || list_changed))
    {
      g_assert (created == NULL);
      g_assert (closed == NULL);
      g_assert (created_apps == NULL);
      g_assert (closed_apps == NULL);
      g_list_free (new_stack_list);
      g_list_free (new_list);      
      --reentrancy_guard;
      return;
    }

  g_list_free (screen->priv->mapped_windows);
  g_list_free (screen->priv->stacked_windows);  
  screen->priv->mapped_windows = new_list;
  screen->priv->stacked_windows = new_stack_list;

  /* Here we could get reentrancy if someone ran the main loop in
   * signal callbacks; though that would be a bit pathological, so we
   * don't handle it, but we do warn about it using reentrancy_guard
   */

  /* Sequence is: application_opened, window_opened, window_closed,
   * application_closed. We have to do all window list changes
   * BEFORE doing any other signals, so that any observers
   * have valid state for the window structure before they take
   * further action
   */
  tmp = created_apps;
  while (tmp != NULL)
    {
      emit_application_opened (screen, WNCK_APPLICATION (tmp->data));
      
      tmp = tmp->next;
    }
  
  tmp = created;
  while (tmp != NULL)
    {
      emit_window_opened (screen, WNCK_WINDOW (tmp->data));
      
      tmp = tmp->next;
    }

  active_changed = FALSE;
  tmp = closed;
  while (tmp != NULL)
    {
      WnckWindow *window;

      window = WNCK_WINDOW (tmp->data);

      if (window == screen->priv->active_window)
        {
          screen->priv->active_window = NULL;
          active_changed = TRUE;
        }
      
      emit_window_closed (screen, window);
      
      tmp = tmp->next;
    }

  tmp = closed_apps;
  while (tmp != NULL)
    {
      WnckApplication *app;

      app = WNCK_APPLICATION (tmp->data);

      emit_application_closed (screen, app);
      
      tmp = tmp->next;
    }

  if (stack_changed)
    emit_window_stacking_changed (screen);

  if (active_changed)
    emit_active_window_changed (screen);
  
  /* Now free the closed windows */
  tmp = closed;
  while (tmp != NULL)
    {
      WnckWindow *window = tmp->data;

      _wnck_window_destroy (window);
      
      tmp = tmp->next;
    }

  /* Free the closed apps */
  tmp = closed_apps;
  while (tmp != NULL)
    {
      WnckApplication *app;

      app = WNCK_APPLICATION (tmp->data);

      _wnck_application_destroy (app);
      
      tmp = tmp->next;
    }
  
  g_list_free (closed);
  g_list_free (created);
  g_list_free (closed_apps);
  g_list_free (created_apps);

  --reentrancy_guard;

  /* Maybe the active window is now valid if it wasn't */
  if (screen->priv->active_window == NULL)
    {
      screen->priv->need_update_active_window = TRUE;
      queue_update (screen);
    }
}

static void
update_workspace_list (WnckScreen *screen)
{
  int n_spaces;
  int old_n_spaces;
  GList *tmp;
  GList *deleted;
  GList *created;
  static int reentrancy_guard = 0;

  g_return_if_fail (reentrancy_guard == 0);
  
  if (!screen->priv->need_update_workspace_list)
    return;
  
  screen->priv->need_update_workspace_list = FALSE;

  ++reentrancy_guard;
  
  n_spaces = 0;
  _wnck_get_cardinal (screen->priv->xroot,
                      _wnck_atom_get ("_NET_NUMBER_OF_DESKTOPS"),
                      &n_spaces);
  
  old_n_spaces = g_list_length (screen->priv->workspaces);

  deleted = NULL;
  created = NULL;
  
  if (old_n_spaces == n_spaces)
    {
      --reentrancy_guard;
      return; /* nothing changed */
    }
  else if (old_n_spaces > n_spaces)
    {
      /* Need to delete some workspaces */
      deleted = g_list_nth (screen->priv->workspaces, n_spaces);
      if (deleted->prev)
        deleted->prev->next = NULL;
      deleted->prev = NULL;

      if (deleted == screen->priv->workspaces)
        screen->priv->workspaces = NULL;
    }
  else
    {
      int i;
      
      g_assert (old_n_spaces < n_spaces);

      /* Need to create some workspaces */
      i = 0;
      while (i < (n_spaces - old_n_spaces))
        {
          WnckWorkspace *space;

          space = _wnck_workspace_create (old_n_spaces + i, screen);

          screen->priv->workspaces = g_list_append (screen->priv->workspaces,
                                                    space);

          created = g_list_prepend (created, space);
          
          ++i;
        }

      created = g_list_reverse (created);
    }

  /* Here we allow reentrancy, going into the main
   * loop could confuse us
   */
  tmp = deleted;
  while (tmp != NULL)
    {
      WnckWorkspace *space = WNCK_WORKSPACE (tmp->data);

      if (space == screen->priv->active_workspace)
        {
          screen->priv->active_workspace = NULL;
          emit_active_workspace_changed (screen);
        }
      
      emit_workspace_destroyed (screen, space);
      
      tmp = tmp->next;
    }
  
  tmp = created;
  while (tmp != NULL)
    {
      emit_workspace_created (screen, WNCK_WORKSPACE (tmp->data));
      
      tmp = tmp->next;
    }
  g_list_free (created);
  
  tmp = deleted;
  while (tmp != NULL)
    {
      g_object_unref (tmp->data);
      
      tmp = tmp->next;
    }
  g_list_free (deleted);

  /* Active workspace property may now be interpretable,
   * if it was a number larger than the active count previously
   */
  if (screen->priv->active_workspace == NULL)
    {
      screen->priv->need_update_active_workspace = TRUE;
      queue_update (screen);
    }
  
  --reentrancy_guard;
}

static void
update_active_workspace (WnckScreen *screen)
{
  int number;
  WnckWorkspace *space;
  
  if (!screen->priv->need_update_active_workspace)
    return;

  screen->priv->need_update_active_workspace = FALSE;

  number = 0;
  if (!_wnck_get_cardinal (screen->priv->xroot,
                           _wnck_atom_get ("_NET_CURRENT_DESKTOP"),
                           &number))
    number = -1;
  
  space = wnck_screen_get_workspace (screen, number);

  if (space == screen->priv->active_workspace)
    return;
  
  screen->priv->active_workspace = space;

  emit_active_workspace_changed (screen);
}

static void
update_active_window (WnckScreen *screen)
{
  WnckWindow *window;
  Window xwindow;
  
  if (!screen->priv->need_update_active_window)
    return;

  screen->priv->need_update_active_window = FALSE;
  
  xwindow = None;
  _wnck_get_window (screen->priv->xroot,
                    _wnck_atom_get ("_NET_ACTIVE_WINDOW"),
                    &xwindow);

  window = wnck_window_get (xwindow);

  if (window == screen->priv->active_window)
    return;

  screen->priv->active_window = window;

  emit_active_window_changed (screen);
}

static void
update_workspace_names (WnckScreen *screen)
{
  char **names;
  int i;
  GList *tmp;
  GList *copy;
  
  if (!screen->priv->need_update_workspace_names)
    return;

  screen->priv->need_update_workspace_names = FALSE;

  names = _wnck_get_utf8_list (screen->priv->xroot,
                               _wnck_atom_get ("_NET_DESKTOP_NAMES"));

  copy = g_list_copy (screen->priv->workspaces);
  
  i = 0;
  tmp = copy;
  while (tmp != NULL)
    {      
      if (names && names[i])
        {
          _wnck_workspace_update_name (tmp->data, names[i]);
          ++i;
        }
      else
        _wnck_workspace_update_name (tmp->data, NULL);

      tmp = tmp->next;
    }

  g_strfreev (names);

  g_list_free (copy);
}

static void
update_bg_pixmap (WnckScreen *screen)
{
  Pixmap p;
  
  if (!screen->priv->need_update_bg_pixmap)
    return;
  
  screen->priv->need_update_bg_pixmap = FALSE;

  p = None;
  _wnck_get_pixmap (screen->priv->xroot,
                    _wnck_atom_get ("_XROOTPMAP_ID"),
                    &p);
  /* may have failed, so p may still be None */

  screen->priv->bg_pixmap = p;
  
  emit_background_changed (screen);
}

static void
update_showing_desktop (WnckScreen *screen)
{
  int showing_desktop;
  
  if (!screen->priv->need_update_showing_desktop)
    return;
  
  screen->priv->need_update_showing_desktop = FALSE;

  showing_desktop = FALSE;
  _wnck_get_cardinal (screen->priv->xroot,
                      _wnck_atom_get ("_NET_SHOWING_DESKTOP"),
                      &showing_desktop);

  screen->priv->showing_desktop = showing_desktop != 0;
  
  emit_showing_desktop_changed (screen);
}

static void
do_update_now (WnckScreen *screen)
{
  if (screen->priv->update_handler)
    {
      g_source_remove (screen->priv->update_handler);
      screen->priv->update_handler = 0;
    }

  /* First get our big-picture state in order */
  update_workspace_list (screen);
  update_client_list (screen);

  /* Then note any smaller-scale changes */
  update_active_workspace (screen);
  update_active_window (screen);
  update_workspace_names (screen);
  update_showing_desktop (screen);
  
  update_bg_pixmap (screen);
}

static gboolean
update_idle (gpointer data)
{
  WnckScreen *screen;

  screen = data;

  screen->priv->update_handler = 0;

  do_update_now (screen);
  
  return FALSE;
}

static void
queue_update (WnckScreen *screen)
{
  if (screen->priv->update_handler != 0)
    return;

  screen->priv->update_handler = g_idle_add (update_idle, screen);
}

static void
unqueue_update (WnckScreen *screen)
{
  if (screen->priv->update_handler != 0)
    {
      g_source_remove (screen->priv->update_handler);
      screen->priv->update_handler = 0;
    }
}

static void
emit_active_window_changed (WnckScreen *screen)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[ACTIVE_WINDOW_CHANGED],
                 0);
}

static void
emit_active_workspace_changed (WnckScreen *screen)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[ACTIVE_WORKSPACE_CHANGED],
                 0);
}

static void
emit_window_stacking_changed (WnckScreen *screen)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[WINDOW_STACKING_CHANGED],
                 0);
}

static void
emit_window_opened (WnckScreen *screen,
                    WnckWindow *window)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[WINDOW_OPENED],
                 0, window);  
}

static void
emit_window_closed (WnckScreen *screen,
                    WnckWindow *window)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[WINDOW_CLOSED],
                 0, window);
}

static void
emit_workspace_created (WnckScreen    *screen,
                        WnckWorkspace *space)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[WORKSPACE_CREATED],
                 0, space);  
}

static void
emit_workspace_destroyed (WnckScreen    *screen,
                          WnckWorkspace *space)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[WORKSPACE_DESTROYED],
                 0, space);
}

static void
emit_application_opened (WnckScreen      *screen,
                         WnckApplication *app)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[APPLICATION_OPENED],
                 0, app);
}

static void
emit_application_closed (WnckScreen      *screen,
                         WnckApplication *app)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[APPLICATION_CLOSED],
                 0, app);
}

static void
emit_background_changed (WnckScreen *screen)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[BACKGROUND_CHANGED],
                 0);
}

static void
emit_showing_desktop_changed (WnckScreen *screen)
{
  g_signal_emit (G_OBJECT (screen),
                 signals[SHOWING_DESKTOP_CHANGED],
                 0);
}

gboolean
wnck_screen_net_wm_supports (WnckScreen *screen,
                             const char *atom)
{
  return gdk_net_wm_supports (gdk_atom_intern (atom, FALSE));
}

gulong
wnck_screen_get_background_pixmap (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), None);
  
  return screen->priv->bg_pixmap;
}

int
wnck_screen_get_width (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), 0);

  return gdk_screen_width ();
}

int
wnck_screen_get_height (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), 0);

  return gdk_screen_height ();
}

Screen *
_wnck_screen_get_xscreen (WnckScreen *screen)
{
  return screen->priv->xscreen;
}

int
_wnck_screen_get_number (WnckScreen *screen)
{
  return screen->priv->number;
}

int
wnck_screen_try_set_workspace_layout (WnckScreen *screen,
                                      int         current_token,
                                      int         rows,
                                      int         columns)
{
  int retval;
  
  g_return_val_if_fail (WNCK_IS_SCREEN (screen),
                        WNCK_NO_MANAGER_TOKEN);
  
  retval = _wnck_try_desktop_layout_manager (screen->priv->xscreen, current_token);

  if (retval != WNCK_NO_MANAGER_TOKEN)
    {
      _wnck_set_desktop_layout (screen->priv->xscreen, rows, columns);
    }

  return retval;
}

void
wnck_screen_release_workspace_layout (WnckScreen *screen,
                                      int         current_token)
{
  _wnck_release_desktop_layout_manager (screen->priv->xscreen,
                                        current_token);

}

gboolean
wnck_screen_get_showing_desktop (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), FALSE);
  
  return screen->priv->showing_desktop;
}

void
wnck_screen_toggle_showing_desktop (WnckScreen *screen,
                                    gboolean    show)
{
  g_return_if_fail (WNCK_IS_SCREEN (screen));

  _wnck_toggle_showing_desktop (screen->priv->xscreen,
                                show);
}

#ifdef HAVE_STARTUP_NOTIFICATION
SnDisplay*
_wnck_screen_get_sn_display (WnckScreen *screen)
{
  g_return_val_if_fail (WNCK_IS_SCREEN (screen), NULL);
  
  return screen->priv->sn_display;
}
#endif /* HAVE_STARTUP_NOTIFICATION */

void
_wnck_screen_change_workspace_name (WnckScreen *screen,
                                    int         number,
                                    const char *name)
{
  int n_spaces;
  char **names;
  int i;
  
  n_spaces = wnck_screen_get_workspace_count (screen);

  names = g_new0 (char*, n_spaces + 1);

  i = 0;
  while (i < n_spaces)
    {
      if (i == number)
        names[i] = (char*) name;
      else
        {
          WnckWorkspace *workspace;
          workspace = wnck_screen_get_workspace (screen, i);
          if (workspace)
            names[i] = (char*) wnck_workspace_get_name (workspace);
          else
            names[i] = (char*) ""; /* maybe this should be a g_warning() */
        }
      
      ++i;
    }

  _wnck_set_utf8_list (screen->priv->xroot,
                       _wnck_atom_get ("_NET_DESKTOP_NAMES"),
                       names);

  g_free (names);
}
