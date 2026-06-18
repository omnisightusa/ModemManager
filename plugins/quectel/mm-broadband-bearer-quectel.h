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

#ifndef MM_BROADBAND_BEARER_QUECTEL_H
#define MM_BROADBAND_BEARER_QUECTEL_H

#include "mm-broadband-bearer.h"
#include "mm-broadband-modem.h"

#define MM_TYPE_BROADBAND_BEARER_QUECTEL            (mm_broadband_bearer_quectel_get_type ())
#define MM_BROADBAND_BEARER_QUECTEL(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_BEARER_QUECTEL, MMBroadbandBearerQuectel))
#define MM_BROADBAND_BEARER_QUECTEL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_BEARER_QUECTEL, MMBroadbandBearerQuectelClass))
#define MM_IS_BROADBAND_BEARER_QUECTEL(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_BEARER_QUECTEL))
#define MM_IS_BROADBAND_BEARER_QUECTEL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_BEARER_QUECTEL))
#define MM_BROADBAND_BEARER_QUECTEL_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_BEARER_QUECTEL, MMBroadbandBearerQuectelClass))

typedef struct _MMBroadbandBearerQuectel      MMBroadbandBearerQuectel;
typedef struct _MMBroadbandBearerQuectelClass MMBroadbandBearerQuectelClass;

struct _MMBroadbandBearerQuectel {
    MMBroadbandBearer parent;
    guint             connected_cid; /* actual CID used, may differ from base class selection */
};

struct _MMBroadbandBearerQuectelClass {
    MMBroadbandBearerClass parent;
};

GType mm_broadband_bearer_quectel_get_type (void);

void          mm_broadband_bearer_quectel_new        (MMBroadbandModem    *modem,
                                                      MMBearerProperties  *properties,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);
MMBaseBearer *mm_broadband_bearer_quectel_new_finish (GAsyncResult  *res,
                                                      GError       **error);

#endif /* MM_BROADBAND_BEARER_QUECTEL_H */
