/*
 * Copyright (C) 2026 Furi Labs
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cui-config.h"

#include "cui-audio-router.h"

#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#define DAEMON_BUS_NAME    "io.furios.Telephony.Daemon"
#define DAEMON_OBJECT_PATH "/io/furios/Telephony/Daemon"
#define DAEMON_INTERFACE   "io.furios.Telephony.Daemon"

/*
 * Call audio belongs to the telephony daemon: it owns the PulseAudio ports,
 * the voice profile and the per-route volumes, and it broadcasts what it has
 * applied. Asking it beats keeping a second opinion here, which could only
 * ever describe what we last requested rather than what is in force.
 */

enum {
  CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

struct _CuiAudioRouter {
  GObject     parent_instance;

  GDBusProxy *proxy;
  GPtrArray  *outputs;
  GPtrArray  *inputs;
  char       *output;
  char       *input;
  gboolean    mic_muted;
};

G_DEFINE_TYPE (CuiAudioRouter, cui_audio_router, G_TYPE_OBJECT)


static void
cui_audio_route_free (gpointer data)
{
  CuiAudioRoute *route = data;

  g_free (route->id);
  g_free (route);
}


static GPtrArray *
routes_from_variant (GVariant *array)
{
  GPtrArray *routes = g_ptr_array_new_with_free_func (cui_audio_route_free);
  GVariantIter iter;
  const char *id;
  gboolean available;

  g_variant_iter_init (&iter, array);
  while (g_variant_iter_next (&iter, "(&sb)", &id, &available)) {
    CuiAudioRoute *route = g_new0 (CuiAudioRoute, 1);

    route->id = g_strdup (id);
    route->available = available;
    g_ptr_array_add (routes, route);
  }

  return routes;
}


static void
take_state (CuiAudioRouter *self, GVariant *state)
{
  const char *route = NULL;
  const char *input = NULL;
  gboolean muted = FALSE;

  if (g_variant_lookup (state, "route", "&s", &route) && route && *route) {
    g_free (self->output);
    self->output = g_strdup (route);
  }

  if (g_variant_lookup (state, "input", "&s", &input) && input && *input) {
    g_free (self->input);
    self->input = g_strdup (input);
  }

  if (g_variant_lookup (state, "mic_muted", "b", &muted))
    self->mic_muted = muted;
}


static void
on_routes_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  CuiAudioRouter *self = data;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) outputs = NULL;
  g_autoptr (GVariant) inputs = NULL;
  g_autoptr (GError) error = NULL;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  if (!reply) {
    g_debug ("Listing audio routes failed: %s", error->message);
    return;
  }

  g_variant_get (reply, "(@a(sb)@a(sb))", &outputs, &inputs);
  g_clear_pointer (&self->outputs, g_ptr_array_unref);
  g_clear_pointer (&self->inputs, g_ptr_array_unref);
  self->outputs = routes_from_variant (outputs);
  self->inputs = routes_from_variant (inputs);

  g_signal_emit (self, signals[CHANGED], 0);
}


static void
on_telephony_state_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  CuiAudioRouter *self = data;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) state = NULL;
  g_autoptr (GError) error = NULL;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  if (!reply) {
    g_debug ("Reading telephony state failed: %s", error->message);
    return;
  }

  state = g_variant_get_child_value (reply, 0);
  take_state (self, state);

  g_signal_emit (self, signals[CHANGED], 0);
}


static void
on_daemon_signal (GDBusProxy *proxy,
                  const char *sender,
                  const char *signal_name,
                  GVariant   *parameters,
                  gpointer    data)
{
  CuiAudioRouter *self = data;
  g_autoptr (GVariant) state = NULL;

  if (!g_str_equal (signal_name, "AudioRouteChanged"))
    return;

  state = g_variant_get_child_value (parameters, 0);
  take_state (self, state);

  g_signal_emit (self, signals[CHANGED], 0);
}


static void
on_proxy_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  CuiAudioRouter *self = data;
  g_autoptr (GError) error = NULL;

  self->proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
  if (!self->proxy) {
    g_warning ("No call audio provider: %s", error->message);
    return;
  }

  g_signal_connect (self->proxy, "g-signal", G_CALLBACK (on_daemon_signal), self);
  cui_audio_router_refresh (self);
}


static void
cui_audio_router_dispose (GObject *object)
{
  CuiAudioRouter *self = CUI_AUDIO_ROUTER (object);

  g_clear_object (&self->proxy);
  g_clear_pointer (&self->outputs, g_ptr_array_unref);
  g_clear_pointer (&self->inputs, g_ptr_array_unref);
  g_clear_pointer (&self->output, g_free);
  g_clear_pointer (&self->input, g_free);

  G_OBJECT_CLASS (cui_audio_router_parent_class)->dispose (object);
}


static void
cui_audio_router_class_init (CuiAudioRouterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = cui_audio_router_dispose;

  signals[CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}


static void
cui_audio_router_init (CuiAudioRouter *self)
{
  self->output = g_strdup ("earpiece");
  self->input = g_strdup ("mic");

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
 * cui_audio_router_get_default:
 *
 * The shared router. One proxy serves every call display, so they all
 * read the same applied state.
 *
 * Returns: (transfer none): the router
 */
CuiAudioRouter *
cui_audio_router_get_default (void)
{
  static CuiAudioRouter *instance;

  if (!instance) {
    instance = g_object_new (CUI_TYPE_AUDIO_ROUTER, NULL);
    g_object_add_weak_pointer (G_OBJECT (instance), (gpointer *) &instance);
  }

  return instance;
}


gboolean
cui_audio_router_is_ready (CuiAudioRouter *self)
{
  g_return_val_if_fail (CUI_IS_AUDIO_ROUTER (self), FALSE);

  return self->proxy != NULL && self->outputs != NULL;
}


/**
 * cui_audio_router_get_outputs:
 *
 * Returns: (transfer none) (element-type CuiAudioRoute): the output routes,
 *   or %NULL before the daemon has answered
 */
GPtrArray *
cui_audio_router_get_outputs (CuiAudioRouter *self)
{
  g_return_val_if_fail (CUI_IS_AUDIO_ROUTER (self), NULL);

  return self->outputs;
}


/**
 * cui_audio_router_get_inputs:
 *
 * Returns: (transfer none) (element-type CuiAudioRoute): the input routes,
 *   or %NULL before the daemon has answered
 */
GPtrArray *
cui_audio_router_get_inputs (CuiAudioRouter *self)
{
  g_return_val_if_fail (CUI_IS_AUDIO_ROUTER (self), NULL);

  return self->inputs;
}


const char *
cui_audio_router_get_output (CuiAudioRouter *self)
{
  g_return_val_if_fail (CUI_IS_AUDIO_ROUTER (self), "earpiece");

  return self->output;
}


const char *
cui_audio_router_get_input (CuiAudioRouter *self)
{
  g_return_val_if_fail (CUI_IS_AUDIO_ROUTER (self), "mic");

  return self->input;
}


gboolean
cui_audio_router_get_mic_muted (CuiAudioRouter *self)
{
  g_return_val_if_fail (CUI_IS_AUDIO_ROUTER (self), FALSE);

  return self->mic_muted;
}


/*
 * Send the intent only. The daemon answers with what it managed to apply,
 * and that broadcast is what moves the check mark.
 */
void
cui_audio_router_set_output (CuiAudioRouter *self, const char *route_id)
{
  g_return_if_fail (CUI_IS_AUDIO_ROUTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "SetAudioRoute",
                     g_variant_new ("(s)", route_id),
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


void
cui_audio_router_set_input (CuiAudioRouter *self, const char *route_id)
{
  g_return_if_fail (CUI_IS_AUDIO_ROUTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "SetInputRoute",
                     g_variant_new ("(s)", route_id),
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


void
cui_audio_router_set_mic_muted (CuiAudioRouter *self, gboolean muted)
{
  g_return_if_fail (CUI_IS_AUDIO_ROUTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, muted ? "MuteMic" : "UnmuteMic", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}


/*
 * Availability moves under us when a headset is plugged in or a headphone
 * pairs, so the list is re-read rather than cached for the call's lifetime.
 */
void
cui_audio_router_refresh (CuiAudioRouter *self)
{
  g_return_if_fail (CUI_IS_AUDIO_ROUTER (self));

  if (!self->proxy)
    return;

  g_dbus_proxy_call (self->proxy, "GetAudioRoutes", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_routes_ready, self);
  g_dbus_proxy_call (self->proxy, "GetTelephonyState", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_telephony_state_ready, self);
}


const char *
cui_audio_router_output_label (const char *route_id)
{
  if (g_str_equal (route_id, "earpiece"))
    return _("Earpiece");
  if (g_str_equal (route_id, "speaker"))
    return _("Speaker");
  if (g_str_equal (route_id, "wired"))
    return _("Wired Headset");
  if (g_str_equal (route_id, "bluetooth"))
    return _("Bluetooth");

  return route_id;
}


const char *
cui_audio_router_output_icon (const char *route_id)
{
  if (g_str_equal (route_id, "speaker"))
    return "audio-speakers-symbolic";
  if (g_str_equal (route_id, "wired"))
    return "audio-headset-symbolic";
  if (g_str_equal (route_id, "bluetooth"))
    return "bluetooth-active-symbolic";

  return "phone-symbolic";
}


const char *
cui_audio_router_input_label (const char *route_id)
{
  if (g_str_equal (route_id, "mic"))
    return _("Microphone");
  if (g_str_equal (route_id, "wired"))
    return _("Wired Mic");
  if (g_str_equal (route_id, "bluetooth"))
    return _("Bluetooth Mic");

  return route_id;
}


const char *
cui_audio_router_input_icon (const char *route_id)
{
  if (g_str_equal (route_id, "wired"))
    return "audio-headset-symbolic";
  if (g_str_equal (route_id, "bluetooth"))
    return "bluetooth-active-symbolic";

  return "audio-input-microphone-symbolic";
}
