/*
 * Copyright (C) 2026 Furi Labs
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define CUI_TYPE_CALL_ROSTER (cui_call_roster_get_type ())

G_DECLARE_FINAL_TYPE (CuiCallRoster, cui_call_roster, CUI, CALL_ROSTER, GObject)

typedef struct {
  char     *path;
  char     *number;
  char     *state;
  gboolean  multiparty;
} CuiRosterCall;

CuiCallRoster *cui_call_roster_get_default (void);
GPtrArray     *cui_call_roster_get_calls   (CuiCallRoster *self);
guint          cui_call_roster_get_count   (CuiCallRoster *self);
void           cui_call_roster_swap        (CuiCallRoster *self);
void           cui_call_roster_hang_up     (CuiCallRoster *self,
                                            const char    *path);
void           cui_call_roster_refresh     (CuiCallRoster *self);

const char    *cui_call_roster_state_label (const char *state);

G_END_DECLS
