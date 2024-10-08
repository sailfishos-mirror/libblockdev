/*
 * Copyright (C) 2014-2023 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Tomas Bzatek <tbzatek@redhat.com>
 */

#include <glib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#include <json-glib/json-glib.h>

#include <blockdev/utils.h>
#include <check_deps.h>

#include "smart.h"
#include "smart-private.h"

#define SMARTCTL_MIN_VERSION "7.0"

static volatile guint avail_deps = 0;
static GMutex deps_check_lock;

#define DEPS_SMART 0
#define DEPS_SMART_MASK (1 << DEPS_SMART)
#define DEPS_LAST 1

static const UtilDep deps[DEPS_LAST] = {
    { "smartctl", SMARTCTL_MIN_VERSION, NULL, "smartctl ([\\d\\.]+) .*" },
};

/**
 * bd_smart_check_deps:
 *
 * Returns: whether the plugin's runtime dependencies are satisfied or not
 *
 * Function checking plugin's runtime dependencies.
 */
gboolean bd_smart_check_deps (void) {
    GError *error = NULL;
    guint i = 0;
    gboolean status = FALSE;
    gboolean ret = TRUE;

    for (i = 0; i < DEPS_LAST; i++) {
        status = bd_utils_check_util_version (deps[i].name, deps[i].version,
                                              deps[i].ver_arg, deps[i].ver_regexp, &error);
        if (!status)
            bd_utils_log_format (BD_UTILS_LOG_WARNING, "%s", error->message);
        else
            g_atomic_int_or (&avail_deps, 1 << i);
        g_clear_error (&error);
        ret = ret && status;
    }

    if (!ret)
        bd_utils_log_format (BD_UTILS_LOG_WARNING, "Cannot load the SMART plugin");

    return ret;
}

/**
 * bd_smart_is_tech_avail:
 * @tech: the queried tech
 * @mode: a bit mask of queried modes of operation (#BDSmartTechMode) for @tech
 * @error: (out) (nullable): place to store error (details about why the @tech-@mode combination is not available)
 *
 * Returns: whether the @tech-@mode combination is available -- supported by the
 *          plugin implementation and having all the runtime dependencies available
 */
gboolean bd_smart_is_tech_avail (G_GNUC_UNUSED BDSmartTech tech, G_GNUC_UNUSED guint64 mode, GError **error) {
    /* all tech-mode combinations are supported by this implementation of the plugin */
    return check_deps (&avail_deps, DEPS_SMART_MASK, deps, DEPS_LAST, &deps_check_lock, error);
}



static const gchar * get_error_message_from_exit_code (gint exit_code) {
    /*
     * bit 0: Command line did not parse.
     * bit 1: Device open failed, device did not return an IDENTIFY DEVICE structure, or device is in a low-power mode
     * bit 2: Some SMART or other ATA command to the disk failed, or there was a checksum error in a SMART data structure
     */

    if (exit_code & 0x01)
        return "Command line did not parse.";
    if (exit_code & 0x02)
        return "Device open failed or device did not return an IDENTIFY DEVICE structure.";
    if (exit_code & 0x04)
        return "Some SMART or other ATA command to the disk failed, or there was a checksum error in a SMART data structure.";
    return NULL;
}

static void lookup_well_known_attr (BDSmartATAAttribute *a,
                                    gchar **well_known_name,
                                    gint64 *pretty_value,
                                    BDSmartATAAttributeUnit *pretty_unit) {
    *well_known_name = g_strdup (well_known_attrs[a->id].libatasmart_name);
    if (*well_known_name) {
        /* verify matching attribute names */
        const gchar * const *n;
        gboolean trusted = FALSE;

        for (n = well_known_attrs[a->id].smartmontools_names; *n; n++)
            if (g_strcmp0 (*n, a->name) == 0) {
                trusted = TRUE;
                break;
            }
        if (trusted) {
            char *endptr = NULL;
            guint64 hour, min, sec, usec;

            *pretty_unit = well_known_attrs[a->id].unit;
            switch (well_known_attrs[a->id].unit) {
                /* FIXME: we have the 64-bit raw value but no context how it's supposed
                 *        to be represented. This is defined in the smartmontools drivedb
                 *        yet not exposed over to JSON.
                 */
                case BD_SMART_ATA_ATTRIBUTE_UNIT_UNKNOWN:
                case BD_SMART_ATA_ATTRIBUTE_UNIT_NONE:
                case BD_SMART_ATA_ATTRIBUTE_UNIT_SECTORS:
                    /* try converting whatever possible and simply ignore the rest */
                    *pretty_value = g_ascii_strtoll (a->pretty_value_string, &endptr, 0);
                    if (! endptr || endptr == a->pretty_value_string)
                        *pretty_value = a->value_raw;
                    break;
                case BD_SMART_ATA_ATTRIBUTE_UNIT_MSECONDS:
                    /* possible formats as printed by ata_format_attr_raw_value():
                     *    "%lluh+%02llum (%u)"
                     *    "%lluh+%02llum+%02llus"
                     *    "%lluh+%02llum"
                     *    "%uh+%02um+%02u.%03us"
                     */
                    hour = min = sec = usec = 0;
                    if (sscanf (a->pretty_value_string, "%" PRIu64 "h+%" PRIu64 "m+%" PRIu64 "s.%" PRIu64 "s", &hour, &min, &sec, &usec) >= 2)
                        *pretty_value = ((hour * 60 + min) * 60 + sec) * 1000 + usec;
                    else
                        *pretty_value = a->value_raw;
                    break;
                case BD_SMART_ATA_ATTRIBUTE_UNIT_MKELVIN:
                    /* possible formats as printed by ata_format_attr_raw_value():
                     *   "%d"
                     *   "%d (Min/Max %d/%d)"
                     *   "%d (Min/Max %d/%d #%d)"
                     *   "%d (%d %d %d %d %d)"
                     *   "%d.%d"
                     */
                    *pretty_value = g_ascii_strtoll (a->pretty_value_string, &endptr, 0);
                    if (! endptr || endptr == a->pretty_value_string)
                        *pretty_value = a->value_raw;
                    else
                        /* temperature in degrees Celsius, need millikelvins */
                        *pretty_value = *pretty_value * 1000 + 273150;
                    break;
                case BD_SMART_ATA_ATTRIBUTE_UNIT_SMALL_PERCENT:
                case BD_SMART_ATA_ATTRIBUTE_UNIT_PERCENT:
                case BD_SMART_ATA_ATTRIBUTE_UNIT_MB:
                default:
                    /* not implemented */
                    *pretty_unit = BD_SMART_ATA_ATTRIBUTE_UNIT_UNKNOWN;
                    break;
            }
        } else {
            g_free (*well_known_name);
            *well_known_name = NULL;
        }
    }

    if (*well_known_name == NULL) {
        /* not a well-known attribute or failed verification */
        *pretty_unit = 0;
        *pretty_value = a->value_raw;
    }
}

/* Returns num elements read or -1 in case of an error. */
static gint parse_int_array (JsonReader *reader, const gchar *key, gint64 *dest, gint max_count, GError **error) {
    gint count;
    int i;

    if (! json_reader_read_member (reader, key)) {
        g_set_error_literal (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                             json_reader_get_error (reader)->message);
        json_reader_end_member (reader);
        return -1;
    }

    count = MIN (max_count, json_reader_count_elements (reader));
    if (count < 0) {
        g_set_error_literal (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                             json_reader_get_error (reader)->message);
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (! json_reader_read_element (reader, i)) {
            g_set_error_literal (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                                 json_reader_get_error (reader)->message);
            json_reader_end_element (reader);
            return -1;
        }
        dest[i] = json_reader_get_int_value (reader);
        json_reader_end_element (reader);
    }

    json_reader_end_member (reader);
    return count;
}

/* Returns null-terminated list of messages marked as severity=error */
static gchar ** parse_error_messages (JsonReader *reader) {
    GPtrArray *a;
    gint count;
    int i;

    if (! json_reader_read_member (reader, "smartctl")) {
        json_reader_end_member (reader);
        return NULL;
    }
    if (! json_reader_read_member (reader, "messages")) {
        json_reader_end_member (reader);
        json_reader_end_member (reader);
        return NULL;
    }

    a = g_ptr_array_new_full (0, g_free);
    count = json_reader_count_elements (reader);

    for (i = 0; count >= 0 && i < count; i++) {
        if (! json_reader_read_element (reader, i)) {
            json_reader_end_element (reader);
            g_ptr_array_free (a, TRUE);
            return NULL;
        }
        if (json_reader_is_object (reader)) {
            gboolean severity_error = FALSE;

            if (json_reader_read_member (reader, "severity"))
                severity_error = g_strcmp0 ("error", json_reader_get_string_value (reader)) == 0;
            json_reader_end_member (reader);

            if (severity_error) {
                if (json_reader_read_member (reader, "string")) {
                    const gchar *val = json_reader_get_string_value (reader);
                    if (val)
                        g_ptr_array_add (a, g_strdup (val));
                }
                json_reader_end_member (reader);
            }
        }
        json_reader_end_element (reader);
    }
    json_reader_end_member (reader);
    json_reader_end_member (reader);

    g_ptr_array_add (a, NULL);
    return (gchar **) g_ptr_array_free (a, FALSE);
}


#define MIN_JSON_FORMAT_VER 1     /* minimal json_format_version */

static gboolean parse_smartctl_error (gint status, const gchar *stdout, const gchar *stderr, JsonParser *parser, GError **error) {
    gint res;
    JsonReader *reader;
    gint64 ver_info[2] = { 0, 0 };
    GError *l_error = NULL;

    if ((!stdout || strlen (stdout) == 0) &&
        (!stderr || strlen (stderr) == 0)) {
        g_set_error_literal (error, BD_SMART_ERROR, BD_SMART_ERROR_FAILED,
                             (status & 0x07) ? get_error_message_from_exit_code (status) : "Empty response");
        return FALSE;
    }
    /* Expecting proper JSON output on stdout, take what has been received on stderr */
    if (!stdout || strlen (stdout) == 0) {
        g_set_error_literal (error, BD_SMART_ERROR, BD_SMART_ERROR_FAILED,
                             stderr);
        return FALSE;
    }

    /* Parse the JSON output */
    if (! json_parser_load_from_data (parser, stdout, -1, &l_error) ||
        ! json_parser_get_root (parser)) {
        g_set_error_literal (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                             l_error->message);
        g_error_free (l_error);
        return FALSE;
    }
    reader = json_reader_new (json_parser_get_root (parser));

    /* Verify the JSON output format */
    res = parse_int_array (reader, "json_format_version", ver_info, G_N_ELEMENTS (ver_info), error);
    if (res < 1) {
        g_prefix_error (error, "Error parsing version info: ");
        g_object_unref (reader);
        return FALSE;
    }
    if (ver_info[0] < MIN_JSON_FORMAT_VER) {
        g_set_error (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                     "Reported smartctl JSON format version too low: %" G_GUINT64_FORMAT " (required: %d)",
                     ver_info[0], MIN_JSON_FORMAT_VER);
        g_object_unref (reader);
        return FALSE;
    }
    if (ver_info[0] > MIN_JSON_FORMAT_VER)
        g_warning ("Reported smartctl JSON format major version higher than expected, expect parse issues");

    /* Find out the return code and associated messages */
    if (status & 0x07) {
        gchar **messages;

        messages = parse_error_messages (reader);
        g_set_error_literal (error, BD_SMART_ERROR, BD_SMART_ERROR_FAILED,
                             messages && messages[0] ? messages[0] : get_error_message_from_exit_code (status));
        g_strfreev (messages);
        g_object_unref (reader);
        return FALSE;
    }

    g_object_unref (reader);
    return TRUE;
}

static BDSmartATAAttribute ** parse_ata_smart_attributes (JsonReader *reader, GError **error) {
    GPtrArray *ptr_array;
    gint count;
    int i;

    ptr_array = g_ptr_array_new_full (0, (GDestroyNotify) bd_smart_ata_attribute_free);
    count = json_reader_count_elements (reader);
    for (i = 0; count > 0 && i < count; i++) {
        BDSmartATAAttribute *attr;
        gint64 f;

        if (! json_reader_read_element (reader, i)) {
            g_set_error (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                         "Error parsing smartctl JSON ata_smart_attributes[%d] element: %s",
                         i, json_reader_get_error (reader)->message);
            g_ptr_array_free (ptr_array, TRUE);
            json_reader_end_element (reader);
            return NULL;
        }

        attr = g_new0 (BDSmartATAAttribute, 1);

#define _READ_AND_CHECK(elem_name) \
        if (! json_reader_read_member (reader, elem_name)) { \
            g_set_error (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT, \
                         "Error parsing smartctl JSON ata_smart_attributes[%d] element: %s", \
                         i, json_reader_get_error (reader)->message); \
            g_ptr_array_free (ptr_array, TRUE); \
            bd_smart_ata_attribute_free (attr); \
            json_reader_end_member (reader); \
            json_reader_end_element (reader); \
            return NULL; \
        }

        _READ_AND_CHECK ("id");
        attr->id = json_reader_get_int_value (reader);
        json_reader_end_member (reader);

        _READ_AND_CHECK ("name");
        attr->name = g_strdup (json_reader_get_string_value (reader));
        json_reader_end_member (reader);

        _READ_AND_CHECK ("value");
        attr->value = json_reader_get_int_value (reader);
        json_reader_end_member (reader);

        _READ_AND_CHECK ("worst");
        attr->worst = json_reader_get_int_value (reader);
        json_reader_end_member (reader);

        _READ_AND_CHECK ("thresh");
        attr->threshold = json_reader_get_int_value (reader);
        json_reader_end_member (reader);

        _READ_AND_CHECK ("when_failed");
        if (g_strcmp0 (json_reader_get_string_value (reader), "past") == 0)
            attr->failed_past = TRUE;
        else
        if (g_strcmp0 (json_reader_get_string_value (reader), "now") == 0)
            attr->failing_now = TRUE;
        json_reader_end_member (reader);

        _READ_AND_CHECK ("raw");
        _READ_AND_CHECK ("value");
        attr->value_raw = json_reader_get_int_value (reader);
        json_reader_end_member (reader);
        _READ_AND_CHECK ("string");
        attr->pretty_value_string = g_strdup (json_reader_get_string_value (reader));
        json_reader_end_member (reader);
        json_reader_end_member (reader);

        _READ_AND_CHECK ("flags");
        _READ_AND_CHECK ("value");
        f = json_reader_get_int_value (reader);
        if (f & 0x01)
            attr->flags |= BD_SMART_ATA_ATTRIBUTE_FLAG_PREFAILURE;
        if (f & 0x02)
            attr->flags |= BD_SMART_ATA_ATTRIBUTE_FLAG_ONLINE;
        if (f & 0x04)
            attr->flags |= BD_SMART_ATA_ATTRIBUTE_FLAG_PERFORMANCE;
        if (f & 0x08)
            attr->flags |= BD_SMART_ATA_ATTRIBUTE_FLAG_ERROR_RATE;
        if (f & 0x10)
            attr->flags |= BD_SMART_ATA_ATTRIBUTE_FLAG_EVENT_COUNT;
        if (f & 0x20)
            attr->flags |= BD_SMART_ATA_ATTRIBUTE_FLAG_SELF_PRESERVING;
        if (f & 0xffc0)
            attr->flags |= BD_SMART_ATA_ATTRIBUTE_FLAG_OTHER;
        json_reader_end_member (reader);
        json_reader_end_member (reader);
        json_reader_end_element (reader);

#undef _READ_AND_CHECK

        lookup_well_known_attr (attr,
                                &attr->well_known_name,
                                &attr->pretty_value,
                                &attr->pretty_value_unit);
        g_ptr_array_add (ptr_array, attr);
    }

    g_ptr_array_add (ptr_array, NULL);
    return (BDSmartATAAttribute **) g_ptr_array_free (ptr_array, FALSE);
}

static BDSmartATA * parse_ata_smart (JsonParser *parser, GError **error) {
    BDSmartATA *data;
    JsonReader *reader;

    data = g_new0 (BDSmartATA, 1);
    reader = json_reader_new (json_parser_get_root (parser));

    /* smart_support section */
    if (json_reader_read_member (reader, "smart_support")) {
        if (json_reader_read_member (reader, "available"))
            data->smart_supported = json_reader_get_boolean_value (reader);
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "enabled"))
            data->smart_enabled = json_reader_get_boolean_value (reader);
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* smart_status section */
    if (json_reader_read_member (reader, "smart_status")) {
        if (json_reader_read_member (reader, "passed"))
            data->overall_status_passed = json_reader_get_boolean_value (reader);
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* ata_smart_data section */
    if (! json_reader_read_member (reader, "ata_smart_data")) {
        g_set_error (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                     "Error parsing smartctl JSON data: %s",
                     json_reader_get_error (reader)->message);
        g_object_unref (reader);
        bd_smart_ata_free (data);
        return NULL;
    }
    if (json_reader_read_member (reader, "offline_data_collection")) {
        if (json_reader_read_member (reader, "status")) {
            if (json_reader_read_member (reader, "value")) {
                gint64 val = json_reader_get_int_value (reader);

                switch (val & 0x7f) {
                    case 0x00:
                        data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_NEVER_STARTED;
                        break;
                    case 0x02:
                        data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_NO_ERROR;
                        break;
                    case 0x03:
                        if (val == 0x03)
                            data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_IN_PROGRESS;
                        else
                            data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_RESERVED;
                        break;
                    case 0x04:
                        data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_SUSPENDED_INTR;
                        break;
                    case 0x05:
                        data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_ABORTED_INTR;
                        break;
                    case 0x06:
                        data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_ABORTED_ERROR;
                        break;
                    default:
                        if ((val & 0x7f) >= 0x40)
                            data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_VENDOR_SPECIFIC;
                        else
                            data->offline_data_collection_status = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_STATUS_RESERVED;
                        break;
                }
                data->auto_offline_data_collection_enabled = val & 0x80;
            }
            json_reader_end_member (reader);
        }
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "completion_seconds"))
            data->offline_data_collection_completion = json_reader_get_int_value (reader);
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);  /* offline_data_collection */

    if (json_reader_read_member (reader, "self_test")) {
        if (json_reader_read_member (reader, "status")) {
            if (json_reader_read_member (reader, "value")) {
                gint64 val = json_reader_get_int_value (reader);

                switch (val >> 4) {
                    case 0x00:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_COMPLETED_NO_ERROR;
                        break;
                    case 0x01:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_ABORTED_HOST;
                        break;
                    case 0x02:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_INTR_HOST_RESET;
                        break;
                    case 0x03:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_ERROR_FATAL;
                        break;
                    case 0x04:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_ERROR_UNKNOWN;
                        break;
                    case 0x05:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_ERROR_ELECTRICAL;
                        break;
                    case 0x06:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_ERROR_SERVO;
                        break;
                    case 0x07:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_ERROR_READ;
                        break;
                    case 0x08:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_ERROR_HANDLING;
                        break;
                    case 0x0f:
                        data->self_test_status = BD_SMART_ATA_SELF_TEST_STATUS_IN_PROGRESS;
                        data->self_test_percent_remaining = (val & 0x0f) * 10;
                        break;
                }
            }
            json_reader_end_member (reader);
        }
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "polling_minutes")) {
            if (json_reader_read_member (reader, "short"))
                data->self_test_polling_short = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "extended"))
                data->self_test_polling_extended = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "conveyance"))
                data->self_test_polling_conveyance = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
        }
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);  /* self_test */

    if (json_reader_read_member (reader, "capabilities")) {
        gint64 val[2] = { 0, 0 };

        if (parse_int_array (reader, "values", val, G_N_ELEMENTS (val), NULL) == G_N_ELEMENTS (val)) {
            if (val[0] == 0x00)
                data->offline_data_collection_capabilities = BD_SMART_ATA_OFFLINE_DATA_COLLECTION_CAP_NOT_SUPPORTED;
            else {
                if (val[0] & 0x01)
                    data->offline_data_collection_capabilities |= BD_SMART_ATA_OFFLINE_DATA_COLLECTION_CAP_EXEC_OFFLINE_IMMEDIATE;
                /* 0x02 is deprecated - SupportAutomaticTimer */
                if (val[0] & 0x04)
                    data->offline_data_collection_capabilities |= BD_SMART_ATA_OFFLINE_DATA_COLLECTION_CAP_OFFLINE_ABORT;
                if (val[0] & 0x08)
                    data->offline_data_collection_capabilities |= BD_SMART_ATA_OFFLINE_DATA_COLLECTION_CAP_OFFLINE_SURFACE_SCAN;
                if (val[0] & 0x10)
                    data->offline_data_collection_capabilities |= BD_SMART_ATA_OFFLINE_DATA_COLLECTION_CAP_SELF_TEST;
                if (val[0] & 0x20)
                    data->offline_data_collection_capabilities |= BD_SMART_ATA_OFFLINE_DATA_COLLECTION_CAP_CONVEYANCE_SELF_TEST;
                if (val[0] & 0x40)
                    data->offline_data_collection_capabilities |= BD_SMART_ATA_OFFLINE_DATA_COLLECTION_CAP_SELECTIVE_SELF_TEST;
            }
            if (val[1] & 0x01)
                data->smart_capabilities |= BD_SMART_ATA_CAP_ATTRIBUTE_AUTOSAVE;
            if (val[1] & 0x02)
                data->smart_capabilities |= BD_SMART_ATA_CAP_AUTOSAVE_TIMER;
        }
        if (json_reader_read_member (reader, "error_logging_supported"))
            if (json_reader_get_boolean_value (reader))
                data->smart_capabilities |= BD_SMART_ATA_CAP_ERROR_LOGGING;
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "gp_logging_supported"))
            if (json_reader_get_boolean_value (reader))
                data->smart_capabilities |= BD_SMART_ATA_CAP_GP_LOGGING;
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);  /* capabilities */
    json_reader_end_member (reader);  /* ata_smart_data */

    /* ata_smart_attributes section */
    if (! json_reader_read_member (reader, "ata_smart_attributes") ||
        ! json_reader_read_member (reader, "table") ||
        ! json_reader_is_array (reader)) {
        g_set_error (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                     "Error parsing smartctl JSON data: %s",
                     json_reader_get_error (reader)->message);
        g_object_unref (reader);
        bd_smart_ata_free (data);
        return NULL;
    }
    data->attributes = parse_ata_smart_attributes (reader, error);
    if (! data->attributes) {
        g_object_unref (reader);
        bd_smart_ata_free (data);
        return NULL;
    }
    json_reader_end_member (reader);  /* table */
    json_reader_end_member (reader);  /* ata_smart_attributes */

    /* power_on_time section */
    if (json_reader_read_member (reader, "power_on_time")) {
        if (json_reader_read_member (reader, "hours"))
            data->power_on_time += json_reader_get_int_value (reader) * 60;
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "minutes"))
            data->power_on_time += json_reader_get_int_value (reader);
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* power_cycle_count section */
    if (json_reader_read_member (reader, "power_cycle_count"))
        data->power_cycle_count = json_reader_get_int_value (reader);
    json_reader_end_member (reader);

    /* temperature section */
    if (json_reader_read_member (reader, "temperature")) {
        if (json_reader_read_member (reader, "current"))
            data->temperature = json_reader_get_int_value (reader) + 273;
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    g_object_unref (reader);
    return data;
}


static BDSmartSCSI * parse_scsi_smart (JsonParser *parser, G_GNUC_UNUSED GError **error) {
    BDSmartSCSI *data;
    JsonReader *reader;

    data = g_new0 (BDSmartSCSI, 1);
    reader = json_reader_new (json_parser_get_root (parser));

    /* smart_support section */
    if (json_reader_read_member (reader, "smart_support")) {
        if (json_reader_read_member (reader, "available"))
            data->smart_supported = json_reader_get_boolean_value (reader);
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "enabled"))
            data->smart_enabled = json_reader_get_boolean_value (reader);
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* smart_status section */
    if (json_reader_read_member (reader, "smart_status")) {
        if (json_reader_read_member (reader, "passed"))
            data->overall_status_passed = json_reader_get_boolean_value (reader);
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "scsi")) {
            gint64 asc = -1;
            gint64 ascq = -1;

            if (json_reader_read_member (reader, "asc")) {
                asc = json_reader_get_int_value (reader);
                data->scsi_ie_asc = asc;
            }
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "ascq")) {
                ascq = json_reader_get_int_value (reader);
                data->scsi_ie_ascq = ascq;
            }
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "ie_string"))
                data->scsi_ie_string = g_strdup (json_reader_get_string_value (reader));
            json_reader_end_member (reader);

            if (asc == 0xb && ascq >= 0) {
                switch (ascq) {
                    case 0x00:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_ABORTED_COMMAND;
                        break;
                    case 0x01:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_TEMPERATURE_EXCEEDED;
                        break;
                    case 0x02:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_ENCLOSURE_DEGRADED;
                        break;
                    case 0x03:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_BACKGROUND_SELFTEST_FAILED;
                        break;
                    case 0x04:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_BACKGROUND_PRESCAN_MEDIUM_ERROR;
                        break;
                    case 0x05:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_BACKGROUND_SCAN_MEDIUM_ERROR;
                        break;
                    case 0x06:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_NV_CACHE_VOLATILE;
                        break;
                    case 0x07:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_NV_CACHE_DEGRADED_POWER;
                        break;
                    case 0x08:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_POWER_LOSS_EXPECTED;
                        break;
                    case 0x09:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_STATISTICS_NOTIFICATION;
                        break;
                    case 0x0a:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_HIGH_CRITICAL_TEMP;
                        break;
                    case 0x0b:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_LOW_CRITICAL_TEMP;
                        break;
                    case 0x0c:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_HIGH_OPERATING_TEMP;
                        break;
                    case 0x0d:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_LOW_OPERATING_TEMP;
                        break;
                    case 0x0e:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_HIGH_CRITICAL_HUMIDITY;
                        break;
                    case 0x0f:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_LOW_CRITICAL_HUMIDITY;
                        break;
                    case 0x10:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_HIGH_OPERATING_HUMIDITY;
                        break;
                    case 0x11:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_LOW_OPERATING_HUMIDITY;
                        break;
                    case 0x12:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_MICROCODE_SECURITY_RISK;
                        break;
                    case 0x13:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_MICROCODE_SIGNATURE_VALIDATION_FAILURE;
                        break;
                    case 0x14:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_PHYSICAL_ELEMENT_STATUS_CHANGE;
                        break;
                    default:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_UNSPECIFIED;
                        break;
                }
            } else if (asc == 0x5d && ascq >= 0) {
                switch (ascq) {
                    case 0x00:
                    case 0xff:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_FAILURE_PREDICTION_THRESH;
                        break;
                    case 0x01:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_MEDIA_FAILURE_PREDICTION_THRESH;
                        break;
                    case 0x02:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_LOGICAL_UNIT_FAILURE_PREDICTION_THRESH;
                        break;
                    case 0x03:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_SPARE_EXHAUSTION_PREDICTION_THRESH;
                        break;
                    case 0x73:
                        data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_MEDIA_ENDURANCE_LIMIT;
                        break;
                    default:
                        if (ascq >= 0x10 && ascq <= 0x1d)
                            data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_HARDWARE_IMPENDING_FAILURE;
                        else if (ascq >= 0x20 && ascq <= 0x2c)
                            data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_CONTROLLER_IMPENDING_FAILURE;
                        else if (ascq >= 0x30 && ascq <= 0x3c)
                            data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_DATA_CHANNEL_IMPENDING_FAILURE;
                        else if (ascq >= 0x40 && ascq <= 0x4c)
                            data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_SERVO_IMPENDING_FAILURE;
                        else if (ascq >= 0x50 && ascq <= 0x5c)
                            data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_SPINDLE_IMPENDING_FAILURE;
                        else if (ascq >= 0x60 && ascq <= 0x6c)
                            data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_FIRMWARE_IMPENDING_FAILURE;
                        else
                            data->scsi_ie = BD_SMART_SCSI_INFORMATIONAL_EXCEPTION_UNSPECIFIED;
                        break;
                }
            }
        }
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* temperature_warning section */
    if (json_reader_read_member (reader, "temperature_warning")) {
        if (json_reader_read_member (reader, "enabled"))
            data->temperature_warning_enabled = json_reader_get_boolean_value (reader);
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* temperature section */
    if (json_reader_read_member (reader, "temperature")) {
        if (json_reader_read_member (reader, "current"))
            data->temperature = json_reader_get_int_value (reader) + 273;
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "drive_trip"))
            data->temperature_drive_trip = json_reader_get_int_value (reader) + 273;
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* scsi_background_scan section */
    if (json_reader_read_member (reader, "scsi_background_scan")) {
        if (json_reader_read_member (reader, "status")) {
            if (json_reader_read_member (reader, "value")) {
                guint64 val = json_reader_get_int_value (reader);

                switch (val) {
                    case 0x00:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_NO_SCANS_ACTIVE;
                        break;
                    case 0x01:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_SCAN_ACTIVE;
                        break;
                    case 0x02:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_PRESCAN_ACTIVE;
                        break;
                    case 0x03:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_HALTED_ERROR_FATAL;
                        break;
                    case 0x04:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_HALTED_PATTERN_VENDOR_SPECIFIC;
                        break;
                    case 0x05:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_HALTED_ERROR_PLIST;
                        break;
                    case 0x06:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_HALTED_VENDOR_SPECIFIC;
                        break;
                    case 0x07:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_HALTED_TEMPERATURE;
                        break;
                    case 0x08:
                        data->background_scan_status = BD_SMART_SCSI_BACKGROUND_SCAN_STATUS_BMS_TIMER;
                        break;
                    default:
                        /* just copy the value, it corresponds to the above anyway */
                        data->background_scan_status = val;
                }
            }
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "scan_progress")) {
                const gchar *val = json_reader_get_string_value (reader);
                float d = 0.0;

                if (sscanf (val, "%f%%", &d) == 1)
                    data->background_scan_progress = d;
            }
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "number_scans_performed"))
                data->background_scan_runs = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "number_medium_scans_performed"))
                data->background_medium_scan_runs = json_reader_get_int_value (reader);
            json_reader_end_member (reader);

        }
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* scsi_start_stop_cycle_counter section */
    if (json_reader_read_member (reader, "scsi_start_stop_cycle_counter")) {
        if (json_reader_read_member (reader, "specified_cycle_count_over_device_lifetime"))
            data->start_stop_cycle_lifetime = json_reader_get_int_value (reader);
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "accumulated_start_stop_cycles"))
            data->start_stop_cycle_count = json_reader_get_int_value (reader);
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "specified_load_unload_count_over_device_lifetime"))
            data->load_unload_cycle_lifetime = json_reader_get_int_value (reader);
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "accumulated_load_unload_cycles"))
            data->load_unload_cycle_count = json_reader_get_int_value (reader);
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* scsi_grown_defect_list section */
    if (json_reader_read_member (reader, "scsi_grown_defect_list"))
        data->scsi_grown_defect_list = json_reader_get_int_value (reader);
    json_reader_end_member (reader);

    /* scsi_error_counter_log section */
    if (json_reader_read_member (reader, "scsi_error_counter_log")) {
        if (json_reader_read_member (reader, "read")) {
            if (json_reader_read_member (reader, "errors_corrected_by_eccfast"))
                data->read_errors_corrected_eccfast = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "errors_corrected_by_eccdelayed"))
                data->read_errors_corrected_eccdelayed = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "errors_corrected_by_rereads_rewrites"))
                data->read_errors_corrected_rereads = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "total_errors_corrected"))
                data->read_errors_corrected_total = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "total_uncorrected_errors"))
                data->read_errors_uncorrected = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "gigabytes_processed")) {
                const gchar *val = json_reader_get_string_value (reader);
                gdouble d = 0.0;

                if (val)
                    d = g_ascii_strtod (val, NULL);
                data->read_processed_bytes = d * 1000000000;
            }
            json_reader_end_member (reader);
        }
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "write")) {
            if (json_reader_read_member (reader, "errors_corrected_by_eccfast"))
                data->write_errors_corrected_eccfast = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "errors_corrected_by_eccdelayed"))
                data->write_errors_corrected_eccdelayed = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "errors_corrected_by_rereads_rewrites"))
                data->write_errors_corrected_rewrites = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "total_errors_corrected"))
                data->write_errors_corrected_total = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "total_uncorrected_errors"))
                data->write_errors_uncorrected = json_reader_get_int_value (reader);
            json_reader_end_member (reader);
            if (json_reader_read_member (reader, "gigabytes_processed")) {
                const gchar *val = json_reader_get_string_value (reader);
                gdouble d = 0.0;

                if (val)
                    d = g_ascii_strtod (val, NULL);
                data->write_processed_bytes = d * 1000000000;
            }
            json_reader_end_member (reader);
        }
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    /* power_on_time section */
    if (json_reader_read_member (reader, "power_on_time")) {
        if (json_reader_read_member (reader, "hours"))
            data->power_on_time += json_reader_get_int_value (reader) * 60;
        json_reader_end_member (reader);
        if (json_reader_read_member (reader, "minutes"))
            data->power_on_time += json_reader_get_int_value (reader);
        json_reader_end_member (reader);
    }
    json_reader_end_member (reader);

    g_object_unref (reader);
    return data;
}


/**
 * bd_smart_ata_get_info:
 * @device: device to check.
 * @extra: (nullable) (array zero-terminated=1): extra options to pass through.
 * @error: (out) (optional): place to store error (if any).
 *
 * Retrieve SMART information from the drive.
 *
 * Returns: (transfer full): ATA SMART log or %NULL in case of an error (with @error set).
 *
 * Tech category: %BD_SMART_TECH_ATA-%BD_SMART_TECH_MODE_INFO
 */
BDSmartATA * bd_smart_ata_get_info (const gchar *device, const BDExtraArg **extra, GError **error) {
    const gchar *args[8] = { "smartctl", "--info", "--health", "--capabilities", "--attributes", "--json", device, NULL };
    gint status = 0;
    gchar *stdout = NULL;
    gchar *stderr = NULL;
    JsonParser *parser;
    BDSmartATA *data = NULL;
    gboolean ret;

    if (!bd_utils_exec_and_capture_output_no_progress (args, extra, &stdout, &stderr, &status, error)) {
        g_prefix_error (error, "Error getting ATA SMART info: ");
        return NULL;
    }

    if (stdout)
        g_strstrip (stdout);
    if (stderr)
        g_strstrip (stderr);

    parser = json_parser_new ();
    ret = parse_smartctl_error (status, stdout, stderr, parser, error);
    g_free (stdout);
    g_free (stderr);
    if (! ret) {
        g_prefix_error (error, "Error getting ATA SMART info: ");
        g_object_unref (parser);
        return NULL;
    }

    data = parse_ata_smart (parser, error);
    g_object_unref (parser);

    return data;
}

/**
 * bd_smart_ata_get_info_from_data:
 * @data: (array length=data_len): binary data to parse.
 * @data_len: length of the data supplied.
 * @error: (out) (optional): place to store error (if any).
 *
 * Retrieve SMART information from the supplied data.
 *
 * Returns: (transfer full): ATA SMART log or %NULL in case of an error (with @error set).
 *
 * Tech category: %BD_SMART_TECH_ATA-%BD_SMART_TECH_MODE_INFO
 */
BDSmartATA * bd_smart_ata_get_info_from_data (const guint8 *data, gsize data_len, GError **error) {
    JsonParser *parser;
    gchar *stdout;
    BDSmartATA *ata_data = NULL;
    gboolean ret;

    g_warn_if_fail (data != NULL);
    g_warn_if_fail (data_len > 0);

    stdout = g_strndup ((gchar *)data, data_len);
    g_strstrip (stdout);

    parser = json_parser_new ();
    ret = parse_smartctl_error (0, stdout, NULL, parser, error);
    g_free (stdout);
    if (! ret) {
        g_prefix_error (error, "Error getting ATA SMART info: ");
        g_object_unref (parser);
        return NULL;
    }

    ata_data = parse_ata_smart (parser, error);
    g_object_unref (parser);

    return ata_data;
}


/**
 * bd_smart_scsi_get_info:
 * @device: device to check.
 * @extra: (nullable) (array zero-terminated=1): extra options to pass through.
 * @error: (out) (optional): place to store error (if any).
 *
 * Retrieve SMART information from SCSI or SAS-compliant drive.
 *
 * Returns: (transfer full): SCSI SMART log or %NULL in case of an error (with @error set).
 *
 * Tech category: %BD_SMART_TECH_SCSI-%BD_SMART_TECH_MODE_INFO
 */
BDSmartSCSI * bd_smart_scsi_get_info (const gchar *device, const BDExtraArg **extra, GError **error) {
    const gchar *args[9] = { "smartctl", "--info", "--health", "--attributes", "--log=error", "--log=background", "--json", device, NULL };
    gint status = 0;
    gchar *stdout = NULL;
    gchar *stderr = NULL;
    JsonParser *parser;
    BDSmartSCSI *data = NULL;
    gboolean ret;

    if (!bd_utils_exec_and_capture_output_no_progress (args, extra, &stdout, &stderr, &status, error)) {
        g_prefix_error (error, "Error getting SCSI SMART info: ");
        return NULL;
    }

    if (stdout)
        g_strstrip (stdout);
    if (stderr)
        g_strstrip (stderr);

    parser = json_parser_new ();
    ret = parse_smartctl_error (status, stdout, stderr, parser, error);
    g_free (stdout);
    g_free (stderr);
    if (! ret) {
        g_prefix_error (error, "Error getting SCSI SMART info: ");
        g_object_unref (parser);
        return NULL;
    }

    data = parse_scsi_smart (parser, error);
    g_object_unref (parser);

    return data;
}


/**
 * bd_smart_set_enabled:
 * @device: SMART-capable device.
 * @enabled: whether to enable or disable the SMART functionality
 * @extra: (nullable) (array zero-terminated=1): extra options to pass through.
 * @error: (out) (optional): place to store error (if any).
 *
 * Enables or disables SMART functionality on device.
 *
 * Returns: %TRUE when the functionality was set successfully or %FALSE in case of an error (with @error set).
 *
 * Tech category: %BD_SMART_TECH_ATA-%BD_SMART_TECH_MODE_INFO
 */
gboolean bd_smart_set_enabled (const gchar *device, gboolean enabled, const BDExtraArg **extra, GError **error) {
    const gchar *args[5] = { "smartctl", "--json", "--smart=on", device, NULL };
    gint status = 0;
    gchar *stdout = NULL;
    gchar *stderr = NULL;
    JsonParser *parser;
    gboolean ret;

    if (!enabled)
        args[2] = "--smart=off";

    if (!bd_utils_exec_and_capture_output_no_progress (args, extra, &stdout, &stderr, &status, error)) {
        g_prefix_error (error, "Error setting SMART functionality: ");
        return FALSE;
    }

    if (stdout)
        g_strstrip (stdout);
    if (stderr)
        g_strstrip (stderr);

    parser = json_parser_new ();
    ret = parse_smartctl_error (status, stdout, stderr, parser, error);
    g_free (stdout);
    g_free (stderr);
    g_object_unref (parser);
    if (! ret) {
        g_prefix_error (error, "Error setting SMART functionality: ");
        return FALSE;
    }

    return TRUE;
}

/**
 * bd_smart_device_self_test:
 * @device: device to trigger the test on.
 * @operation: #BDSmartSelfTestOp self-test operation.
 * @extra: (nullable) (array zero-terminated=1): extra options to pass through.
 * @error: (out) (optional): place to store error (if any).
 *
 * Executes or aborts device self-test.
 *
 * Returns: %TRUE when the self-test was triggered successfully or %FALSE in case of an error (with @error set).
 *
 * Tech category: %BD_SMART_TECH_ATA-%BD_SMART_TECH_MODE_SELFTEST
 */
gboolean bd_smart_device_self_test (const gchar *device, BDSmartSelfTestOp operation, const BDExtraArg **extra, GError **error) {
    const gchar *args[5] = { "smartctl", "--json", "--test=", device, NULL };
    gint status = 0;
    gchar *stdout = NULL;
    gchar *stderr = NULL;
    JsonParser *parser;
    gboolean ret;

    switch (operation) {
        case BD_SMART_SELF_TEST_OP_ABORT:
            args[2] = "--abort";
            break;
        case BD_SMART_SELF_TEST_OP_OFFLINE:
            args[2] = "--test=offline";
            break;
        case BD_SMART_SELF_TEST_OP_SHORT:
            args[2] = "--test=short";
            break;
        case BD_SMART_SELF_TEST_OP_LONG:
            args[2] = "--test=long";
            break;
        case BD_SMART_SELF_TEST_OP_CONVEYANCE:
            args[2] = "--test=conveyance";
            break;
        default:
            g_set_error_literal (error, BD_SMART_ERROR, BD_SMART_ERROR_INVALID_ARGUMENT,
                                 "Invalid self-test operation.");
            return FALSE;
    }

    if (!bd_utils_exec_and_capture_output_no_progress (args, extra, &stdout, &stderr, &status, error)) {
        g_prefix_error (error, "Error executing SMART self-test: ");
        return FALSE;
    }

    if (stdout)
        g_strstrip (stdout);
    if (stderr)
        g_strstrip (stderr);

    parser = json_parser_new ();
    ret = parse_smartctl_error (status, stdout, stderr, parser, error);
    g_free (stdout);
    g_free (stderr);
    g_object_unref (parser);
    if (! ret) {
        g_prefix_error (error, "Error executing SMART self-test: ");
        return FALSE;
    }

    return TRUE;
}
