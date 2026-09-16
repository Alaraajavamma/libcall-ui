/*
 * Copyright (C) 2026 Furi Labs
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define CUI_TYPE_AUDIO_ROUTER (cui_audio_router_get_type ())

G_DECLARE_FINAL_TYPE (CuiAudioRouter, cui_audio_router, CUI, AUDIO_ROUTER, GObject)

typedef struct {
  char     *id;
  gboolean  available;
} CuiAudioRoute;

CuiAudioRouter *cui_audio_router_get_default    (void);
gboolean        cui_audio_router_is_ready       (CuiAudioRouter *self);
GPtrArray      *cui_audio_router_get_outputs    (CuiAudioRouter *self);
GPtrArray      *cui_audio_router_get_inputs     (CuiAudioRouter *self);
const char     *cui_audio_router_get_output     (CuiAudioRouter *self);
const char     *cui_audio_router_get_input      (CuiAudioRouter *self);
gboolean        cui_audio_router_get_mic_muted  (CuiAudioRouter *self);
void            cui_audio_router_set_output     (CuiAudioRouter *self,
                                                 const char     *route_id);
void            cui_audio_router_set_input      (CuiAudioRouter *self,
                                                 const char     *route_id);
void            cui_audio_router_set_mic_muted  (CuiAudioRouter *self,
                                                 gboolean        muted);
void            cui_audio_router_refresh        (CuiAudioRouter *self);

const char     *cui_audio_router_output_label   (const char *route_id);
const char     *cui_audio_router_output_icon    (const char *route_id);
const char     *cui_audio_router_input_label    (const char *route_id);
const char     *cui_audio_router_input_icon     (const char *route_id);

G_END_DECLS
