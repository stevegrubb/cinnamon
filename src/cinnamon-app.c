/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#define GMENU_I_KNOW_THIS_IS_UNSTABLE
#include <gmenu-desktopappinfo.h>

#include <meta/display.h>

#include "cinnamon-app-private.h"
#include "cinnamon-enum-types.h"
#include "cinnamon-global-private.h"
#include "cinnamon-util.h"
#include "cinnamon-app-system-private.h"
#include "cinnamon-window-tracker-private.h"
#include "st.h"

/* This is mainly a memory usage optimization - the user is going to
 * be running far fewer of the applications at one time than they have
 * installed.  But it also just helps keep the code more logically
 * separated.
 */
typedef struct {
  guint refcount;

  /* Signal connection to dirty window sort list on workspace changes */
  guint workspace_switch_id;

  GSList *windows;

  /* Whether or not we need to resort the windows; this is done on demand */
  guint window_sort_stale : 1;
} CinnamonAppRunningState;

/**
 * SECTION:cinnamon-app
 * @short_description: Object representing an application
 *
 * This object wraps a #GMenuTreeEntry, providing methods and signals
 * primarily useful for running applications.
 */
struct _CinnamonApp
{
  GObject parent;

  CinnamonGlobal *global;

  int started_on_workspace;

  CinnamonAppState state;

  GMenuTreeEntry *entry; /* If NULL, this app is backed by one or more
                          * MetaWindow.  For purposes of app title
                          * etc., we use the first window added,
                          * because it's most likely to be what we
                          * want (e.g. it will be of TYPE_NORMAL from
                          * the way cinnamon-window-tracker.c works).
                          */
  GMenuDesktopAppInfo *info;

  CinnamonAppRunningState *running_state;

  char *window_id_string;

  char *keywords;
  char *unique_name;

  gboolean hidden_as_duplicate;
  gboolean is_flatpak;
};

G_DEFINE_TYPE (CinnamonApp, cinnamon_app, G_TYPE_OBJECT);

enum {
  PROP_0,
  PROP_STATE
};

enum {
  WINDOWS_CHANGED,
  LAST_SIGNAL
};

static guint cinnamon_app_signals[LAST_SIGNAL] = { 0 };

static void create_running_state (CinnamonApp *app);
static void unref_running_state (CinnamonAppRunningState *state);

static void
cinnamon_app_get_property (GObject    *gobject,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  CinnamonApp *app = CINNAMON_APP (gobject);

  switch (prop_id)
    {
    case PROP_STATE:
      g_value_set_enum (value, app->state);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

const char *
cinnamon_app_get_id (CinnamonApp *app)
{
  if (app->entry)
    return gmenu_tree_entry_get_desktop_file_id (app->entry);
  return app->window_id_string;
}

char *
cinnamon_app_get_flatpak_app_id (CinnamonApp *app)
{
  if (app->info)
  {
    gchar *id;

    id = g_strdup (gmenu_desktopappinfo_get_flatpak_app_id (app->info));

    if (id != NULL)
    {
        return id;
    }
    else
    {
        const gchar *desktop_file = cinnamon_app_get_id (app);

        gchar **split = g_strsplit (desktop_file, ".desktop", -1);
        id = g_strdup (split[0]);
        g_strfreev (split);

        return id;
    }
  }

  // This should never occur
  return NULL;
}

static MetaWindow *
window_backed_app_get_window (CinnamonApp     *app)
{
  g_assert (app->info == NULL);
  if (app->running_state)
    {
      g_assert (app->running_state->windows);
      return app->running_state->windows->data;
    }
  else
    return NULL;
}

static ClutterActor *
get_actor_for_icon_name (CinnamonApp *app,
                         const gchar *icon_name,
                         gint         size)
{
  ClutterActor *actor;
  GIcon *icon;

  icon = NULL;
  actor = NULL;

  if (g_path_is_absolute (icon_name))
    {
      GFile *icon_file;

      icon_file = g_file_new_for_path (icon_name);
      icon = g_file_icon_new (icon_file);

      g_object_unref (icon_file);
    }
  else
    {
      icon = g_themed_icon_new (icon_name);
    }

  if (icon != NULL)
  {
    actor = g_object_new (ST_TYPE_ICON, "gicon", icon, "icon-size", size, NULL);
    g_object_unref (icon);
  }

  return actor;
}

static ClutterActor *
get_failsafe_icon (int size)
{
  GIcon *icon = g_themed_icon_new ("application-x-executable");
  ClutterActor *actor = g_object_new (ST_TYPE_ICON, "gicon", icon, "icon-size", size, NULL);
  g_object_unref (icon);
  return actor;
}

static ClutterActor *
window_backed_app_get_icon (CinnamonApp *app,
                            int          size)
{
  MetaWindow *window = NULL;
  GdkPixbuf *pixbuf;
  gint scale;
  CinnamonGlobal *global;
  StThemeContext *context;

  global = app->global;
  context = st_theme_context_get_for_stage (global->stage);
  g_object_get (context, "scale-factor", &scale, NULL);

  /* During a state transition from running to not-running for
   * window-backend apps, it's possible we get a request for the icon.
   * Avoid asserting here and just return an empty image.
   */
  if (app->running_state != NULL)
    window = window_backed_app_get_window (app);

  size *= scale;

  if (window == NULL)
    return get_failsafe_icon (size);

  pixbuf = meta_window_create_icon (window, size);

  if (pixbuf == NULL)
    return get_failsafe_icon (size);

  return st_texture_cache_load_from_pixbuf (pixbuf, size);
}

/**
 * cinnamon_app_create_icon_texture:
 * @app: a #CinnamonApp
 * @size: the size of the icon to create
 *
 * Look up the icon for this application, and create a #ClutterTexture
 * for it at the given size.
 *
 * Return value: (transfer none): A floating #ClutterActor
 */
ClutterActor *
cinnamon_app_create_icon_texture (CinnamonApp   *app,
                                  int            size)
{
  GIcon *icon;
  ClutterActor *ret;

  ret = NULL;

  if (app->entry == NULL)
    return window_backed_app_get_icon (app, size);

  icon = g_app_info_get_icon (G_APP_INFO (app->info));

  if (icon != NULL)
    ret = g_object_new (ST_TYPE_ICON, "gicon", icon, "icon-size", size, NULL);

  if (ret == NULL)
    ret = get_failsafe_icon (size);

  return ret;
}

/**
 * cinnamon_app_create_icon_texture_for_window:
 * @app: a #CinnamonApp
 * @size: the size of the icon to create
 * @for_window: (nullable): Optional - the backing MetaWindow to look up for.
 *
 * Look up the icon for this application, and create a #ClutterTexture
 * for it at the given size.  If for_window is NULL, it bases the icon
 * off the most-recently-used window for the app, otherwise it attempts to
 * use for_window for determining the icon.
 *
 * Return value: (transfer none): A floating #ClutterActor
 */
ClutterActor *
cinnamon_app_create_icon_texture_for_window (CinnamonApp   *app,
                                             int            size,
                                             MetaWindow    *for_window)
{
  MetaWindow *window;

  window = NULL;

  if (app->running_state != NULL)
  {
    const gchar *icon_name;

    if (for_window != NULL)
      {
        if (g_slist_find (app->running_state->windows, for_window) != NULL)
          {
            window = for_window;
          }
        else
          {
            g_warning ("cinnamon_app_create_icon_texture: MetaWindow %p provided that does not match App %p",
                       for_window, app);
          }
      }

    if (window != NULL)
      {
        icon_name = meta_window_get_icon_name (window);

        if (icon_name != NULL)
          {
            return get_actor_for_icon_name (app, icon_name, size);
          }
      }
  }

  return cinnamon_app_create_icon_texture (app, size);
}

static const char *
get_common_name (CinnamonApp *app)
{
  if (app->entry)
    return g_app_info_get_name (G_APP_INFO (app->info));
  else if (app->running_state == NULL)
    return _("Unknown");
  else
    {
      MetaWindow *window = window_backed_app_get_window (app);
      const char *name = NULL;

      if (window)
        name = meta_window_get_wm_class (window);
      if (!name)
        name = _("Unknown");
      return name;
    }
}

const char *
cinnamon_app_get_name (CinnamonApp *app)
{
  if (app->unique_name)
    return app->unique_name;

  return get_common_name (app);
}

const char *
cinnamon_app_get_description (CinnamonApp *app)
{
  if (app->entry)
    return g_app_info_get_description (G_APP_INFO (app->info));
  else
    return NULL;
}

const char *
cinnamon_app_get_keywords (CinnamonApp *app)
{
  const char * const *keywords;
  const char *keyword;
  gint i;
  gchar *ret = NULL;

  if (app->keywords)
    return app->keywords;

  if (app->info)
    keywords = gmenu_desktopappinfo_get_keywords (app->info);
  else
    keywords = NULL;

  if (keywords != NULL)
    {
      GString *keyword_list = g_string_new(NULL);

      for (i = 0; keywords[i] != NULL; i++)
        {
          keyword = keywords[i];
          g_string_append_printf (keyword_list, "%s;", keyword);
        }

      ret = g_string_free (keyword_list, FALSE);
    }

    app->keywords = ret;

    return ret;
}

gboolean
cinnamon_app_get_nodisplay (CinnamonApp *app)
{
  if (app->hidden_as_duplicate)
    {
      return TRUE;
    }

  if (app->entry)
    {
      g_return_val_if_fail (app->info != NULL, TRUE);
      return gmenu_desktopappinfo_get_nodisplay (app->info);
      // return !g_app_info_should_show (G_APP_INFO (app->info));
    }

  return FALSE;
}

/**
 * cinnamon_app_is_window_backed:
 *
 * A window backed application is one which represents just an open
 * window, i.e. there's no .desktop file association, so we don't know
 * how to launch it again.
 */
gboolean
cinnamon_app_is_window_backed (CinnamonApp *app)
{
  return app->entry == NULL;
}

typedef struct {
  MetaWorkspace *workspace;
  GSList **transients;
} CollectTransientsData;

static gboolean
collect_transients_on_workspace (MetaWindow *window,
                                 gpointer    datap)
{
  CollectTransientsData *data = datap;

  if (data->workspace && meta_window_get_workspace (window) != data->workspace)
    return TRUE;

  *data->transients = g_slist_prepend (*data->transients, window);
  return TRUE;
}

/* The basic idea here is that when we're targeting a window,
 * if it has transients we want to pick the most recent one
 * the user interacted with.
 * This function makes raising GEdit with the file chooser
 * open work correctly.
 */
static MetaWindow *
find_most_recent_transient_on_same_workspace (MetaDisplay *display,
                                              MetaWindow  *reference)
{
  GSList *transients, *transients_sorted, *iter;
  MetaWindow *result;
  CollectTransientsData data;

  transients = NULL;
  data.workspace = meta_window_get_workspace (reference);
  data.transients = &transients;

  meta_window_foreach_transient (reference, collect_transients_on_workspace, &data);

  transients_sorted = meta_display_sort_windows_by_stacking (display, transients);
  /* Reverse this so we're top-to-bottom (yes, we should probably change the order
   * returned from the sort_windows_by_stacking function)
   */
  transients_sorted = g_slist_reverse (transients_sorted);
  g_slist_free (transients);
  transients = NULL;

  result = NULL;
  for (iter = transients_sorted; iter; iter = iter->next)
    {
      MetaWindow *window = iter->data;
      MetaWindowType wintype = meta_window_get_window_type (window);

      /* Don't want to focus UTILITY types, like the Gimp toolbars */
      if (wintype == META_WINDOW_NORMAL ||
          wintype == META_WINDOW_DIALOG)
        {
          result = window;
          break;
        }
    }
  g_slist_free (transients_sorted);
  return result;
}

/**
 * cinnamon_app_activate_window:
 * @app: a #CinnamonApp
 * @window: (nullable): Window to be focused
 * @timestamp: Event timestamp
 *
 * Bring all windows for the given app to the foreground,
 * but ensure that @window is on top.  If @window is %NULL,
 * the window with the most recent user time for the app
 * will be used.
 *
 * This function has no effect if @app is not currently running.
 */
void
cinnamon_app_activate_window (CinnamonApp     *app,
                           MetaWindow   *window,
                           guint32       timestamp)
{
  GSList *windows;

  if (app->state != CINNAMON_APP_STATE_RUNNING)
    return;

  windows = cinnamon_app_get_windows (app);
  if (window == NULL && windows)
    window = windows->data;

  if (!g_slist_find (windows, window))
    return;
  else
    {
      GSList *iter;
      CinnamonGlobal *global = app->global;
      MetaScreen *screen = global->meta_screen;
      MetaDisplay *display = global->meta_display;
      MetaWorkspace *active = meta_screen_get_active_workspace (screen);
      MetaWorkspace *workspace = meta_window_get_workspace (window);
      guint32 last_user_timestamp = meta_display_get_last_user_time (display);
      MetaWindow *most_recent_transient;

      if (meta_display_xserver_time_is_before (display, timestamp, last_user_timestamp))
        {
          meta_window_set_demands_attention (window);
          return;
        }

      /* Now raise all the other windows for the app that are on
       * the same workspace, in reverse order to preserve the stacking.
       */
      for (iter = windows; iter; iter = iter->next)
        {
          MetaWindow *other_window = iter->data;

          if (other_window != window)
            meta_window_raise (other_window);
        }

      /* If we have a transient that the user's interacted with more recently than
       * the window, pick that.
       */
      most_recent_transient = find_most_recent_transient_on_same_workspace (display, window);
      if (most_recent_transient
          && meta_display_xserver_time_is_before (display,
                                                  meta_window_get_user_time (window),
                                                  meta_window_get_user_time (most_recent_transient)))
        window = most_recent_transient;

      if (active != workspace)
        meta_workspace_activate_with_focus (workspace, window, timestamp);
      else
        meta_window_activate (window, timestamp);
    }
}

/**
 * cinnamon_app_activate:
 * @app: a #CinnamonApp
 *
 * Like cinnamon_app_activate_full(), but using the default workspace and
 * event timestamp.
 */
void
cinnamon_app_activate (CinnamonApp      *app)
{
  return cinnamon_app_activate_full (app, -1, 0);
}

/**
 * cinnamon_app_activate_full:
 * @app: a #CinnamonApp
 * @workspace: launch on this workspace, or -1 for default. Ignored if
 *   activating an existing window
 * @timestamp: Event timestamp
 *
 * Perform an appropriate default action for operating on this application,
 * dependent on its current state.  For example, if the application is not
 * currently running, launch it.  If it is running, activate the most
 * recently used NORMAL window (or if that window has a transient, the most
 * recently used transient for that window).
 */
void
cinnamon_app_activate_full (CinnamonApp      *app,
                         int            workspace,
                         guint32        timestamp)
{
  CinnamonGlobal *global;

  global = app->global;

  if (timestamp == 0)
    timestamp = cinnamon_global_get_current_time (global);

  switch (app->state)
    {
      case CINNAMON_APP_STATE_STOPPED:
        {
          GError *error = NULL;
          if (!cinnamon_app_launch (app,
                                 timestamp,
                                 NULL,
                                 workspace,
                                 NULL,
                                 &error))
            {
              char *msg;
              msg = g_strdup_printf (_("Failed to launch '%s'"), cinnamon_app_get_name (app));
              cinnamon_global_notify_error (global,
                                         msg,
                                         error->message);
              g_free (msg);
              g_clear_error (&error);
            }
        }
        break;
      case CINNAMON_APP_STATE_STARTING:
        break;
      case CINNAMON_APP_STATE_RUNNING:
        cinnamon_app_activate_window (app, NULL, timestamp);
        break;
      default:
        g_warning("cinnamon_app_activate_full: default case");
        break;
    }
}

/**
 * cinnamon_app_open_new_window:
 * @app: a #CinnamonApp
 * @workspace: open on this workspace, or -1 for default
 *
 * Request that the application create a new window.
 */
void
cinnamon_app_open_new_window (CinnamonApp      *app,
                           int            workspace)
{
  g_return_if_fail (app->entry != NULL);

  /* Here we just always launch the application again, even if we know
   * it was already running.  For most applications this
   * should have the effect of creating a new window, whether that's
   * a second process (in the case of Calculator) or IPC to existing
   * instance (Firefox).  There are a few less-sensical cases such
   * as say Pidgin.  Ideally, we have the application express to us
   * that it supports an explicit new-window action.
   */
  cinnamon_app_launch (app,
                    0,
                    NULL,
                    workspace,
                    NULL,
                    NULL);
}

/**
 * cinnamon_app_can_open_new_window:
 * @app: a #CinnamonApp
 *
 * Returns %TRUE if the app supports opening a new window through
 * cinnamon_app_open_new_window() (ie, if calling that function will
 * result in actually opening a new window and not something else,
 * like presenting the most recently active one)
 */
gboolean
cinnamon_app_can_open_new_window (CinnamonApp *app)
{
  /* Apps that are not running can always open new windows, because
     activating them would open the first one */
  if (!app->running_state)
    return TRUE;

  /* If the app doesn't have a desktop file, then nothing is possible */
  if (!app->info)
    return FALSE;

  /* If the app is explicitly telling us, then we know for sure */
  if (gmenu_desktopappinfo_has_key (GMENU_DESKTOPAPPINFO (app->info),
                                  "X-GNOME-SingleWindow"))
    return !gmenu_desktopappinfo_get_boolean (GMENU_DESKTOPAPPINFO (app->info),
                                            "X-GNOME-SingleWindow");

  /* In all other cases, we don't have a reliable source of information
     or a decent heuristic, so we err on the compatibility side and say
     yes.
  */
  return TRUE;
}

/**
 * cinnamon_app_get_state:
 * @app: a #CinnamonApp
 *
 * Returns: State of the application
 */
CinnamonAppState
cinnamon_app_get_state (CinnamonApp *app)
{
  return app->state;
}

/**
 * cinnamon_app_get_is_flatpak:
 * @app: a #CinnamonApp
 *
 * Returns: TRUE if #app is a flatpak app, FALSE if not
 */
gboolean
cinnamon_app_get_is_flatpak (CinnamonApp *app)
{
  return app->is_flatpak;
}

typedef struct {
  CinnamonApp *app;
  MetaWorkspace *active_workspace;
} CompareWindowsData;

static int
cinnamon_app_compare_windows (gconstpointer   a,
                           gconstpointer   b,
                           gpointer        datap)
{
  MetaWindow *win_a = (gpointer)a;
  MetaWindow *win_b = (gpointer)b;
  CompareWindowsData *data = datap;
  gboolean ws_a, ws_b;
  gboolean vis_a, vis_b;

  ws_a = meta_window_get_workspace (win_a) == data->active_workspace;
  ws_b = meta_window_get_workspace (win_b) == data->active_workspace;

  if (ws_a && !ws_b)
    return -1;
  else if (!ws_a && ws_b)
    return 1;

  vis_a = meta_window_showing_on_its_workspace (win_a);
  vis_b = meta_window_showing_on_its_workspace (win_b);

  if (vis_a && !vis_b)
    return -1;
  else if (!vis_a && vis_b)
    return 1;

  return meta_window_get_user_time (win_b) - meta_window_get_user_time (win_a);
}

/**
 * cinnamon_app_get_windows:
 * @app:
 *
 * Get the toplevel, interesting windows which are associated with this
 * application.  The returned list will be sorted first by whether
 * they're on the active workspace, then by whether they're visible,
 * and finally by the time the user last interacted with them.
 *
 * Returns: (transfer none) (element-type MetaWindow): List of windows
 */
GSList *
cinnamon_app_get_windows (CinnamonApp *app)
{
  if (app->running_state == NULL)
    return NULL;

  if (app->running_state->window_sort_stale)
    {
      CompareWindowsData data;
      data.app = app;
      data.active_workspace = meta_screen_get_active_workspace (app->global->meta_screen);
      app->running_state->windows = g_slist_sort_with_data (app->running_state->windows, cinnamon_app_compare_windows, &data);
      app->running_state->window_sort_stale = FALSE;
    }

  return app->running_state->windows;
}

guint
cinnamon_app_get_n_windows (CinnamonApp *app)
{
  if (app->running_state == NULL)
    return 0;
  return g_slist_length (app->running_state->windows);
}

gboolean
cinnamon_app_is_on_workspace (CinnamonApp *app,
                           MetaWorkspace   *workspace)
{
  GSList *iter;

  if (app->state == CINNAMON_APP_STATE_STARTING)
    {
      if (app->started_on_workspace == -1 ||
          meta_workspace_index (workspace) == app->started_on_workspace)
        return TRUE;
      else
        return FALSE;
    }

  if (app->running_state == NULL)
    return FALSE;

  for (iter = app->running_state->windows; iter; iter = iter->next)
    {
      if (meta_window_get_workspace (iter->data) == workspace)
        return TRUE;
    }

  return FALSE;
}

CinnamonApp *
_cinnamon_app_new_for_window (MetaWindow      *window)
{
  CinnamonApp *app;

  app = g_object_new (CINNAMON_TYPE_APP, NULL);

  app->window_id_string = g_strdup_printf ("window:%d", meta_window_get_stable_sequence (window));

  _cinnamon_app_add_window (app, window);

  return app;
}

CinnamonApp *
_cinnamon_app_new (GMenuTreeEntry *info)
{
  CinnamonApp *app;

  app = g_object_new (CINNAMON_TYPE_APP, NULL);

  _cinnamon_app_set_entry (app, info);

  return app;
}

void
_cinnamon_app_set_entry (CinnamonApp       *app,
                      GMenuTreeEntry *entry)
{
  g_clear_pointer (&app->entry, gmenu_tree_item_unref);
  g_clear_object (&app->info);

  /* If our entry has changed, our name may have as well, so clear
   * anything set by appsys while deduplicating desktop items. */
  g_clear_pointer (&app->unique_name, g_free);
  app->hidden_as_duplicate = FALSE;

  app->entry = gmenu_tree_item_ref (entry);

  if (entry != NULL)
    {
      app->info = g_object_ref (gmenu_tree_entry_get_app_info (entry));
      app->is_flatpak = app->info && gmenu_desktopappinfo_get_is_flatpak (app->info);
    }
}

static void
cinnamon_app_state_transition (CinnamonApp      *app,
                            CinnamonAppState  state)
{
  if (app->state == state)
    return;
  g_return_if_fail (!(app->state == CINNAMON_APP_STATE_RUNNING &&
                      state == CINNAMON_APP_STATE_STARTING));
  app->state = state;

  if (app->state == CINNAMON_APP_STATE_STOPPED && app->running_state)
    {
      unref_running_state (app->running_state);
      app->running_state = NULL;
    }

  _cinnamon_app_system_notify_app_state_changed (cinnamon_app_system_get_default (), app);

  g_object_notify (G_OBJECT (app), "state");
}

static void
cinnamon_app_on_unmanaged (MetaWindow      *window,
                        CinnamonApp *app)
{
  _cinnamon_app_remove_window (app, window);
}

static void
cinnamon_app_on_ws_switch (MetaScreen         *screen,
                        int                 from,
                        int                 to,
                        MetaMotionDirection direction,
                        gpointer            data)
{
  CinnamonApp *app = CINNAMON_APP (data);

  g_assert (app->running_state != NULL);

  app->running_state->window_sort_stale = TRUE;

  g_signal_emit (app, cinnamon_app_signals[WINDOWS_CHANGED], 0);
}

void
_cinnamon_app_add_window (CinnamonApp        *app,
                       MetaWindow      *window)
{
  if (app->running_state && g_slist_find (app->running_state->windows, window))
    return;

  g_object_freeze_notify (G_OBJECT (app));

  if (!app->running_state)
      create_running_state (app);

  app->running_state->window_sort_stale = TRUE;
  app->running_state->windows = g_slist_prepend (app->running_state->windows, g_object_ref (window));
  g_signal_connect (window, "unmanaged", G_CALLBACK(cinnamon_app_on_unmanaged), app);

  if (app->state != CINNAMON_APP_STATE_STARTING)
    cinnamon_app_state_transition (app, CINNAMON_APP_STATE_RUNNING);

  g_object_thaw_notify (G_OBJECT (app));

  g_signal_emit (app, cinnamon_app_signals[WINDOWS_CHANGED], 0);
}

void
_cinnamon_app_remove_window (CinnamonApp   *app,
                          MetaWindow *window)
{
  g_assert (app->running_state != NULL);

  if (!g_slist_find (app->running_state->windows, window))
    return;

  g_signal_handlers_disconnect_by_func (window, G_CALLBACK(cinnamon_app_on_unmanaged), app);
  g_object_unref (window);
  app->running_state->windows = g_slist_remove (app->running_state->windows, window);

  if (app->running_state->windows == NULL)
    cinnamon_app_state_transition (app, CINNAMON_APP_STATE_STOPPED);

  g_signal_emit (app, cinnamon_app_signals[WINDOWS_CHANGED], 0);
}

/**
 * cinnamon_app_get_pids:
 * @app: a #CinnamonApp
 *
 * Returns: (transfer container) (element-type int): An unordered list of process identifiers associated with this application.
 */
GSList *
cinnamon_app_get_pids (CinnamonApp *app)
{
  GSList *result;
  GSList *iter;

  result = NULL;
  for (iter = cinnamon_app_get_windows (app); iter; iter = iter->next)
    {
      MetaWindow *window = iter->data;
      int pid = meta_window_get_pid (window);
      /* Note in the (by far) common case, app will only have one pid, so
       * we'll hit the first element, so don't worry about O(N^2) here.
       */
      if (!g_slist_find (result, GINT_TO_POINTER (pid)))
        result = g_slist_prepend (result, GINT_TO_POINTER (pid));
    }
  return result;
}

void
_cinnamon_app_handle_startup_sequence (CinnamonApp          *app,
                                    SnStartupSequence *sequence)
{
  gboolean starting = !sn_startup_sequence_get_completed (sequence);

  /* The Cinnamon design calls for on application launch, the app title
   * appears at top, and no X window is focused.  So when we get
   * a startup-notification for this app, transition it to STARTING
   * if it's currently stopped, set it as our application focus,
   * but focus the no_focus window.
   */
  if (starting && app->state == CINNAMON_APP_STATE_STOPPED)
    {
      MetaScreen *screen = app->global->meta_screen;
      MetaDisplay *display = meta_screen_get_display (screen);

      cinnamon_app_state_transition (app, CINNAMON_APP_STATE_STARTING);
      meta_display_focus_the_no_focus_window (display, screen,
                                              sn_startup_sequence_get_timestamp (sequence));
      app->started_on_workspace = sn_startup_sequence_get_workspace (sequence);
    }

  if (!starting)
    {
      if (app->running_state && app->running_state->windows)
        cinnamon_app_state_transition (app, CINNAMON_APP_STATE_RUNNING);
      else /* application have > 1 .desktop file */
        cinnamon_app_state_transition (app, CINNAMON_APP_STATE_STOPPED);
    }
}

const char *
_cinnamon_app_get_common_name (CinnamonApp *app)
{
  return get_common_name (app);
}

void
_cinnamon_app_set_unique_name (CinnamonApp *app,
                               gchar       *unique_name)
{
  if (app->unique_name)
    {
      g_free (app->unique_name);
    }

  app->unique_name = unique_name;
}

const char *
_cinnamon_app_get_unique_name (CinnamonApp *app)
{
  return app->unique_name;
}

const char *
_cinnamon_app_get_executable (CinnamonApp *app)
{
  if (app->entry)
    {
      return g_app_info_get_executable (G_APP_INFO (app->info));
    }

  return NULL;
}

const char *
_cinnamon_app_get_desktop_path (CinnamonApp *app)
{
  if (app->entry)
    {
      return gmenu_desktopappinfo_get_filename (app->info);
    }

  return NULL;
}

void
_cinnamon_app_set_hidden_as_duplicate (CinnamonApp *app,
                                     gboolean     hide)
{
  app->hidden_as_duplicate = hide;
}

/**
 * cinnamon_app_request_quit:
 * @app: A #CinnamonApp
 *
 * Initiate an asynchronous request to quit this application.
 * The application may interact with the user, and the user
 * might cancel the quit request from the application UI.
 *
 * This operation may not be supported for all applications.
 *
 * Returns: %TRUE if a quit request is supported for this application
 */
gboolean
cinnamon_app_request_quit (CinnamonApp   *app)
{
  CinnamonGlobal *global;
  GSList *iter;

  if (app->state != CINNAMON_APP_STATE_RUNNING)
    return FALSE;

  /* TODO - check for an XSMP connection; we could probably use that */

  global = app->global;

  for (iter = app->running_state->windows; iter; iter = iter->next)
    {
      MetaWindow *win = iter->data;

      if (!meta_window_can_close (win))
        continue;

      meta_window_delete (win, cinnamon_global_get_current_time (global));
    }
  return TRUE;
}

static void
_gather_pid_callback (GDesktopAppInfo   *gapp,
                      GPid               pid,
                      gpointer           data)
{
  CinnamonApp *app;
  CinnamonWindowTracker *tracker;

  g_return_if_fail (data != NULL);

  app = CINNAMON_APP (data);
  tracker = cinnamon_window_tracker_get_default ();

  _cinnamon_window_tracker_add_child_process_app (tracker,
                                               pid,
                                               app);
}

static void
apply_discrete_gpu_env (GAppLaunchContext *context)
{
  g_app_launch_context_setenv (context, "__NV_PRIME_RENDER_OFFLOAD", "1");
  g_app_launch_context_setenv (context, "__GLX_VENDOR_LIBRARY_NAME", "nvidia");
}

static gboolean
real_app_launch (CinnamonApp   *app,
                  guint         timestamp,
                  GList        *uris,
                  int           workspace,
                  char        **startup_id,
                  gboolean      offload,
                  GError      **error)
{
  GdkAppLaunchContext *context;
  gboolean ret;
  CinnamonGlobal *global;
  MetaScreen *screen;
  GdkDisplay *gdisplay;

  if (startup_id)
    *startup_id = NULL;

  if (app->entry == NULL)
    {
      MetaWindow *window = window_backed_app_get_window (app);
      /* We can't pass URIs into a window; shouldn't hit this
       * code path.  If we do, fix the caller to disallow it.
       */
      g_return_val_if_fail (uris == NULL, TRUE);

      meta_window_activate (window, timestamp);
      return TRUE;
    }

  global = app->global;
  screen = global->meta_screen;
  gdisplay = global->gdk_display;

  if (timestamp == 0)
    timestamp = cinnamon_global_get_current_time (global);

  if (workspace < 0)
    workspace = meta_screen_get_active_workspace_index (screen);

  context = gdk_display_get_app_launch_context (gdisplay);
  gdk_app_launch_context_set_timestamp (context, timestamp);
  gdk_app_launch_context_set_desktop (context, workspace);

  GMenuDesktopAppInfo *launch_info;
  GMenuDesktopAppInfo *offload_appinfo = NULL;

  if (offload)
    {
      GKeyFile *keyfile;

      apply_discrete_gpu_env (G_APP_LAUNCH_CONTEXT (context));
      g_debug ("Offloading '%s' to discrete gpu.", cinnamon_app_get_name (app));

      /* Desktop files marked DBusActivatable are launched using their GApplication
       * interface. The offload environment variables aren't used in this case. So
       * construct a temporary appinfo via keyfile instead - this disables dbus
       * launching as a side-effect, since that requires the original filename.
       */

      keyfile = g_key_file_new ();
      if (!g_key_file_load_from_file (keyfile,
                                      gmenu_desktopappinfo_get_filename (app->info),
                                      G_KEY_FILE_NONE,
                                      error))
        {
            g_key_file_unref (keyfile);
            g_object_unref (context);
            return FALSE;
        }

      offload_appinfo = gmenu_desktopappinfo_new_from_keyfile (keyfile);
      g_key_file_unref (keyfile);

      launch_info = offload_appinfo;
    }
  else
    {
      launch_info = app->info;
    }

  ret = gmenu_desktopappinfo_launch_uris_as_manager (launch_info, uris,
                                                   G_APP_LAUNCH_CONTEXT (context),
                                                   G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL  | G_SPAWN_STDERR_TO_DEV_NULL,
                                                   NULL, NULL,
                                                   _gather_pid_callback, app,
                                                   error);
  g_object_unref (context);
  g_clear_object (&offload_appinfo);

  return ret;
}

/**
 * cinnamon_app_launch:
 * @timestamp: Event timestamp, or 0 for current event timestamp
 * @uris: (element-type utf8): List of uris to pass to application
 * @workspace: Start on this workspace, or -1 for default
 * @startup_id: (out): Returned startup notification ID, or %NULL if none
 * @error: A #GError
 */
gboolean
cinnamon_app_launch (CinnamonApp     *app,
                     guint            timestamp,
                     GList           *uris,
                     int              workspace,
                     char           **startup_id,
                     GError         **error)
{
  GMenuDesktopAppInfo *app_info = cinnamon_app_get_app_info(app);
  gboolean wants_offload = (app_info && gmenu_desktopappinfo_get_boolean(app_info, "PrefersNonDefaultGPU"));
  return real_app_launch (app,
                          timestamp,
                          uris,
                          workspace,
                          startup_id,
                          wants_offload,
                          error);
}

/**
 * cinnamon_app_launch_offloaded:
 * @timestamp: Event timestamp, or 0 for current event timestamp
 * @uris: (element-type utf8): List of uris to pass to application
 * @workspace: Start on this workspace, or -1 for default
 * @startup_id: (out): Returned startup notification ID, or %NULL if none
 * @error: A #GError
 *
 * Launch an application using the dedicated gpu (if available)
 */
gboolean
cinnamon_app_launch_offloaded (CinnamonApp     *app,
                               guint            timestamp,
                               GList           *uris,
                               int              workspace,
                               char           **startup_id,
                               GError         **error)
{
  return real_app_launch (app,
                          timestamp,
                          uris,
                          workspace,
                          startup_id,
                          TRUE,
                          error);
}

/**
 * cinnamon_app_get_app_info:
 * @app: a #CinnamonApp
 *
 * Returns: (transfer none): The #GMenuDesktopAppInfo for this app, or %NULL if backed by a window
 */
GMenuDesktopAppInfo *
cinnamon_app_get_app_info (CinnamonApp *app)
{
  return app->info;
}

/**
 * cinnamon_app_get_tree_entry:
 * @app: a #CinnamonApp
 *
 * Returns: (transfer none): The #GMenuTreeEntry for this app, or %NULL if backed by a window
 */
GMenuTreeEntry *
cinnamon_app_get_tree_entry (CinnamonApp *app)
{
  return app->entry;
}

static void
create_running_state (CinnamonApp *app)
{
  MetaScreen *screen;

  g_assert (app->running_state == NULL);

  screen = app->global->meta_screen;
  app->running_state = g_slice_new0 (CinnamonAppRunningState);
  app->running_state->refcount = 1;
  app->running_state->workspace_switch_id =
    g_signal_connect (screen, "workspace-switched", G_CALLBACK(cinnamon_app_on_ws_switch), app);
}

static void
unref_running_state (CinnamonAppRunningState *state)
{
  MetaScreen *screen;
  CinnamonGlobal *global;

  state->refcount--;
  if (state->refcount > 0)
    return;

  global = cinnamon_global_get ();
  screen = global->meta_screen;

  g_signal_handler_disconnect (screen, state->workspace_switch_id);
  g_slice_free (CinnamonAppRunningState, state);
}

static void
cinnamon_app_init (CinnamonApp *self)
{
  self->state = CINNAMON_APP_STATE_STOPPED;
  self->keywords = NULL;
  self->global = cinnamon_global_get ();
}

static void
cinnamon_app_dispose (GObject *object)
{
  CinnamonApp *app = CINNAMON_APP (object);

  if (app->entry)
    {
      gmenu_tree_item_unref (app->entry);
      app->entry = NULL;
    }

  if (app->info)
    {
      g_object_unref (app->info);
      app->info = NULL;
    }

  while (app->running_state)
    _cinnamon_app_remove_window (app, app->running_state->windows->data);

  g_clear_pointer (&app->keywords, g_free);
  g_clear_pointer (&app->unique_name, g_free);

  G_OBJECT_CLASS(cinnamon_app_parent_class)->dispose (object);
}

static void
cinnamon_app_finalize (GObject *object)
{
  CinnamonApp *app = CINNAMON_APP (object);

  g_free (app->window_id_string);

  G_OBJECT_CLASS(cinnamon_app_parent_class)->finalize (object);
}

static void
cinnamon_app_class_init(CinnamonAppClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = cinnamon_app_get_property;
  gobject_class->dispose = cinnamon_app_dispose;
  gobject_class->finalize = cinnamon_app_finalize;

  cinnamon_app_signals[WINDOWS_CHANGED] = g_signal_new ("windows-changed",
                                     CINNAMON_TYPE_APP,
                                     G_SIGNAL_RUN_LAST,
                                     0,
                                     NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);

  /**
   * CinnamonApp:state:
   *
   * The high-level state of the application, effectively whether it's
   * running or not, or transitioning between those states.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "Application state",
                                                      CINNAMON_TYPE_APP_STATE,
                                                      CINNAMON_APP_STATE_STOPPED,
                                                      G_PARAM_READABLE));
}
