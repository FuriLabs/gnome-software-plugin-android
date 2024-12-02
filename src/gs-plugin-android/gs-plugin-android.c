/*
 * Copyright (C) 2024 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gs-plugin-android.h"
#include <appstream.h>
#include <json-glib/json-glib.h>
#include <glib/gi18n.h>
#include <gnome-software.h>
#include <gs-app-list.h>
#include <gs-app-query.h>

struct _GsPluginAndroid
{
  GsPlugin parent;

  GDBusProxy *fdroid_proxy;  /* Proxy for FuriOS Android Store */
  GsAppList *installed_apps;  /* List of installed apps */
  GsAppList *updatable_apps;  /* List of apps with updates */
};

G_DEFINE_TYPE (GsPluginAndroid, gs_plugin_android, GS_TYPE_PLUGIN);

static void
fdroid_proxy_setup_cb (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginAndroid *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;
  GDBusProxy *proxy;

  proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_clear_object (&self->fdroid_proxy);
  self->fdroid_proxy = proxy;

  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_android_setup_finish (GsPlugin *plugin,
                                GAsyncResult *result,
                                GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_android_setup_async (GsPlugin *plugin,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_autoptr (GTask) task = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_android_setup_async);

  g_debug ("Android plugin version: %s", GS_PLUGIN_ANDROID_VERSION);

  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_NONE,
                            NULL,
                            "io.FuriOS.AndroidStore",
                            "/fdroid",
                            "io.FuriOS.AndroidStore.fdroid",
                            cancellable,
                            fdroid_proxy_setup_cb,
                            g_steal_pointer (&task));
}

static void
fdroid_update_cache_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginAndroid *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;
  GVariant *value;
  gboolean success;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (result == NULL) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  value = g_variant_get_child_value (result, 0);
  success = g_variant_get_boolean (value);
  g_variant_unref (value);

  gs_plugin_updates_changed (GS_PLUGIN (self));
  g_task_return_boolean (task, success);
}

static gboolean
gs_plugin_android_refresh_metadata_finish (GsPlugin *plugin,
                                           GAsyncResult *result,
                                           GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_android_refresh_metadata_async (GsPlugin *plugin,
                                          guint64 cache_age_secs,
                                          GsPluginRefreshMetadataFlags flags,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data)
{
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (plugin);
  g_autoptr (GTask) task = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_android_refresh_metadata_async);

  g_debug ("Refreshing repositories");

  gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_DOWNLOADING);
  g_dbus_proxy_call (self->fdroid_proxy,
                     "UpdateCache",
                     g_variant_new ("()"),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,  /* timeout, -1 for default */
                     cancellable,
                     fdroid_update_cache_cb,
                     g_steal_pointer (&task));
}

static void
fdroid_get_repositories_cb (GObject *source_object,
                            GAsyncResult *res,
                            gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (g_steal_pointer (&user_data));
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (g_task_get_source_object(task));
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) repositories = NULL;
  g_autoptr (GsAppList) list = gs_app_list_new ();

  repositories = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &local_error);
  if (repositories == NULL) {
    g_dbus_error_strip_remote_error (local_error);
    g_task_return_error (task, g_steal_pointer(&local_error));
    return;
  }

  GVariantIter *iter = NULL;
  const gchar *repo_name = NULL;
  const gchar *repo_url = NULL;
  g_variant_get (repositories, "(a(ss))", &iter);

  while (g_variant_iter_next (iter, "(&s&s)", &repo_name, &repo_url)) {
    g_autoptr (GsApp) app = NULL;

    g_debug ("Processing F-Droid repository: %s (%s)", repo_name, repo_url);

    app = gs_app_new (repo_name);
    gs_app_set_kind (app, AS_COMPONENT_KIND_REPOSITORY);
    gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
    gs_app_set_state (app, GS_APP_STATE_INSTALLED);
    gs_app_add_quirk (app, GS_APP_QUIRK_NOT_LAUNCHABLE);
    gs_app_set_name (app, GS_APP_QUALITY_NORMAL, repo_name);
    gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, repo_url);
    gs_app_set_metadata (app, "fdroid::repo-url", repo_url);
    gs_app_set_management_plugin (app, GS_PLUGIN (self));
    gs_app_set_metadata (app, "GnomeSoftware::SortKey", "300");
    gs_app_set_origin_ui (app, "F-Droid (Android)");

    gs_plugin_cache_add (GS_PLUGIN (self), repo_url, app);
    gs_app_list_add (list, g_steal_pointer (&app));
  }

  g_variant_iter_free (iter);
  g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
}

static void
fdroid_get_upgradable_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (g_steal_pointer (&user_data));
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (g_task_get_source_object (task));
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GsAppList) list = gs_app_list_new ();
  GVariantIter iter;
  GVariant *child;
  guint upgradable_count = 0;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &local_error);
  if (result == NULL) {
    g_dbus_error_strip_remote_error (local_error);
    g_task_return_error (task, g_steal_pointer (&local_error));
    return;
  }

  /* Parse upgradable apps and save them */
  g_variant_iter_init (&iter, g_variant_get_child_value (result, 0));
  while ((child = g_variant_iter_next_value (&iter))) {
    g_autoptr (GsApp) app = NULL;
    g_autoptr (GVariantDict) dict = NULL;
    const gchar *package_name = NULL;
    const gchar *name = NULL;
    const gchar *id = NULL;
    const gchar *current_version = NULL;
    const gchar *available_version = NULL;
    const gchar *repository = NULL;
    const gchar *package_info = NULL;

    dict = g_variant_dict_new (child);
    g_variant_dict_lookup (dict, "packageName", "&s", &package_name);
    g_variant_dict_lookup (dict, "name", "&s", &name);
    g_variant_dict_lookup (dict, "id", "&s", &id);
    g_variant_dict_lookup (dict, "currentVersion", "&s", &current_version);
    g_variant_dict_lookup (dict, "availableVersion", "&s", &available_version);
    g_variant_dict_lookup (dict, "repository", "&s", &repository);
    g_variant_dict_lookup (dict, "package", "&s", &package_info);

    if (package_name != NULL) {
      app = gs_app_new (id);
      gs_app_set_kind (app, AS_COMPONENT_KIND_DESKTOP_APP);
      gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
      gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
      gs_app_set_allow_cancel (app, FALSE);
      gs_app_set_management_plugin (app, GS_PLUGIN (self));

      if (name != NULL && *name != '\0')
        gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);
      else
        gs_app_set_name (app, GS_APP_QUALITY_LOWEST, package_name);

      gs_app_set_metadata (app, "android::package-name", id);
      if (repository != NULL)
        gs_app_set_metadata (app, "android-store::repository", repository);

      gs_app_add_source (app, id);
      gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "apk");
      gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
      gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

      if (current_version != NULL)
        gs_app_set_version (app, current_version);
      if (available_version != NULL)
        gs_app_set_update_version (app, available_version);

      gs_app_list_add (list, app);
      gs_app_list_add (self->updatable_apps, app);
      upgradable_count++;

      g_debug ("Found upgrade for %s: %s -> %s",
               package_name,
               current_version != NULL ? current_version : "unknown",
               available_version != NULL ? available_version : "unknown");
    }
    g_variant_unref (child);
  }

  if (upgradable_count > 0)
    g_debug ("Found %u upgradable Android apps", upgradable_count);
  else
    g_debug ("No upgradable Android apps found");

  g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
}

static void
fdroid_get_installed_apps_cb (GObject *source_object,
                              GAsyncResult *res,
                              gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (g_task_get_source_object (task));
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GsAppList) list = gs_app_list_new ();
  GVariantIter iter;
  GVariant *child;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &local_error);
  if (result == NULL) {
    g_dbus_error_strip_remote_error (local_error);
    g_task_return_error (task, g_steal_pointer (&local_error));
    return;
  }

  /* Clear previous list and build new one */
  gs_app_list_remove_all (self->installed_apps);

  g_variant_iter_init (&iter, g_variant_get_child_value (result, 0));
  while ((child = g_variant_iter_next_value (&iter))) {
    g_autoptr (GsApp) app = NULL;
    g_autoptr (GVariantDict) dict = NULL;
    const gchar *package_name = NULL;
    const gchar *name = NULL;
    const gchar *id = NULL;

    dict = g_variant_dict_new (child);
    g_variant_dict_lookup (dict, "packageName", "&s", &package_name);
    g_variant_dict_lookup (dict, "name", "&s", &name);
    g_variant_dict_lookup (dict, "id", "&s", &id);

    if (package_name != NULL) {
      app = gs_app_new (id);

      gs_app_set_kind (app, AS_COMPONENT_KIND_DESKTOP_APP);
      gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
      gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
      gs_app_add_quirk (app, GS_APP_QUIRK_HAS_SOURCE);
      gs_app_set_allow_cancel (app, FALSE);
      gs_app_set_management_plugin (app, GS_PLUGIN (self));
      gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

      if (name != NULL && *name != '\0')
        gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);
      else
        gs_app_set_name (app, GS_APP_QUALITY_LOWEST, package_name);

      gs_app_set_metadata (app, "android::package-name", package_name);
      gs_app_add_source (app, id);
      gs_app_set_state (app, GS_APP_STATE_INSTALLED);

      gs_app_list_add (list, app);
      gs_app_list_add (self->installed_apps, app);

      g_debug ("Added installed Android app: %s (package: %s)",
               gs_app_get_name (app), package_name);
    }
    g_variant_unref (child);
  }

  g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
}

static void
fdroid_search_cb (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (g_task_get_source_object (task));
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GsAppList) list = gs_app_list_new ();
  g_autoptr (JsonParser) parser = NULL;
  JsonNode *root;
  JsonArray *array;
  const gchar *json_data;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &local_error);
  if (result == NULL) {
    g_dbus_error_strip_remote_error (local_error);
    g_task_return_error (task, g_steal_pointer (&local_error));
    return;
  }

  g_variant_get (result, "(s)", &json_data);
  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json_data, -1, &local_error)) {
    g_task_return_error (task, g_steal_pointer (&local_error));
    return;
  }

  root = json_parser_get_root (parser);
  array = json_node_get_array (root);

  for (guint i = 0; i < json_array_get_length (array); i++) {
    JsonNode *element = json_array_get_element (array, i);
    JsonObject *app_obj = json_node_get_object (element);
    g_autoptr (GsApp) app = NULL;

    const gchar *id;
    const gchar *name;
    const gchar *summary;
    const gchar *description;
    const gchar *license;
    const gchar *author;
    const gchar *web_url;
    const gchar *icon_url;
    const gchar *repository;
    JsonObject *package;
    const gchar *version;
    gboolean is_installed = FALSE;

    id = json_object_get_string_member (app_obj, "id");
    name = json_object_get_string_member (app_obj, "name");
    summary = json_object_get_string_member (app_obj, "summary");
    description = json_object_get_string_member (app_obj, "description");
    license = json_object_get_string_member (app_obj, "license");
    author = json_object_get_string_member (app_obj, "author");
    web_url = json_object_get_string_member (app_obj, "web_url");
    repository = json_object_get_string_member (app_obj, "repository");

    package = json_object_get_object_member (app_obj, "package");
    if (package) {
      version = json_object_get_string_member (package, "version");
      icon_url = json_object_get_string_member (package, "icon_url");

      for (guint j = 0; j < gs_app_list_length (self->installed_apps); j++) {
        GsApp *installed_app = gs_app_list_index (self->installed_apps, j);
        const gchar *installed_name = gs_app_get_metadata_item (installed_app, "android::package-name");
        if (g_strcmp0 (installed_name, id) == 0) {
          is_installed = TRUE;
          break;
        }
      }
    }

    app = gs_app_new (id);
    gs_app_set_kind (app, AS_COMPONENT_KIND_DESKTOP_APP);
    gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
    gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
    gs_app_add_quirk (app, GS_APP_QUIRK_HAS_SOURCE);
    gs_app_set_metadata (app, "GnomeSoftware::Creator",
                         gs_plugin_get_name (GS_PLUGIN (self)));
    gs_app_set_management_plugin (app, GS_PLUGIN (self));
    gs_app_set_metadata (app, "android::package-name", id);
    gs_app_set_metadata (app, "android-store::repository", repository);
    gs_app_add_source (app, id);

    gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);
    gs_app_set_summary (app, GS_APP_QUALITY_NORMAL, summary);
    gs_app_set_description (app, GS_APP_QUALITY_NORMAL, description);
    gs_app_set_version (app, version);
    gs_app_set_license (app, GS_APP_QUALITY_NORMAL, license);
    gs_app_set_developer_name (app, author);
    gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, web_url);
    gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

    if (icon_url != NULL) {
        if (!g_str_has_prefix (icon_url, "http://") && !g_str_has_prefix (icon_url, "https://")) {
            g_debug ("App '%s' has invalid icon URL: %s", name, icon_url);
        } else {
            g_autoptr (GIcon) icon = gs_remote_icon_new (icon_url);
            gs_app_add_icon (app, icon);
        }
    }

    gs_app_set_state (app, is_installed ? GS_APP_STATE_INSTALLED : GS_APP_STATE_AVAILABLE);
    gs_app_list_add (list, app);
  }

  g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
}

static GsAppList *
gs_plugin_android_list_apps_finish (GsPlugin *plugin,
                                    GAsyncResult *result,
                                    GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
gs_plugin_android_list_apps_async (GsPlugin *plugin,
                                   GsAppQuery *query,
                                   GsPluginListAppsFlags flags,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (plugin);
  g_autoptr (GTask) task = NULL;
  GsAppQueryTristate is_installed = GS_APP_QUERY_TRISTATE_UNSET;
  GsAppQueryTristate is_source = GS_APP_QUERY_TRISTATE_UNSET;
  GsAppQueryTristate is_for_updates = GS_APP_QUERY_TRISTATE_UNSET;
  const gchar * const *keywords = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_android_list_apps_async);

  if (query != NULL) {
    is_source = gs_app_query_get_is_source (query);
    is_installed = gs_app_query_get_is_installed (query);
    is_for_updates = gs_app_query_get_is_for_update (query);
    keywords = gs_app_query_get_keywords (query);
  }

  /* Currently only support one query type at a time */
  if (gs_app_query_get_n_properties_set (query) != 1 ||
      is_source == GS_APP_QUERY_TRISTATE_FALSE ||
      is_installed == GS_APP_QUERY_TRISTATE_FALSE ||
      is_for_updates == GS_APP_QUERY_TRISTATE_FALSE) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Unsupported query");
    return;
  }

  if (is_source == GS_APP_QUERY_TRISTATE_TRUE) {
    g_debug ("Listing repositories");
    g_dbus_proxy_call (self->fdroid_proxy,
                       "GetRepositories",
                       g_variant_new ("()"),
                       G_DBUS_CALL_FLAGS_NONE,
                       -1,
                       cancellable,
                       fdroid_get_repositories_cb,
                       g_steal_pointer (&task));
  } else if (is_installed == GS_APP_QUERY_TRISTATE_TRUE) {
    g_debug ("Listing installed apps");
    g_dbus_proxy_call (self->fdroid_proxy,
                       "GetInstalledApps",
                       g_variant_new ("()"),
                       G_DBUS_CALL_FLAGS_NONE,
                       -1,
                       cancellable,
                       fdroid_get_installed_apps_cb,
                       g_steal_pointer (&task));
  } else if (is_for_updates == GS_APP_QUERY_TRISTATE_TRUE) {
    g_debug ("Listing updates");
    g_dbus_proxy_call (self->fdroid_proxy,
                       "GetUpgradable",
                       g_variant_new ("()"),
                       G_DBUS_CALL_FLAGS_NONE,
                       -1,
                       cancellable,
                       fdroid_get_upgradable_cb,
                       g_steal_pointer (&task));
  } else if (keywords != NULL) {
    g_autofree gchar *query_str = NULL;
    query_str = g_strjoinv (" ", (gchar **) keywords);
    g_debug ("Searching for apps: %s", query_str);

    g_dbus_proxy_call (self->fdroid_proxy,
                       "Search",
                       g_variant_new ("(s)", query_str),
                       G_DBUS_CALL_FLAGS_NONE,
                       -1,
                       cancellable,
                       fdroid_search_cb,
                       g_steal_pointer (&task));
  } else {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Unsupported query type");
  }
}

static void
fdroid_install_app_cb (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (g_task_get_source_object (task));
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) result = NULL;
  GsAppList *install_list = g_task_get_task_data (task);

  if (install_list == NULL || gs_app_list_length (install_list) == 0) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "No app in install list");
    return;
  }

  GsApp *app = gs_app_list_index (install_list, 0);
  if (app == NULL) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Failed to get app from install list");
    return;
  }

  const gchar *package_name = gs_app_get_metadata_item (app, "android::package-name");
  g_debug ("Installed F-Droid app: %s", package_name);

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &local_error);
  if (result == NULL) {
    gs_app_set_state_recover (app);
    g_dbus_error_strip_remote_error (local_error);
    g_task_return_error (task, g_steal_pointer (&local_error));
    return;
  }

  gs_app_set_state (app, GS_APP_STATE_INSTALLED);
  gs_plugin_updates_changed (GS_PLUGIN (self));
  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_android_install_apps_finish (GsPlugin *plugin,
                                       GAsyncResult *result,
                                       GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_android_install_apps_async (GsPlugin *plugin,
                                      GsAppList *list,
                                      GsPluginInstallAppsFlags flags,
                                      GsPluginProgressCallback progress_callback,
                                      gpointer progress_user_data,
                                      GsPluginAppNeedsUserActionCallback app_needs_user_action_callback,
                                      gpointer app_needs_user_action_data,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (plugin);
  g_autoptr (GTask) task = NULL;
  g_autoptr (GsAppList) install_list = gs_app_list_new ();

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_android_install_apps_async);

  /* So far, we only support installing one app at a time */
  if (flags &
      (GS_PLUGIN_INSTALL_APPS_FLAGS_NO_DOWNLOAD |
       GS_PLUGIN_INSTALL_APPS_FLAGS_NO_APPLY)) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Unsupported flags");
    return;
  }

  for (guint i = 0; i < gs_app_list_length (list); i++) {
    GsApp *app = gs_app_list_index (list, i);

    /* enable repo, handled by dedicated function */
    g_assert (gs_app_get_kind (app) != AS_COMPONENT_KIND_REPOSITORY);

    /* We can only install apps we know of */
    if (!gs_app_has_management_plugin (app, plugin)) {
      g_debug ("App is not managed by us, not installing");
      continue;
    }

    const gchar *package_name = gs_app_get_metadata_item (app, "android::package-name");
    if (package_name == NULL) {
      g_debug ("No package name found for app, skipping installation");
      continue;
    }

    g_debug ("Considering app %s for installation", package_name);

    gs_app_list_add (install_list, app);
    gs_app_set_state (app, GS_APP_STATE_INSTALLING);
  }

  if (gs_app_list_length (install_list) != 1) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Can only install one app at a time");
    return;
  }

  g_task_set_task_data (task, g_steal_pointer (&install_list), g_object_unref);
  GsApp *app = gs_app_list_index (g_task_get_task_data (task), 0);
  const gchar *package_name = gs_app_get_metadata_item (app, "android::package-name");

  g_dbus_proxy_call (self->fdroid_proxy,
                     "Install",
                     g_variant_new ("(s)", package_name),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     cancellable,
                     fdroid_install_app_cb,
                     g_steal_pointer (&task));
}

static void
fdroid_remove_repository_cb (GObject *source_object,
                             GAsyncResult *res,
                             gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) result = NULL;
  GsApp *app = g_task_get_task_data (task);

  g_debug ("Removing F-Droid repository: %s", gs_app_get_unique_id (app));

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &local_error);
  if (result == NULL) {
    gs_app_set_state_recover (app);
    g_dbus_error_strip_remote_error (local_error);
    g_task_return_error (task, g_steal_pointer (&local_error));
    return;
  }

  gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_android_remove_repository_finish (GsPlugin *plugin,
                                            GAsyncResult *result,
                                            GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_android_remove_repository_async (GsPlugin *plugin,
                                           GsApp *repo,
                                           GsPluginManageRepositoryFlags flags,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data)
{
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (plugin);
  g_autoptr (GTask) task = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_android_remove_repository_async);

  g_assert (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY);
  g_task_set_task_data (task, g_object_ref (repo), g_object_unref);

  gs_app_set_state (repo, GS_APP_STATE_REMOVING);

  g_dbus_proxy_call (self->fdroid_proxy,
                     "RemoveRepository",
                     g_variant_new ("(s)", gs_app_get_id (repo)),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     cancellable,
                     fdroid_remove_repository_cb,
                     g_steal_pointer (&task));
}

static void
fdroid_uninstall_app_cb (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (g_task_get_source_object (task));
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) result = NULL;
  GsApp *app = g_task_get_task_data (task);

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &local_error);
  if (result == NULL) {
    gs_app_set_state_recover (app);
    g_dbus_error_strip_remote_error (local_error);
    g_task_return_error (task, g_steal_pointer (&local_error));
    return;
  }

  gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
  gs_plugin_updates_changed (GS_PLUGIN (self));
  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_android_uninstall_apps_finish (GsPlugin *plugin,
                                         GAsyncResult *result,
                                         GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_android_uninstall_apps_async (GsPlugin *plugin,
                                        GsAppList *list,
                                        GsPluginUninstallAppsFlags flags,
                                        GsPluginProgressCallback progress_callback,
                                        gpointer progress_user_data,
                                        GsPluginAppNeedsUserActionCallback app_needs_user_action_callback,
                                        gpointer app_needs_user_action_data,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (plugin);
  g_autoptr (GTask) task = NULL;
  g_autoptr (GsAppList) uninstall_list = gs_app_list_new ();

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_android_uninstall_apps_async);

  for (guint i = 0; i < gs_app_list_length (list); i++) {
    GsApp *app = gs_app_list_index (list, i);

    g_assert (gs_app_get_kind (app) != AS_COMPONENT_KIND_REPOSITORY);
    g_debug ("Considering app %s for uninstallation", gs_app_get_unique_id (app));

    if (!gs_app_has_management_plugin (app, plugin)) {
      g_debug ("App %s is not managed by us, not uninstalling", gs_app_get_unique_id (app));
      continue;
    }

    gs_app_list_add (uninstall_list, app);
    gs_app_set_state (app, GS_APP_STATE_REMOVING);
  }

  if (gs_app_list_length (uninstall_list) != 1) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Can only uninstall one app at a time");
    return;
  }

  g_task_set_task_data (task, g_object_ref (gs_app_list_index (uninstall_list, 0)), g_object_unref);

  g_dbus_proxy_call (self->fdroid_proxy,
                     "UninstallApp",
                     g_variant_new ("(s)", gs_app_get_metadata_item (gs_app_list_index (uninstall_list, 0), "android::package-name")),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     cancellable,
                     fdroid_uninstall_app_cb,
                     g_steal_pointer (&task));
}

static gboolean
gs_plugin_android_filter_desktop_file_cb (GsPlugin *plugin,
                                          GsApp *app,
                                          const gchar *filename,
                                          GKeyFile *key_file,
                                          gpointer user_data)
{
  return strstr (filename, "/snapd/") == NULL &&
         strstr (filename, "/snap/") == NULL &&
         strstr (filename, "/flatpak/") == NULL &&
         g_key_file_has_group (key_file, "Desktop Entry") &&
         !g_key_file_has_key (key_file, "Desktop Entry", "X-Flatpak", NULL) &&
         !g_key_file_has_key (key_file, "Desktop Entry", "X-SnapInstanceName", NULL);
}

static gboolean
gs_plugin_android_launch_finish (GsPlugin *plugin,
                                 GAsyncResult *result,
                                 GError **error)
{
  return gs_plugin_app_launch_filtered_finish (plugin, result, error);
}

static void
gs_plugin_android_launch_async (GsPlugin *plugin,
                                GsApp *app,
                                GsPluginLaunchFlags flags,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)

{
  gs_plugin_app_launch_filtered_async (plugin, app, flags, gs_plugin_android_filter_desktop_file_cb, NULL, cancellable, callback, user_data);
}

static void
fdroid_upgrade_packages_cb (GObject *source_object,
                            GAsyncResult *res,
                            gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (g_task_get_source_object (task));
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) result = NULL;
  GsAppList *list = g_task_get_task_data (task);
  gboolean success = FALSE;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &local_error);
  if (result == NULL) {
    g_dbus_error_strip_remote_error (local_error);
    g_task_return_error (task, g_steal_pointer (&local_error));
    return;
  }

  g_variant_get (result, "(b)", &success);
  if (!success) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Failed to upgrade packages");
    return;
  }

  for (guint i = 0; i < gs_app_list_length (list); i++) {
    GsApp *app = gs_app_list_index (list, i);
    gs_app_set_state (app, GS_APP_STATE_INSTALLED);
    g_debug ("Updated app: %s", gs_app_get_unique_id (app));
  }

  gs_plugin_updates_changed (GS_PLUGIN (self));
  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_android_update_apps_finish (GsPlugin *plugin,
                                      GAsyncResult *result,
                                      GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_android_update_apps_async (GsPlugin *plugin,
                                     GsAppList *list,
                                     GsPluginUpdateAppsFlags flags,
                                     GsPluginProgressCallback progress_callback,
                                     gpointer progress_user_data,
                                     GsPluginAppNeedsUserActionCallback app_needs_user_action_callback,
                                     gpointer app_needs_user_action_data,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (plugin);
  g_autoptr (GTask) task = NULL;
  g_autoptr (GVariantBuilder) builder = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_android_update_apps_async);

  if (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY) {
    g_task_return_boolean (task, TRUE);
    return;
  }

  gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_WAITING);

  builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
  for (guint i = 0; i < gs_app_list_length (list); i++) {
    GsApp *app = gs_app_list_index (list, i);
    const gchar *package_name = gs_app_get_metadata_item (app, "android::package-name");
    if (package_name != NULL) {
      g_debug ("Adding package to upgrade: %s", package_name);
      g_variant_builder_add (builder, "s", package_name);
      gs_app_set_state (app, GS_APP_STATE_INSTALLING);
    }
  }

  g_task_set_task_data (task, g_object_ref (list), g_object_unref);

  g_dbus_proxy_call (self->fdroid_proxy,
                     "UpgradePackages",
                     g_variant_new ("(as)", builder),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     cancellable,
                     fdroid_upgrade_packages_cb,
                     g_steal_pointer (&task));
}

static void
gs_plugin_android_init (GsPluginAndroid *self)
{
  GsPlugin *plugin = GS_PLUGIN (self);

  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "icons");
  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "generic-updates");

  self->installed_apps = gs_app_list_new ();
  self->updatable_apps = gs_app_list_new ();
}

static void
gs_plugin_android_dispose (GObject *object)
{
  GsPluginAndroid *self = GS_PLUGIN_ANDROID (object);

  g_clear_object (&self->fdroid_proxy);
  g_clear_object (&self->installed_apps);
  g_clear_object (&self->updatable_apps);

  G_OBJECT_CLASS (gs_plugin_android_parent_class)->dispose (object);
}

static void
gs_plugin_android_class_init (GsPluginAndroidClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GsPluginClass *plugin_class = GS_PLUGIN_CLASS (klass);

  object_class->dispose = gs_plugin_android_dispose;

  plugin_class->setup_async = gs_plugin_android_setup_async;
  plugin_class->setup_finish = gs_plugin_android_setup_finish;
  plugin_class->refresh_metadata_async = gs_plugin_android_refresh_metadata_async;
  plugin_class->refresh_metadata_finish = gs_plugin_android_refresh_metadata_finish;
  plugin_class->list_apps_async = gs_plugin_android_list_apps_async;
  plugin_class->list_apps_finish = gs_plugin_android_list_apps_finish;
  plugin_class->install_apps_async = gs_plugin_android_install_apps_async;
  plugin_class->install_apps_finish = gs_plugin_android_install_apps_finish;
  plugin_class->remove_repository_async = gs_plugin_android_remove_repository_async;
  plugin_class->remove_repository_finish = gs_plugin_android_remove_repository_finish;
  plugin_class->uninstall_apps_async = gs_plugin_android_uninstall_apps_async;
  plugin_class->uninstall_apps_finish = gs_plugin_android_uninstall_apps_finish;
  plugin_class->launch_async = gs_plugin_android_launch_async;
  plugin_class->launch_finish = gs_plugin_android_launch_finish;
  plugin_class->update_apps_async = gs_plugin_android_update_apps_async;
  plugin_class->update_apps_finish = gs_plugin_android_update_apps_finish;
}

GType
gs_plugin_query_type (void)
{
  return GS_TYPE_PLUGIN_ANDROID;
}
