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
  GObject     parent_instance;

  GDBusProxy *proxy;
  GPtrArray  *calls;
};

G_DEFINE_TYPE (CuiCallRoster, cui_call_roster, G_TYPE_OBJECT)


static void
cui_roster_call_free (gpointer data)
{
  CuiRosterCall *call = data;

  g_free (call->path);
  g_free (call->number);
  g_free (call->state);
  g_free (call);
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


static void
cui_call_roster_dispose (GObject *object)
{
  CuiCallRoster *self = CUI_CALL_ROSTER (object);

  g_clear_object (&self->proxy);
  g_clear_pointer (&self->calls, g_ptr_array_unref);

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
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                            NULL,
                            DAEMON_BUS_NAME,
                            DAEMON_OBJECT_PATH,
                            DAEMON_INTERFACE,
                            NULL,
                            on_proxy_ready,
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


void
cui_call_roster_refresh (CuiCallRoster *self)
{
  g_return_if_fail (CUI_IS_CALL_ROSTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "GetTelephonyState", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_state_ready, self);
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
