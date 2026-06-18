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
 * Copyright (C) 2018-2020 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include "mm-broadband-modem-quectel.h"
#include "mm-broadband-bearer-quectel.h"
#include "mm-log-object.h"
#include "mm-iface-modem-firmware.h"
#include "mm-iface-modem-location.h"
#include "mm-iface-modem-time.h"
#include "mm-shared-quectel.h"
#include "mm-sim-quectel.h"
#include "mm-log.h"

static void iface_modem_init          (MMIfaceModem         *iface);
static void iface_modem_firmware_init (MMIfaceModemFirmware *iface);
static void iface_modem_location_init (MMIfaceModemLocation *iface);
static void iface_modem_time_init     (MMIfaceModemTime     *iface);
static void shared_quectel_init       (MMSharedQuectel      *iface);

static MMIfaceModem         *iface_modem_parent;
static MMIfaceModemLocation *iface_modem_location_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemQuectel, mm_broadband_modem_quectel, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_FIRMWARE, iface_modem_firmware_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_TIME, iface_modem_time_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_QUECTEL, shared_quectel_init))

/*****************************************************************************/

MMBroadbandModemQuectel *
mm_broadband_modem_quectel_new (const gchar  *device,
                                const gchar **drivers,
                                const gchar  *plugin,
                                guint16       vendor_id,
                                guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_QUECTEL,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* ECM bearer via AT+QNETDEVCTL: use net port, not PPP */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_SUPPORTED, TRUE,
                         NULL);
}

static void
mm_broadband_modem_quectel_init (MMBroadbandModemQuectel *self)
{
}

static MMBaseSim *
modem_create_sim_finish (MMIfaceModem  *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    return mm_sim_quectel_new_finish (res, error);
}

static void
modem_create_sim (MMIfaceModem        *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    mm_sim_quectel_new (MM_BASE_MODEM (self),
                      NULL, /* cancellable */
                      callback,
                      user_data);
}

/*****************************************************************************/
/* Create bearer (Modem interface) */

static MMBaseBearer *
modem_create_bearer_finish (MMIfaceModem  *self,
                            GAsyncResult  *res,
                            GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
quectel_bearer_new_ready (GObject      *source,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMBaseBearer *bearer;
    GError       *error = NULL;

    bearer = mm_broadband_bearer_quectel_new_finish (res, &error);
    if (!bearer)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

static void
modem_create_bearer (MMIfaceModem        *self,
                     MMBearerProperties  *properties,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    mm_obj_dbg (self, "creating Quectel ECM bearer...");
    mm_broadband_bearer_quectel_new (MM_BROADBAND_MODEM (self),
                                     properties,
                                     NULL, /* cancellable */
                                     (GAsyncReadyCallback) quectel_bearer_new_ready,
                                     task);
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);

    iface->create_sim = modem_create_sim;
    iface->create_sim_finish = modem_create_sim_finish;
    iface->create_bearer = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;
    iface->setup_sim_hot_swap = mm_shared_quectel_setup_sim_hot_swap;
    iface->setup_sim_hot_swap_finish = mm_shared_quectel_setup_sim_hot_swap_finish;
    iface->cleanup_sim_hot_swap = mm_shared_quectel_cleanup_sim_hot_swap;
}

static MMIfaceModem *
peek_parent_modem_interface (MMSharedQuectel *self)
{
    return iface_modem_parent;
}

static MMBroadbandModemClass *
peek_parent_broadband_modem_class (MMSharedQuectel *self)
{
    return MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_quectel_parent_class);
}

static void
iface_modem_firmware_init (MMIfaceModemFirmware *iface)
{
    iface->load_update_settings = mm_shared_quectel_firmware_load_update_settings;
    iface->load_update_settings_finish = mm_shared_quectel_firmware_load_update_settings_finish;
}

static void
iface_modem_location_init (MMIfaceModemLocation *iface)
{
    iface_modem_location_parent = g_type_interface_peek_parent (iface);

    iface->load_capabilities                 = mm_shared_quectel_location_load_capabilities;
    iface->load_capabilities_finish          = mm_shared_quectel_location_load_capabilities_finish;
    iface->enable_location_gathering         = mm_shared_quectel_enable_location_gathering;
    iface->enable_location_gathering_finish  = mm_shared_quectel_enable_location_gathering_finish;
    iface->disable_location_gathering        = mm_shared_quectel_disable_location_gathering;
    iface->disable_location_gathering_finish = mm_shared_quectel_disable_location_gathering_finish;
}

static MMIfaceModemLocation *
peek_parent_modem_location_interface (MMSharedQuectel *self)
{
    return iface_modem_location_parent;
}

static void
iface_modem_time_init (MMIfaceModemTime *iface)
{
    iface->check_support        = mm_shared_quectel_time_check_support;
    iface->check_support_finish = mm_shared_quectel_time_check_support_finish;
}

static void
shared_quectel_init (MMSharedQuectel *iface)
{
    iface->peek_parent_modem_interface          = peek_parent_modem_interface;
    iface->peek_parent_modem_location_interface = peek_parent_modem_location_interface;
    iface->peek_parent_broadband_modem_class    = peek_parent_broadband_modem_class;
}

static void
mm_broadband_modem_quectel_class_init (MMBroadbandModemQuectelClass *klass)
{
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    broadband_modem_class->setup_ports = mm_shared_quectel_setup_ports;
}
