/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (c) 2025 xuruofan / GNOME Atomic Community
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE 1

#include <gnome-software.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <json-glib/json-glib.h>
#include <fcntl.h>
#include <unistd.h>

#include "gs-os-release.h"

#define BOOTC_CLI_PATH "/usr/bin/bootc"
#define BOOTC_OS_APP_ID "org.containers.bootc.os"
#define BOOTC_HELPER_PATH "/usr/libexec/gs-bootc-helper"

#define GS_TYPE_PLUGIN_BOOTC (gs_plugin_bootc_get_type ())
G_DECLARE_FINAL_TYPE (GsPluginBootc, gs_plugin_bootc, GS, PLUGIN_BOOTC, GsPlugin)

struct _GsPluginBootc {
	GsPlugin parent;
	GsApp *os_app;
	gboolean update_available;
	gchar *os_name;
	gchar *os_logo;
	gchar *booted_digest;  /* 12-character digest of the booted image */
	gchar *booted_version; /* Version string reported by bootc for the booted image */
	gboolean is_composefs; /* True if using the pure ComposeFS backend */
};

G_DEFINE_TYPE (GsPluginBootc, gs_plugin_bootc, GS_TYPE_PLUGIN)

typedef struct {
	GsApp *app;
	GsPlugin *plugin;
	GsPluginProgressCallback progress_callback;
	gpointer progress_user_data;
} UpgradeTaskData;

static void
upgrade_task_data_free (UpgradeTaskData *data)
{
	g_clear_object (&data->app);
	g_free (data);
}

static void
gs_plugin_bootc_init (GsPluginBootc *self)
{
	self->os_app = NULL;
	self->update_available = FALSE;
	self->os_name = NULL;
	self->os_logo = NULL;
	self->booted_digest = NULL;
	self->booted_version = NULL;
	self->is_composefs = FALSE;
}

static void
gs_plugin_bootc_dispose (GObject *object)
{
	GsPluginBootc *self = GS_PLUGIN_BOOTC (object);
	g_clear_object (&self->os_app);
	g_clear_pointer (&self->os_name, g_free);
	g_clear_pointer (&self->os_logo, g_free);
	g_clear_pointer (&self->booted_digest, g_free);
	g_clear_pointer (&self->booted_version, g_free);  // <-- NOVA
	G_OBJECT_CLASS (gs_plugin_bootc_parent_class)->dispose (object);
}

/* Parse human-readable size strings to bytes */
static guint64
parse_size_c (const gchar *size_str)
{
	if (size_str == NULL || *size_str == '\0') return 0;
	g_autofree gchar *clean_str = g_strdup (size_str);
	gchar *p;
	while ((p = strstr(clean_str, "\xc2\xa0")) != NULL) {
		*p = ' ';
		*(p+1) = ' ';
	}
	g_strstrip (clean_str);

	double val = 0;
	gchar unit[16] = {0};
	if (sscanf(clean_str, "%lf %15s", &val, unit) >= 1) {
		guint64 multiplier = 1;
		if (g_ascii_strcasecmp(unit, "GB") == 0) {
			multiplier = 1024ULL * 1024 * 1024;
		} else if (g_ascii_strcasecmp(unit, "MB") == 0) {
			multiplier = 1024ULL * 1024;
		} else if (g_ascii_strcasecmp(unit, "KB") == 0 || g_ascii_strcasecmp(unit, "kB") == 0) {
			multiplier = 1024ULL;
		}
		return (guint64)(val * multiplier);
	}
	return 0;
}

static void
ensure_os_app_created (GsPluginBootc *self)
{
	if (self->os_app != NULL) return;

	self->os_app = gs_app_new (BOOTC_OS_APP_ID);
	gs_app_set_kind (self->os_app, AS_COMPONENT_KIND_OPERATING_SYSTEM);
	gs_app_set_scope (self->os_app, AS_COMPONENT_SCOPE_SYSTEM);
	gs_app_set_name (self->os_app, GS_APP_QUALITY_NORMAL, self->os_name);
	gs_app_set_summary (self->os_app, GS_APP_QUALITY_NORMAL, "System Update");
	gs_app_set_description (self->os_app, GS_APP_QUALITY_NORMAL, "Container-native atomic host update.");
	
	/* Use the image digest as the version to prevent "unknown" state in UI */
	gs_app_set_version (self->os_app, self->booted_version ? self->booted_version : "unknown");
	
	if (self->os_logo != NULL) {
		g_autoptr(GIcon) ic = g_themed_icon_new (self->os_logo);
		gs_app_add_icon (self->os_app, ic);
	}
	
	gs_app_add_quirk (self->os_app, GS_APP_QUIRK_PROVENANCE | GS_APP_QUIRK_NOT_REVIEWABLE | GS_APP_QUIRK_COMPULSORY);
	gs_app_set_allow_cancel (self->os_app, TRUE);
	gs_app_set_state (self->os_app, GS_APP_STATE_INSTALLED);
	gs_app_set_management_plugin (self->os_app, GS_PLUGIN (self));
}

static gchar *
get_entry_version (JsonObject *entry_obj)
{
	JsonNode *img_node = json_object_get_member (entry_obj, "image");
	if (img_node == NULL || !JSON_NODE_HOLDS_OBJECT (img_node))
		return NULL;

	JsonNode *ver_node = json_object_get_member (json_node_get_object (img_node), "version");
	if (ver_node == NULL || !JSON_NODE_HOLDS_VALUE (ver_node) ||
	    json_node_get_value_type (ver_node) != G_TYPE_STRING)
		return NULL;

	const gchar *ver = json_node_get_string (ver_node);
	return (ver != NULL && *ver != '\0') ? g_strdup (ver) : NULL;
}


static void
parse_bootc_status_json (GsPluginBootc *self, const gchar *json_data)
{
	g_autoptr(JsonParser) parser = json_parser_new_immutable ();
	g_autoptr(GError) error = NULL;

	if (!json_parser_load_from_data (parser, json_data, -1, &error)) return;
	JsonNode *root = json_parser_get_root (parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root)) return;
	JsonObject *root_obj = json_node_get_object (root);

	JsonNode *status_node = json_object_get_member (root_obj, "status");
	if (status_node == NULL || !JSON_NODE_HOLDS_OBJECT (status_node)) return;
	JsonObject *status_obj = json_node_get_object (status_node);

	JsonNode *booted_node = json_object_get_member (status_obj, "booted");
	if (booted_node != NULL && JSON_NODE_HOLDS_OBJECT (booted_node)) {
		JsonObject *booted_obj = json_node_get_object (booted_node);
		
		JsonNode *cfs_node = json_object_get_member (booted_obj, "composefs");
		if (cfs_node != NULL && !json_node_is_null (cfs_node)) {
			self->is_composefs = TRUE;
		}

		JsonNode *img_node = json_object_get_member (booted_obj, "image");
		if (img_node != NULL && JSON_NODE_HOLDS_OBJECT (img_node)) {
			JsonObject *img_outer = json_node_get_object (img_node);
			if (json_object_has_member (img_outer, "imageDigest")) {
				const gchar *digest = json_object_get_string_member (img_outer, "imageDigest");
				if (digest && *digest) {
					const gchar *raw_digest = g_str_has_prefix (digest, "sha256:") ? digest + 7 : digest;
					g_clear_pointer (&self->booted_version, g_free);
					self->booted_version = get_entry_version (booted_obj);
				}
			}
		}
	}

	ensure_os_app_created (self);

	gboolean has_staged = FALSE;
	JsonNode *staged_node = json_object_get_member (status_obj, "staged");
	if (staged_node != NULL && !json_node_is_null (staged_node)) {
		has_staged = TRUE;
	}

	if (has_staged) {
		gs_app_set_state (self->os_app, GS_APP_STATE_PENDING_INSTALL);
		gs_app_add_quirk (self->os_app, GS_APP_QUIRK_NEEDS_REBOOT);
		g_autofree gchar *staged_version = NULL;
		if (JSON_NODE_HOLDS_OBJECT (staged_node))
			staged_version = get_entry_version (json_node_get_object (staged_node));
		gs_app_set_update_version (self->os_app, staged_version ? staged_version : "latest");
	} else if (self->update_available) {
		gs_app_set_state (self->os_app, GS_APP_STATE_UPDATABLE);
		gs_app_remove_quirk (self->os_app, GS_APP_QUIRK_NEEDS_REBOOT);
	} else {
		gs_app_set_state (self->os_app, GS_APP_STATE_INSTALLED);
		gs_app_remove_quirk (self->os_app, GS_APP_QUIRK_NEEDS_REBOOT);
		gs_app_set_update_version (self->os_app, NULL);
	}
}

static void
status_communicate_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	g_autoptr(GTask) task = G_TASK (user_data);
	GsPluginBootc *self = GS_PLUGIN_BOOTC (g_task_get_source_object (task));
	g_autofree gchar *stdout_buf = NULL;
	
	if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source_object), res, &stdout_buf, NULL, NULL) && stdout_buf) {
		parse_bootc_status_json (self, stdout_buf);
	}
	g_task_return_boolean (task, TRUE);
}

/* --- Setup --- */

static void
gs_plugin_bootc_setup_async (GsPlugin            *plugin,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
	GsPluginBootc *self = GS_PLUGIN_BOOTC (plugin);
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	g_autoptr(GError) error = NULL;

	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");

	if (!g_file_test (BOOTC_CLI_PATH, G_FILE_TEST_EXISTS)) {
		gs_plugin_set_enabled (plugin, FALSE);
		g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NOT_SUPPORTED, "bootc CLI not found");
		return;
	}

	g_autoptr(GError) os_err = NULL;
	g_autoptr(GsOsRelease) os_release = gs_os_release_new (&os_err);
	if (os_release != NULL) {
		self->os_name = g_strdup (gs_os_release_get_pretty_name (os_release));
		self->os_logo = g_strdup (gs_os_release_get_logo (os_release));
	} else {
		self->os_name = g_strdup ("Atomic Operating System");
	}

	const gchar *status_argv[] = {
		"/usr/bin/pkexec",
		BOOTC_CLI_PATH, "status",
		"--json",
		NULL
	};

	g_autoptr(GSubprocess) subprocess = g_subprocess_newv (status_argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
	if (!subprocess) {
		g_task_return_boolean (task, TRUE);
		return;
	}
	g_subprocess_communicate_utf8_async (subprocess, NULL, cancellable, status_communicate_cb, g_steal_pointer (&task));
}

static gboolean
gs_plugin_bootc_setup_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

/* --- Refine & List Apps --- */

static void
gs_plugin_bootc_refine_async (GsPlugin *plugin, GsAppList *list, GsPluginRefineFlags job_flags,
                              GsPluginRefineRequireFlags require_flags, GsPluginEventCallback event_callback,
                              void *event_user_data, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	g_autoptr(GError) error = NULL;

	const gchar *status_argv[] = {
		"/usr/bin/pkexec",
		BOOTC_CLI_PATH, "status",
		"--json",
		NULL
	};

	g_autoptr(GSubprocess) subprocess = g_subprocess_newv (status_argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
	if (!subprocess) {
		g_task_return_boolean (task, TRUE);
		return;
	}
	g_subprocess_communicate_utf8_async (subprocess, NULL, cancellable, status_communicate_cb, g_steal_pointer (&task));
}

static gboolean
gs_plugin_bootc_refine_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_bootc_list_apps_async (GsPlugin *plugin, GsAppQuery *query, GsPluginListAppsFlags flags,
                                 GsPluginEventCallback event_callback, void *event_user_data, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
	GsPluginBootc *self = GS_PLUGIN_BOOTC (plugin);
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	g_autoptr(GsAppList) app_list = gs_app_list_new ();

	ensure_os_app_created (self);
	gboolean should_add = TRUE;

	if (query != NULL) {
		GsAppQueryTristate is_for_update = gs_app_query_get_is_for_update (query);
		GsAppQueryTristate is_installed = gs_app_query_get_is_installed (query);
		GsAppState state = gs_app_get_state (self->os_app);

		if (is_for_update == GS_APP_QUERY_TRISTATE_TRUE) {
			if (state != GS_APP_STATE_UPDATABLE &&
			    state != GS_APP_STATE_UPDATABLE_LIVE &&
			    state != GS_APP_STATE_PENDING_INSTALL) {
				should_add = FALSE;
			}
		}
		if (is_installed == GS_APP_QUERY_TRISTATE_TRUE) {
			if (state == GS_APP_STATE_UNKNOWN) {
				should_add = FALSE;
			}
		}
	}

	if (should_add) {
		gs_app_list_add (app_list, self->os_app);
	}
	
	g_task_return_pointer (task, g_steal_pointer (&app_list), g_object_unref);
}

static GsAppList *
gs_plugin_bootc_list_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- Refresh Metadata --- */

static void
check_communicate_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	g_autoptr(GTask) task = G_TASK (user_data);
	GsPluginBootc *self = GS_PLUGIN_BOOTC (g_task_get_source_object (task));
	g_autofree gchar *stdout_buf = NULL;
	g_autofree gchar *new_version = NULL;
	guint64 download_size = 0;

	if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source_object), res, &stdout_buf, NULL, NULL) && stdout_buf) {
		self->update_available = (g_strstr_len (stdout_buf, -1, "Total new layers:") != NULL || 
		                          g_strstr_len (stdout_buf, -1, "Update available") != NULL);
		
		if (self->update_available) {
			gchar *digest_line = g_strstr_len (stdout_buf, -1, "Digest:");
			if (digest_line != NULL) {
				gchar *digest_val = digest_line + 7;
				while (*digest_val == ' ' || *digest_val == '\t') digest_val++;
				if (g_str_has_prefix (digest_val, "sha256:")) {
					digest_val += 7;
				}
				gchar *version_line = g_strstr_len (stdout_buf, -1, "Version:");
				if (version_line != NULL) {
					gchar *version_val = version_line + 8;
					while (*version_val == ' ' || *version_val == '\t') version_val++;
					g_autofree gchar *version_str = g_strdup (version_val);
					gchar *nl = strchr (version_str, '\n');
					if (nl) *nl = '\0';
					g_strstrip (version_str);
					if (*version_str != '\0') {
					g_clear_pointer (&new_version, g_free);
						new_version = g_steal_pointer (&version_str);
					}
				}
			}

			gchar *added_line = g_strstr_len (stdout_buf, -1, "Added layers:");
			if (added_line != NULL) {
				gchar *size_ptr = g_strstr_len (added_line, -1, "Size:");
				if (size_ptr != NULL) {
					size_ptr += 5;
					while (*size_ptr == ' ' || *size_ptr == '\t') size_ptr++;
					g_autofree gchar *size_str = g_strdup (size_ptr);
					gchar *nl = strchr (size_str, '\n');
					if (nl) *nl = '\0';
					g_strstrip (size_str);
					download_size = parse_size_c (size_str);
				}
			}
		}
	} else {
		self->update_available = FALSE;
	}

	if (self->os_app != NULL) {
		if (self->update_available && gs_app_get_state (self->os_app) != GS_APP_STATE_PENDING_INSTALL) {
			gs_app_set_state (self->os_app, GS_APP_STATE_UPDATABLE);
			gs_app_add_quirk (self->os_app, GS_APP_QUIRK_NEEDS_REBOOT);
			gs_app_set_update_version (self->os_app, new_version ? new_version : "latest");
			gs_app_set_size_download (self->os_app, GS_SIZE_TYPE_VALID, download_size);
		} else if (!self->update_available && gs_app_get_state (self->os_app) != GS_APP_STATE_PENDING_INSTALL) {
			gs_app_set_state (self->os_app, GS_APP_STATE_INSTALLED);
			gs_app_set_update_version (self->os_app, NULL);
			gs_app_set_size_download (self->os_app, GS_SIZE_TYPE_UNKNOWN, 0);
		}
		gs_plugin_updates_changed (GS_PLUGIN (self));
	}
	g_task_return_boolean (task, TRUE);
}

static void
gs_plugin_bootc_refresh_metadata_async (GsPlugin *plugin, guint64 cache_age_secs, GsPluginRefreshMetadataFlags flags,
                                        GsPluginEventCallback event_callback, void *event_user_data, GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	g_autoptr(GError) error = NULL;

	const gchar *argv[] = {
		"/usr/bin/pkexec",
		BOOTC_CLI_PATH, "upgrade",
		"--check",
		NULL
	};

	g_autoptr(GSubprocess) subprocess = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &error);
	if (!subprocess) {
		g_task_return_boolean (task, TRUE);
		return;
	}
	g_subprocess_communicate_utf8_async (subprocess, NULL, cancellable, check_communicate_cb, g_steal_pointer (&task));
}

static gboolean
gs_plugin_bootc_refresh_metadata_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

/* --- Update Apps --- */

static void
parse_progress_line (GsApp *app, const gchar *json_line)
{
	g_autoptr(JsonParser) parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, json_line, -1, NULL)) return;

	JsonNode *root = json_parser_get_root (parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root)) return;
	JsonObject *obj = json_node_get_object (root);
	const gchar *type = json_object_get_string_member_with_default (obj, "type", "");
	const gchar *task = json_object_get_string_member_with_default (obj, "task", "");

	if (g_strcmp0 (type, "ProgressBytes") == 0) {
		gint64 bytes = json_object_get_int_member_with_default (obj, "bytes", 0);
		gint64 bytes_total = json_object_get_int_member_with_default (obj, "bytesTotal", 0);
		if (bytes_total > 0) {
			guint percentage = (guint)(((double)bytes / bytes_total) * 100.0);
			gs_app_set_progress (app, MIN (percentage, 100));
		}
		gs_app_set_state (app, GS_APP_STATE_DOWNLOADING);
	} else if (g_strcmp0 (type, "ProgressSteps") == 0) {
		gint64 steps = json_object_get_int_member_with_default (obj, "steps", 0);
		gint64 steps_total = json_object_get_int_member_with_default (obj, "stepsTotal", 0);
		if (steps_total > 0) {
			guint percentage = (guint)(((double)steps / steps_total) * 100.0);
			gs_app_set_progress (app, MIN (percentage, 100));
		}
		if (g_strcmp0 (task, "importing") == 0 || g_strcmp0 (task, "staging") == 0) {
			gs_app_set_state (app, GS_APP_STATE_INSTALLING);
		}
	}
}

static void
upgrade_read_line_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GDataInputStream *stream = G_DATA_INPUT_STREAM (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	UpgradeTaskData *task_data = g_task_get_task_data (task);
	gsize length;
	g_autofree gchar *line = NULL;

	line = g_data_input_stream_read_line_finish_utf8 (stream, res, &length, NULL);
	if (line == NULL) return;

	parse_progress_line (task_data->app, line);

	if (task_data->progress_callback != NULL) {
		guint pct = gs_app_get_progress (task_data->app);
		task_data->progress_callback (task_data->plugin, pct, task_data->progress_user_data);
	}

	g_data_input_stream_read_line_async (stream, G_PRIORITY_DEFAULT, g_task_get_cancellable (task),
	                                     upgrade_read_line_cb, g_object_ref (task));
}

static void
upgrade_cancelled_cb (GCancellable *cancellable, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS (user_data);
	g_subprocess_force_exit (subprocess);
}

static void
upgrade_wait_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	UpgradeTaskData *task_data = g_task_get_task_data (task);
	GsPluginBootc *self = GS_PLUGIN_BOOTC (g_task_get_source_object (task));
	g_autoptr(GError) error = NULL;

	if (!g_subprocess_wait_finish (subprocess, res, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			gs_app_set_state (task_data->app, GS_APP_STATE_UPDATABLE_LIVE);
			gs_app_set_progress (task_data->app, GS_APP_PROGRESS_UNKNOWN);
		}
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	if (g_subprocess_get_successful (subprocess)) {
		gs_app_set_state (task_data->app, GS_APP_STATE_PENDING_INSTALL);
		gs_app_add_quirk (task_data->app, GS_APP_QUIRK_NEEDS_REBOOT);
		gs_app_set_progress (task_data->app, GS_APP_PROGRESS_UNKNOWN);
		self->update_available = FALSE;
		gs_plugin_updates_changed (GS_PLUGIN (self));
		g_task_return_boolean (task, TRUE);
	} else {
		if (g_cancellable_is_cancelled (g_task_get_cancellable (task))) {
			gs_app_set_state (task_data->app, GS_APP_STATE_UPDATABLE);
			gs_app_set_progress (task_data->app, GS_APP_PROGRESS_UNKNOWN);
			g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Update was manually cancelled.");
		} else {
			gs_app_set_state (task_data->app, GS_APP_STATE_UPDATABLE);
			gs_app_set_progress (task_data->app, GS_APP_PROGRESS_UNKNOWN);
			g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED, "bootc upgrade failed.");
		}
	}
}

static void
gs_plugin_bootc_update_apps_async (GsPlugin                           *plugin,
                                   GsAppList                          *apps,
                                   GsPluginUpdateAppsFlags             flags,
                                   GsPluginProgressCallback            progress_callback,
                                   gpointer                            progress_user_data,
                                   GsPluginEventCallback               event_callback,
                                   void                               *event_user_data,
                                   GsPluginAppNeedsUserActionCallback  app_needs_user_action_callback,
                                   gpointer                            app_needs_user_action_data,
                                   GCancellable                       *cancellable,
                                   GAsyncReadyCallback                 callback,
                                   gpointer                            user_data)
{
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	GsPluginBootc *self = GS_PLUGIN_BOOTC (plugin);

	GsApp *app = NULL;
	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *a = gs_app_list_index (apps, i);
		if (gs_app_has_management_plugin (a, plugin)) {
			app = a;
			break;
		}
	}

	if (app == NULL) {
		g_task_return_boolean (task, TRUE);
		return;
	}

	UpgradeTaskData *task_data = g_new0 (UpgradeTaskData, 1);
	task_data->app = g_object_ref (app);
	task_data->plugin = plugin;
	task_data->progress_callback = progress_callback;
	task_data->progress_user_data = progress_user_data;

	g_task_set_task_data (task, task_data, (GDestroyNotify) upgrade_task_data_free);

	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);

	const gchar *argv[] = {
		"/usr/bin/pkexec",
		BOOTC_HELPER_PATH,
		NULL
	};

	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocess) subprocess = g_subprocess_launcher_spawnv (launcher, argv, &error);
	if (subprocess == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	GInputStream *stdout_stream = g_subprocess_get_stdout_pipe (subprocess);
	g_autoptr(GDataInputStream) data_stream = g_data_input_stream_new (stdout_stream);

	gs_app_set_state (app, GS_APP_STATE_DOWNLOADING);
	
	/* 
	 * Fallback to pulsing UI for ComposeFS backends,
	 * as bootc does not support standard progress output there yet.
	 */
	if (self->is_composefs) {
		gs_app_set_progress (app, GS_APP_PROGRESS_UNKNOWN);
		if (progress_callback != NULL) {
			progress_callback (plugin, GS_APP_PROGRESS_UNKNOWN, progress_user_data);
		}
	} else {
		gs_app_set_progress (app, 0);
	}

	if (cancellable) {
		g_cancellable_connect (cancellable, G_CALLBACK (upgrade_cancelled_cb), subprocess, NULL);
	}

	g_data_input_stream_read_line_async (data_stream, G_PRIORITY_DEFAULT, cancellable,
	                                     upgrade_read_line_cb, g_object_ref (task));

	g_subprocess_wait_async (subprocess, cancellable, upgrade_wait_cb, g_steal_pointer (&task));
}

static gboolean
gs_plugin_bootc_update_apps_finish (GsPlugin      *plugin,
                                    GAsyncResult  *result,
                                    GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

/* --- App Install/Download Overrides --- */

static void
gs_plugin_bootc_install_apps_async (GsPlugin *plugin, GsAppList *apps, GsPluginInstallAppsFlags flags,
                                    GsPluginProgressCallback progress_callback, gpointer progress_user_data,
                                    GsPluginEventCallback event_callback, void *event_user_data,
                                    GsPluginAppNeedsUserActionCallback app_needs_user_action_callback, gpointer app_needs_user_action_data,
                                    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	gs_plugin_bootc_update_apps_async (plugin, apps, 0, progress_callback, progress_user_data,
	                                   event_callback, event_user_data, app_needs_user_action_callback,
	                                   app_needs_user_action_data, cancellable, callback, user_data);
}

static gboolean
gs_plugin_bootc_install_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return gs_plugin_bootc_update_apps_finish (plugin, result, error);
}

static void
gs_plugin_bootc_download_upgrade_async (GsPlugin *plugin, GsApp *app, GsPluginDownloadUpgradeFlags flags,
                                        GsPluginEventCallback event_callback, void *event_user_data,
                                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	g_autoptr(GsAppList) apps = gs_app_list_new ();
	gs_app_list_add (apps, app);
	
	gs_plugin_bootc_update_apps_async (plugin, apps, 0, NULL, NULL, event_callback, event_user_data,
	                                   NULL, NULL, cancellable, callback, user_data);
}

static gboolean
gs_plugin_bootc_download_upgrade_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return gs_plugin_bootc_update_apps_finish (plugin, result, error);
}

static void
gs_plugin_bootc_class_init (GsPluginBootcClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPluginClass *plugin_class = GS_PLUGIN_CLASS (klass);

	object_class->dispose = gs_plugin_bootc_dispose;

	plugin_class->setup_async = gs_plugin_bootc_setup_async;
	plugin_class->setup_finish = gs_plugin_bootc_setup_finish;
	plugin_class->refine_async = gs_plugin_bootc_refine_async;
	plugin_class->refine_finish = gs_plugin_bootc_refine_finish;
	plugin_class->list_apps_async = gs_plugin_bootc_list_apps_async;
	plugin_class->list_apps_finish = gs_plugin_bootc_list_apps_finish;
	plugin_class->refresh_metadata_async = gs_plugin_bootc_refresh_metadata_async;
	plugin_class->refresh_metadata_finish = gs_plugin_bootc_refresh_metadata_finish;
	
	plugin_class->update_apps_async = gs_plugin_bootc_update_apps_async;
	plugin_class->update_apps_finish = gs_plugin_bootc_update_apps_finish;
	plugin_class->install_apps_async = gs_plugin_bootc_install_apps_async;
	plugin_class->install_apps_finish = gs_plugin_bootc_install_apps_finish;
	plugin_class->download_upgrade_async = gs_plugin_bootc_download_upgrade_async;
	plugin_class->download_upgrade_finish = gs_plugin_bootc_download_upgrade_finish;
}

GType
gs_plugin_query_type (void)
{
	return GS_TYPE_PLUGIN_BOOTC;
}
