/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2024 7th Generation Networks
 */

#include <config.h>
#include <stdio.h>
#include <string.h>

#include "mm-broadband-bearer-quectel.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-log.h"

G_DEFINE_TYPE (MMBroadbandBearerQuectel, mm_broadband_bearer_quectel, MM_TYPE_BROADBAND_BEARER)

/*****************************************************************************/
/* 3GPP Dialing (sub-step of the 3GPP Connection sequence)                   */

typedef struct {
    MMBroadbandModem *modem;
    MMPortSerialAt   *primary;
    guint             cid;
    MMPort           *data;
} DialContext;

static void
dial_context_free (DialContext *ctx)
{
    g_object_unref (ctx->modem);
    g_object_unref (ctx->primary);
    g_clear_object (&ctx->data);
    g_slice_free (DialContext, ctx);
}

static MMPort *
dial_3gpp_finish (MMBroadbandBearer  *self,
                  GAsyncResult       *res,
                  GError            **error)
{
    return MM_PORT (g_task_propagate_pointer (G_TASK (res), error));
}

static void
qnetdevctl_activate_ready (MMBaseModem  *modem,
                           GAsyncResult *res,
                           GTask        *task)
{
    DialContext *ctx;
    GError      *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_task_return_pointer (task, g_object_ref (ctx->data), g_object_unref);
    g_object_unref (task);
}

static void
cgact_activate_ready (MMBaseModem  *modem,
                      GAsyncResult *res,
                      GTask        *task)
{
    DialContext      *ctx;
    GError           *error = NULL;
    g_autofree gchar *cmd = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* AT+QNETDEVCTL=<enable>,<contextID>,<access_mode>
     * enable=1, access_mode=1 for ECM */
    cmd = g_strdup_printf ("+QNETDEVCTL=1,%u,1", ctx->cid);
    mm_base_modem_at_command (modem,
                              cmd,
                              MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                              FALSE,
                              (GAsyncReadyCallback) qnetdevctl_activate_ready,
                              task);
}

static void
do_cgact_activate (GTask *task)
{
    DialContext      *ctx;
    g_autofree gchar *cmd = NULL;

    ctx = g_task_get_task_data (task);
    cmd = g_strdup_printf ("+CGACT=1,%u", ctx->cid);
    mm_base_modem_at_command (MM_BASE_MODEM (ctx->modem),
                              cmd,
                              MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                              FALSE,
                              (GAsyncReadyCallback) cgact_activate_ready,
                              task);
}


/* Returns the first active CID found in a +CGACT? response that is not skip_cid, or 0 if none. */
static guint
parse_cgact_active_cid (const gchar *response, guint skip_cid)
{
    gchar **lines;
    guint   i;
    guint   active_cid = 0;

    lines = g_strsplit (response, "\n", -1);
    for (i = 0; lines[i] && !active_cid; i++) {
        gchar *line = g_strstrip (lines[i]);
        guint  cid, state;
        if (sscanf (line, "+CGACT: %u,%u", &cid, &state) == 2) {
            if (state == 1 && cid != skip_cid)
                active_cid = cid;
        }
    }
    g_strfreev (lines);
    return active_cid;
}

static void
cgact_query_ready (MMBaseModem  *modem,
                   GAsyncResult *res,
                   GTask        *task)
{
    MMBroadbandBearerQuectel *self;
    DialContext              *ctx;
    const gchar              *response;
    GError                   *error = NULL;
    guint                     active_cid;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        mm_obj_dbg (self, "AT+CGACT? failed, proceeding anyway: %s", error->message);
        g_clear_error (&error);
        do_cgact_activate (task);
        return;
    }

    active_cid = parse_cgact_active_cid (response, ctx->cid);
    if (active_cid) {
        /* The modem won't deactivate an already-active context on the same APN.
         * Switch to that CID instead — AT+CGACT=1,<cid> is idempotent when
         * the context is already active, and QNETDEVCTL works with any active CID. */
        mm_obj_dbg (self, "CID %u already active, switching from CID %u",
                    active_cid, ctx->cid);
        ctx->cid = active_cid;
    }

    /* Record the CID we will actually use so get_ip_config_3gpp can query the
     * right context — the base class passes its original CID, which may differ. */
    MM_BROADBAND_BEARER_QUECTEL (self)->connected_cid = ctx->cid;

    do_cgact_activate (task);
}

static void
dial_3gpp (MMBroadbandBearer  *self,
           MMBaseModem        *modem,
           MMPortSerialAt     *primary,
           guint               cid,
           GCancellable       *cancellable,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
    DialContext *ctx;
    GTask       *task;

    ctx          = g_slice_new0 (DialContext);
    ctx->modem   = g_object_ref (MM_BROADBAND_MODEM (modem));
    ctx->primary = g_object_ref (primary);
    ctx->cid     = cid;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) dial_context_free);

    ctx->data = mm_base_modem_get_best_data_port (modem, MM_PORT_TYPE_NET);
    if (!ctx->data) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_NOT_FOUND,
                                 "No valid net data port found to launch connection");
        g_object_unref (task);
        return;
    }

    /* Query active contexts first; deactivate any conflicting one before activating ours.
     * The modem rejects AT+CGACT=1,<cid> if another context is already active on the same APN. */
    mm_base_modem_at_command (modem,
                              "+CGACT?",
                              10,
                              FALSE,
                              (GAsyncReadyCallback) cgact_query_ready,
                              task);
}

/*****************************************************************************/
/* 3GPP Disconnect sequence                                                  */

typedef struct {
    MMBroadbandModem *modem;
    guint             cid;
} DisconnectContext;

static void
disconnect_context_free (DisconnectContext *ctx)
{
    g_object_unref (ctx->modem);
    g_slice_free (DisconnectContext, ctx);
}

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer  *self,
                        GAsyncResult       *res,
                        GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
cgact_deactivate_ready (MMBaseModem  *modem,
                        GAsyncResult *res,
                        GTask        *task)
{
    GError *error = NULL;

    /* Non-fatal: log and succeed regardless */
    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        mm_obj_dbg (g_task_get_source_object (task),
                    "AT+CGACT=0 failed (ignored): %s", error->message);
        g_clear_error (&error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
qnetdevctl_deactivate_ready (MMBaseModem  *modem,
                             GAsyncResult *res,
                             GTask        *task)
{
    DisconnectContext *ctx;
    GError            *error = NULL;
    g_autofree gchar  *cmd = NULL;

    ctx = g_task_get_task_data (task);

    /* Non-fatal: log and continue to CGACT=0 */
    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        mm_obj_dbg (g_task_get_source_object (task),
                    "AT+QNETDEVCTL=0 failed (ignored): %s", error->message);
        g_clear_error (&error);
    }

    cmd = g_strdup_printf ("+CGACT=0,%u", ctx->cid);
    mm_base_modem_at_command (modem,
                              cmd,
                              MM_BASE_BEARER_DEFAULT_DISCONNECTION_TIMEOUT,
                              FALSE,
                              (GAsyncReadyCallback) cgact_deactivate_ready,
                              task);
}

static void
disconnect_3gpp (MMBroadbandBearer  *self,
                 MMBroadbandModem   *modem,
                 MMPortSerialAt     *primary,
                 MMPortSerialAt     *secondary,
                 MMPort             *data,
                 guint               cid,
                 GAsyncReadyCallback callback,
                 gpointer            user_data)
{
    DisconnectContext *ctx;
    GTask             *task;
    g_autofree gchar  *cmd = NULL;

    ctx        = g_slice_new0 (DisconnectContext);
    ctx->modem = g_object_ref (modem);
    ctx->cid   = cid;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) disconnect_context_free);

    cmd = g_strdup_printf ("+QNETDEVCTL=0,%u,1", cid);
    mm_base_modem_at_command (MM_BASE_MODEM (modem),
                              cmd,
                              MM_BASE_BEARER_DEFAULT_DISCONNECTION_TIMEOUT,
                              FALSE,
                              (GAsyncReadyCallback) qnetdevctl_deactivate_ready,
                              task);
}

/*****************************************************************************/
/* 3GPP IP config (sub-step of the 3GPP Connection sequence)                 */

typedef struct {
    MMPort           *data;
    MMBearerIpConfig *ip_config;
} IpConfigContext;

static void
ip_config_context_free (IpConfigContext *ctx)
{
    g_clear_object (&ctx->data);
    g_clear_object (&ctx->ip_config);
    g_slice_free (IpConfigContext, ctx);
}

static gboolean
get_ip_config_3gpp_finish (MMBroadbandBearer  *self,
                           GAsyncResult       *res,
                           MMBearerIpConfig  **ipv4_config,
                           MMBearerIpConfig  **ipv6_config,
                           GError            **error)
{
    MMBearerConnectResult *configs;
    MMBearerIpConfig      *ipv4;

    configs = g_task_propagate_pointer (G_TASK (res), error);
    if (!configs)
        return FALSE;

    ipv4 = mm_bearer_connect_result_peek_ipv4_config (configs);
    g_assert (ipv4);
    if (ipv4_config)
        *ipv4_config = g_object_ref (ipv4);
    if (ipv6_config)
        *ipv6_config = NULL;
    mm_bearer_connect_result_unref (configs);
    return TRUE;
}

/*
 * Parse the IPv4 MTU from a +CGCONTRDP response.
 *
 * 3GPP TS 27.007 defines two response formats:
 *   new (>= v9.4.0): cid,bid,apn,"ip.subnet",gw,dns1,dns2,pcscf1,pcscf2,im_cn,lipa,mtu,...
 *   old (< v9.4.0):  cid,bid,apn,"ip","subnet",gw,dns1,dns2,pcscf1,pcscf2,im_cn,lipa,mtu,...
 *
 * The new format has the subnet embedded in field 3 (e.g. "10.0.0.1.255.255.255.0"),
 * so MTU lands at comma-index 11. The old format uses a separate subnet field, pushing
 * MTU to comma-index 12. We detect the format by looking for a dot-separated quad inside
 * the ip+subnet field — if it contains more than 4 octets it is the new format.
 *
 * Returns 0 if the MTU could not be parsed.
 */
static guint
parse_cgcontrdp_mtu (const gchar *response)
{
    const gchar  *p;
    gchar       **fields;
    guint         n_fields;
    guint         mtu = 0;

    p = response;
    if (g_str_has_prefix (p, "+CGCONTRDP:"))
        p += strlen ("+CGCONTRDP:");
    while (*p == ' ')
        p++;

    fields   = g_strsplit (p, ",", -1);
    n_fields = g_strv_length (fields);

    /*
     * 3GPP TS 27.007 new format (>= v9.4.0):
     *   cid, bid, apn, local_addr[+subnet], gw, dns1, dns2,
     *   pcscf1, pcscf2, im_cn, lipa, IPv4_MTU, ...
     *   → MTU at index 11
     *
     * Old format (< v9.4.0) has subnet in a separate field 4,
     * shifting MTU to index 12.
     *
     * Rather than guessing the format from field[3] content
     * (unreliable when modems omit the subnet), try index 11
     * first and fall back to 12.
     */
    {
        static const guint candidates[] = { 11, 12 };
        guint i;

        for (i = 0; i < G_N_ELEMENTS (candidates); i++) {
            guint    idx = candidates[i];
            guint64  val;
            gchar   *end = NULL;

            if (n_fields <= idx)
                break;

            val = g_ascii_strtoull (fields[idx], &end, 10);
            if (end != fields[idx] && val > 500 && val <= 65535) {
                mtu = (guint) val;
                break;
            }
        }
    }

    g_strfreev (fields);
    return mtu;
}

static void
cgcontrdp_ready (MMBaseModem  *modem,
                 GAsyncResult *res,
                 GTask        *task)
{
    MMBroadbandBearerQuectel *self;
    IpConfigContext          *ctx;
    const gchar              *response;
    GError                   *error = NULL;
    guint                     mtu;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        mm_obj_dbg (self, "AT+CGCONTRDP failed, using DHCP without MTU: %s", error->message);
        g_clear_error (&error);
        goto out_dhcp;
    }

    mtu = parse_cgcontrdp_mtu (response);
    if (mtu) {
        mm_obj_dbg (self, "IPv4 MTU from +CGCONTRDP: %u", mtu);
        mm_bearer_ip_config_set_mtu (ctx->ip_config, mtu);
    } else {
        mm_obj_dbg (self, "no MTU in +CGCONTRDP response, using DHCP without MTU");
    }

out_dhcp:
    g_task_return_pointer (task,
                           mm_bearer_connect_result_new (ctx->data, ctx->ip_config, NULL),
                           (GDestroyNotify) mm_bearer_connect_result_unref);
    g_object_unref (task);
}

static void
get_ip_config_3gpp (MMBroadbandBearer   *self,
                    MMBroadbandModem    *modem,
                    MMPortSerialAt      *primary,
                    MMPortSerialAt      *secondary,
                    MMPort              *data,
                    guint                cid,
                    MMBearerIpFamily     ip_family,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    IpConfigContext  *ctx;
    GTask            *task;
    g_autofree gchar *cmd = NULL;

    ctx             = g_slice_new0 (IpConfigContext);
    ctx->data       = g_object_ref (data);
    ctx->ip_config  = mm_bearer_ip_config_new ();
    mm_bearer_ip_config_set_method (ctx->ip_config, MM_BEARER_IP_METHOD_DHCP);

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) ip_config_context_free);

    /* Use the CID we actually activated, which may differ from what the base
     * class selected if we switched to an already-active context. */
    if (MM_BROADBAND_BEARER_QUECTEL (self)->connected_cid)
        cid = MM_BROADBAND_BEARER_QUECTEL (self)->connected_cid;

    cmd = g_strdup_printf ("+CGCONTRDP=%u", cid);
    mm_base_modem_at_command (MM_BASE_MODEM (modem),
                              cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback) cgcontrdp_ready,
                              task);
}

/*****************************************************************************/

MMBaseBearer *
mm_broadband_bearer_quectel_new_finish (GAsyncResult  *res,
                                        GError       **error)
{
    GObject *bearer;
    GObject *source;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!bearer)
        return NULL;

    mm_base_bearer_export (MM_BASE_BEARER (bearer));
    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_quectel_new (MMBroadbandModem    *modem,
                                 MMBearerProperties  *config,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_QUECTEL,
        G_PRIORITY_DEFAULT,
        cancellable,
        callback,
        user_data,
        MM_BASE_BEARER_MODEM,  modem,
        MM_BASE_BEARER_CONFIG, config,
        NULL);
}

static void
mm_broadband_bearer_quectel_init (MMBroadbandBearerQuectel *self)
{
}

static void
mm_broadband_bearer_quectel_class_init (MMBroadbandBearerQuectelClass *klass)
{
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    broadband_bearer_class->dial_3gpp              = dial_3gpp;
    broadband_bearer_class->dial_3gpp_finish        = dial_3gpp_finish;
    broadband_bearer_class->get_ip_config_3gpp      = get_ip_config_3gpp;
    broadband_bearer_class->get_ip_config_3gpp_finish = get_ip_config_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp         = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish  = disconnect_3gpp_finish;
}
