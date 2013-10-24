/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013  Instituto Nokia de Tecnologia - INdT
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>

#include "lib/uuid.h"
#include "attrib/att.h"

#define APP_MANAGER_INTERFACE "org.bluez.ApplicationManager1"

#define SERVICE_INTERFACE "org.bluez.Service1"
#define CHARACTERISTIC_INTERFACE "org.bluez.Characteristic1"
#define ERROR_INTERFACE "org.bluez.Error"

#define SERVICE_PATH "/service%d"
#define CHARACTERISTIC_PATH "/characteristic%d"

#define IMMEDIATE_ALERT_UUID16 "1802"
#define LINK_LOSS_UUID16 "1803"
#define ALERT_LEVEL_CHR_UUID16 "2a06"

/*
 * Unique application identifier generated by uuidgen tool
 * The application identifier (group id) is used by bluetoothd
 * to "try"to allocate the same handle range across connections
 * (RegisterAgent calls). When colision happens bluetoothd adds
 * the attributes at the end of the database, and overwrites the
 * service start handle.
 */

#define GATT_SERVICE_APP_ID	"3ab1b08c-6f6b-44a0-a5f0-2f0ee2506c5c"

#define TIMEOUT 60*1000 /* Timeout for user response (miliseconds) */

static GMainLoop *main_loop;
static DBusConnection *dbus_conn;
static GSList *services = NULL;

struct service {
	char *uuid;
	GSList *includes;
};

enum char_features_t {
	CHAR_FEATURE_PROP_VALUE = (1 << 0),
	CHAR_FEATURE_HAS_PERMS = (1 << 1),
	CHAR_FEATURE_HAS_AUTH = (1 << 2),
};

struct characteristic {
	char *uuid;
	uint8_t *value;
	int vlen;
	int features;
	uint8_t perms;
	bool auth;
	char **props;
	GSList *descriptors;
};

const char *properties[] = {
	"broadcast",
	"read",
	"write-without-response",
	"write",
	"notify",
	"indicate",
	"authenticated-signed-writes",
	NULL
};
const char *extproperties[] = {
	"reliable-write",
	"writable-auxiliaries",
	NULL
};

static char **properties2string(uint8_t proper, uint16_t extproper)
{
	char **propers = g_malloc0(25 * sizeof(char *));
	int i, num = 0;

	for (i = 0; properties[i]; i++) {
		if (!(proper & (1 << i)))
			continue;

		propers[num] = g_strdup(properties[i]);
		num++;
	}

	for (i = 0; extproperties[i]; i++) {
		if (!(extproper & (1 << i)))
			continue;

		propers[num] = g_strdup(extproperties[i]);
		num++;
	}

	return propers;
}

static gboolean service_get_uuid(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct service *service = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &service->uuid);

	return TRUE;
}

static gboolean service_get_includes(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	return TRUE;
}

static gboolean service_exist_includes(const GDBusPropertyTable *property,
								void *data)
{
	struct service *service = data;

	return service->includes != NULL;
}

static const GDBusPropertyTable service_properties[] = {
	{ "UUID", "s", service_get_uuid },
	{ "Includes", "ao", service_get_includes, NULL,
					service_exist_includes },
	{ }
};

static gboolean chr_get_uuid(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct characteristic *chr = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &chr->uuid);

	return TRUE;
}

static gboolean chr_get_value(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct characteristic *chr = data;
	DBusMessageIter array;

	fprintf(stdout, "Get(\"Value\")\n");

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);

	dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
						&chr->value, chr->vlen);

	dbus_message_iter_close_container(iter, &array);

	return TRUE;
}

static void chr_set_value(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct characteristic *chr = user_data;
	DBusMessageIter array;
	uint8_t *value;
	int len;

	fprintf(stdout, "Set(\"Value\", ...)\n");

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY) {
		fprintf(stderr, "Invalid value for Set(\"Value\"...)\n");
		g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");
		return;
	}

	dbus_message_iter_recurse(iter, &array);
	dbus_message_iter_get_fixed_array(&array, &value, &len);

	g_free(chr->value);
	chr->value = g_memdup(value, len);
	chr->vlen = len;

	g_dbus_pending_property_success(id);
}

static gboolean chr_exist_value(const GDBusPropertyTable *property,
								void *data)
{
	struct characteristic *chr = data;

	return !!(chr->features & CHAR_FEATURE_PROP_VALUE);
}

static gboolean chr_get_perms(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct characteristic *chr = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE, &chr->perms);

	return TRUE;
}

static gboolean chr_exist_perms(const GDBusPropertyTable *property,
								void *data)
{
	struct characteristic *chr = data;

	return !!(chr->features & CHAR_FEATURE_HAS_PERMS);
}


static gboolean chr_get_props(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct characteristic *chr = data;
	DBusMessageIter array;
	int i;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_STRING_AS_STRING, &array);

	for (i = 0; chr->props[i]; i++)
		dbus_message_iter_append_basic(&array,
					DBUS_TYPE_STRING, &chr->props[i]);

	dbus_message_iter_close_container(iter, &array);

	return TRUE;
}

static gboolean chr_exist_props(const GDBusPropertyTable *property,
								void *data)
{
	return TRUE;
}

static gboolean chr_get_auth(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct characteristic *chr = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE, &chr->auth);

	return TRUE;
}

static gboolean chr_exist_auth(const GDBusPropertyTable *property, void *data)
{
	struct characteristic *chr = data;

	return !!(chr->features & CHAR_FEATURE_HAS_AUTH);
}

static gboolean chr_get_descriptors(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	return TRUE;
}

static void chr_set_descriptors(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	g_dbus_pending_property_success(id);
}

static gboolean chr_exist_descriptors(const GDBusPropertyTable *property,
								void *data)
{
	struct characteristic *chr = data;

	return chr->descriptors != NULL;
}

static const GDBusPropertyTable chr_properties[] = {
	{ "UUID", "s", chr_get_uuid },
	{ "Value", "ay", chr_get_value, chr_set_value, chr_exist_value },
	{ "Permissions", "y", chr_get_perms, NULL, chr_exist_perms },
	{ "Authenticate", "b", chr_get_auth, NULL, chr_exist_auth },
	{ "Flags", "as", chr_get_props, NULL, chr_exist_props },
	{ "Descriptors", "a{a{sv}}", chr_get_descriptors, chr_set_descriptors,
						chr_exist_descriptors },
	{ }
};

static bool register_agent(DBusConnection *conn, GSList *paths)
{
	DBusMessage *msg;
	DBusMessageIter iter, dict, entry, value;
	DBusPendingCall *call;
	const char *gid_key = "GID";
	const char *gid_val = GATT_SERVICE_APP_ID;
	const char *app_path = "/gatt/example/app";

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
				APP_MANAGER_INTERFACE, "RegisterAgent");
	if (msg == NULL) {
		fprintf(stderr, "Couldn't allocate D-Bus message\n");
		return false;
	}

	dbus_message_iter_init_append(msg, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &app_path);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

	/* Group ID key */
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL,
								&entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &gid_key);

	/* Group ID value */
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &value);
	dbus_message_iter_append_basic(&value, DBUS_TYPE_STRING, &gid_val);
	dbus_message_iter_close_container(&entry, &value);

	dbus_message_iter_close_container(&dict, &entry);
	dbus_message_iter_close_container(&iter, &dict);

	if (!g_dbus_send_message_with_reply(conn, msg, &call, TIMEOUT)) {
		dbus_message_unref(msg);
		return false;
	}

	dbus_message_unref(msg);
	dbus_pending_call_unref(call);

	return true;
}

static bool populate_service(DBusConnection *conn, const char *uuid,
							const char **path)
{
	struct service *service;
	static char service_path[64];
	static int id = 1;

	snprintf(service_path, sizeof(service_path), SERVICE_PATH, id++);

	service = g_new0(struct service, 1);

	service->uuid = g_strdup(uuid);

	if (g_dbus_register_interface(conn, service_path, SERVICE_INTERFACE,
					NULL, NULL, service_properties,
					service, NULL) == FALSE) {
		fprintf(stderr, "Couldn't register service interface\n");
		return false;
	}

	*path = service_path;

	return true;
}

static bool populate_characteristic(DBusConnection *conn, const char *uuid,
					uint8_t props, const char *service_path)
{
	struct characteristic *chr;
	char chr_path[64];
	static int id = 1;

	snprintf(chr_path, sizeof(chr_path), "%s" CHARACTERISTIC_PATH,
							service_path, id++);

	chr = g_new0(struct characteristic, 1);

	chr->uuid = g_strdup(uuid);

	chr->value = g_new0(uint8_t, 1);
	chr->vlen = sizeof(uint8_t);
	chr->props = properties2string(props, 0);

	chr->features = CHAR_FEATURE_PROP_VALUE;

	if (g_dbus_register_interface(conn, chr_path,
					CHARACTERISTIC_INTERFACE,
					NULL, NULL, chr_properties,
					chr, NULL) == FALSE) {
		fprintf(stderr, "Couldn't register characteristic interface\n");
		return false;
	}

	return true;
}

static void create_services(DBusConnection *conn)
{
	const char *service_path;

	/* Immediate Alert Service (IAS) */

	if (!populate_service(conn, IMMEDIATE_ALERT_UUID16, &service_path))
		return;

	if (!populate_characteristic(conn, ALERT_LEVEL_CHR_UUID16,
					ATT_CHAR_PROPER_WRITE_WITHOUT_RESP,
					service_path))
		return;

	services = g_slist_append(services, g_strdup(service_path));

	/* Link Loss Service (LLS) */

	if (!populate_service(conn, LINK_LOSS_UUID16, &service_path))
		return;

	if (!populate_characteristic(conn, ALERT_LEVEL_CHR_UUID16,
				ATT_CHAR_PROPER_READ | ATT_CHAR_PROPER_WRITE,
				service_path))
		return;

	services = g_slist_append(services, g_strdup(service_path));
}

static void connect_handler(DBusConnection *conn, void *user_data)
{
	if (services == NULL)
		return;

	if (!register_agent(conn, services))
		fprintf(stderr, "Could not send RegisterAgent\n");
}

static gboolean signal_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	static unsigned int __terminated = 0;
	struct signalfd_siginfo si;
	ssize_t result;
	int fd;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	result = read(fd, &si, sizeof(si));
	if (result != sizeof(si))
		return FALSE;

	switch (si.ssi_signo) {
	case SIGINT:
	case SIGTERM:
		if (__terminated == 0) {
			fprintf(stderr, "Terminating...\n");
			g_main_loop_quit(main_loop);
		}

		__terminated = 1;
		break;
	}

	return TRUE;
}

static guint setup_signalfd(void)
{
	GIOChannel *channel;
	guint source;
	sigset_t mask;
	int fd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("Failed to set signal mask");
		return 0;
	}

	fd = signalfd(-1, &mask, 0);
	if (fd < 0) {
		perror("Failed to create signal descriptor");
		return 0;
	}

	channel = g_io_channel_unix_new(fd);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				signal_handler, NULL);

	g_io_channel_unref(channel);

	return source;
}

int main(int argc, char *argv[])
{
	GDBusClient *client;
	guint signal;

	main_loop = g_main_loop_new(NULL, FALSE);
	dbus_conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, NULL);

	client = g_dbus_client_new(dbus_conn, "org.bluez", "/org/bluez");

	g_dbus_client_set_connect_watch(client, connect_handler, NULL);

	g_dbus_attach_object_manager(dbus_conn);

	create_services(dbus_conn);

	signal = setup_signalfd();

	g_main_loop_run(main_loop);

	g_source_remove(signal);

	g_dbus_client_unref(client);

	g_slist_free_full(services, g_free);
	dbus_connection_unref(dbus_conn);
	g_main_loop_unref(main_loop);

	return 0;
}
