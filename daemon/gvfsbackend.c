/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsbackend.h"
#include "gvfsjobsource.h"
#include <gvfsjobopenforread.h>
#include <gvfsjobopeniconforread.h>
#include <gvfsjobopenforwrite.h>
#include <gvfsjobqueryinfo.h>
#include <gvfsjobqueryfsinfo.h>
#include <gvfsjobsetdisplayname.h>
#include <gvfsjobenumerate.h>
#include <gvfsjobdelete.h>
#include <gvfsjobtrash.h>
#include <gvfsjobunmount.h>
#include <gvfsjobmountmountable.h>
#include <gvfsjobunmountmountable.h>
#include <gvfsjobstartmountable.h>
#include <gvfsjobstopmountable.h>
#include <gvfsjobpollmountable.h>
#include <gvfsjobmakedirectory.h>
#include <gvfsjobmakesymlink.h>
#include <gvfsjobcreatemonitor.h>
#include <gvfsjobcopy.h>
#include <gvfsjobmove.h>
#include <gvfsjobpush.h>
#include <gvfsjobpull.h>
#include <gvfsjobsetattribute.h>
#include <gvfsjobqueryattributes.h>
#include <gvfsdbus.h>

enum {
  PROP_0,
  PROP_OBJECT_PATH,
  PROP_DAEMON
};

struct _GVfsBackendPrivate
{
  GVfsDaemon *daemon;
  char *object_path;

  gboolean is_mounted;
  char *display_name;
  char *stable_name;
  char **x_content_types;
  GIcon *icon;
  GIcon *symbolic_icon;
  char *prefered_filename_encoding;
  gboolean user_visible;
  char *default_location;
  GMountSpec *mount_spec;
  gboolean block_requests;
};


/* TODO: Real P_() */
#define P_(_x) (_x)

static void              g_vfs_backend_job_source_iface_init (GVfsJobSourceIface    *iface);
static void              g_vfs_backend_get_property          (GObject               *object,
							      guint                  prop_id,
							      GValue                *value,
							      GParamSpec            *pspec);
static void              g_vfs_backend_set_property          (GObject               *object,
							      guint                  prop_id,
							      const GValue          *value,
							      GParamSpec            *pspec);
static GObject*          g_vfs_backend_constructor           (GType                  type,
							      guint                  n_construct_properties,
							      GObjectConstructParam *construct_params);


G_DEFINE_TYPE_WITH_CODE (GVfsBackend, g_vfs_backend, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_VFS_TYPE_JOB_SOURCE,
						g_vfs_backend_job_source_iface_init))


static GHashTable *registered_backends = NULL;

void
g_vfs_register_backend (GType backend_type,
			const char *type)
{
  if (registered_backends == NULL)
    registered_backends = g_hash_table_new_full (g_str_hash, g_str_equal,
						 g_free, NULL);

  g_hash_table_insert (registered_backends,
		       g_strdup (type), (void *)backend_type);
}

GType
g_vfs_lookup_backend (const char *type)
{
  gpointer res;
  
  if (registered_backends != NULL)
    {
      res = g_hash_table_lookup (registered_backends, type);
      if (res != NULL)
	return (GType)res;
    }
  
  return G_TYPE_INVALID;
}
  
static void
g_vfs_backend_finalize (GObject *object)
{
  GVfsBackend *backend;

  backend = G_VFS_BACKEND (object);

  g_vfs_daemon_unregister_path (backend->priv->daemon, backend->priv->object_path);
  g_object_unref (backend->priv->daemon);
  g_free (backend->priv->object_path);
  
  g_free (backend->priv->display_name);
  g_free (backend->priv->stable_name);
  g_strfreev (backend->priv->x_content_types);
  g_clear_object (&backend->priv->icon);
  g_clear_object (&backend->priv->symbolic_icon);
  g_free (backend->priv->prefered_filename_encoding);
  g_free (backend->priv->default_location);
  if (backend->priv->mount_spec)
    g_mount_spec_unref (backend->priv->mount_spec);
  
  if (G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize) (object);
}

static void
g_vfs_backend_job_source_iface_init (GVfsJobSourceIface *iface)
{
}

static void
g_vfs_backend_class_init (GVfsBackendClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsBackendPrivate));
  
  gobject_class->constructor = g_vfs_backend_constructor;
  gobject_class->finalize = g_vfs_backend_finalize;
  gobject_class->set_property = g_vfs_backend_set_property;
  gobject_class->get_property = g_vfs_backend_get_property;

  g_object_class_install_property (gobject_class,
				   PROP_OBJECT_PATH,
				   g_param_spec_string ("object-path",
							P_("Backend object path"),
							P_("The dbus object path for the backend object."),
							"",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
				   PROP_DAEMON,
				   g_param_spec_object ("daemon",
							P_("Daemon"),
							P_("The daemon this backend is handled by."),
							G_VFS_TYPE_DAEMON,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
}

static void
g_vfs_backend_init (GVfsBackend *backend)
{
  backend->priv = G_TYPE_INSTANCE_GET_PRIVATE (backend, G_VFS_TYPE_BACKEND, GVfsBackendPrivate);
  backend->priv->icon = NULL;
  backend->priv->symbolic_icon = NULL;
  backend->priv->prefered_filename_encoding = g_strdup ("");
  backend->priv->display_name = g_strdup ("");
  backend->priv->stable_name = g_strdup ("");
  backend->priv->user_visible = TRUE;
  backend->priv->default_location = g_strdup ("");
}

static void
g_vfs_backend_set_property (GObject         *object,
			    guint            prop_id,
			    const GValue    *value,
			    GParamSpec      *pspec)
{
  GVfsBackend *backend = G_VFS_BACKEND (object);
  
  switch (prop_id)
    {
    case PROP_OBJECT_PATH:
      backend->priv->object_path = g_value_dup_string (value);
      break;
    case PROP_DAEMON:
      backend->priv->daemon = G_VFS_DAEMON (g_value_dup_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_backend_get_property (GObject    *object,
			    guint       prop_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
  GVfsBackend *backend = G_VFS_BACKEND (object);
  
  switch (prop_id)
    {
    case PROP_OBJECT_PATH:
      g_value_set_string (value, backend->priv->object_path);
      break;
    case PROP_DAEMON:
      g_value_set_object (value, backend->priv->daemon);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GDBusInterfaceSkeleton *
register_path_cb (GDBusConnection *conn,
                  const char *obj_path,
                  gpointer data)
{
  GError *error;
  GVfsDBusMount *skeleton;
  
  skeleton = gvfs_dbus_mount_skeleton_new ();
  g_signal_connect (skeleton, "handle-enumerate", G_CALLBACK (g_vfs_job_enumerate_new_handle), data);
  g_signal_connect (skeleton, "handle-query-info", G_CALLBACK (g_vfs_job_query_info_new_handle), data);
  g_signal_connect (skeleton, "handle-query-filesystem-info", G_CALLBACK (g_vfs_job_query_fs_info_new_handle), data);
  g_signal_connect (skeleton, "handle-set-display-name", G_CALLBACK (g_vfs_job_set_display_name_new_handle), data);
  g_signal_connect (skeleton, "handle-delete", G_CALLBACK (g_vfs_job_delete_new_handle), data);
  g_signal_connect (skeleton, "handle-trash", G_CALLBACK (g_vfs_job_trash_new_handle), data);
  g_signal_connect (skeleton, "handle-make-directory", G_CALLBACK (g_vfs_job_make_directory_new_handle), data);
  g_signal_connect (skeleton, "handle-make-symbolic-link", G_CALLBACK (g_vfs_job_make_symlink_new_handle), data);
  g_signal_connect (skeleton, "handle-query-settable-attributes", G_CALLBACK (g_vfs_job_query_settable_attributes_new_handle), data);
  g_signal_connect (skeleton, "handle-query-writable-namespaces", G_CALLBACK (g_vfs_job_query_writable_namespaces_new_handle), data);
  g_signal_connect (skeleton, "handle-set-attribute", G_CALLBACK (g_vfs_job_set_attribute_new_handle), data);
  g_signal_connect (skeleton, "handle-poll-mountable", G_CALLBACK (g_vfs_job_poll_mountable_new_handle), data);
  g_signal_connect (skeleton, "handle-start-mountable", G_CALLBACK (g_vfs_job_start_mountable_new_handle), data);
  g_signal_connect (skeleton, "handle-stop-mountable", G_CALLBACK (g_vfs_job_stop_mountable_new_handle), data);
  g_signal_connect (skeleton, "handle-unmount-mountable", G_CALLBACK (g_vfs_job_unmount_mountable_new_handle), data);
  g_signal_connect (skeleton, "handle-eject-mountable", G_CALLBACK (g_vfs_job_eject_mountable_new_handle), data);
  g_signal_connect (skeleton, "handle-mount-mountable", G_CALLBACK (g_vfs_job_mount_mountable_new_handle), data);
  g_signal_connect (skeleton, "handle-unmount", G_CALLBACK (g_vfs_job_unmount_new_handle), data);
  g_signal_connect (skeleton, "handle-open-for-read", G_CALLBACK (g_vfs_job_open_for_read_new_handle), data);
  g_signal_connect (skeleton, "handle-open-for-write", G_CALLBACK (g_vfs_job_open_for_write_new_handle), data);
  g_signal_connect (skeleton, "handle_copy", G_CALLBACK (g_vfs_job_copy_new_handle), data);
  g_signal_connect (skeleton, "handle-move", G_CALLBACK (g_vfs_job_move_new_handle), data);
  g_signal_connect (skeleton, "handle-push", G_CALLBACK (g_vfs_job_push_new_handle), data);
  g_signal_connect (skeleton, "handle-pull", G_CALLBACK (g_vfs_job_pull_new_handle), data);
  g_signal_connect (skeleton, "handle-create-directory-monitor", G_CALLBACK (g_vfs_job_create_directory_monitor_new_handle), data);
  g_signal_connect (skeleton, "handle-create-file-monitor", G_CALLBACK (g_vfs_job_create_file_monitor_new_handle), data);
  g_signal_connect (skeleton, "handle-open-icon-for-read", G_CALLBACK (g_vfs_job_open_icon_for_read_new_handle), data);
  
  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (skeleton),
                                         conn,
                                         obj_path,
                                         &error))
    {
      g_warning ("Error registering path: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  return G_DBUS_INTERFACE_SKELETON (skeleton);
}
  
static GObject*
g_vfs_backend_constructor (GType                  type,
			   guint                  n_construct_properties,
			   GObjectConstructParam *construct_params)
{
  GObject *object;
  GVfsBackend *backend;

  object = (* G_OBJECT_CLASS (g_vfs_backend_parent_class)->constructor) (type,
									 n_construct_properties,
									 construct_params);
  backend = G_VFS_BACKEND (object);
  
  g_vfs_daemon_register_path (backend->priv->daemon,
			      backend->priv->object_path, 
			      register_path_cb,
			      backend);
  
  return object;
}

GVfsDaemon *
g_vfs_backend_get_daemon (GVfsBackend *backend)
{
  return backend->priv->daemon;
}

gboolean
g_vfs_backend_is_mounted (GVfsBackend *backend)
{
  return backend->priv->is_mounted;
}

void
g_vfs_backend_set_display_name (GVfsBackend *backend,
				const char *display_name)
{
  g_free (backend->priv->display_name);
  backend->priv->display_name = g_strdup (display_name);
}

/**
 * g_vfs_backend_set_stable_name:
 * @backend: backend
 * @stable_name: the stable name
 *
 * For filesystems that can change the name during the lifetime
 * of the filesystem this can be uses to set a separate stable
 * name. This is used for instance as the directory representing
 * the mounted file system in the standard UNIX file system
 * namespace.
 *
 * If this function isn't called, the value passed to
 * g_vfs_backend_set_display_name() will be used instead.
 **/
void
g_vfs_backend_set_stable_name (GVfsBackend        *backend,
			       const char         *stable_name)
{
  g_free (backend->priv->stable_name);
  backend->priv->stable_name = g_strdup (stable_name);
}

/**
 * g_vfs_backend_set_x_content_types:
 * @backend: backend
 * @x_content_types: the x-content types
 *
 * For backends where the x-content type is known ahead of time and
 * won't change (such as a CDDA audio disc backend), this function
 * should be called when the backend is constructed with the given
 * types.
 *
 * See the <ulink url="http://www.freedesktop.org/wiki/Specifications/shared-mime-info-spec">shared-mime-info</ulink>
 * specification for more on x-content types.
 **/
void
g_vfs_backend_set_x_content_types (GVfsBackend        *backend,
                                   char              **x_content_types)
{
  g_strfreev (backend->priv->x_content_types);
  backend->priv->x_content_types = g_strdupv (x_content_types);
}

void
g_vfs_backend_set_icon_name (GVfsBackend *backend,
			     const char *icon_name)
{
  g_clear_object (&backend->priv->icon);
  backend->priv->icon = g_themed_icon_new_with_default_fallbacks (icon_name);
}

void
g_vfs_backend_set_icon (GVfsBackend *backend,
                        GIcon       *icon)
{
  g_clear_object (&backend->priv->icon);
  backend->priv->icon = g_object_ref (icon);
}

void
g_vfs_backend_set_symbolic_icon_name (GVfsBackend *backend,
                                      const char *icon_name)
{
  g_clear_object (&backend->priv->symbolic_icon);
  backend->priv->symbolic_icon = g_themed_icon_new_with_default_fallbacks (icon_name);
}

void
g_vfs_backend_set_symbolic_icon (GVfsBackend *backend,
                                 GIcon       *icon)
{
  g_clear_object (&backend->priv->symbolic_icon);
  backend->priv->symbolic_icon = g_object_ref (icon);
}

void
g_vfs_backend_set_prefered_filename_encoding (GVfsBackend  *backend,
					      const char *prefered_filename_encoding)
{
  g_free (backend->priv->prefered_filename_encoding);
  backend->priv->prefered_filename_encoding = g_strdup (prefered_filename_encoding);
}

void
g_vfs_backend_set_user_visible (GVfsBackend  *backend,
				gboolean user_visible)
{
  backend->priv->user_visible = user_visible;
}

/**
 * g_vfs_backend_set_default_location:
 * @backend: backend
 * @location: the default location
 *
 * With this function the backend can set a "default location", which is a path
 * that reflects the main entry point for the user (e.g.  * the home directory,
 * or the root of the volume).
 *
 * NB: Does not include the mount prefix, you need to prepend that if there is
 * one.
 **/
void
g_vfs_backend_set_default_location (GVfsBackend  *backend,
                                    const char   *location)
{
  g_free (backend->priv->default_location);
  backend->priv->default_location = g_strdup (location);
}

void
g_vfs_backend_set_mount_spec (GVfsBackend *backend,
			      GMountSpec *mount_spec)
{
  if (backend->priv->mount_spec)
    g_mount_spec_unref (backend->priv->mount_spec);
  backend->priv->mount_spec = g_mount_spec_ref (mount_spec);
}

const char *
g_vfs_backend_get_backend_type (GVfsBackend *backend)
{
  if (backend->priv->mount_spec)
    return g_mount_spec_get_type (backend->priv->mount_spec);
  return NULL;
}

const char *
g_vfs_backend_get_display_name (GVfsBackend *backend)
{
  return backend->priv->display_name;
}

const char *
g_vfs_backend_get_stable_name (GVfsBackend *backend)
{
  return backend->priv->stable_name;
}

char **
g_vfs_backend_get_x_content_types (GVfsBackend *backend)
{
  return backend->priv->x_content_types;
}

GIcon *
g_vfs_backend_get_icon (GVfsBackend *backend)
{
  return backend->priv->icon;
}

GIcon *
g_vfs_backend_get_symbolic_icon (GVfsBackend *backend)
{
  return backend->priv->symbolic_icon;
}

const char *
g_vfs_backend_get_default_location (GVfsBackend  *backend)
{
  return backend->priv->default_location;
}

GMountSpec *
g_vfs_backend_get_mount_spec (GVfsBackend *backend)
{
  return backend->priv->mount_spec;
}

static void
get_thumbnail_attributes (const char *uri,
                          GFileInfo  *info)
{
  GChecksum *checksum;
  char *filename;
  char *basename;

  checksum = g_checksum_new (G_CHECKSUM_MD5);
  g_checksum_update (checksum, (const guchar *) uri, strlen (uri));

  basename = g_strconcat (g_checksum_get_string (checksum), ".png", NULL);
  g_checksum_free (checksum);

  filename = g_build_filename (g_get_user_cache_dir (),
                               "thumbnails", "normal", basename,
                               NULL);

  if (g_file_test (filename, G_FILE_TEST_IS_REGULAR))
    g_file_info_set_attribute_byte_string (info, G_FILE_ATTRIBUTE_THUMBNAIL_PATH, filename);
  else
    {
      g_free (filename);
      filename = g_build_filename (g_get_user_cache_dir (),
                                   "thumbnails", "fail",
                                   "gnome-thumbnail-factory",
                                   basename,
                                   NULL);
      
      if (g_file_test (filename, G_FILE_TEST_IS_REGULAR))
	g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_THUMBNAILING_FAILED, TRUE);
    }
  g_free (basename);
  g_free (filename);
}

void
g_vfs_backend_add_auto_info (GVfsBackend *backend,
			     GFileAttributeMatcher *matcher,
			     GFileInfo *info,
			     const char *uri)
{
  GMountSpec *spec;
  char *id;
  
  if (g_file_attribute_matcher_matches (matcher,
					G_FILE_ATTRIBUTE_ID_FILESYSTEM))
    {
      spec = g_vfs_backend_get_mount_spec (backend);
      if (spec)
	{
	  id = g_mount_spec_to_string (spec);
	  g_file_info_set_attribute_string (info,
					    G_FILE_ATTRIBUTE_ID_FILESYSTEM,
					    id);
	  g_free (id);
	}
    }

  if (uri != NULL &&
      g_file_attribute_matcher_matches (matcher,
					G_FILE_ATTRIBUTE_THUMBNAIL_PATH))
    get_thumbnail_attributes (uri, info);
  
}

void
g_vfs_backend_set_block_requests (GVfsBackend *backend)
{
  backend->priv->block_requests = TRUE;
}

gboolean
g_vfs_backend_get_block_requests (GVfsBackend *backend)
{
  return backend->priv->block_requests;
}

gboolean
g_vfs_backend_invocation_first_handler (GVfsDBusMount *object,
                                        GDBusMethodInvocation *invocation,
                                        GVfsBackend *backend)
{
  g_debug ("backend_dbus_handler %s:%s\n",
           g_dbus_method_invocation_get_interface_name (invocation),
           g_dbus_method_invocation_get_method_name (invocation));

  if (backend->priv->block_requests)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_MOUNTED,
                                             "%s", "Backend currently unmounting");
      return TRUE;
    }

  return FALSE;
}


typedef struct {
  GVfsBackend *backend;
  GAsyncReadyCallback callback;
  gpointer callback_data;
} AsyncProxyCreate;

static void
async_proxy_create_free (AsyncProxyCreate *data)
{
  g_clear_object (&data->backend);
  g_free (data);
}

static void
create_mount_tracker_proxy (GVfsBackend *backend,
                            GAsyncReadyCallback op_callback,
                            gpointer op_callback_data,
                            GAsyncReadyCallback callback)
{
  AsyncProxyCreate *data;

  data = g_new0 (AsyncProxyCreate, 1);
  data->callback = op_callback;
  data->callback_data = op_callback_data;
  data->backend = g_object_ref (backend);

  gvfs_dbus_mount_tracker_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                             G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                             G_VFS_DBUS_DAEMON_NAME,
                                             G_VFS_DBUS_MOUNTTRACKER_PATH,
                                             NULL,
                                             callback,
                                             data);
}

static void
register_mount_got_proxy_cb (GObject *source_object,
                             GAsyncResult *res,
                             gpointer user_data)
{
  AsyncProxyCreate *data = user_data;
  GVfsDBusMountTracker *proxy;
  GError *error = NULL;
  GSimpleAsyncResult *result;
  GVfsBackend *backend;
  const char *stable_name;
  char *x_content_types_string;
  char *icon_str;
  char *symbolic_icon_str;

  proxy = gvfs_dbus_mount_tracker_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      g_dbus_error_strip_remote_error (error);
      result = g_simple_async_result_new_take_error (source_object,
                                                     data->callback, data->callback_data,
                                                     error);
      g_simple_async_result_complete_in_idle (result);
      g_object_unref (result);
      async_proxy_create_free (data);
      return;
    }

  backend = data->backend;
  backend->priv->is_mounted = TRUE;

  if (backend->priv->x_content_types != NULL && g_strv_length (backend->priv->x_content_types) > 0)
    x_content_types_string = g_strjoinv (" ", backend->priv->x_content_types);
  else
    x_content_types_string = g_strdup ("");

  if (backend->priv->icon != NULL)
    icon_str = g_icon_to_string (backend->priv->icon);
  else
    icon_str = g_strdup ("");

  if (backend->priv->symbolic_icon != NULL)
    symbolic_icon_str = g_icon_to_string (backend->priv->symbolic_icon);
  else
    symbolic_icon_str = g_strdup ("");

  if (backend->priv->stable_name != NULL &&
      *backend->priv->stable_name != 0)
   stable_name = backend->priv->stable_name;
  else
   stable_name = backend->priv->display_name;

  gvfs_dbus_mount_tracker_call_register_mount (proxy,
                                               backend->priv->object_path,
                                               backend->priv->display_name,
                                               stable_name,
                                               x_content_types_string,
                                               icon_str,
                                               symbolic_icon_str,
                                               backend->priv->prefered_filename_encoding,
                                               backend->priv->user_visible,
                                               g_mount_spec_to_dbus (backend->priv->mount_spec),
                                               backend->priv->default_location ? backend->priv->default_location : "",
                                               NULL,
                                               data->callback, data->callback_data);

  g_free (x_content_types_string);
  g_free (icon_str);
  g_free (symbolic_icon_str);
  g_object_unref (proxy);
  async_proxy_create_free (data);
}

void
g_vfs_backend_register_mount (GVfsBackend *backend,
                              GAsyncReadyCallback callback,
			      gpointer user_data)
{
  create_mount_tracker_proxy (backend, callback, user_data, register_mount_got_proxy_cb);
}

static void
unregister_mount_got_proxy_cb (GObject *source_object,
                               GAsyncResult *res,
                               gpointer user_data)
{
  AsyncProxyCreate *data = user_data;
  GVfsDBusMountTracker *proxy;
  GError *error = NULL;
  GSimpleAsyncResult *result;
  GVfsBackend *backend;

  proxy = gvfs_dbus_mount_tracker_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      g_dbus_error_strip_remote_error (error);
      result = g_simple_async_result_new_take_error (source_object,
                                                     data->callback, data->callback_data,
                                                     error);
      g_simple_async_result_complete_in_idle (result);
      g_object_unref (result);
      async_proxy_create_free (data);
      return;
    }

  backend = data->backend;
  
  gvfs_dbus_mount_tracker_call_unregister_mount (proxy,
                                                 backend->priv->object_path,
                                                 NULL,
                                                 data->callback, data->callback_data);

  g_object_unref (proxy);
  async_proxy_create_free (data);
}

void
g_vfs_backend_unregister_mount (GVfsBackend *backend,
                                GAsyncReadyCallback callback,
				gpointer user_data)
{
  create_mount_tracker_proxy (backend, callback, user_data, unregister_mount_got_proxy_cb);
}

/* ------------------------------------------------------------------------------------------------- */

typedef struct
{
  GVfsBackend *backend;
  GMountSource *mount_source;

  gboolean ret;
  gboolean aborted;
  gint choice;

  const gchar *message;
  const gchar *choices[3];

  gboolean no_more_processes;

  GAsyncReadyCallback callback;
  gpointer            user_data;

  guint timeout_id;
} UnmountWithOpData;

static void
complete_unmount_with_op (UnmountWithOpData *data)
{
  gboolean ret;
  GSimpleAsyncResult *simple;

  g_source_remove (data->timeout_id);

  ret = TRUE;

  if (data->no_more_processes)
    {
      /* do nothing, e.g. return TRUE to signal we should unmount */
    }
  else
    {
      if (data->aborted || data->choice == 1)
        {
          ret = FALSE;
        }
    }

  simple = g_simple_async_result_new (G_OBJECT (data->backend),
                                      data->callback,
                                      data->user_data,
                                      NULL);
  g_simple_async_result_set_op_res_gboolean (simple, ret);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
on_show_processes_reply (GMountSource  *mount_source,
                         GAsyncResult  *res,
                         gpointer       user_data)
{
  UnmountWithOpData *data = user_data;

  /* Do nothing if we've handled this already */
  if (data->no_more_processes)
    return;

  data->ret = g_mount_source_show_processes_finish (mount_source,
                                                    res,
                                                    &data->aborted,
                                                    &data->choice);

  complete_unmount_with_op (data);
}

static gboolean
on_update_processes_timeout (gpointer user_data)
{
  UnmountWithOpData *data = user_data;
  GArray *processes;

  processes = g_vfs_daemon_get_blocking_processes (g_vfs_backend_get_daemon (data->backend));
  if (processes->len == 0)
    {
      /* no more processes, abort mount op */
      g_mount_source_abort (data->mount_source);
      data->no_more_processes = TRUE;
      complete_unmount_with_op (data);
    }
  else
    {
      /* ignore reply */
      g_mount_source_show_processes_async (data->mount_source,
                                           data->message,
                                           processes,
                                           data->choices,
                                           g_strv_length ((gchar **) data->choices),
                                           NULL,
                                           NULL);
    }

  g_array_unref (processes);

  /* keep timeout around */
  return TRUE;
}

static void
unmount_with_op_data_free (UnmountWithOpData *data)
{
  g_free (data);
}


/**
 * g_vfs_backend_unmount_with_operation_finish:
 * @backend: A #GVfsBackend.
 * @res: A #GAsyncResult obtained from the @callback function passed
 *     to g_vfs_backend_unmount_with_operation().
 *
 * Gets the result of the operation started by
 * gvfs_backend_unmount_with_operation_sync().
 *
 * Returns: %TRUE if the backend should be unmounted (either no blocking
 *     processes or the user decided to unmount anyway), %FALSE if
 *     no action should be taken.
 */
gboolean
g_vfs_backend_unmount_with_operation_finish (GVfsBackend *backend,
                                             GAsyncResult *res)
{
  gboolean ret;
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);

  if (g_simple_async_result_propagate_error (simple, NULL))
    {
      ret = FALSE;
    }
  else
    {
      ret = g_simple_async_result_get_op_res_gboolean (simple);
    }

  return ret;
}

/**
 * gvfs_backend_unmount_with_operation:
 * @backend: A #GVfsBackend.
 * @callback: A #GAsyncReadyCallback.
 * @user_data: User data to pass to @callback.
 *
 * Utility function to checks if there are pending operations on
 * @backend preventing unmount. If not, then @callback is invoked
 * immediately.
 *
 * Otherwise, a dialog will be shown (using @mount_source) to interact
 * with the user about blocking processes (e.g. using the
 * #GMountOperation::show-processes signal). The list of blocking
 * processes is continuously updated.
 *
 * Once the user has decided (or if it's not possible to interact with
 * the user), @callback will be invoked. You can then call
 * g_vfs_backend_unmount_with_operation_finish() to get the result
 * of the operation.
 */
void
g_vfs_backend_unmount_with_operation (GVfsBackend        *backend,
                                      GMountSource       *mount_source,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data)
{
  GArray *processes;
  UnmountWithOpData *data;

  g_return_if_fail (G_VFS_IS_BACKEND (backend));
  g_return_if_fail (G_IS_MOUNT_SOURCE (mount_source));
  g_return_if_fail (callback != NULL);

  processes = g_vfs_daemon_get_blocking_processes (g_vfs_backend_get_daemon (backend));
  /* if no processes are blocking, complete immediately */
  if (processes->len == 0)
    {
      GSimpleAsyncResult *simple;
      simple = g_simple_async_result_new (G_OBJECT (backend),
                                          callback,
                                          user_data,
                                          NULL);
      g_simple_async_result_set_op_res_gboolean (simple, TRUE);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      goto out;
    }

  data = g_new0 (UnmountWithOpData, 1);
  data->backend = backend;
  data->mount_source = mount_source;
  data->callback = callback;
  data->user_data = user_data;

  data->choices[0] = _("Unmount Anyway");
  data->choices[1] = _("Cancel");
  data->choices[2] = NULL;
  data->message = _("Volume is busy\n"
                    "One or more applications are keeping the volume busy.");

  /* free data when the mount source goes away */
  g_object_set_data_full (G_OBJECT (mount_source),
                          "unmount-op-data",
                          data,
                          (GDestroyNotify) unmount_with_op_data_free);

  /* show processes */
  g_mount_source_show_processes_async (mount_source,
                                       data->message,
                                       processes,
                                       data->choices,
                                       g_strv_length ((gchar **) data->choices),
                                       (GAsyncReadyCallback) on_show_processes_reply,
                                       data);

  /* update these processes every two secs */
  data->timeout_id = g_timeout_add_seconds (2,
                                            on_update_processes_timeout,
                                            data);

 out:
  g_array_unref (processes);

}

gboolean
g_vfs_backend_has_blocking_processes (GVfsBackend  *backend)
{
  gboolean ret;
  GArray *processes;

  ret = FALSE;
  processes = g_vfs_daemon_get_blocking_processes (g_vfs_backend_get_daemon (backend));
  if (processes->len > 0)
    ret = TRUE;

  g_array_unref (processes);

  return ret;
}

