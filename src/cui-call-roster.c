/*
 * Copyright (C) 2026 Furi Labs
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cui-config.h"

#include "cui-call-roster.h"

#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#define DAEMON_BUS_NAME    "io.furios.Telephony.Daemon"
#define DAEMON_OBJECT_PATH "/io/furios/Telephony/Daemon"
#define DAEMON_INTERFACE   "io.furios.Telephony.Daemon"

#define CALLS_BUS_NAME     "org.gnome.Calls"
#define CALLS_OBJECT_PATH  "/org/gnome/Calls"
#define CALLS_INTERFACE    "org.gnome.Calls.Call"

/*
 * CuiCall describes one call and knows nothing of any other, so a display
 * holding a single call cannot tell whether a second one exists. The daemon
 * keeps the whole line, so the roster asks it who else is on the modem.
 */

enum {
  CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

struct _CuiCallRoster {
  GObject         parent_instance;

  GDBusProxy     *proxy;
  GDBusProxy     *calls_proxy;
  GDBusConnection *bus;
  guint           watch_id;
  GPtrArray      *calls;
  GHashTable     *names;
};

G_DEFINE_TYPE (CuiCallRoster, cui_call_roster, G_TYPE_OBJECT)


static void
cui_roster_call_free (gpointer data)
{
  CuiRosterCall *call = data;

  g_free (call->path);
  g_free (call->number);
  g_free (call->name);
  g_free (call->state);
  g_free (call);
}


/*
 * The daemon keys calls by modem path and carries no contact name, while
 * org.gnome.Calls resolves one per number. That is the same name the display
 * already shows for the featured call, so taking it from there keeps one
 * screen from naming the same person two different ways.
 */
/* The roster and the names arrive on their own schedule, so join them on both. */
static void
apply_names (CuiCallRoster *self)
{
  for (guint i = 0; self->calls && i < self->calls->len; i++) {
    CuiRosterCall *call = g_ptr_array_index (self->calls, i);
    const char *name = g_hash_table_lookup (self->names, call->number);

    g_clear_pointer (&call->name, g_free);
    if (name && !g_str_equal (name, call->number))
      call->name = g_strdup (name);
  }
}


static void
on_managed_objects_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  CuiCallRoster *self = data;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) objects = NULL;
  g_autoptr (GError) error = NULL;
  GVariantIter iter;
  const char *path;
  GVariant *interfaces;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  if (!reply) {
    g_debug ("Listing calls failed: %s", error->message);
    return;
  }

  objects = g_variant_get_child_value (reply, 0);
  if (!g_variant_is_of_type (objects, G_VARIANT_TYPE ("a{oa{sa{sv}}}"))) {
    g_debug ("Unexpected managed object type %s", g_variant_get_type_string (objects));
    return;
  }

  g_hash_table_remove_all (self->names);

  g_variant_iter_init (&iter, objects);
  while (g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
    g_autoptr (GVariant) props = g_variant_lookup_value (interfaces, CALLS_INTERFACE,
                                                         G_VARIANT_TYPE_VARDICT);

    if (props) {
      const char *id = NULL;
      const char *display_name = NULL;

      g_variant_lookup (props, "Id", "&s", &id);
      g_variant_lookup (props, "DisplayName", "&s", &display_name);

      if (id && *id && display_name && *display_name)
        g_hash_table_insert (self->names, g_strdup (id), g_strdup (display_name));
    }
    g_variant_unref (interfaces);
  }

  apply_names (self);
  g_signal_emit (self, signals[CHANGED], 0);
}


static void
refresh_names (CuiCallRoster *self)
{
  if (!self->calls_proxy)
    return;

  g_dbus_proxy_call (self->calls_proxy, "GetManagedObjects", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_managed_objects_ready, self);
}


static void
on_state_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  CuiCallRoster *self = data;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) state = NULL;
  g_autoptr (GVariant) calls = NULL;
  g_autoptr (GError) error = NULL;
  GVariantIter iter;
  const char *path;
  GVariant *props;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  if (!reply) {
    g_debug ("Reading telephony state failed: %s", error->message);
    return;
  }

  state = g_variant_get_child_value (reply, 0);
  if (!g_variant_lookup (state, "calls", "@a{sv}", &calls))
    return;

  g_clear_pointer (&self->calls, g_ptr_array_unref);
  self->calls = g_ptr_array_new_with_free_func (cui_roster_call_free);

  g_variant_iter_init (&iter, calls);
  while (g_variant_iter_next (&iter, "{&sv}", &path, &props)) {
    CuiRosterCall *call = g_new0 (CuiRosterCall, 1);
    const char *number = NULL;
    const char *call_state = NULL;
    gboolean multiparty = FALSE;

    if (g_variant_is_of_type (props, G_VARIANT_TYPE_VARIANT)) {
      GVariant *boxed = props;

      props = g_variant_get_variant (boxed);
      g_variant_unref (boxed);
    }

    g_variant_lookup (props, "number", "&s", &number);
    g_variant_lookup (props, "state", "&s", &call_state);
    g_variant_lookup (props, "multiparty", "b", &multiparty);

    call->path = g_strdup (path);
    call->number = g_strdup (number ? number : "");
    call->state = g_strdup (call_state ? call_state : "");
    call->multiparty = multiparty;
    g_ptr_array_add (self->calls, call);
    g_variant_unref (props);
  }

  apply_names (self);
  g_signal_emit (self, signals[CHANGED], 0);
}


static void
on_daemon_signal (GDBusProxy *proxy,
                  const char *sender,
                  const char *signal_name,
                  GVariant   *parameters,
                  gpointer    data)
{
  CuiCallRoster *self = data;

  if (g_str_equal (signal_name, "IncomingCall") ||
      g_str_equal (signal_name, "CallChanged") ||
      g_str_equal (signal_name, "CallRemoved"))
    cui_call_roster_refresh (self);
}


static void
on_proxy_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  CuiCallRoster *self = data;
  g_autoptr (GError) error = NULL;

  self->proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
  if (!self->proxy) {
    g_debug ("No call provider: %s", error->message);
    return;
  }

  g_signal_connect (self->proxy, "g-signal", G_CALLBACK (on_daemon_signal), self);
  cui_call_roster_refresh (self);
}


/*
 * A name can resolve after the call it belongs to appears, so watch for the
 * later property change as well as for calls coming and going.
 */
static void
on_calls_signal (GDBusConnection *connection,
                 const char      *sender,
                 const char      *path,
                 const char      *interface,
                 const char      *signal_name,
                 GVariant        *parameters,
                 gpointer         data)
{
  refresh_names (CUI_CALL_ROSTER (data));
}


static void
on_calls_proxy_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  CuiCallRoster *self = data;
  g_autoptr (GError) error = NULL;

  self->calls_proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
  if (!self->calls_proxy) {
    g_debug ("No call name provider: %s", error->message);
    return;
  }

  self->bus = g_dbus_proxy_get_connection (self->calls_proxy);
  self->watch_id = g_dbus_connection_signal_subscribe (self->bus,
                                                       CALLS_BUS_NAME,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                       on_calls_signal,
                                                       self,
                                                       NULL);
  refresh_names (self);
}


static void
cui_call_roster_dispose (GObject *object)
{
  CuiCallRoster *self = CUI_CALL_ROSTER (object);

  if (self->watch_id) {
    g_dbus_connection_signal_unsubscribe (self->bus, self->watch_id);
    self->watch_id = 0;
  }

  g_clear_object (&self->proxy);
  g_clear_object (&self->calls_proxy);
  g_clear_pointer (&self->calls, g_ptr_array_unref);
  g_clear_pointer (&self->names, g_hash_table_unref);

  G_OBJECT_CLASS (cui_call_roster_parent_class)->dispose (object);
}


static void
cui_call_roster_class_init (CuiCallRosterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = cui_call_roster_dispose;

  signals[CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}


static void
cui_call_roster_init (CuiCallRoster *self)
{
  self->names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                            NULL,
                            DAEMON_BUS_NAME,
                            DAEMON_OBJECT_PATH,
                            DAEMON_INTERFACE,
                            NULL,
                            on_proxy_ready,
                            self);

  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                            NULL,
                            CALLS_BUS_NAME,
                            CALLS_OBJECT_PATH,
                            "org.freedesktop.DBus.ObjectManager",
                            NULL,
                            on_calls_proxy_ready,
                            self);
}


/**
 * cui_call_roster_get_default:
 *
 * The shared roster.
 *
 * Returns: (transfer none): the roster
 */
CuiCallRoster *
cui_call_roster_get_default (void)
{
  static CuiCallRoster *instance;

  if (!instance) {
    instance = g_object_new (CUI_TYPE_CALL_ROSTER, NULL);
    g_object_add_weak_pointer (G_OBJECT (instance), (gpointer *) &instance);
  }

  return instance;
}


/**
 * cui_call_roster_get_calls:
 *
 * Returns: (transfer none) (element-type CuiRosterCall): every call on the
 *   modem, or %NULL before the daemon has answered
 */
GPtrArray *
cui_call_roster_get_calls (CuiCallRoster *self)
{
  g_return_val_if_fail (CUI_IS_CALL_ROSTER (self), NULL);

  return self->calls;
}


guint
cui_call_roster_get_count (CuiCallRoster *self)
{
  g_return_val_if_fail (CUI_IS_CALL_ROSTER (self), 0);

  return self->calls ? self->calls->len : 0;
}


/*
 * There is no per-call hold on the modem: swapping is what puts the active
 * call on hold and brings the held one back, so one entry point serves both.
 */
void
cui_call_roster_swap (CuiCallRoster *self)
{
  g_return_if_fail (CUI_IS_CALL_ROSTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "SwapCalls", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


void
cui_call_roster_hang_up (CuiCallRoster *self, const char *path)
{
  g_return_if_fail (CUI_IS_CALL_ROSTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "Hangup", g_variant_new ("(s)", path),
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


/* Answering a waiting call parks the one in progress; the modem does that. */
void
cui_call_roster_answer (CuiCallRoster *self, const char *path)
{
  g_return_if_fail (CUI_IS_CALL_ROSTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "Answer", g_variant_new ("(s)", path),
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


/*
 * Telephony offers a picker when several quick responses are configured; the
 * list lives in a json setting this has no parser for, so the single legacy
 * message is used and the same wording stands in when nothing is set.
 */
static char *
quick_response_text (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = NULL;
  char *text = NULL;

  if (source)
    schema = g_settings_schema_source_lookup (source, "io.furios.Telephony", TRUE);

  if (schema && g_settings_schema_has_key (schema, "reject-call-message")) {
    g_autoptr (GSettings) settings = g_settings_new ("io.furios.Telephony");

    text = g_settings_get_string (settings, "reject-call-message");
  }

  if (!text || !*text) {
    g_free (text);
    text = g_strdup (_("I can't talk right now."));
  }

  return text;
}


void
cui_call_roster_send_reply (CuiCallRoster *self, const char *number)
{
  g_autofree char *text = NULL;

  g_return_if_fail (CUI_IS_CALL_ROSTER (self));

  if (!self->proxy || !number || !*number)
    return;

  text = quick_response_text ();
  g_dbus_proxy_call (self->proxy, "SendTrackedSms",
                     g_variant_new ("(ss)", number, text),
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  cui_call_roster_silence (self);
}


void
cui_call_roster_hang_up_all (CuiCallRoster *self)
{
  g_return_if_fail (CUI_IS_CALL_ROSTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "HangupAll", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


void
cui_call_roster_silence (CuiCallRoster *self)
{
  g_return_if_fail (CUI_IS_CALL_ROSTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "SilenceRing", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


void
cui_call_roster_refresh (CuiCallRoster *self)
{
  g_return_if_fail (CUI_IS_CALL_ROSTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "GetTelephonyState", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_state_ready, self);
  refresh_names (self);
}


const char *
cui_call_roster_state_label (const char *state)
{
  if (g_str_equal (state, "held"))
    return _("On Hold");
  if (g_str_equal (state, "active"))
    return _("Active");
  if (g_str_equal (state, "dialing") || g_str_equal (state, "alerting"))
    return _("Calling");
  if (g_str_equal (state, "incoming") || g_str_equal (state, "waiting"))
    return _("Incoming");

  return state;
}
