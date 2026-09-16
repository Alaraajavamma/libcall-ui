/*
 * Copyright (C) 2021, 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 *         Evangelos Ribeiro Tzaras <devrtz@fortysixandtwo.eu>
 *
 * Somewhat based on call's call-display by:
 * Author: Bob Ham <bob.ham@puri.sm>
 */

#include "cui-config.h"

#include "cui-call-display.h"
#include "cui-encryption-indicator-priv.h"

#include "cui-call.h"

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <handy.h>
#include "cui-audio-router.h"

#define IS_NULL_OR_EMPTY(x)  ((x) == NULL || (x)[0] == '\0')

#define HDY_AVATAR_SIZE_BIG 160
#define HDY_AVATAR_SIZE_DEFAULT 80

/**
 * CuiCallDisplay:
 *
 * A [class@Gtk.Widget] that handles the UI elements of a
 * phone call. It displays the [iface@Cui.Call]'s information and allows
 * actions such as accepting or rejecting the call, hanging up, etc.
 */

enum {
  PROP_0,
  PROP_CALL,
  PROP_ALLOW_ADD_CALL,
  PROP_LAST_PROP,
};
static GParamSpec *props[PROP_LAST_PROP];

struct _CuiCallDisplay {
  GtkOverlay              parent_instance;

  CuiCall                *call;

  HdyAvatar              *avatar;
  GtkLabel               *primary_contact_info;
  GtkLabel               *secondary_contact_info;
  GtkLabel               *status;

  GtkBox                 *controls;
  GtkBox                 *gsm_controls;
  GtkBox                 *general_controls;
  GtkToggleButton        *speaker;
  GtkToggleButton        *mute;
  GtkLabel               *mute_label;
  GtkButton              *hang_up;
  GtkButton              *add_call;
  gboolean                allow_add_call;
  GtkButton              *answer;
  CuiEncryptionIndicator *encryption_indicator;

  GCancellable           *cancel;
  GtkRevealer            *dial_pad_revealer;
  GtkToggleButton        *dial_pad;
  GtkToggleButton        *actions;
  GtkRevealer            *actions_revealer;
  GtkRevealer            *speaker_revealer;
  GtkRevealer            *mute_revealer;
  GtkLabel               *speaker_label;
  GtkWidget              *keypad_speaker;
  GtkToggleButton        *hold;
  GtkBox                 *box_speaker;
  GtkBox                 *box_mute;
  GtkEntry               *keypad_entry;

  CuiAudioRouter         *router;
  gulong                  router_changed_id;

  GBinding               *dtmf_bind;
  GBinding               *avatar_icon_bind;
  GBinding               *encryption_bind;

  gboolean                update_status_time;
};

G_DEFINE_TYPE (CuiCallDisplay, cui_call_display, GTK_TYPE_OVERLAY);


static void
on_answer_clicked (CuiCallDisplay *self)
{
  g_return_if_fail (CUI_IS_CALL_DISPLAY (self));

  self->update_status_time = FALSE;
  gtk_label_set_label (self->status,
                       _("Accepting call…"));

  gtk_widget_set_sensitive (GTK_WIDGET (self->answer), FALSE);

  cui_call_accept (self->call);
}


static void
on_hang_up_clicked (CuiCallDisplay *self)
{
  g_return_if_fail (CUI_IS_CALL_DISPLAY (self));

  self->update_status_time = FALSE;
  gtk_label_set_label (self->status,
                       _("Hanging up…"));

  gtk_widget_set_sensitive (GTK_WIDGET (self->hang_up), FALSE);

  cui_call_hang_up (self->call);
}


static void
hold_toggled_cb (GtkToggleButton *togglebutton,
                 CuiCallDisplay  *self)
{
}


/* Re-ask on the way open: a headset can arrive while the sheet is shut. */
static void
mute_toggled_cb (GtkToggleButton *togglebutton,
                 CuiCallDisplay  *self)
{
  if (gtk_toggle_button_get_active (togglebutton))
    cui_audio_router_refresh (self->router);
}


static void
speaker_toggled_cb (GtkToggleButton *togglebutton,
                    CuiCallDisplay  *self)
{
  if (gtk_toggle_button_get_active (togglebutton))
    cui_audio_router_refresh (self->router);
}


static void
add_call_clicked_cb (GtkButton      *button,
                     CuiCallDisplay *self)
{
}


static void
hide_dial_pad_clicked_cb (CuiCallDisplay *self)
{
  gtk_revealer_set_reveal_child (self->dial_pad_revealer, FALSE);
}



/*
 * The route and the microphone are chosen from their own sheets rather than
 * from the pills, so each pill can say what is in force instead of only
 * whether it is pressed. Both sheets list what the daemon offers, so a
 * headset that arrives mid-call shows up without the display guessing.
 */
static GtkWidget *
add_sheet_row (CuiCallDisplay *self,
               GtkBox         *box,
               const char     *icon_name,
               const char     *label,
               gboolean        selected,
               gboolean        available,
               GCallback       callback)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_BUTTON);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 20);
  gtk_widget_set_halign (row, GTK_ALIGN_CENTER);
  gtk_box_pack_start (GTK_BOX (row), icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), gtk_label_new (label), FALSE, FALSE, 0);

  if (!available) {
    GtkWidget *subtitle = gtk_label_new (_("Not connected"));

    gtk_style_context_add_class (gtk_widget_get_style_context (subtitle), "dim-label");
    gtk_box_pack_start (GTK_BOX (row), subtitle, FALSE, FALSE, 0);
  } else if (selected) {
    GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic",
                                                     GTK_ICON_SIZE_BUTTON);

    gtk_image_set_pixel_size (GTK_IMAGE (check), 16);
    gtk_box_pack_start (GTK_BOX (row), check, FALSE, FALSE, 0);
  }

  gtk_widget_set_can_default (button, FALSE);
  gtk_widget_set_size_request (button, -1, 56);
  gtk_container_add (GTK_CONTAINER (button), row);
  gtk_style_context_add_class (gtk_widget_get_style_context (button), "cui-row-button");
  gtk_widget_set_sensitive (button, available);

  if (available && callback)
    g_signal_connect_swapped (button, "clicked", callback, self);

  gtk_box_pack_start (box, button, FALSE, FALSE, 0);
  gtk_widget_show_all (button);

  return button;
}


static void
clear_sheet_rows (GtkBox *box)
{
  g_autoptr (GList) children = gtk_container_get_children (GTK_CONTAINER (box));

  for (GList *l = children ? children->next : NULL; l; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
}


static void
output_row_clicked_cb (CuiCallDisplay *self, GtkButton *button)
{
  const char *route_id = g_object_get_data (G_OBJECT (button), "route-id");

  cui_audio_router_set_output (self->router, route_id);
  gtk_toggle_button_set_active (self->speaker, FALSE);
}


static void
input_row_clicked_cb (CuiCallDisplay *self, GtkButton *button)
{
  const char *route_id = g_object_get_data (G_OBJECT (button), "route-id");

  cui_audio_router_set_mic_muted (self->router, FALSE);
  cui_audio_router_set_input (self->router, route_id);
  gtk_toggle_button_set_active (self->mute, FALSE);
}


static void
input_mute_clicked_cb (CuiCallDisplay *self)
{
  cui_audio_router_set_mic_muted (self->router, TRUE);
  gtk_toggle_button_set_active (self->mute, FALSE);
}


static void
rebuild_output_sheet (CuiCallDisplay *self)
{
  GPtrArray *routes = cui_audio_router_get_outputs (self->router);
  const char *active = cui_audio_router_get_output (self->router);

  clear_sheet_rows (self->box_speaker);

  for (guint i = 0; routes && i < routes->len; i++) {
    CuiAudioRoute *route = g_ptr_array_index (routes, i);
    GtkWidget *button = add_sheet_row (self, self->box_speaker,
                                       cui_audio_router_output_icon (route->id),
                                       cui_audio_router_output_label (route->id),
                                       g_strcmp0 (route->id, active) == 0,
                                       route->available,
                                       G_CALLBACK (output_row_clicked_cb));

    g_object_set_data_full (G_OBJECT (button), "route-id",
                            g_strdup (route->id), g_free);
  }
}


static void
rebuild_input_sheet (CuiCallDisplay *self)
{
  GPtrArray *routes = cui_audio_router_get_inputs (self->router);
  const char *active = cui_audio_router_get_input (self->router);
  gboolean muted = cui_audio_router_get_mic_muted (self->router);

  clear_sheet_rows (self->box_mute);

  for (guint i = 0; routes && i < routes->len; i++) {
    CuiAudioRoute *route = g_ptr_array_index (routes, i);
    GtkWidget *button = add_sheet_row (self, self->box_mute,
                                       cui_audio_router_input_icon (route->id),
                                       cui_audio_router_input_label (route->id),
                                       !muted && g_strcmp0 (route->id, active) == 0,
                                       route->available,
                                       G_CALLBACK (input_row_clicked_cb));

    g_object_set_data_full (G_OBJECT (button), "route-id",
                            g_strdup (route->id), g_free);
  }

  add_sheet_row (self, self->box_mute, "microphone-sensitivity-muted-symbolic",
                 _("Muted"), muted, TRUE, G_CALLBACK (input_mute_clicked_cb));
}


static void
on_router_changed (CuiCallDisplay *self)
{
  const char *route = cui_audio_router_get_output (self->router);
  gboolean muted = cui_audio_router_get_mic_muted (self->router);
  GtkStyleContext *style = gtk_widget_get_style_context (GTK_WIDGET (self->mute_label));

  gtk_label_set_label (self->speaker_label, cui_audio_router_output_label (route));
  gtk_widget_set_visible (self->keypad_speaker, g_str_equal (route, "earpiece"));

  /* Nothing to choose from until a provider answers, so do not offer the sheets. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->speaker), cui_audio_router_is_ready (self->router));
  gtk_widget_set_sensitive (GTK_WIDGET (self->mute), cui_audio_router_is_ready (self->router));

  gtk_label_set_label (self->mute_label,
                       muted ? _("Muted")
                             : cui_audio_router_input_label (cui_audio_router_get_input (self->router)));
  if (muted)
    gtk_style_context_add_class (style, "cui-row-muted");
  else
    gtk_style_context_remove_class (style, "cui-row-muted");

  rebuild_output_sheet (self);
  rebuild_input_sheet (self);
}


/* Offered on the keypad sheet only while the call is still on the earpiece. */
static void
keypad_speaker_clicked_cb (CuiCallDisplay *self)
{
  cui_audio_router_set_output (self->router, "speaker");
}


static void
hide_output_clicked_cb (CuiCallDisplay *self)
{
  gtk_revealer_set_reveal_child (self->speaker_revealer, FALSE);
}


static void
hide_input_clicked_cb (CuiCallDisplay *self)
{
  gtk_revealer_set_reveal_child (self->mute_revealer, FALSE);
}


static void
hide_actions_clicked_cb (CuiCallDisplay *self)
{
  gtk_revealer_set_reveal_child (self->actions_revealer, FALSE);
}


static void
set_pretty_time (CuiCallDisplay *self)
{
  gdouble elapsed;
  g_autofree char *duration = NULL;

  g_assert (CUI_IS_CALL_DISPLAY (self));
  g_assert (CUI_IS_CALL (self->call));

  elapsed = cui_call_get_active_time (self->call);
  duration = cui_call_format_duration (elapsed);

  gtk_label_set_label (self->status, duration);
}


static void
on_call_state_changed (CuiCallDisplay *self,
                       GParamSpec     *psepc,
                       CuiCall        *call)
{
  GtkStyleContext *hang_up_style;
  CuiCallState state;

  g_return_if_fail (CUI_IS_CALL_DISPLAY (self));
  g_return_if_fail (CUI_IS_CALL (call));

  state = cui_call_get_state (call);

  g_debug ("Call %p changed state to %s",
           call,
           cui_call_state_to_string (state));

  hang_up_style = gtk_widget_get_style_context
                    (GTK_WIDGET (self->hang_up));

  /* if the state changed than the call must be responsive */
  self->update_status_time = TRUE;
  gtk_widget_set_sensitive (GTK_WIDGET (self->answer), TRUE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->hang_up), TRUE);

  /* Widgets and call audio mode*/
  switch (state)
  {
  case CUI_CALL_STATE_INCOMING:
    hdy_avatar_set_size (self->avatar, HDY_AVATAR_SIZE_BIG);

    gtk_widget_hide (GTK_WIDGET (self->controls));
    gtk_widget_show (GTK_WIDGET (self->answer));
    gtk_style_context_remove_class
      (hang_up_style, GTK_STYLE_CLASS_DESTRUCTIVE_ACTION);
    break;

  case CUI_CALL_STATE_ACTIVE:
    hdy_avatar_set_size (self->avatar, HDY_AVATAR_SIZE_DEFAULT);
    G_GNUC_FALLTHROUGH;

  case CUI_CALL_STATE_CALLING:
  case CUI_CALL_STATE_HELD:
    gtk_style_context_add_class
      (hang_up_style, GTK_STYLE_CLASS_DESTRUCTIVE_ACTION);
    gtk_widget_hide (GTK_WIDGET (self->answer));
    gtk_widget_show (GTK_WIDGET (self->controls));

    gtk_widget_set_visible
      (GTK_WIDGET (self->gsm_controls),
      state != CUI_CALL_STATE_CALLING);

    /*
     * The daemon raises the voice profile and picks the opening route when
     * the call starts; ask it what it settled on rather than deciding here.
     */
    cui_audio_router_refresh (self->router);
    break;

  case CUI_CALL_STATE_DISCONNECTED:
    gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);
    break;

  case CUI_CALL_STATE_UNKNOWN:
  default:
    g_warn_if_reached ();
  }

  /* Status text */
  switch (state)
  {
  case CUI_CALL_STATE_ACTIVE:
    set_pretty_time (self);
    break;

  case CUI_CALL_STATE_INCOMING:
  case CUI_CALL_STATE_CALLING:
  case CUI_CALL_STATE_HELD:
  case CUI_CALL_STATE_DISCONNECTED:
    gtk_label_set_label (self->status, cui_call_state_to_string (state));
    break;

  case CUI_CALL_STATE_UNKNOWN:
  default:
    g_warn_if_reached ();
  }
}


static void
on_update_contact_information (CuiCallDisplay *self)
{
  const char *number;
  const char *display_name;
  gboolean show_initials;

  g_assert (CUI_IS_CALL_DISPLAY (self));
  g_assert (CUI_IS_CALL (self->call));

  number = cui_call_get_id (self->call);
  if (IS_NULL_OR_EMPTY (number))
    number = _("Unknown");

  display_name = cui_call_get_display_name (self->call);
  if (IS_NULL_OR_EMPTY (display_name) == FALSE &&
      g_strcmp0 (number, display_name) != 0) {
    show_initials = TRUE;

    gtk_label_set_label (self->primary_contact_info, display_name);
    gtk_label_set_label (self->secondary_contact_info, number);
  } else {
    show_initials = FALSE;

    gtk_label_set_label (self->primary_contact_info, number);
    gtk_label_set_label (self->secondary_contact_info, "");
  }

  hdy_avatar_set_text (self->avatar, display_name);
  hdy_avatar_set_show_initials (self->avatar, show_initials);
}


static void
on_time_updated (CuiCallDisplay *self)
{
  CuiCallState state;

  g_assert (CUI_IS_CALL_DISPLAY (self));
  g_assert (CUI_IS_CALL (self->call));

  state = cui_call_get_state (self->call);
  if (state != CUI_CALL_STATE_ACTIVE &&
      state != CUI_CALL_STATE_HELD) {
    g_warning ("Received timer update, but call is not active!");
    return;
  }

  /* We don't want to overwrite the status text if there
   * is an unfinished operation */
  if (!self->update_status_time)
    return;

  set_pretty_time (self);
}


static void
on_dialpad_revealed (CuiCallDisplay *self)
{
  g_assert (CUI_IS_CALL_DISPLAY (self));

  if (gtk_revealer_get_child_revealed (self->dial_pad_revealer))
    gtk_widget_grab_focus (GTK_WIDGET (self->keypad_entry));
}


static void
reset_ui (CuiCallDisplay *self)
{
  g_assert (CUI_IS_CALL_DISPLAY (self));

  g_debug ("Resetting UI");

  self->update_status_time = TRUE;
  hdy_avatar_set_loadable_icon (self->avatar, NULL);
  hdy_avatar_set_text (self->avatar, "");
  hdy_avatar_set_size (self->avatar, HDY_AVATAR_SIZE_DEFAULT);
  gtk_label_set_label (self->primary_contact_info, "");
  gtk_label_set_label (self->secondary_contact_info, "");
  gtk_label_set_label (self->status, "");
  gtk_widget_show (GTK_WIDGET (self->answer));
  gtk_widget_show (GTK_WIDGET (self->hang_up));
  gtk_widget_show (GTK_WIDGET (self->controls));
  gtk_widget_show (GTK_WIDGET (self->gsm_controls));
  gtk_widget_set_sensitive (GTK_WIDGET (self->answer), TRUE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->hang_up), TRUE);
}


static void
on_call_unrefed (CuiCallDisplay *self,
                 CuiCall        *call)
{
  g_assert (CUI_IS_CALL_DISPLAY (self));

  g_debug ("Dropping call %p", call);

  self->call = NULL;
  self->dtmf_bind = NULL;
  self->avatar_icon_bind = NULL;
  self->encryption_bind = NULL;
  reset_ui (self);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CALL]);
}


static void
cui_call_display_get_property (GObject    *object,
                               guint       property_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  CuiCallDisplay *self = CUI_CALL_DISPLAY (object);

  switch (property_id) {
  case PROP_CALL:
    g_value_set_object (value, self->call);
    break;
  case PROP_ALLOW_ADD_CALL:
    g_value_set_boolean (value, self->allow_add_call);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
cui_call_display_set_property (GObject      *object,
                               guint         property_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  CuiCallDisplay *self = CUI_CALL_DISPLAY (object);

  switch (property_id) {
  case PROP_CALL:
    cui_call_display_set_call (self, g_value_get_object (value));
    break;
  case PROP_ALLOW_ADD_CALL:
    cui_call_display_set_allow_add_call (self, g_value_get_boolean (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
cui_call_display_constructed (GObject *object)
{
  CuiCallDisplay *self = CUI_CALL_DISPLAY (object);

  G_OBJECT_CLASS (cui_call_display_parent_class)->constructed (object);

  g_signal_connect_swapped (self->dial_pad_revealer,
                            "notify::child-revealed",
                            G_CALLBACK (on_dialpad_revealed),
                            self);
}


static void
block_delete_cb (GtkWidget *widget)
{
  g_signal_stop_emission_by_name (widget, "delete-text");
}


static void
insert_text_cb (GtkEditable    *editable,
                gchar          *text,
                gint            length,
                gint           *position,
                CuiCallDisplay *self)
{
  gint end_pos = -1;

  cui_call_send_dtmf (self->call, text);

  // Make sure that new chars are inserted at the end of the input
  *position = end_pos;
  g_signal_handlers_block_by_func (editable,
                                   (gpointer) insert_text_cb, self);
  gtk_editable_insert_text (editable, text, length, &end_pos);
  g_signal_handlers_unblock_by_func (editable,
                                     (gpointer) insert_text_cb, self);

  g_signal_stop_emission_by_name (editable, "insert-text");
}


static void
cui_call_display_dispose (GObject *object)
{
  CuiCallDisplay *self = CUI_CALL_DISPLAY (object);

  if (self->call) {
    g_object_weak_unref (G_OBJECT (self->call), (GWeakNotify) on_call_unrefed, self);
    self->call = NULL;
  }

  g_clear_signal_handler (&self->router_changed_id, self->router);
  g_clear_object (&self->router);

  G_OBJECT_CLASS (cui_call_display_parent_class)->dispose (object);
}


static void
cui_call_display_class_init (CuiCallDisplayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = cui_call_display_get_property;
  object_class->set_property = cui_call_display_set_property;

  object_class->constructed = cui_call_display_constructed;
  object_class->dispose = cui_call_display_dispose;

  /**
   * CuiCallDisplay:call:
   *
   * An opaque handle to a call
   */
  props[PROP_CALL] = g_param_spec_object ("call",
                                          "",
                                          "",
                                          CUI_TYPE_CALL,
                                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                          G_PARAM_EXPLICIT_NOTIFY);

  /**
   * CuiCallDisplay:allow-add-call:
   *
   * Whether starting a second call is offered. A lock screen shows the
   * call without offering a way into the dialer, so it turns this off.
   */
  props[PROP_ALLOW_ADD_CALL] = g_param_spec_boolean ("allow-add-call",
                                                     "",
                                                     "",
                                                     TRUE,
                                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                                     G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/CallUI/ui/cui-call-display.ui");
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, actions);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, actions_revealer);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, speaker_revealer);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, mute_revealer);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, speaker_label);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, keypad_speaker);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, hold);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, box_speaker);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, box_mute);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, add_call);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, answer);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, avatar);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, controls);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, dial_pad);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, dial_pad_revealer);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, encryption_indicator);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, general_controls);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, gsm_controls);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, hang_up);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, keypad_entry);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, mute);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, mute_label);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, primary_contact_info);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, secondary_contact_info);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, speaker);
  gtk_widget_class_bind_template_child (widget_class, CuiCallDisplay, status);
  gtk_widget_class_bind_template_callback (widget_class, add_call_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, block_delete_cb);
  gtk_widget_class_bind_template_callback (widget_class, hide_actions_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, hide_input_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, hide_output_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, keypad_speaker_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, hide_dial_pad_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, hold_toggled_cb);
  gtk_widget_class_bind_template_callback (widget_class, insert_text_cb);
  gtk_widget_class_bind_template_callback (widget_class, mute_toggled_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_answer_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_hang_up_clicked);
  gtk_widget_class_bind_template_callback (widget_class, speaker_toggled_cb);

  gtk_widget_class_set_css_name (widget_class, "cui-call-display");
}



/*
 * Both sheets slide up over the display from the same edge, so only one of
 * them may be open: opening either shuts the other rather than stacking.
 */
static void
sheet_toggled_cb (GtkToggleButton *button,
                  CuiCallDisplay  *self)
{
  GtkToggleButton *sheets[] = { self->dial_pad, self->actions, self->speaker, self->mute };
  guint i;

  if (!gtk_toggle_button_get_active (button))
    return;

  for (i = 0; i < G_N_ELEMENTS (sheets); i++) {
    if (sheets[i] != button)
      gtk_toggle_button_set_active (sheets[i], FALSE);
  }
}


static void
cui_force_css_on_buttons (GtkWidget *hang_up, GtkWidget *answer)
{
  /*
   * Attaches a provider directly to the widgets at USER priority.
   * This is extra insurance if screen-level provider is late or overridden.
   */
  GtkCssProvider *prov = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (prov, "/org/gnome/CallUI/style.css");

  if (hang_up) {
    GtkStyleContext *ctx = gtk_widget_get_style_context (hang_up);
    gtk_style_context_add_provider (ctx,
                                    GTK_STYLE_PROVIDER (prov),
                                    GTK_STYLE_PROVIDER_PRIORITY_USER);
  }
  if (answer) {
    GtkStyleContext *ctx = gtk_widget_get_style_context (answer);
    gtk_style_context_add_provider (ctx,
                                    GTK_STYLE_PROVIDER (prov),
                                    GTK_STYLE_PROVIDER_PRIORITY_USER);
  }
  g_object_unref (prov);
}

static void
cui_call_display_init (CuiCallDisplay *self)
{
  self->allow_add_call = TRUE;
  gtk_widget_init_template (GTK_WIDGET (self));

  self->router = g_object_ref (cui_audio_router_get_default ());
  self->router_changed_id = g_signal_connect_swapped (self->router, "changed",
                                                      G_CALLBACK (on_router_changed), self);
  on_router_changed (self);

  g_signal_connect (self->dial_pad, "toggled", G_CALLBACK (sheet_toggled_cb), self);
  g_signal_connect (self->actions, "toggled", G_CALLBACK (sheet_toggled_cb), self);
  g_signal_connect (self->speaker, "toggled", G_CALLBACK (sheet_toggled_cb), self);
  g_signal_connect (self->mute, "toggled", G_CALLBACK (sheet_toggled_cb), self);

  gtk_widget_set_visible (self->keypad_speaker, TRUE);

  /*
   * Hold and add-call have nothing behind them: CuiCall can accept, hang up
   * and send DTMF, and nothing else. They stay insensitive until the call
   * interface can express them rather than looking live and doing nothing.
   */
  gtk_widget_set_sensitive (GTK_WIDGET (self->hold), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->add_call), FALSE);

  cui_force_css_on_buttons (GTK_WIDGET (self->hang_up),
                            GTK_WIDGET (self->answer));
}

/**
 * cui_call_display_new:
 * @call: The call this #CuiCalLDisplay handles
 *
 * Creates a new #CuiCallDisplay.
 * Returns: the new #CuiCalLDisplay
 */
CuiCallDisplay *
cui_call_display_new (CuiCall *call)
{
  return g_object_new (CUI_TYPE_CALL_DISPLAY,
                       "call", call,
                       NULL);
}


/**
 * cui_call_display_get_call:
 * @self: The call display
 *
 * Returns the current [iface@CuiCall]
 * Returns: (transfer none) (nullable): The current [iface@CuiCall].
 */
/**
 * cui_call_display_set_allow_add_call:
 * @self: a #CuiCallDisplay
 * @allow_add_call: whether a second call may be started
 *
 * Hides the add call button when a second call must not be reachable.
 */
void
cui_call_display_set_allow_add_call (CuiCallDisplay *self, gboolean allow_add_call)
{
  g_return_if_fail (CUI_IS_CALL_DISPLAY (self));

  allow_add_call = !!allow_add_call;

  if (self->allow_add_call == allow_add_call)
    return;

  self->allow_add_call = allow_add_call;
  gtk_widget_set_visible (GTK_WIDGET (self->add_call), allow_add_call);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ALLOW_ADD_CALL]);
}


gboolean
cui_call_display_get_allow_add_call (CuiCallDisplay *self)
{
  g_return_val_if_fail (CUI_IS_CALL_DISPLAY (self), TRUE);

  return self->allow_add_call;
}


CuiCall *
cui_call_display_get_call (CuiCallDisplay *self)
{
  g_return_val_if_fail (CUI_IS_CALL_DISPLAY (self), NULL);

  return self->call;
}

/**
 * cui_call_display_set_call:
 * @self: The call display
 * @call: (nullable): The current call
 *
 * Set a [iface@CuiCall]. The current call will be removed form the display and the
 * new call displayed instead.
 */
void
cui_call_display_set_call (CuiCallDisplay *self, CuiCall *call)
{
  g_return_if_fail (CUI_IS_CALL_DISPLAY (self));
  g_return_if_fail (CUI_IS_CALL (call) || call == NULL);

  if (self->call == call)
    return;

  if (self->call != NULL) {
    g_object_weak_unref (G_OBJECT (self->call), (GWeakNotify) on_call_unrefed, self);
    g_signal_handlers_disconnect_by_data (self->call, self);
    g_clear_pointer (&self->dtmf_bind, g_binding_unbind);
    g_clear_pointer (&self->avatar_icon_bind, g_binding_unbind);
    g_clear_pointer (&self->encryption_bind, g_binding_unbind);
  }

  self->update_status_time = TRUE;

  self->call = call;
  gtk_widget_set_sensitive (GTK_WIDGET (self), !!self->call);

  if (self->call == NULL) {
    reset_ui (self);
    return;
  }

  g_object_weak_ref (G_OBJECT (call),
                     (GWeakNotify) on_call_unrefed,
                     self);

  g_signal_connect_object (call,
                           "notify::display-name",
                           G_CALLBACK (on_update_contact_information),
                           self,
                           G_CONNECT_SWAPPED);
  on_update_contact_information (self);

  g_signal_connect_object (call, "notify::state",
                           G_CALLBACK (on_call_state_changed),
                           self,
                           G_CONNECT_SWAPPED);
  on_call_state_changed (self, NULL, call);

  g_signal_connect_object (call, "notify::active-time",
                           G_CALLBACK (on_time_updated),
                           self,
                           G_CONNECT_SWAPPED);

  self->dtmf_bind = g_object_bind_property (call,
                                            "can-dtmf",
                                            self->dial_pad,
                                            "sensitive",
                                            G_BINDING_SYNC_CREATE);

  self->avatar_icon_bind = g_object_bind_property (call,
                                                   "avatar-icon",
                                                   self->avatar,
                                                   "loadable-icon",
                                                   G_BINDING_SYNC_CREATE);
  self->encryption_bind = g_object_bind_property (call,
                                                  "encrypted",
                                                  self->encryption_indicator,
                                                  "encrypted",
                                                  G_BINDING_SYNC_CREATE);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CALL]);
}
