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
 * Copyright (C) 2014 Aleksander Morgado <aleksander@aleksander.es>
 * Copyright (C) 2016 Trimble Navigation Limited
 * Copyright (C) 2016 Matthew Stanger <matthew_stanger@trimble.com>
 * Copyright (C) 2019 Purism SPC
 */

#include <config.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ModemManager.h"
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>
#include "mm-log-object.h"
#include "mm-charsets.h"
#include "mm-errors-types.h"
#include "mm-modem-helpers-cinterion.h"
#include "mm-modem-helpers.h"

/* Setup relationship between the 3G band bitmask in the modem and the bitmask
 * in ModemManager. */
typedef struct {
    guint32 cinterion_band_flag;
    MMModemBand mm_band;
} CinterionBand;

/* Table checked in PLS8-X/E/J/V/US, HC25 & PHS8 references. The table includes 2/3/4G
 * frequencies. Depending on which one is configured, one access technology or
 * the other will be used. This may conflict with the allowed mode configuration
 * set, so you shouldn't for example set 3G frequency bands, and then use a
 * 2G-only allowed mode. */
static const CinterionBand cinterion_bands[] = {
    { (1 << 0), MM_MODEM_BAND_EGSM       },
    { (1 << 1), MM_MODEM_BAND_DCS        },
    { (1 << 2), MM_MODEM_BAND_G850       },
    { (1 << 3), MM_MODEM_BAND_PCS        },
    { (1 << 4), MM_MODEM_BAND_UTRAN_1    },
    { (1 << 5), MM_MODEM_BAND_UTRAN_2    },
    { (1 << 6), MM_MODEM_BAND_UTRAN_5    },
    { (1 << 7), MM_MODEM_BAND_UTRAN_8    },
    { (1 << 8), MM_MODEM_BAND_UTRAN_6    },
    { (1 << 9), MM_MODEM_BAND_UTRAN_4    },
    { (1 << 10), MM_MODEM_BAND_UTRAN_19  },
    { (1 << 12), MM_MODEM_BAND_UTRAN_3   },
    { (1 << 13), MM_MODEM_BAND_EUTRAN_1  },
    { (1 << 14), MM_MODEM_BAND_EUTRAN_2  },
    { (1 << 15), MM_MODEM_BAND_EUTRAN_3  },
    { (1 << 16), MM_MODEM_BAND_EUTRAN_4  },
    { (1 << 17), MM_MODEM_BAND_EUTRAN_5  },
    { (1 << 18), MM_MODEM_BAND_EUTRAN_7  },
    { (1 << 19), MM_MODEM_BAND_EUTRAN_8  },
    { (1 << 20), MM_MODEM_BAND_EUTRAN_17 },
    { (1 << 21), MM_MODEM_BAND_EUTRAN_20 },
    { (1 << 22), MM_MODEM_BAND_EUTRAN_13 },
    { (1 << 24), MM_MODEM_BAND_EUTRAN_19 }
};

/* Check valid combinations in 2G-only devices */
#define VALIDATE_2G_BAND(cinterion_mask) \
    (cinterion_mask == 1  ||             \
     cinterion_mask == 2  ||             \
     cinterion_mask == 4  ||             \
     cinterion_mask == 8  ||             \
     cinterion_mask == 3  ||             \
     cinterion_mask == 5  ||             \
     cinterion_mask == 10 ||             \
     cinterion_mask == 12 ||             \
     cinterion_mask == 15)

/*****************************************************************************/
/* ^SCFG (3G) test parser
 *
 * Example:
 *   AT^SCFG=?
 *     ...
 *     ^SCFG: "MEShutdown/OnIgnition",("on","off")
 *     ^SCFG: "Radio/Band",("1-511","0-1")
 *     ^SCFG: "Radio/NWSM",("0","1","2")
 *     ...
 *
 *     ^SCFG: "Radio/Band\",("1"-"147")
 */

gboolean
mm_cinterion_parse_scfg_test (const gchar *response,
                              MMModemCharset charset,
                              GArray **supported_bands,
                              GError **error)
{
    GRegex *r;
    GMatchInfo *match_info;
    GError *inner_error = NULL;
    GArray *bands = NULL;

    if (!response) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Missing response");
        return FALSE;
    }

    r = g_regex_new ("\\^SCFG:\\s*\"Radio/Band\",\\((?:\")?([0-9]*)(?:\")?-(?:\")?([0-9]*)(?:\")?.*\\)",
                     G_REGEX_DOLLAR_ENDONLY | G_REGEX_RAW,
                     0, NULL);
    g_assert (r != NULL);

    g_regex_match_full (r, response, strlen (response), 0, 0, &match_info, &inner_error);
    if (!inner_error && g_match_info_matches (match_info)) {
        gchar *maxbandstr;
        guint maxband = 0;

        maxbandstr = mm_get_string_unquoted_from_match_info (match_info, 2);
        if (maxbandstr) {
            /* Handle charset conversion if the number is given in UCS2 */
            if (charset != MM_MODEM_CHARSET_UNKNOWN)
                maxbandstr = mm_charset_take_and_convert_to_utf8 (maxbandstr, charset);

            mm_get_uint_from_str (maxbandstr, &maxband);
        }

        if (maxband == 0) {
            inner_error = g_error_new (MM_CORE_ERROR,
                                       MM_CORE_ERROR_FAILED,
                                       "Couldn't parse ^SCFG=? response");
        } else {
            guint i;

            for (i = 0; i < G_N_ELEMENTS (cinterion_bands); i++) {
                if (maxband & cinterion_bands[i].cinterion_band_flag) {
                    if (G_UNLIKELY (!bands))
                        bands = g_array_sized_new (FALSE, FALSE, sizeof (MMModemBand), 9);
                    g_array_append_val (bands, cinterion_bands[i].mm_band);
                }
            }
        }

        g_free (maxbandstr);
    }

    g_match_info_free (match_info);
    g_regex_unref (r);

    if (!bands)
        inner_error = g_error_new (MM_CORE_ERROR,
                                   MM_CORE_ERROR_FAILED,
                                   "No valid bands found in ^SCFG=? response");

    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    g_assert (bands != NULL && bands->len > 0);
    *supported_bands = bands;

    return TRUE;
}

/*****************************************************************************/
/* ^SCFG response parser
 *
 * Example (3G):
 *   AT^SCFG="Radio/Band"
 *     ^SCFG: "Radio/Band",127
 *
 * Example (2G, UCS-2):
 *   AT+SCFG="Radio/Band"
 *     ^SCFG: "Radio/Band","0031","0031"
 *
 * Example (2G):
 *   AT+SCFG="Radio/Band"
 *     ^SCFG: "Radio/Band","3","3"
 */

gboolean
mm_cinterion_parse_scfg_response (const gchar *response,
                                  MMModemCharset charset,
                                  GArray **current_bands,
                                  GError **error)
{
    GRegex *r;
    GMatchInfo *match_info;
    GError *inner_error = NULL;
    GArray *bands = NULL;

    if (!response) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Missing response");
        return FALSE;
    }

    r = g_regex_new ("\\^SCFG:\\s*\"Radio/Band\",\\s*\"?([0-9a-fA-F]*)\"?", 0, 0, NULL);
    g_assert (r != NULL);

    if (g_regex_match (r, response, 0, &match_info)) {
        gchar *currentstr;
        guint current = 0;

        currentstr = mm_get_string_unquoted_from_match_info (match_info, 1);
        if (currentstr) {
            /* Handle charset conversion if the number is given in UCS2 */
            if (charset != MM_MODEM_CHARSET_UNKNOWN)
                currentstr = mm_charset_take_and_convert_to_utf8 (currentstr, charset);

            mm_get_uint_from_str (currentstr, &current);
        }

        if (current == 0) {
            inner_error = g_error_new (MM_CORE_ERROR,
                                       MM_CORE_ERROR_FAILED,
                                       "Couldn't parse ^SCFG response");
        } else {
            guint i;

            for (i = 0; i < G_N_ELEMENTS (cinterion_bands); i++) {
                if (current & cinterion_bands[i].cinterion_band_flag) {
                    if (G_UNLIKELY (!bands))
                        bands = g_array_sized_new (FALSE, FALSE, sizeof (MMModemBand), 9);
                    g_array_append_val (bands, cinterion_bands[i].mm_band);
                }
            }
        }

        g_free (currentstr);
    }

    g_match_info_free (match_info);
    g_regex_unref (r);

    if (!bands)
        inner_error = g_error_new (MM_CORE_ERROR,
                                   MM_CORE_ERROR_FAILED,
                                   "No valid bands found in ^SCFG response");

    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    g_assert (bands != NULL && bands->len > 0);
    *current_bands = bands;

    return TRUE;
}

/*****************************************************************************/
/* +CNMI test parser
 *
 * Example (PHS8):
 *   AT+CNMI=?
 *   +CNMI: (0,1,2),(0,1),(0,2),(0),(1)
 */

gboolean
mm_cinterion_parse_cnmi_test (const gchar *response,
                              GArray **supported_mode,
                              GArray **supported_mt,
                              GArray **supported_bm,
                              GArray **supported_ds,
                              GArray **supported_bfr,
                              GError **error)
{
    GRegex *r;
    GMatchInfo *match_info;
    GError *inner_error = NULL;
    GArray *tmp_supported_mode = NULL;
    GArray *tmp_supported_mt = NULL;
    GArray *tmp_supported_bm = NULL;
    GArray *tmp_supported_ds = NULL;
    GArray *tmp_supported_bfr = NULL;

    if (!response) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Missing response");
        return FALSE;
    }

    r = g_regex_new ("\\+CNMI:\\s*\\((.*)\\),\\((.*)\\),\\((.*)\\),\\((.*)\\),\\((.*)\\)",
                     G_REGEX_DOLLAR_ENDONLY | G_REGEX_RAW,
                     0, NULL);
    g_assert (r != NULL);

    g_regex_match_full (r, response, strlen (response), 0, 0, &match_info, &inner_error);
    if (!inner_error && g_match_info_matches (match_info)) {
        if (supported_mode) {
            gchar *str;

            str = mm_get_string_unquoted_from_match_info (match_info, 1);
            tmp_supported_mode = mm_parse_uint_list (str, &inner_error);
            g_free (str);
            if (inner_error)
                goto out;
        }
        if (supported_mt) {
            gchar *str;

            str = mm_get_string_unquoted_from_match_info (match_info, 2);
            tmp_supported_mt = mm_parse_uint_list (str, &inner_error);
            g_free (str);
            if (inner_error)
                goto out;
        }
        if (supported_bm) {
            gchar *str;

            str = mm_get_string_unquoted_from_match_info (match_info, 3);
            tmp_supported_bm = mm_parse_uint_list (str, &inner_error);
            g_free (str);
            if (inner_error)
                goto out;
        }
        if (supported_ds) {
            gchar *str;

            str = mm_get_string_unquoted_from_match_info (match_info, 4);
            tmp_supported_ds = mm_parse_uint_list (str, &inner_error);
            g_free (str);
            if (inner_error)
                goto out;
        }
        if (supported_bfr) {
            gchar *str;

            str = mm_get_string_unquoted_from_match_info (match_info, 5);
            tmp_supported_bfr = mm_parse_uint_list (str, &inner_error);
            g_free (str);
            if (inner_error)
                goto out;
        }
    }

out:

    g_match_info_free (match_info);
    g_regex_unref (r);

    if (inner_error) {
        g_clear_pointer (&tmp_supported_mode, g_array_unref);
        g_clear_pointer (&tmp_supported_mt,   g_array_unref);
        g_clear_pointer (&tmp_supported_bm,   g_array_unref);
        g_clear_pointer (&tmp_supported_ds,   g_array_unref);
        g_clear_pointer (&tmp_supported_bfr,  g_array_unref);
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    if (supported_mode)
        *supported_mode = tmp_supported_mode;
    if (supported_mt)
        *supported_mt = tmp_supported_mt;
    if (supported_bm)
        *supported_bm = tmp_supported_bm;
    if (supported_ds)
        *supported_ds = tmp_supported_ds;
    if (supported_bfr)
        *supported_bfr = tmp_supported_bfr;

    return TRUE;
}

/*****************************************************************************/
/* Build Cinterion-specific band value */

gboolean
mm_cinterion_build_band (GArray *bands,
                         guint supported,
                         gboolean only_2g,
                         guint *out_band,
                         GError **error)
{
    guint band = 0;

    /* The special case of ANY should be treated separately. */
    if (bands->len == 1 && g_array_index (bands, MMModemBand, 0) == MM_MODEM_BAND_ANY) {
        band = supported;
    } else {
        guint i;

        for (i = 0; i < G_N_ELEMENTS (cinterion_bands); i++) {
            guint j;

            for (j = 0; j < bands->len; j++) {
                if (g_array_index (bands, MMModemBand, j) == cinterion_bands[i].mm_band) {
                    band |= cinterion_bands[i].cinterion_band_flag;
                    break;
                }
            }
        }

        /* 2G-only modems only support a subset of the possible band
         * combinations. Detect it early and error out.
         */
        if (only_2g && !VALIDATE_2G_BAND (band))
            band = 0;
    }

    if (band == 0) {
        gchar *bands_string;

        bands_string = mm_common_build_bands_string ((MMModemBand *)(gpointer)bands->data, bands->len);
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "The given band combination is not supported: '%s'",
                     bands_string);
        g_free (bands_string);
        return FALSE;
    }

    *out_band = band;
    return TRUE;
}

/*****************************************************************************/
/* Single ^SIND response parser */

gboolean
mm_cinterion_parse_sind_response (const gchar *response,
                                  gchar **description,
                                  guint *mode,
                                  guint *value,
                                  GError **error)
{
    GRegex *r;
    GMatchInfo *match_info;
    guint errors = 0;

    if (!response) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Missing response");
        return FALSE;
    }

    r = g_regex_new ("\\^SIND:\\s*(.*),(\\d+),(\\d+)(\\r\\n)?", 0, 0, NULL);
    g_assert (r != NULL);

    if (g_regex_match (r, response, 0, &match_info)) {
        if (description) {
            *description = mm_get_string_unquoted_from_match_info (match_info, 1);
            if (*description == NULL)
                errors++;
        }
        if (mode && !mm_get_uint_from_match_info (match_info, 2, mode))
            errors++;
        if (value && !mm_get_uint_from_match_info (match_info, 3, value))
            errors++;
    } else
        errors++;

    g_match_info_free (match_info);
    g_regex_unref (r);

    if (errors > 0) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Failed parsing ^SIND response");
        return FALSE;
    }

    return TRUE;
}

/*****************************************************************************/
/* ^SWWAN read parser
 *
 * Description: Parses <cid>, <state>[, <WWAN adapter>] or CME ERROR from SWWAN.
 *
 * The method returns a MMSwwanState with the connection status of a single
 * PDP context, the one being queried via the cid given as input.
 *
 * Note that we use CID for matching because the WWAN adapter field is optional
 * it seems.
 *
 *     Read Command
 *         AT^SWWAN?
 *         Response(s)
 *         [^SWWAN: <cid>, <state>[, <WWAN adapter>]]
 *         [^SWWAN: ...]
 *         OK
 *         ERROR
 *         +CME ERROR: <err>
 *
 *     Examples:
 *         OK              - If no WWAN connection is active, then read command just returns OK
 *         ^SWWAN: 3,1,1   - 3rd PDP Context, Activated, First WWAN Adaptor
 *         +CME ERROR: ?   -
 */

enum {
    MM_SWWAN_STATE_DISCONNECTED =  0,
    MM_SWWAN_STATE_CONNECTED    =  1,
};

MMBearerConnectionStatus
mm_cinterion_parse_swwan_response (const gchar  *response,
                                   guint         cid,
                                   gpointer      log_object,
                                   GError      **error)
{
    GRegex                   *r;
    GMatchInfo               *match_info;
    GError                   *inner_error = NULL;
    MMBearerConnectionStatus  status;

    g_assert (response);

    /* If no WWAN connection is active, then ^SWWAN read command just returns OK
     * (which we receive as an empty string) */
    if (!response[0])
        return MM_BEARER_CONNECTION_STATUS_DISCONNECTED;

    if (!g_str_has_prefix (response, "^SWWAN:")) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Couldn't parse ^SWWAN response: '%s'", response);
        return MM_BEARER_CONNECTION_STATUS_UNKNOWN;
    }

    r = g_regex_new ("\\^SWWAN:\\s*(\\d+),\\s*(\\d+)(?:,\\s*(\\d+))?(?:\\r\\n)?",
                     G_REGEX_DOLLAR_ENDONLY | G_REGEX_RAW, 0, NULL);
    g_assert (r != NULL);

    status = MM_BEARER_CONNECTION_STATUS_UNKNOWN;
    g_regex_match_full (r, response, strlen (response), 0, 0, &match_info, &inner_error);
    while (!inner_error && g_match_info_matches (match_info)) {
        guint read_state;
        guint read_cid;

        if (!mm_get_uint_from_match_info (match_info, 1, &read_cid))
            mm_obj_warn (log_object, "couldn't read cid in ^SWWAN response: %s", response);
        else if (!mm_get_uint_from_match_info (match_info, 2, &read_state))
            mm_obj_warn (log_object, "couldn't read state in ^SWWAN response: %s", response);
        else if (read_cid == cid) {
            if (read_state == MM_SWWAN_STATE_CONNECTED) {
                status = MM_BEARER_CONNECTION_STATUS_CONNECTED;
                break;
            }
            if (read_state == MM_SWWAN_STATE_DISCONNECTED) {
                status = MM_BEARER_CONNECTION_STATUS_DISCONNECTED;
                break;
            }
            mm_obj_warn (log_object, "invalid state read in ^SWWAN response: %u", read_state);
            break;
        }
        g_match_info_next (match_info, &inner_error);
    }

    g_match_info_free (match_info);
    g_regex_unref (r);

    if (status == MM_BEARER_CONNECTION_STATUS_UNKNOWN)
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "No state returned for CID %u", cid);

    return status;
}

/*****************************************************************************/
/* ^SMONG response parser */

static MMModemAccessTechnology
get_access_technology_from_smong_gprs_status (guint    gprs_status,
                                              GError **error)
{
    switch (gprs_status) {
    case 0:
        return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    case 1:
    case 2:
        return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
    case 3:
    case 4:
        return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;
    default:
        break;
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_INVALID_ARGS,
                 "Couldn't get network capabilities, "
                 "unsupported GPRS status value: '%u'",
                 gprs_status);
    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

gboolean
mm_cinterion_parse_smong_response (const gchar              *response,
                                   MMModemAccessTechnology  *access_tech,
                                   GError                  **error)
{
    GError     *inner_error = NULL;
    GMatchInfo *match_info = NULL;
    GRegex     *regex;

    /* The AT^SMONG command returns a cell info table, where the second
     * column identifies the "GPRS status", which is exactly what we want.
     * So we'll try to read that second number in the values row.
     *
     * AT^SMONG
     * GPRS Monitor
     * BCCH  G  PBCCH  PAT MCC  MNC  NOM  TA      RAC    # Cell #
     * 0776  1  -      -   214   03  2    00      01
     * OK
     */
    regex = g_regex_new (".*GPRS Monitor(?:\r\n)*"
                         "BCCH\\s*G.*\\r\\n"
                         "\\s*(\\d+)\\s*(\\d+)\\s*",
                         G_REGEX_DOLLAR_ENDONLY | G_REGEX_RAW,
                         0, NULL);
    g_assert (regex);

    if (g_regex_match_full (regex, response, strlen (response), 0, 0, &match_info, &inner_error)) {
        guint value = 0;

        if (!mm_get_uint_from_match_info (match_info, 2, &value))
            inner_error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                       "Couldn't read 'GPRS status' field from AT^SMONG response");
        else if (access_tech)
            *access_tech = get_access_technology_from_smong_gprs_status (value, &inner_error);
    }

    g_match_info_free (match_info);
    g_regex_unref (regex);

    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    g_assert (access_tech != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN);
    return TRUE;
}

/*****************************************************************************/
/* ^SIND psinfo helper */

MMModemAccessTechnology
mm_cinterion_get_access_technology_from_sind_psinfo (guint    val,
                                                     gpointer log_object)
{
    switch (val) {
    case 0:
        return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    case 1:
    case 2:
        return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
    case 3:
    case 4:
        return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;
    case 5:
    case 6:
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    case 7:
    case 8:
        return MM_MODEM_ACCESS_TECHNOLOGY_HSDPA;
    case 9:
    case 10:
        return (MM_MODEM_ACCESS_TECHNOLOGY_HSDPA | MM_MODEM_ACCESS_TECHNOLOGY_HSUPA);
    case 16:
    case 17:
        return MM_MODEM_ACCESS_TECHNOLOGY_LTE;
    default:
        mm_obj_dbg (log_object, "unable to identify access technology from psinfo reported value: %u", val);
        return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    }
}

/*****************************************************************************/
/* ^SLCC psinfo helper */

GRegex *
mm_cinterion_get_slcc_regex (void)
{
    /* The list of active calls displayed with this URC will always be terminated
     * with an empty line preceded by prefix "^SLCC: ", in order to indicate the end
     * of the list.
     */
    return g_regex_new ("\\r\\n(\\^SLCC: .*\\r\\n)*\\^SLCC: \\r\\n",
                        G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}

static void
cinterion_call_info_free (MMCallInfo *info)
{
    if (!info)
        return;
    g_free (info->number);
    g_slice_free (MMCallInfo, info);
}

gboolean
mm_cinterion_parse_slcc_list (const gchar *str,
                              gpointer     log_object,
                              GList      **out_list,
                              GError     **error)
{
    GRegex     *r;
    GList      *list = NULL;
    GError     *inner_error = NULL;
    GMatchInfo *match_info  = NULL;

    static const MMCallDirection cinterion_call_direction[] = {
        [0] = MM_CALL_DIRECTION_OUTGOING,
        [1] = MM_CALL_DIRECTION_INCOMING,
    };

    static const MMCallState cinterion_call_state[] = {
        [0] = MM_CALL_STATE_ACTIVE,
        [1] = MM_CALL_STATE_HELD,
        [2] = MM_CALL_STATE_DIALING,     /* Dialing  (MOC) */
        [3] = MM_CALL_STATE_RINGING_OUT, /* Alerting (MOC) */
        [4] = MM_CALL_STATE_RINGING_IN,  /* Incoming (MTC) */
        [5] = MM_CALL_STATE_WAITING,     /* Waiting  (MTC) */
    };

    g_assert (out_list);

    /*
     *         1      2      3       4       5       6            7         8     9
     *  ^SLCC: <idx>, <dir>, <stat>, <mode>, <mpty>, <Reserved>[, <number>, <type>[,<alpha>]]
     *  [^SLCC: <idx>, <dir>, <stat>, <mode>, <mpty>, <Reserved>[, <number>, <type>[,<alpha>]]]
     *  [... ]
     *  ^SLCC :
     */

    r = g_regex_new ("\\^SLCC:\\s*(\\d+),\\s*(\\d+),\\s*(\\d+),\\s*(\\d+),\\s*(\\d+),\\s*(\\d+)" /* mandatory fields */
                     "(?:,\\s*([^,]*),\\s*(\\d+)"                                                /* number and type */
                     "(?:,\\s*([^,]*)"                                                           /* alpha */
                     ")?)?$",
                     G_REGEX_RAW | G_REGEX_MULTILINE | G_REGEX_NEWLINE_CRLF,
                     G_REGEX_MATCH_NEWLINE_CRLF,
                     NULL);
    g_assert (r != NULL);

    g_regex_match_full (r, str, strlen (str), 0, 0, &match_info, &inner_error);
    if (inner_error)
        goto out;

    /* Parse the results */
    while (g_match_info_matches (match_info)) {
        MMCallInfo *call_info;
        guint       aux;

        call_info = g_slice_new0 (MMCallInfo);

        if (!mm_get_uint_from_match_info (match_info, 1, &call_info->index)) {
            mm_obj_warn (log_object, "couldn't parse call index from ^SLCC line");
            goto next;
        }

        if (!mm_get_uint_from_match_info (match_info, 2, &aux) ||
            (aux >= G_N_ELEMENTS (cinterion_call_direction))) {
            mm_obj_warn (log_object, "couldn't parse call direction from ^SLCC line");
            goto next;
        }
        call_info->direction = cinterion_call_direction[aux];

        if (!mm_get_uint_from_match_info (match_info, 3, &aux) ||
            (aux >= G_N_ELEMENTS (cinterion_call_state))) {
            mm_obj_warn (log_object, "couldn't parse call state from ^SLCC line");
            goto next;
        }
        call_info->state = cinterion_call_state[aux];

        if (g_match_info_get_match_count (match_info) >= 8)
            call_info->number = mm_get_string_unquoted_from_match_info (match_info, 7);

        list = g_list_append (list, call_info);
        call_info = NULL;

    next:
        cinterion_call_info_free (call_info);
        g_match_info_next (match_info, NULL);
    }

out:
    g_clear_pointer (&match_info, g_match_info_free);
    g_regex_unref (r);

    if (inner_error) {
        mm_cinterion_call_info_list_free (list);
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    *out_list = list;

    return TRUE;
}

void
mm_cinterion_call_info_list_free (GList *call_info_list)
{
    g_list_free_full (call_info_list, (GDestroyNotify) cinterion_call_info_free);
}

/*****************************************************************************/
/* +CTZU URC helpers */

GRegex *
mm_cinterion_get_ctzu_regex (void)
{
    /*
     * From PLS-8 AT command spec:
     *  +CTZU:<nitzUT>, <nitzTZ>[, <nitzDST>]
     * E.g.:
     *  +CTZU: "19/07/09,10:19:15",+08,1
     */

    return g_regex_new ("\\r\\n\\+CTZU:\\s*\"(\\d+)\\/(\\d+)\\/(\\d+),(\\d+):(\\d+):(\\d+)\",([\\-\\+\\d]+)(?:,(\\d+))?(?:\\r\\n)?",
                        G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}

gboolean
mm_cinterion_parse_ctzu_urc (GMatchInfo         *match_info,
                             gchar             **iso8601p,
                             MMNetworkTimezone **tzp,
                             GError            **error)
{
    guint year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, dst = 0;
    gint tz = 0;

    if (!mm_get_uint_from_match_info (match_info, 1, &year)   ||
        !mm_get_uint_from_match_info (match_info, 2, &month)  ||
        !mm_get_uint_from_match_info (match_info, 3, &day)    ||
        !mm_get_uint_from_match_info (match_info, 4, &hour)   ||
        !mm_get_uint_from_match_info (match_info, 5, &minute) ||
        !mm_get_uint_from_match_info (match_info, 6, &second) ||
        !mm_get_int_from_match_info  (match_info, 7, &tz)) {
        g_set_error_literal (error,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Failed to parse +CTZU URC");
        return FALSE;
    }

    /* adjust year */
    if (year < 100)
        year += 2000;

    /*
     * tz = timezone offset in 15 minute intervals
     */
    if (iso8601p) {
        /* Return ISO-8601 format date/time string */
        *iso8601p = mm_new_iso8601_time (year, month, day, hour,
                                         minute, second,
                                         TRUE, tz * 15);
    }

    if (tzp) {
        *tzp = mm_network_timezone_new ();
        mm_network_timezone_set_offset (*tzp, tz * 15);
    }

    /* dst flag is optional in the URC
    *
     * tz = timezone offset in 15 minute intervals
     * dst = daylight adjustment, 0 = none, 1 = 1 hour, 2 = 2 hours
     */
    if (tzp && mm_get_uint_from_match_info (match_info, 8, &dst))
        mm_network_timezone_set_dst_offset (*tzp, dst * 60);

    return TRUE;
}

/*****************************************************************************/
/* ^SMONI response parser */

gboolean
mm_cinterion_parse_smoni_query_response (const gchar           *response,
                                         MMCinterionSmoniTech  *out_tech,
                                         gdouble               *out_rssi,
                                         gdouble               *out_ecn0,
                                         gdouble               *out_rscp,
                                         gdouble               *out_rsrp,
                                         gdouble               *out_rsrq,
                                         GError               **error)
{
    g_autoptr(GRegex)        r = NULL;
    g_autoptr(GRegex)        pre = NULL;
    g_autoptr(GMatchInfo)    match_info = NULL;
    g_autoptr(GMatchInfo)    match_info_pre = NULL;
    GError                  *inner_error = NULL;
    MMCinterionSmoniTech     tech = MM_CINTERION_SMONI_NO_TECH;
    gdouble                  rssi = -G_MAXDOUBLE;
    gdouble                  ecn0 = -G_MAXDOUBLE;
    gdouble                  rscp = -G_MAXDOUBLE;
    gdouble                  rsrq = -G_MAXDOUBLE;
    gdouble                  rsrp = -G_MAXDOUBLE;
    gboolean                 success = FALSE;

    g_assert (out_tech);
    g_assert (out_rssi);
    g_assert (out_ecn0);
    g_assert (out_rscp);
    g_assert (out_rsrp);
    g_assert (out_rsrq);
    g_assert (out_rssi);

    /* Possible Responses:
     * 2G
     * ^SMONI: 2G,ARFCN,BCCH,MCC,MNC,LAC,cell,C1,C2,NCC,BCC,GPRS,Conn_state                                     // registered
     * ^SMONI: 2G,ARFCN,BCCH,MCC,MNC,LAC,cell,C1,C2,NCC,BCC,GPRS,ARFCN,TS,timAdv,dBm,Q,ChMod                    // searching
     * ^SMONI: 2G,ARFCN,BCCH,MCC,MNC,LAC,cell,C1,C2,NCC,BCC,GPRS,PWR,RXLev,ARFCN,TS,timAdv,dBm,Q,ChMod          // limsrv
     * ^SMONI: 2G,ARFCN,BCCH,MCC,MNC,LAC,cell,C1,C2,NCC,BCC,GPRS,ARFCN,TS,timAdv,dBm,Q,ChMod                    // dedicated channel
     *
     * ^SMONI: 2G,71,-61,262,02,0143,83BA,33,33,3,6,G,NOCONN
     *               ^^^
     * ^SMONI: 2G,SEARCH,SEARCH
     * ^SMONI: 2G,673,-89,262,07,4EED,A500,16,16,7,4,G,5,-107,LIMSRV
     *                ^^^                                ^^^^ RXLev dBm
     * ^SMONI: 2G,673,-80,262,07,4EED,A500,35,35,7,4,G,643,4,0,-80,0,S_FR
     *                ^^^                                      ^^^ dBm: Receiving level of the traffic channel carrier in dBm
     *  BCCH: Receiving level of the BCCH carrier in dBm (level is limited from -110dBm to -47dBm)
     *   -> rssi for 2G, directly without mm_3gpp_rxlev_to_rssi
     *
     *
     * 3G
     * ^SMONI: 3G,UARFCN,PSC,EC/n0,RSCP,MCC,MNC,LAC,cell,SQual,SRxLev,,Conn_state",
     * ^SMONI: 3G,UARFCN,PSC,EC/n0,RSCP,MCC,MNC,LAC,cell,SQual,SRxLev,PhysCh, SF,Slot,EC/n0,RSCP,ComMod,HSUPA,HSDPA",
     * ^SMONI: 3G,UARFCN,PSC,EC/n0,RSCP,MCC,MNC,LAC,cell,SQual,SRxLev,PhysCh, SF,Slot,EC/n0,RSCP,ComMod,HSUPA,HSDPA",
     * ^SMONI: 3G,UARFCN,PSC,EC/n0,RSCP,MCC,MNC,LAC,cell,SQual,SRxLev,PhysCh, SF,Slot,EC/n0,RSCP,ComMod,HSUPA,HSDPA",
     *
     * ^SMONI: 3G,10564,296,-7.5,-79,262,02,0143,00228FF,-92,-78,NOCONN
     *                      ^^^^ ^^^
     * ^SMONI: 3G,SEARCH,SEARCH
     * ^SMONI: 3G,10564,96,-7.5,-79,262,02,0143,00228FF,-92,-78,LIMSRV
     *                     ^^^^ ^^^
     * ^SMONI: 3G,10737,131,-5,-93,260,01,7D3D,C80BC9A,--,--,----,---,-,-5,-93,0,01,06
     *                      ^^ ^^^
     *   RSCP: Received Signal Code Power in dBm -> no need for mm_3gpp_rscp_level_to_rscp
     *   EC/n0: EC/n0   Carrier to noise ratio in dB = measured Ec/Io value in dB. Please refer to 3GPP 25.133, section 9.1.2.3, Table 9.9 for details on the mapping from EC/n0 to EC/Io.
     *     -> direct value, without need for mm_3gpp_ecn0_level_to_ecio
     *
     *
     * 4G
     * ^SMONI: 4G,EARFCN,Band,DL bandwidth,UL bandwidth,Mode,MCC,MNC,TAC,Global Cell ID,Physical Cell ID,Srxlev,RSRP,RSRQ,Conn_state
     * ^SMONI: 4G,EARFCN,Band,DL bandwidth,UL bandwidth,Mode,MCC,MNC,TAC,Global Cell ID,Physical Cell ID,Srxlev,RSRP,RSRQ,Conn_state
     * ^SMONI: 4G,EARFCN,Band,DL bandwidth,UL bandwidth,Mode,MCC,MNC,TAC,Global Cell ID,Physical Cell ID,Srxlev,RSRP,RSRQ,Conn_state
     * ^SMONI: 4G,EARFCN,Band,DL bandwidth,UL bandwidth,Mode,MCC,MNC,TAC,Global Cell ID,Physical Cell ID,TX_power,RSRP,RSRQ,Conn_state
     *
     * ^SMONI: 4G,6300,20,10,10,FDD,262,02,BF75,0345103,350,33,-94,-7,NOCONN
     *                                                         ^^^ ^^
     * ^SMONI: 4G,SEARCH
     * ^SMONI: 4G,6300,20,10,10,FDD,262,02,BF75,0345103,350,33,-94,-7,LIMSRV
     *                                                         ^^^ ^^
     * ^SMONI: 4G,6300,20,10,10,FDD,262,02,BF75,0345103,350,90,-94,-7,CONN
     *                                                         ^^^ ^^
     *  RSRP    Reference Signal Received Power (see 3GPP 36.214 Section 5.1.1.) -> directly the value without mm_3gpp_rsrq_level_to_rsrp
     *  RSRQ    Reference Signal Received Quality (see 3GPP 36.214 Section 5.1.2.) -> directly the value without mm_3gpp_rsrq_level_to_rsrq
     */
    if (g_regex_match_simple ("\\^SMONI:\\s*[234]G,SEARCH", response, 0, 0)) {
        success = TRUE;
        goto out;
    }
    pre = g_regex_new ("\\^SMONI:\\s*([234])", 0, 0, NULL);
    g_assert (pre != NULL);
    g_regex_match_full (pre, response, strlen (response), 0, 0, &match_info_pre, &inner_error);
    if (!inner_error && g_match_info_matches (match_info_pre)) {
        if (!mm_get_uint_from_match_info (match_info_pre, 1, &tech)) {
            inner_error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Couldn't read tech");
            goto out;
        }
        #define FLOAT "([-+]?[0-9]+\\.?[0-9]*)"
        switch (tech) {
        case MM_CINTERION_SMONI_2G:
            r = g_regex_new ("\\^SMONI:\\s*2G,(\\d+),"FLOAT, 0, 0, NULL);
            g_assert (r != NULL);
            g_regex_match_full (r, response, strlen (response), 0, 0, &match_info, &inner_error);
            if (!inner_error && g_match_info_matches (match_info)) {
                /* skip ARFCN */
                if (!mm_get_double_from_match_info (match_info, 2, &rssi)) {
                    inner_error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Couldn't read BCCH=rssi");
                    goto out;
                }
            }
            break;
        case MM_CINTERION_SMONI_3G:
            r = g_regex_new ("\\^SMONI:\\s*3G,(\\d+),(\\d+),"FLOAT","FLOAT, 0, 0, NULL);
            g_assert (r != NULL);
            g_regex_match_full (r, response, strlen (response), 0, 0, &match_info, &inner_error);
            if (!inner_error && g_match_info_matches (match_info)) {
                /* skip UARFCN */
                /* skip PSC (Primary scrambling code) */
                if (!mm_get_double_from_match_info (match_info, 3, &ecn0)) {
                    inner_error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Couldn't read EcN0");
                    goto out;
                }
                if (!mm_get_double_from_match_info (match_info, 4, &rscp)) {
                    inner_error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Couldn't read RSCP");
                    goto out;
                }
            }
            break;
        case MM_CINTERION_SMONI_4G:
            r = g_regex_new ("\\^SMONI:\\s*4G,(\\d+),(\\d+),(\\d+),(\\d+),(\\w+),(\\d+),(\\d+),(\\w+),(\\w+),(\\d+),([^,]*),"FLOAT","FLOAT, 0, 0, NULL);
            g_assert (r != NULL);
            g_regex_match_full (r, response, strlen (response), 0, 0, &match_info, &inner_error);
            if (!inner_error && g_match_info_matches (match_info)) {
                /* skip EARFCN */
                /* skip Band */
                /* skip DL bandwidth */
                /* skip UL bandwidth */
                /* skip Mode */
                /* skip MCC */
                /* skip MNC */
                /* skip TAC */
                /* skip Global Cell ID */
                /* skip Physical Cell ID */
                /* skip Srxlev/TX_power */
                if (!mm_get_double_from_match_info (match_info, 12, &rsrp)) {
                    inner_error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Couldn't read RSRQ");
                    goto out;
                }
                if (!mm_get_double_from_match_info (match_info, 13, &rsrq)) {
                    inner_error = g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Couldn't read RSRP");
                    goto out;
                }
            }
            break;
        case MM_CINTERION_SMONI_NO_TECH:
        default:
            goto out;
        }
        #undef FLOAT
        success = TRUE;
    }

out:
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    if (!success) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Couldn't parse ^SMONI response: %s", response);
        return FALSE;
    }

    *out_tech = tech;
    *out_rssi = rssi;
    *out_rscp = rscp;
    *out_ecn0 = ecn0;
    *out_rsrq = rsrq;
    *out_rsrp = rsrp;
    return TRUE;
}

/*****************************************************************************/
/* Get extended signal information */

gboolean
mm_cinterion_smoni_response_to_signal_info (const gchar  *response,
                                            MMSignal    **out_gsm,
                                            MMSignal    **out_umts,
                                            MMSignal    **out_lte,
                                            GError      **error)
{
    MMCinterionSmoniTech    tech    = MM_CINTERION_SMONI_NO_TECH;
    gdouble                 rssi    = MM_SIGNAL_UNKNOWN;
    gdouble                 ecn0    = MM_SIGNAL_UNKNOWN;
    gdouble                 rscp    = MM_SIGNAL_UNKNOWN;
    gdouble                 rsrq    = MM_SIGNAL_UNKNOWN;
    gdouble                 rsrp    = MM_SIGNAL_UNKNOWN;
    MMSignal               *gsm     = NULL;
    MMSignal               *umts    = NULL;
    MMSignal               *lte     = NULL;

    if (!mm_cinterion_parse_smoni_query_response (response,
                                                  &tech, &rssi,
                                                  &ecn0, &rscp,
                                                  &rsrp, &rsrq,
                                                  error))
        return FALSE;

    switch (tech) {
    case MM_CINTERION_SMONI_2G:
        gsm = mm_signal_new ();
        mm_signal_set_rssi (gsm, rssi);
        break;
    case MM_CINTERION_SMONI_3G:
        umts = mm_signal_new ();
        mm_signal_set_rscp (umts, rscp);
        mm_signal_set_ecio (umts, ecn0); /* UMTS EcIo (assumed EcN0) */
        break;
    case MM_CINTERION_SMONI_4G:
        lte = mm_signal_new ();
        mm_signal_set_rsrp (lte, rsrp);
        mm_signal_set_rsrq (lte, rsrq);
        break;
    case MM_CINTERION_SMONI_NO_TECH: /* not registered, searching */
        break; /* no error case */
    default: /* should not happen, so if it does, error */
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Couldn't build detailed signal info");
        return FALSE;
    }

    if (out_gsm)
        *out_gsm = gsm;
    if (out_umts)
        *out_umts = umts;
    if (out_lte)
        *out_lte = lte;

    return TRUE;
}
