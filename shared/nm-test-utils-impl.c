// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2010 - 2015 Red Hat, Inc.
 */

#include "nm-default.h"

#include <sys/wait.h>

#include "NetworkManager.h"
#include "nm-std-aux/nm-dbus-compat.h"

#include "nm-test-libnm-utils.h"

#define NMTSTC_NM_SERVICE NM_BUILD_SRCDIR"/tools/test-networkmanager-service.py"

/*****************************************************************************/

static gboolean
name_exists (GDBusConnection *c, const char *name)
{
	GVariant *reply;
	gboolean exists = FALSE;

	reply = g_dbus_connection_call_sync (c,
	                                     DBUS_SERVICE_DBUS,
	                                     DBUS_PATH_DBUS,
	                                     DBUS_INTERFACE_DBUS,
	                                     "GetNameOwner",
	                                     g_variant_new ("(s)", name),
	                                     NULL,
	                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                                     -1,
	                                     NULL,
	                                     NULL);
	if (reply != NULL) {
		exists = TRUE;
		g_variant_unref (reply);
	}

	return exists;
}

typedef struct {
	GMainLoop *mainloop;
	GDBusConnection *bus;
	int exit_code;
	bool exited:1;
	bool name_found:1;
} ServiceInitWaitData;

static gboolean
_service_init_wait_probe_name (gpointer user_data)
{
	ServiceInitWaitData *data = user_data;

	if (!name_exists (data->bus, "org.freedesktop.NetworkManager"))
		return G_SOURCE_CONTINUE;

	data->name_found = TRUE;
	g_main_loop_quit (data->mainloop);
	return G_SOURCE_REMOVE;
}

static void
_service_init_wait_child_wait (GPid pid,
                               int status,
                               gpointer user_data)
{
	ServiceInitWaitData *data = user_data;

	data->exited = TRUE;
	data->exit_code = status;
	g_main_loop_quit (data->mainloop);
}

NMTstcServiceInfo *
nmtstc_service_available (NMTstcServiceInfo *info)
{
	gs_free char *m = NULL;

	if (info)
		return info;

	/* This happens, when test-networkmanager-service.py exits with 77 status
	 * code. */
	m = g_strdup_printf ("missing dependency for running NetworkManager stub service %s", NMTSTC_NM_SERVICE);
	g_test_skip (m);
	return NULL;
}

NMTstcServiceInfo *
nmtstc_service_init (void)
{
	NMTstcServiceInfo *info;
	const char *args[] = { TEST_NM_PYTHON, NMTSTC_NM_SERVICE, NULL };
	GError *error = NULL;

	info = g_malloc0 (sizeof (*info));

	info->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL,  &error);
	g_assert_no_error (error);

	/* Spawn the test service. info->keepalive_fd will be a pipe to the service's
	 * stdin; if it closes, the service will exit immediately. We use this to
	 * make sure the service exits if the test program crashes.
	 */
	g_spawn_async_with_pipes (NULL, (char **) args, NULL,
	                            G_SPAWN_SEARCH_PATH
	                          | G_SPAWN_DO_NOT_REAP_CHILD,
	                          NULL, NULL,
	                          &info->pid, &info->keepalive_fd, NULL, NULL, &error);
	g_assert_no_error (error);

	{
		nm_auto_unref_gsource GSource *timeout_source = NULL;
		nm_auto_unref_gsource GSource *child_source = NULL;
		GMainContext *context = g_main_context_new ();
		ServiceInitWaitData data = {
			.bus = info->bus,
			.mainloop = g_main_loop_new (context, FALSE),
		};
		gboolean had_timeout;

		timeout_source = g_timeout_source_new (50);
		g_source_set_callback (timeout_source, _service_init_wait_probe_name, &data, NULL);
		g_source_attach (timeout_source, context);

		child_source = g_child_watch_source_new (info->pid);
		g_source_set_callback (child_source, (GSourceFunc)(void (*) (void)) _service_init_wait_child_wait, &data, NULL);
		g_source_attach (child_source, context);

		had_timeout = !nmtst_main_loop_run (data.mainloop, 30000);

		g_source_destroy (timeout_source);
		g_source_destroy (child_source);
		g_main_loop_unref (data.mainloop);
		g_main_context_unref (context);

		if (had_timeout)
			g_error ("test service %s did not start in time", NMTSTC_NM_SERVICE);
		if (!data.name_found) {
			g_assert (data.exited);
			info->pid = NM_PID_T_INVAL;
			nmtstc_service_cleanup (info);

			if (   WIFEXITED (data.exit_code)
			    && WEXITSTATUS (data.exit_code) == 77) {
				/* If the stub service exited with status 77 it means that it decided
				 * that it cannot conduct the tests and the test should be (gracefully)
				 * skip. The likely reason for that, is that libnm is not available
				 * via pygobject. */
				return NULL;
			}
			g_error ("test service %s exited with error code %d", NMTSTC_NM_SERVICE, data.exit_code);
		}
	}

	/* Grab a proxy to our fake NM service to trigger tests */
	info->proxy = g_dbus_proxy_new_sync (info->bus,
	                                     G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
	                                     G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS |
	                                     G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
	                                     NULL,
	                                     NM_DBUS_SERVICE,
	                                     NM_DBUS_PATH,
	                                     "org.freedesktop.NetworkManager.LibnmGlibTest",
	                                     NULL, &error);
	g_assert_no_error (error);

	return info;
}

void
nmtstc_service_cleanup (NMTstcServiceInfo *info)
{
	int ret;
	gint64 t;
	int status;

	if (!info)
		return;

	nm_close (nm_steal_fd (&info->keepalive_fd));

	g_clear_object (&info->proxy);

	if (info->pid != NM_PID_T_INVAL) {
		kill (info->pid, SIGTERM);

		t = g_get_monotonic_time ();
again_wait:
		ret = waitpid (info->pid, &status, WNOHANG);
		if (ret == 0) {
			if (t + 2000000 < g_get_monotonic_time ()) {
				kill (info->pid, SIGKILL);
				g_error ("child process %lld did not exit within timeout", (long long) info->pid);
			}
			g_usleep (G_USEC_PER_SEC / 50);
			goto again_wait;
		}
		if (ret == -1 && errno == EINTR)
			goto again_wait;

		g_assert (ret == info->pid);
	}

	g_assert (!name_exists (info->bus, "org.freedesktop.NetworkManager"));

	g_clear_object (&info->bus);

	memset (info, 0, sizeof (*info));
	g_free (info);
}

typedef struct {
	GMainLoop *loop;
	const char *ifname;
	char *path;
	NMDevice *device;
} AddDeviceInfo;

static void
device_added_cb (NMClient *client,
                 NMDevice *device,
                 gpointer user_data)
{
	AddDeviceInfo *info = user_data;

	g_assert (device);
	g_assert_cmpstr (nm_object_get_path (NM_OBJECT (device)), ==, info->path);
	g_assert_cmpstr (nm_device_get_iface (device), ==, info->ifname);

	info->device = device;
	g_main_loop_quit (info->loop);
}

static gboolean
timeout (gpointer user_data)
{
	g_assert_not_reached ();
	return G_SOURCE_REMOVE;
}

static GVariant *
call_add_wired_device (GDBusProxy *proxy, const char *ifname, const char *hwaddr,
                       const char **subchannels, GError **error)
{
	const char *empty[] = { NULL };

	if (!hwaddr)
		hwaddr = "/";
	if (!subchannels)
		subchannels = empty;

	return g_dbus_proxy_call_sync (proxy,
	                               "AddWiredDevice",
	                               g_variant_new ("(ss^as)", ifname, hwaddr, subchannels),
	                               G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                               3000,
	                               NULL,
	                               error);
}

static GVariant *
call_add_device (GDBusProxy *proxy, const char *method, const char *ifname, GError **error)
{
	return g_dbus_proxy_call_sync (proxy,
	                               method,
	                               g_variant_new ("(s)", ifname),
	                               G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                               3000,
	                               NULL,
	                               error);
}

static NMDevice *
add_device_common (NMTstcServiceInfo *sinfo, NMClient *client,
                   const char *method, const char *ifname,
                   const char *hwaddr, const char **subchannels)
{
	AddDeviceInfo info;
	GError *error = NULL;
	GVariant *ret;
	guint timeout_id;

	if (g_strcmp0 (method, "AddWiredDevice") == 0)
		ret = call_add_wired_device (sinfo->proxy, ifname, hwaddr, subchannels, &error);
	else
		ret = call_add_device (sinfo->proxy, method, ifname, &error);

	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpstr (g_variant_get_type_string (ret), ==, "(o)");
	g_variant_get (ret, "(o)", &info.path);
	g_variant_unref (ret);

	/* Wait for libnm to find the device */
	info.ifname = ifname;
	info.loop = g_main_loop_new (NULL, FALSE);
	g_signal_connect (client, "device-added",
	                  G_CALLBACK (device_added_cb), &info);
	timeout_id = g_timeout_add_seconds (5, timeout, NULL);
	g_main_loop_run (info.loop);

	g_source_remove (timeout_id);
	g_signal_handlers_disconnect_by_func (client, device_added_cb, &info);
	g_free (info.path);
	g_main_loop_unref (info.loop);

	return info.device;
}

NMDevice *
nmtstc_service_add_device (NMTstcServiceInfo *sinfo, NMClient *client,
                           const char *method, const char *ifname)
{
	return add_device_common (sinfo, client, method, ifname, NULL, NULL);
}

NMDevice *
nmtstc_service_add_wired_device (NMTstcServiceInfo *sinfo, NMClient *client,
                                 const char *ifname, const char *hwaddr,
                                 const char **subchannels)
{
	return add_device_common (sinfo, client, "AddWiredDevice", ifname, hwaddr, subchannels);
}

void
nmtstc_service_add_connection (NMTstcServiceInfo *sinfo,
                               NMConnection *connection,
                               gboolean verify_connection,
                               char **out_path)
{
	nmtstc_service_add_connection_variant (sinfo,
	                                       nm_connection_to_dbus (connection, NM_CONNECTION_SERIALIZE_ALL),
	                                       verify_connection,
	                                       out_path);
}

void
nmtstc_service_add_connection_variant (NMTstcServiceInfo *sinfo,
                                       GVariant *connection,
                                       gboolean verify_connection,
                                       char **out_path)
{
	GVariant *result;
	GError *error = NULL;

	g_assert (sinfo);
	g_assert (G_IS_DBUS_PROXY (sinfo->proxy));
	g_assert (g_variant_is_of_type (connection, G_VARIANT_TYPE ("a{sa{sv}}")));

	result = g_dbus_proxy_call_sync (sinfo->proxy,
	                                 "AddConnection",
	                                 g_variant_new ("(vb)", connection, verify_connection),
	                                 G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                                 3000,
	                                 NULL,
	                                 &error);
	g_assert_no_error (error);
	g_assert (g_variant_is_of_type (result, G_VARIANT_TYPE ("(o)")));
	if (out_path)
		g_variant_get (result, "(o)", out_path);
	g_variant_unref (result);
}

void
nmtstc_service_update_connection (NMTstcServiceInfo *sinfo,
                                  const char *path,
                                  NMConnection *connection,
                                  gboolean verify_connection)
{
	if (!path)
		path = nm_connection_get_path (connection);
	g_assert (path);

	nmtstc_service_update_connection_variant (sinfo,
	                                          path,
	                                          nm_connection_to_dbus (connection, NM_CONNECTION_SERIALIZE_ALL),
	                                          verify_connection);
}

void
nmtstc_service_update_connection_variant (NMTstcServiceInfo *sinfo,
                                          const char *path,
                                          GVariant *connection,
                                          gboolean verify_connection)
{
	GVariant *result;
	GError *error = NULL;

	g_assert (sinfo);
	g_assert (G_IS_DBUS_PROXY (sinfo->proxy));
	g_assert (g_variant_is_of_type (connection, G_VARIANT_TYPE ("a{sa{sv}}")));
	g_assert (path && path[0] == '/');

	result = g_dbus_proxy_call_sync (sinfo->proxy,
	                                 "UpdateConnection",
	                                 g_variant_new ("(ovb)", path, connection, verify_connection),
	                                 G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                                 3000,
	                                 NULL,
	                                 &error);
	g_assert_no_error (error);
	g_assert (g_variant_is_of_type (result, G_VARIANT_TYPE ("()")));
	g_variant_unref (result);
}

/*****************************************************************************/

typedef struct {
	GMainLoop *loop;
	NMClient *client;
} NMTstcClientNewData;

static void
_nmtstc_client_new_cb (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
	NMTstcClientNewData *d = user_data;
	gs_free_error GError *error = NULL;

	g_assert (!d->client);

	d->client = nm_client_new_finish (res,
	                                  nmtst_get_rand_bool () ? &error : NULL);

	nmtst_assert_success (NM_IS_CLIENT (d->client), error);

	g_main_loop_quit (d->loop);
}

static NMClient *
_nmtstc_client_new (gboolean sync)
{
	gs_free GError *error = NULL;
	NMClient *client;

	/* Create a NMClient instance synchronously, and arbitrarily use either
	 * the sync or async constructor.
	 *
	 * Note that the sync and async construct differ in one important aspect:
	 * the async constructor iterates the current g_main_context_get_thread_default(),
	 * while the sync constructor does not! Aside from that, both should behave
	 * pretty much the same way. */

	if (sync) {
		nm_auto_destroy_and_unref_gsource GSource *source = NULL;

		if (nmtst_get_rand_bool ()) {
			/* the current main context must not be iterated! */
			source = g_idle_source_new ();
			g_source_set_callback (source, nmtst_g_source_assert_not_called, NULL, NULL);
			g_source_attach (source, g_main_context_get_thread_default ());
		}

		if (nmtst_get_rand_bool ()) {
			gboolean success;

			client = g_object_new (NM_TYPE_CLIENT, NULL);
			g_assert (NM_IS_CLIENT (client));

			success = g_initable_init (G_INITABLE (client),
			                           NULL,
			                           nmtst_get_rand_bool () ? &error : NULL);
			nmtst_assert_success (success, error);
		} else {
			client = nm_client_new (NULL,
			                        nmtst_get_rand_bool () ? &error : NULL);
		}
	} else {
		nm_auto_unref_gmainloop GMainLoop *loop = NULL;
		NMTstcClientNewData d = { .loop = NULL, };

		loop = g_main_loop_new (g_main_context_get_thread_default (), FALSE);

		d.loop = loop;
		nm_client_new_async (NULL,
		                     _nmtstc_client_new_cb,
		                     &d);
		g_main_loop_run (loop);
		g_assert (NM_IS_CLIENT (d.client));
		client = d.client;
	}

	nmtst_assert_success (NM_IS_CLIENT (client), error);
	return client;
}

typedef struct {
	GMainLoop *loop;
	NMClient *client;
	bool sync;
} NewSyncInsideDispatchedData;

static gboolean
_nmtstc_client_new_inside_loop_do (gpointer user_data)
{
	NewSyncInsideDispatchedData *d = user_data;

	g_assert (d->loop);
	g_assert (!d->client);

	d->client = nmtstc_client_new (d->sync);
	g_main_loop_quit (d->loop);
	return G_SOURCE_CONTINUE;
}

static NMClient *
_nmtstc_client_new_inside_loop (gboolean sync)
{
	GMainContext *context = g_main_context_get_thread_default ();
	nm_auto_unref_gmainloop GMainLoop *loop = g_main_loop_new (context, FALSE);
	NewSyncInsideDispatchedData d = {
		.sync = sync,
		.loop = loop,
	};
	nm_auto_destroy_and_unref_gsource GSource *source = NULL;

	source = g_idle_source_new ();
	g_source_set_callback (source, _nmtstc_client_new_inside_loop_do, &d, NULL);
	g_source_attach (source, context);

	g_main_loop_run (loop);
	g_assert (NM_IS_CLIENT (d.client));
	return d.client;
}

static NMClient *
_nmtstc_client_new_extra_context (void)
{
	GMainContext *inner_context;
	NMClient *client;
	GSource *source;
	guint key_idx;

	inner_context = g_main_context_new ();
	g_main_context_push_thread_default (inner_context);

	client = nmtstc_client_new (TRUE);

	source = nm_utils_g_main_context_create_integrate_source (inner_context);

	g_main_context_pop_thread_default (inner_context);
	g_main_context_unref (inner_context);

	g_source_attach (source, g_main_context_get_thread_default ());

	for (key_idx = 0; TRUE; key_idx++) {
		char s[100];

		/* nmtstc_client_new() may call _nmtstc_client_new_extra_context() repeatedly. We
		 * need to attach the source to a previously unused key. */
		nm_sprintf_buf (s, "nm-test-extra-context-%u", key_idx);
		if (!g_object_get_data (G_OBJECT (client), s)) {
			g_object_set_data_full (G_OBJECT (client),
			                        s,
			                        source,
			                        (GDestroyNotify) nm_g_source_destroy_and_unref);
			break;
		}
	}

	return client;
}

NMClient *
nmtstc_client_new (gboolean allow_iterate_main_context)
{
	gboolean inside_loop;
	gboolean sync;

	if (nmtst_get_rand_uint32 () % 5 == 0)
		return _nmtstc_client_new_extra_context ();

	if (!allow_iterate_main_context) {
		sync = TRUE;
		inside_loop = FALSE;
	} else {
		/* The caller allows to iterate the main context. That that point,
		 * we can both use the synchronous and the asynchronous initialization,
		 * both should yield the same result. Choose one randomly. */
		sync = nmtst_get_rand_bool ();
		inside_loop = ((nmtst_get_rand_uint32 () % 3) == 0);
	}

	if (inside_loop) {
		/* Create the client on an idle handler of the current context.
		 * In practice, it should make no difference, which this check
		 * tries to prove. */
		return _nmtstc_client_new_inside_loop (sync);
	}

	return _nmtstc_client_new (sync);
}
