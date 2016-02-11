/*
 * Copyright (C) 2015  Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Vratislav Podzimek <vpodzime@redhat.com>
 */

#include <glib.h>
#include <math.h>
#include <string.h>
#include <libdevmapper.h>
#include <unistd.h>
#include <utils.h>
#include <gio/gio.h>

#include "lvm.h"

#define INT_FLOAT_EPS 1e-5
#define SECTOR_SIZE 512

GMutex global_config_lock;
static gchar *global_config_str = NULL;

#define LVM_BUS_NAME "com.redhat.lvmdbus1"
#define LVM_OBJ_PREFIX "/com/redhat/lvmdbus1"
#define MANAGER_OBJ "/com/redhat/lvmdbus1/Manager"
#define MANAGER_INTF "com.redhat.lvmdbus1.Manager"
#define JOB_OBJ_PREFIX "/com/redhat/lvmdbus1/Job/"
#define JOB_INTF "com.redhat.lvmdbus1.Job"
#define PV_OBJ_PREFIX LVM_OBJ_PREFIX"/Pv"
#define VG_OBJ_PREFIX LVM_OBJ_PREFIX"/Vg"
#define LV_OBJ_PREFIX LVM_OBJ_PREFIX"/Lv"
#define PV_INTF LVM_BUS_NAME".Pv"
#define VG_INTF LVM_BUS_NAME".Vg"
#define LV_CMN_INTF LVM_BUS_NAME".LvCommon"
#define LV_INTF LVM_BUS_NAME".Lv"
#define THPOOL_INTF LVM_BUS_NAME".ThinPool"
#define DBUS_TOP_IFACE "org.freedesktop.DBus"
#define DBUS_TOP_OBJ "/org/freedesktop/DBus"
#define DBUS_PROPS_IFACE "org.freedesktop.DBus.Properties"
#define DBUS_INTRO_IFACE "org.freedesktop.DBus.Introspectable"
#define DBUS_LONG_CALL_TIMEOUT 10000 /* msecs */
#define METHOD_CALL_TIMEOUT (DBUS_LONG_CALL_TIMEOUT / 2)

static GDBusConnection *bus = NULL;

/**
 * SECTION: lvm
 * @short_description: plugin for operations with LVM
 * @title: LVM
 * @include: lvm.h
 *
 * A plugin for operations with LVM. All sizes passed in/out to/from
 * the functions are in bytes.
 */

/**
 * bd_lvm_error_quark: (skip)
 */
GQuark bd_lvm_error_quark (void)
{
    return g_quark_from_static_string ("g-bd-lvm-error-quark");
}

BDLVMPVdata* bd_lvm_pvdata_copy (BDLVMPVdata *data) {
    BDLVMPVdata *new_data = g_new0 (BDLVMPVdata, 1);

    new_data->pv_name = g_strdup (data->pv_name);
    new_data->pv_uuid = g_strdup (data->pv_uuid);
    new_data->pv_free = data->pv_free;
    new_data->pe_start = data->pe_start;
    new_data->vg_name = g_strdup (data->vg_name);
    new_data->vg_size = data->vg_size;
    new_data->vg_free = data->vg_free;
    new_data->vg_extent_size = data->vg_extent_size;
    new_data->vg_extent_count = data->vg_extent_count;
    new_data->vg_free_count = data->vg_free_count;
    new_data->vg_pv_count = data->vg_pv_count;

    return new_data;
}

void bd_lvm_pvdata_free (BDLVMPVdata *data) {
    g_free (data->pv_name);
    g_free (data->pv_uuid);
    g_free (data->vg_name);
    g_free (data);
}

BDLVMVGdata* bd_lvm_vgdata_copy (BDLVMVGdata *data) {
    BDLVMVGdata *new_data = g_new0 (BDLVMVGdata, 1);

    new_data->name = g_strdup (data->name);
    new_data->uuid = g_strdup (data->uuid);
    new_data->size = data->size;
    new_data->free = data->free;
    new_data->extent_size = data->extent_size;
    new_data->extent_count = data->extent_count;
    new_data->free_count = data->free_count;
    new_data->pv_count = data->pv_count;
    return new_data;
}

void bd_lvm_vgdata_free (BDLVMVGdata *data) {
    g_free (data->name);
    g_free (data->uuid);
    g_free (data);
}

BDLVMLVdata* bd_lvm_lvdata_copy (BDLVMLVdata *data) {
    BDLVMLVdata *new_data = g_new0 (BDLVMLVdata, 1);

    new_data->lv_name = g_strdup (data->lv_name);
    new_data->vg_name = g_strdup (data->vg_name);
    new_data->uuid = g_strdup (data->uuid);
    new_data->size = data->size;
    new_data->attr = g_strdup (data->attr);
    new_data->segtype = g_strdup (data->segtype);
    return new_data;
}

void bd_lvm_lvdata_free (BDLVMLVdata *data) {
    g_free (data->lv_name);
    g_free (data->vg_name);
    g_free (data->uuid);
    g_free (data->attr);
    g_free (data->segtype);
    g_free (data);
}

BDLVMCacheStats* bd_lvm_cache_stats_copy (BDLVMCacheStats *data) {
    BDLVMCacheStats *new = g_new0 (BDLVMCacheStats, 1);

    new->block_size = data->block_size;
    new->cache_size = data->cache_size;
    new->cache_used = data->cache_used;
    new->md_block_size = data->md_block_size;
    new->md_size = data->md_size;
    new->md_used = data->md_used;
    new->read_hits = data->read_hits;
    new->read_misses = data->read_misses;
    new->write_hits = data->write_hits;
    new->write_misses = data->write_misses;
    new->mode = data->mode;

    return new;
}

void bd_lvm_cache_stats_free (BDLVMCacheStats *data) {
    g_free (data);
}

static gboolean setup_dbus_connection (GError **error) {
    gchar *addr = NULL;

    addr = g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SYSTEM, NULL, error);
    if (!addr) {
        g_critical ("Failed to get system bus address: %s\n", (*error)->message);
        return FALSE;
    }

    bus = g_dbus_connection_new_for_address_sync (addr,
                                                  G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|
                                                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                  NULL, NULL, error);

    g_free (addr);

    if (!bus || g_dbus_connection_is_closed (bus)) {
        g_critical ("Failed to create a new connection for the system bus: %s\n", (*error)->message);
        return FALSE;
    }

    g_dbus_connection_set_exit_on_close (bus, FALSE);

    return TRUE;
}

/**
 * check: (skip)
 */
gboolean check() {
    GVariant *ret = NULL;
    GVariant *real_ret = NULL;
    GVariantIter iter;
    GVariant *service = NULL;
    gboolean found = FALSE;
    GError *error = NULL;

    if (!bus && !setup_dbus_connection (&error)) {
        g_critical ("Failed to setup DBus connection: %s", error->message);
        return FALSE;
    }

    ret = g_dbus_connection_call_sync (bus, DBUS_TOP_IFACE, DBUS_TOP_OBJ, DBUS_TOP_IFACE,
                                       "ListNames", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                                       -1, NULL, &error);
    if (!ret) {
        g_critical ("Failed to get available DBus services: %s", error->message);
        return FALSE;
    }

    real_ret = g_variant_get_child_value (ret, 0);
    g_variant_unref (ret);

    g_variant_iter_init (&iter, real_ret);
    while (!found && (service = g_variant_iter_next_value (&iter))) {
        found = (g_strcmp0 (g_variant_get_string (service, NULL), LVM_BUS_NAME) == 0);
        g_variant_unref (service);
    }

    g_variant_unref (real_ret);

    return found;
}

/**
 * init: (skip)
 */
gboolean init() {
    GError *error = NULL;

    /* the check() call should create the DBus connection for us, but let's not
       completely rely on it */
    if (G_UNLIKELY(!bus) && !setup_dbus_connection (&error)) {
        g_critical ("Failed to setup DBus connection: %s", error->message);
        return FALSE;
    }

    return TRUE;
}

/* ====================== REMOVE THIS ===================================== */
static gboolean call_lvm_and_report_error (gchar **args, GError **error) {
    gboolean success = FALSE;
    guint i = 0;
    guint args_length = g_strv_length (args);

    /* don't allow global config string changes during the run */
    g_mutex_lock (&global_config_lock);

    /* allocate enough space for the args plus "lvm", "--config" and NULL */
    gchar **argv = g_new0 (gchar*, args_length + 3);

    /* construct argv from args with "lvm" prepended */
    argv[0] = "lvm";
    for (i=0; i < args_length; i++)
        argv[i+1] = args[i];
    argv[args_length + 1] = global_config_str ? g_strdup_printf("--config=%s", global_config_str) : NULL;
    argv[args_length + 2] = NULL;

    success = bd_utils_exec_and_report_error (argv, error);
    g_mutex_unlock (&global_config_lock);
    g_free (argv[args_length + 1]);
    g_free (argv);

    return success;
}

static gboolean call_lvm_and_capture_output (gchar **args, gchar **output, GError **error) {
    gboolean success = FALSE;
    guint i = 0;
    guint args_length = g_strv_length (args);

    /* don't allow global config string changes during the run */
    g_mutex_lock (&global_config_lock);

    /* allocate enough space for the args plus "lvm", "--config" and NULL */
    gchar **argv = g_new0 (gchar*, args_length + 3);

    /* construct argv from args with "lvm" prepended */
    argv[0] = "lvm";
    for (i=0; i < args_length; i++)
        argv[i+1] = args[i];
    argv[args_length + 1] = global_config_str ? g_strdup_printf("--config=%s", global_config_str) : NULL;
    argv[args_length + 2] = NULL;

    success = bd_utils_exec_and_capture_output (argv, output, error);
    g_mutex_unlock (&global_config_lock);
    g_free (argv[args_length + 1]);
    g_free (argv);

    return success;
}

/**
 * parse_lvm_vars:
 * @str: string to parse
 * @num_items: (out): number of parsed items
 *
 * Returns: (transfer full): a GHashTable containing key-value items parsed from the @string
 */
static GHashTable* parse_lvm_vars (gchar *str, guint *num_items) {
    GHashTable *table = NULL;
    gchar **items = NULL;
    gchar **item_p = NULL;
    gchar **key_val = NULL;

    table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    *num_items = 0;

    items = g_strsplit_set (str, " \t\n", 0);
    for (item_p=items; *item_p; item_p++) {
        key_val = g_strsplit (*item_p, "=", 2);
        if (g_strv_length (key_val) == 2) {
            /* we only want to process valid lines (with the '=' character) */
            g_hash_table_insert (table, key_val[0], key_val[1]);
            (*num_items)++;
        } else
            /* invalid line, just free key_val */
            g_strfreev (key_val);
    }

    g_strfreev (items);
    return table;
}

static BDLVMLVdata* get_lv_data_from_table (GHashTable *table, gboolean free_table) {
    BDLVMLVdata *data = g_new0 (BDLVMLVdata, 1);
    gchar *value = NULL;

    data->lv_name = g_strdup (g_hash_table_lookup (table, "LVM2_LV_NAME"));
    data->vg_name = g_strdup (g_hash_table_lookup (table, "LVM2_VG_NAME"));
    data->uuid = g_strdup (g_hash_table_lookup (table, "LVM2_LV_UUID"));

    value = (gchar*) g_hash_table_lookup (table, "LVM2_LV_SIZE");
    if (value)
        data->size = g_ascii_strtoull (value, NULL, 0);
    else
        data->size = 0;

    data->attr = g_strdup (g_hash_table_lookup (table, "LVM2_LV_ATTR"));
    data->segtype = g_strdup (g_hash_table_lookup (table, "LVM2_SEGTYPE"));

    if (free_table)
        g_hash_table_destroy (table);

    return data;
}
/* =============================== REMOVE THIS =================================== */

static gchar** get_existing_objects (gchar *obj_prefix, GError **error) {
    GVariant *intro_v = NULL;
    gchar *intro_data = NULL;
    GDBusNodeInfo *info = NULL;
    gchar **ret = NULL;
    GDBusNodeInfo **nodes;
    guint64 n_nodes = 0;
    guint64 i = 0;

    intro_v = g_dbus_connection_call_sync (bus, LVM_BUS_NAME, obj_prefix, DBUS_INTRO_IFACE,
                                           "Introspect", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                                           -1, NULL, error);

    g_variant_get (intro_v, "(s)", &intro_data);
    info = g_dbus_node_info_new_for_xml (intro_data, error);
    g_free (intro_data);

    for (nodes = info->nodes; (*nodes); nodes++)
        n_nodes++;

    ret = g_new0 (gchar*, n_nodes + 1);
    for (nodes = info->nodes, i=0; (*nodes); nodes++, i++) {
        ret[i] = g_strdup_printf ("%s/%s", obj_prefix, ((*nodes)->path));
    }
    ret[i] = NULL;

    g_dbus_node_info_unref (info);

    return ret;
}

static gchar* get_object_path (gchar *obj_id, GError **error) {
    GVariant *args = NULL;
    GVariant *ret = NULL;
    gchar *obj_path = NULL;

    args = g_variant_new ("(s)", obj_id);
    /* consumes (frees) the 'args' parameter */
    ret = g_dbus_connection_call_sync (bus, LVM_BUS_NAME, MANAGER_OBJ, MANAGER_INTF,
                                       "LookUpByLvmId", args, NULL, G_DBUS_CALL_FLAGS_NONE,
                                       -1, NULL, error);
    if (!ret)
        /* error is already set */
        return NULL;

    g_variant_get (ret, "(o)", &obj_path);
    g_variant_unref (ret);

    if (g_strcmp0 (obj_path, "/") == 0) {
        /* not a valid path (at least for us) */
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_NOEXIST,
                     "The object with LVM ID '%s' doesn't exist", obj_id);
        g_free (obj_path);
        return NULL;
    }

    return obj_path;
}

static GVariant* get_object_property (gchar *obj_path, gchar *iface, gchar *property, GError **error) {
    GVariant *args = NULL;
    GVariant *ret = NULL;
    GVariant *real_ret = NULL;

    args = g_variant_new ("(ss)", iface, property);

    /* consumes (frees) the 'args' parameter */
    ret = g_dbus_connection_call_sync (bus, LVM_BUS_NAME, obj_path, DBUS_PROPS_IFACE,
                                       "Get", args, NULL, G_DBUS_CALL_FLAGS_NONE,
                                       -1, NULL, error);
    if (!ret) {
        g_prefix_error (error, "Failed to get %s property of the %s object: ", property, obj_path);
        return NULL;
    }

    g_variant_get (ret, "(v)", &real_ret);
    g_variant_unref (ret);

    return real_ret;
}

static GVariant* get_lvm_object_property (gchar *obj_id, gchar *iface, gchar *property, GError **error) {
    gchar *obj_path = NULL;
    GVariant *ret = NULL;

    obj_path = get_object_path (obj_id, error);
    if (!obj_path)
        /* error is already set */
        return NULL;
    else {
        ret = get_object_property (obj_path, iface, property, error);
        g_free (obj_path);
        return ret;
    }
}

static GVariant* call_lvm_method (gchar *obj, gchar *intf, gchar *method, GVariant *params, GVariant *extra_params, GError **error) {
    GVariant *config = NULL;
    GVariant *param = NULL;
    GVariantIter iter;
    GVariantBuilder builder;
    GVariantBuilder extra_builder;
    GVariant *config_extra_params = NULL;
    GVariant *tmo = NULL;
    GVariant *all_params = NULL;
    GVariant *ret = NULL;
    gchar *params_str = NULL;

    /* don't allow global config string changes during the run */
    g_mutex_lock (&global_config_lock);

    if (global_config_str || extra_params) {
        if (global_config_str) {
            /* add the global config to the extra_params */
            g_variant_builder_init (&extra_builder, G_VARIANT_TYPE_DICTIONARY);

            config = g_variant_new ("s", global_config_str);
            g_variant_builder_add (&extra_builder, "{sv}", "--config", config);

            g_variant_iter_init (&iter, extra_params);
            while ((param = g_variant_iter_next_value (&iter))) {
                g_variant_builder_add_value (&extra_builder, param);
            }
            config_extra_params = g_variant_builder_end (&extra_builder);
            g_variant_builder_clear (&extra_builder);
        } else
            /* just use the extra_params */
            config_extra_params = extra_params;
    } else
        /* create an empty dictionary with the extra arguments */
        config_extra_params = g_variant_new_array (G_VARIANT_TYPE("{sv}"), NULL, 0);

    /* create new GVariant holding the given parameters with the global
       config and extra_params merged together appended */
    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);

    if (params) {
        /* add parameters */
        g_variant_iter_init (&iter, params);
        while ((param = g_variant_iter_next_value (&iter))) {
            g_variant_builder_add_value (&builder, param);
        }
    }

    /* XXX: remove this hack */
    if (g_strcmp0 (method, "Move") != 0) {
        /* add the timeout spec */
        tmo = g_variant_new ("i", METHOD_CALL_TIMEOUT);
        g_variant_builder_add_value (&builder, tmo);
    }

    /* add extra parameters including config */
    g_variant_builder_add_value (&builder, config_extra_params);

    all_params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    params_str = g_variant_print (all_params, FALSE);
    g_debug ("Calling the '%s.%s' method on the '%s' object with the following parameters: '%s'",
             intf, method, obj, params_str);
    /* now do the call with all the parameters */
    ret = g_dbus_connection_call_sync (bus, LVM_BUS_NAME, obj, intf, method, all_params,
                                       NULL, G_DBUS_CALL_FLAGS_NONE, DBUS_LONG_CALL_TIMEOUT, NULL, error);
    g_debug ("Called");

    g_mutex_unlock (&global_config_lock);

    if (!ret) {
        g_prefix_error (error, "Failed to call the '%s' method on the '%s' object: ", method, obj);
        return NULL;
    }

    return ret;
}

static gchar* call_lvm_method_sync (gchar *obj, gchar *intf, gchar *method, GVariant *params, GVariant *extra_params, GError **error) {
    GVariant *ret = NULL;
    gchar *obj_path = NULL;
    gchar *task_path = NULL;

    ret = call_lvm_method (obj, intf, method, params, extra_params, error);
    if (!ret) {
        g_debug ((*error)->message);
        return NULL;
    }
    if (g_variant_check_format_string (ret, "((oo))", TRUE)) {
        g_variant_get (ret, "((oo))", &obj_path, &task_path);
        if (g_strcmp0 (obj_path, "/") != 0) {
            g_debug ("Got path");
            /* got a valid result, just return it */
            g_variant_unref (ret);
            g_free (task_path);
            return obj_path;
        } else {
            g_debug ("Got task");
            g_variant_unref (ret);
            g_free (obj_path);
        }
    } else if (g_variant_check_format_string (ret, "(o)", TRUE)) {
        g_variant_get (ret, "(o)", &task_path);
        if (g_strcmp0 (task_path, "/") != 0) {
            g_debug ("Got task");
            g_variant_unref (ret);
        } else
            return NULL;
    } else {
        g_variant_unref (ret);
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_PARSE,
                     "Failed to parse the returned value!");
        return NULL;
    }

    g_debug ("waiting for task %s", task_path);
    ret = NULL;
    while (!ret && !(*error)) {
        ret = g_dbus_connection_call_sync (bus, LVM_BUS_NAME, task_path, JOB_INTF, "Wait", g_variant_new ("(i)", -1),
                                           NULL, G_DBUS_CALL_FLAGS_NONE, DBUS_LONG_CALL_TIMEOUT, NULL, error);
        if (!ret && g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
            /* let's wait longer */
            g_clear_error (error);
    }
    if (ret) {
        g_variant_unref (ret);
        ret = get_object_property (task_path, JOB_INTF, "Result", error);
        if (!ret) {
            g_prefix_error (error, "Getting result after waiting for '%s' method of the '%s' object failed: ",
                            method, obj);
            g_free (task_path);
            return NULL;
        } else {
            g_variant_get (ret, "s", &obj_path);
            g_variant_unref (ret);

            /* remove the job object and clean after ourselves */
            ret = g_dbus_connection_call_sync (bus, LVM_BUS_NAME, task_path, JOB_INTF, "Remove", NULL,
                                               NULL, G_DBUS_CALL_FLAGS_NONE, DBUS_LONG_CALL_TIMEOUT, NULL, error);
            if (ret)
                g_variant_unref (ret);
            if (*error)
                g_clear_error (error);

            g_free (task_path);
            return obj_path;
        }
    } else
        /* some real error */
        g_prefix_error (error, "Waiting for '%s' method of the '%s' object to finish failed: ",
                        method, obj);
    g_free (task_path);
    return NULL;
}

static gchar* call_lvm_obj_method_sync (gchar *obj_id, gchar *intf, gchar *method, GVariant *params, GVariant *extra_params, GError **error) {
    gchar *ret = NULL;
    gchar *obj_path = get_object_path (obj_id, error);
    if (!obj_path)
        return FALSE;

    ret = call_lvm_method_sync (obj_path, intf, method, params, extra_params, error);
    g_free (obj_path);

    return ret;
}

static gchar* call_lv_method_sync (gchar *vg_name, gchar *lv_name, gchar *method, GVariant *params, GVariant *extra_params, GError **error) {
    gchar *ret = NULL;
    gchar *obj_id = g_strdup_printf ("%s/%s", vg_name, lv_name);

    ret = call_lvm_obj_method_sync (obj_id, LV_INTF, method, params, extra_params, error);
    g_free (obj_id);

    return ret;
}

static gchar* call_thpool_method_sync (gchar *vg_name, gchar *pool_name, gchar *method, GVariant *params, GVariant *extra_params, GError **error) {
    gchar *ret = NULL;
    gchar *obj_id = g_strdup_printf ("%s/%s", vg_name, pool_name);

    ret = call_lvm_obj_method_sync (obj_id, THPOOL_INTF, method, params, extra_params, error);
    g_free (obj_id);

    return ret;
}

static GVariant* get_vg_property (gchar *vg_name, gchar *property, GError **error) __attribute__((unused));
static GVariant* get_vg_property (gchar *vg_name, gchar *property, GError **error) {
    GVariant *ret = NULL;

    ret = get_lvm_object_property (vg_name, VG_INTF, property, error);

    return ret;
}

static GVariant* get_lv_property (gchar *vg_name, gchar *lv_name, gchar *property, GError **error) {
    gchar *lv_spec = NULL;
    GVariant *ret = NULL;

    lv_spec = g_strdup_printf ("%s/%s", vg_name, lv_name);

    ret = get_lvm_object_property (lv_spec, LV_CMN_INTF, property, error);
    g_free (lv_spec);

    return ret;
}

static GVariant* get_object_properties (gchar *obj_path, gchar *iface, GError **error) {
    GVariant *args = NULL;
    GVariant *ret = NULL;
    GVariant *real_ret = NULL;

    args = g_variant_new ("(s)", iface);

    /* consumes (frees) the 'args' parameter */
    ret = g_dbus_connection_call_sync (bus, LVM_BUS_NAME, obj_path, DBUS_PROPS_IFACE,
                                       "GetAll", args, NULL, G_DBUS_CALL_FLAGS_NONE,
                                       -1, NULL, error);
    if (!ret) {
        g_prefix_error (error, "Failed to get properties of the %s object: ", obj_path);
        return NULL;
    }

    real_ret = g_variant_get_child_value (ret, 0);
    g_variant_unref (ret);

    return real_ret;
}

static GVariant* get_lvm_object_properties (gchar *obj_id, gchar *iface, GError **error) {
    GVariant *args = NULL;
    GVariant *ret = NULL;
    gchar *obj_path = NULL;

    args = g_variant_new ("(s)", obj_id);
    /* consumes (frees) the 'args' parameter */
    ret = g_dbus_connection_call_sync (bus, LVM_BUS_NAME, MANAGER_OBJ, MANAGER_INTF,
                                       "LookUpByLvmId", args, NULL, G_DBUS_CALL_FLAGS_NONE,
                                       -1, NULL, error);
    g_variant_get (ret, "(o)", &obj_path);
    g_variant_unref (ret);

    if (g_strcmp0 (obj_path, "/") == 0) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_NOEXIST,
                     "The object with LVM ID '%s' doesn't exist", obj_id);
        g_free (obj_path);
        return NULL;
    }

    ret = get_object_properties (obj_path, iface, error);
    g_free (obj_path);
    return ret;
}


static GVariant* get_pv_properties (gchar *pv_name, GError **error) {
    gchar *obj_id = NULL;
    GVariant *ret = NULL;

    if (!g_str_has_prefix (pv_name, "/dev/")) {
        obj_id = g_strdup_printf ("/dev/%s", pv_name);
        ret = get_lvm_object_properties (obj_id, PV_INTF, error);
        g_free (obj_id);
    } else
        ret = get_lvm_object_properties (pv_name, PV_INTF, error);

    return ret;
}

static GVariant* get_vg_properties (gchar *vg_name, GError **error) {
    GVariant *ret = NULL;

    ret = get_lvm_object_properties (vg_name, VG_INTF, error);

    return ret;
}

static GVariant* get_lv_properties (gchar *vg_name, gchar *lv_name, GError **error) {
    gchar *lvm_spec = NULL;
    GVariant *ret = NULL;

    lvm_spec = g_strdup_printf ("%s/%s", vg_name, lv_name);

    ret = get_lvm_object_properties (lvm_spec, LV_CMN_INTF, error);
    g_free (lvm_spec);

    return ret;
}

static BDLVMPVdata* get_pv_data_from_props (GVariant *props, GError **error) {
    BDLVMPVdata *data = g_new0 (BDLVMPVdata, 1);
    GVariantDict dict;
    gchar *value = NULL;
    GVariant *vg_props = NULL;

    g_variant_dict_init (&dict, props);

    g_variant_dict_lookup (&dict, "Name", "s", &(data->pv_name));
    g_variant_dict_lookup (&dict, "Uuid", "s", &(data->pv_uuid));
    g_variant_dict_lookup (&dict, "FreeBytes", "t", &(data->pv_free));
    g_variant_dict_lookup (&dict, "PeStart", "t", &(data->pe_start));

    /* returns an object path for the VG */
    g_variant_dict_lookup (&dict, "Vg", "s", &value);
    if (g_strcmp0 (value, "/") == 0) {
        /* no VG, the PV is not part of any VG */
        g_variant_dict_clear (&dict);
        return data;
    }

    vg_props = get_object_properties (value, VG_INTF, error);
    g_variant_dict_clear (&dict);
    if (!vg_props)
        return data;

    g_variant_dict_init (&dict, vg_props);
    g_variant_dict_lookup (&dict, "Name", "s", &(data->vg_name));
    g_variant_dict_lookup (&dict, "Uuid", "s", &(data->vg_uuid));
    g_variant_dict_lookup (&dict, "SizeBytes", "t", &(data->vg_size));
    g_variant_dict_lookup (&dict, "FreeBytes", "t", &(data->vg_free));
    g_variant_dict_lookup (&dict, "ExtentSizeBytes", "t", &(data->vg_extent_size));
    g_variant_dict_lookup (&dict, "ExtentCount", "t", &(data->vg_extent_count));
    g_variant_dict_lookup (&dict, "FreeCount", "t", &(data->vg_free_count));
    g_variant_dict_lookup (&dict, "PvCount", "t", &(data->vg_pv_count));

    g_variant_dict_clear (&dict);
    g_variant_unref (vg_props);

    return data;
}

static BDLVMVGdata* get_vg_data_from_props (GVariant *props, GError **error __attribute__((unused))) {
    BDLVMVGdata *data = g_new0 (BDLVMVGdata, 1);
    GVariantDict dict;

    g_variant_dict_init (&dict, props);

    g_variant_dict_lookup (&dict, "Name", "s", &(data->name));
    g_variant_dict_lookup (&dict, "Uuid", "s", &(data->uuid));

    g_variant_dict_lookup (&dict, "SizeBytes", "t", &(data->size));
    g_variant_dict_lookup (&dict, "FreeBytes", "t", &(data->free));
    g_variant_dict_lookup (&dict, "ExtentSizeBytes", "t", &(data->extent_size));
    g_variant_dict_lookup (&dict, "ExtentCount", "t", &(data->extent_count));
    g_variant_dict_lookup (&dict, "FreeCount", "t", &(data->free_count));
    g_variant_dict_lookup (&dict, "PvCount", "t", &(data->pv_count));

    g_variant_dict_clear (&dict);

    return data;
}

static BDLVMLVdata* get_lv_data_from_props (GVariant *props, GError **error __attribute__((unused))) {
    BDLVMLVdata *data = g_new0 (BDLVMLVdata, 1);
    GVariantDict dict;
    gchar *value = NULL;
    GVariant *vg_name = NULL;

    g_variant_dict_init (&dict, props);

    g_variant_dict_lookup (&dict, "Name", "s", &(data->lv_name));
    g_variant_dict_lookup (&dict, "Uuid", "s", &(data->uuid));
    g_variant_dict_lookup (&dict, "SizeBytes", "t", &(data->size));
    g_variant_dict_lookup (&dict, "Attr", "s", &(data->attr));
    g_variant_dict_lookup (&dict, "SegType", "s", &(data->segtype));

    /* returns an object path for the VG */
    g_variant_dict_lookup (&dict, "Vg", "o", &value);

    vg_name = get_object_property (value, VG_INTF, "Name", error);
    g_variant_get (vg_name, "s", &(data->vg_name));

    g_variant_dict_clear (&dict);
    g_variant_unref (vg_name);

    return data;
}

static GVariant* create_size_str_param (guint64 size, gchar *unit) {
    gchar *str = NULL;

    str = g_strdup_printf ("%"G_GUINT64_FORMAT"%s", size, unit ? unit : "");
    return g_variant_new_take_string (str);
}

/**
 * bd_lvm_is_supported_pe_size:
 * @size: size (in bytes) to test
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the given size is supported physical extent size or not
 */
gboolean bd_lvm_is_supported_pe_size (guint64 size, GError **error __attribute__((unused))) {
    return (((size % 2) == 0) && (size >= (BD_LVM_MIN_PE_SIZE)) && (size <= (BD_LVM_MAX_PE_SIZE)));
}

/**
 * bd_lvm_get_supported_pe_sizes:
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full) (array zero-terminated=1): list of supported PE sizes
 */
guint64 *bd_lvm_get_supported_pe_sizes (GError **error __attribute__((unused))) {
    guint8 i;
    guint64 val = BD_LVM_MIN_PE_SIZE;
    guint8 num_items = ((guint8) round (log2 ((double) BD_LVM_MAX_PE_SIZE))) - ((guint8) round (log2 ((double) BD_LVM_MIN_PE_SIZE))) + 2;
    guint64 *ret = g_new0 (guint64, num_items);

    for (i=0; (val <= BD_LVM_MAX_PE_SIZE); i++, val = val * 2)
        ret[i] = val;

    ret[num_items-1] = 0;

    return ret;
}

/**
 * bd_lvm_get_max_lv_size:
 * @error: (out): place to store error (if any)
 *
 * Returns: maximum LV size in bytes
 */
guint64 bd_lvm_get_max_lv_size (GError **error __attribute__((unused))) {
    return BD_LVM_MAX_LV_SIZE;
}

/**
 * bd_lvm_round_size_to_pe:
 * @size: size to be rounded
 * @pe_size: physical extent (PE) size or 0 to use the default
 * @roundup: whether to round up or down (ceil or floor)
 * @error: (out): place to store error (if any)
 *
 * Returns: @size rounded to @pe_size according to the @roundup
 *
 * Rounds given @size up/down to a multiple of @pe_size according to the value
 * of the @roundup parameter. If the rounded value is too big to fit in the
 * return type, the result is rounded down (floored) regardless of the @roundup
 * parameter.
 */
guint64 bd_lvm_round_size_to_pe (guint64 size, guint64 pe_size, gboolean roundup, GError **error __attribute__((unused))) {
    pe_size = RESOLVE_PE_SIZE(pe_size);
    guint64 delta = size % pe_size;
    if (delta == 0)
        return size;

    if (roundup && (((G_MAXUINT64 - (pe_size - delta)) >= size)))
        return size + (pe_size - delta);
    else
        return size - delta;
}

/**
 * bd_lvm_get_lv_physical_size:
 * @lv_size: LV size
 * @pe_size: PE size
 * @error: (out): place to store error (if any)
 *
 * Returns: space taken on disk(s) by the LV with given @size
 *
 * Gives number of bytes needed for an LV with the size @lv_size on an LVM stack
 * using given @pe_size.
 */
guint64 bd_lvm_get_lv_physical_size (guint64 lv_size, guint64 pe_size, GError **error) {
    /* TODO: should take into account mirroring and RAID in general? */

    /* add one PE for metadata */
    pe_size = RESOLVE_PE_SIZE(pe_size);

    return bd_lvm_round_size_to_pe (lv_size, pe_size, TRUE, error) + pe_size;
}

/**
 * bd_lvm_get_thpool_padding:
 * @size: size of the thin pool
 * @pe_size: PE size or 0 if the default value should be used
 * @included: if padding is already included in the size
 * @error: (out): place to store error (if any)
 *
 * Returns: size of the padding needed for a thin pool with the given @size
 *         according to the @pe_size and @included
 */
guint64 bd_lvm_get_thpool_padding (guint64 size, guint64 pe_size, gboolean included, GError **error) {
    guint64 raw_md_size;
    pe_size = RESOLVE_PE_SIZE(pe_size);

    if (included)
        raw_md_size = (guint64) ceil (size * THPOOL_MD_FACTOR_EXISTS);
    else
        raw_md_size = (guint64) ceil (size * THPOOL_MD_FACTOR_NEW);

    return MIN (bd_lvm_round_size_to_pe(raw_md_size, pe_size, TRUE, error),
                bd_lvm_round_size_to_pe(BD_LVM_MAX_THPOOL_MD_SIZE, pe_size, TRUE, error));
}

/**
 * bd_lvm_is_valid_thpool_md_size:
 * @size: the size to be tested
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the given size is a valid thin pool metadata size or not
 */
gboolean bd_lvm_is_valid_thpool_md_size (guint64 size, GError **error __attribute__((unused))) {
    return ((BD_LVM_MIN_THPOOL_MD_SIZE <= size) && (size <= BD_LVM_MAX_THPOOL_MD_SIZE));
}

/**
 * bd_lvm_is_valid_thpool_chunk_size:
 * @size: the size to be tested
 * @discard: whether discard/TRIM is required to be supported or not
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the given size is a valid thin pool chunk size or not
 */
gboolean bd_lvm_is_valid_thpool_chunk_size (guint64 size, gboolean discard, GError **error __attribute__((unused))) {
    gdouble size_log2 = 0.0;

    if ((size < BD_LVM_MIN_THPOOL_CHUNK_SIZE) || (size > BD_LVM_MAX_THPOOL_CHUNK_SIZE))
        return FALSE;

    /* To support discard, chunk size must be a power of two. Otherwise it must be a
       multiple of 64 KiB. */
    if (discard) {
        size_log2 = log2 ((double) size);
        return ABS (((int) round (size_log2)) - size_log2) <= INT_FLOAT_EPS;
    } else
        return (size % (64 KiB)) == 0;
}

/**
 * bd_lvm_pvcreate:
 * @device: the device to make PV from
 * @data_alignment: data (first PE) alignment or 0 to use the default
 * @metadata_size: size of the area reserved for metadata or 0 to use the default
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the PV was successfully created or not
 */
gboolean bd_lvm_pvcreate (gchar *device, guint64 data_alignment, guint64 metadata_size, GError **error) {
    GVariantBuilder builder;
    GVariant *param = NULL;
    GVariant *params = NULL;
    GVariant *extra_params = NULL;

    if (data_alignment != 0 || metadata_size != 0) {
        g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
        if (data_alignment != 0) {
            param = create_size_str_param (data_alignment, "b");
            g_variant_builder_add (&builder, "{sv}", "dataalignment", param);
        }

        if (metadata_size != 0) {
            param = create_size_str_param (metadata_size, "b");
            g_variant_builder_add (&builder, "{sv}", "metadatasize", param);
        }
        extra_params = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);
    }

    params = g_variant_new ("(s)", device);

    call_lvm_method_sync (MANAGER_OBJ, MANAGER_INTF, "PvCreate", params, extra_params, error);
    return ((*error) == NULL);
}

/**
 * bd_lvm_pvresize:
 * @device: the device to resize
 * @size: the new requested size of the PV or 0 if it should be adjusted to device's size
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the PV's size was successfully changed or not
 *
 * If given @size different from 0, sets the PV's size to the given value (see
 * pvresize(8)). If given @size 0, adjusts the PV's size to the underlaying
 * block device's size.
 */
gboolean bd_lvm_pvresize (gchar *device, guint64 size, GError **error) {
    GVariant *params = NULL;
    gchar *obj_path = get_object_path (device, error);
    if (!obj_path)
        return FALSE;

    params = g_variant_new ("(u)", size);
    call_lvm_method_sync (obj_path, PV_INTF, "ReSize", params, NULL, error);

    return (*error == NULL);
}

/**
 * bd_lvm_pvremove:
 * @device: the PV device to be removed/destroyed
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the PV was successfully removed/destroyed or not
 */
gboolean bd_lvm_pvremove (gchar *device, GError **error) {
    GVariantBuilder builder;
    GVariant *params = NULL;

    /* one has to be really persuasive to remove a PV (the double --force is not
       bug, at least not in this code) */
    g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
    g_variant_builder_add (&builder, "{sv}", "-ff", g_variant_new ("s", ""));
    g_variant_builder_add (&builder, "{sv}", "--yes", g_variant_new ("s", ""));

    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);
    params = g_variant_new ("(v)", params);
    call_lvm_obj_method_sync (device, PV_INTF, "Remove", NULL, params, error);

    return (*error == NULL);
}

/**
 * bd_lvm_pvmove:
 * @src: the PV device to move extents off of
 * @dest: (allow-none): the PV device to move extents onto or %NULL
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the extents from the @src PV where successfully moved or not
 *
 * If @dest is %NULL, VG allocation rules are used for the extents from the @src
 * PV (see pvmove(8)).
 */
gboolean bd_lvm_pvmove (gchar *src, gchar *dest, GError **error) {
    GVariant *prop = NULL;
    gchar *src_path = NULL;
    gchar *dest_path = NULL;
    gchar *vg_obj_path = NULL;
    GVariantBuilder builder;
    GVariantType *type = NULL;
    GVariant *dest_var = NULL;
    GVariant *params = NULL;

    src_path = get_object_path (src, error);
    if (!src_path || (g_strcmp0 (src_path, "/") == 0)) {
        if (!(*error))
            g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_NOEXIST,
                         "The source PV '%s' doesn't exist", src);
        return FALSE;
    }
    if (dest) {
        dest_path = get_object_path (dest, error);
        if (!dest_path || (g_strcmp0 (dest_path, "/") == 0)) {
            if (!(*error))
                g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_NOEXIST,
                             "The destination PV '%s' doesn't exist", dest);
            return FALSE;
        }
    }
    prop = get_object_property (src_path, PV_INTF, "Vg", error);
    if (!prop) {
        g_free (src_path);
        return FALSE;
    }
    g_variant_get (prop, "s", &vg_obj_path);

    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    g_variant_builder_add_value (&builder, g_variant_new ("s", src_path));
    g_variant_builder_add_value (&builder, g_variant_new ("(tt)", 0, 0));
    if (dest) {
        dest_var = g_variant_new ("(ott)", dest_path, 0, 0);
        g_variant_builder_add_value (&builder, g_variant_new_array (NULL, &dest_var, 1));
    } else {
        type = g_variant_type_new ("a(ott)");
        g_variant_builder_add_value (&builder, g_variant_new_array (type, NULL, 0));
        g_variant_type_free (type);
    }
    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    call_lvm_method_sync (vg_obj_path, VG_INTF, "Move", params, NULL, error);

    g_free (src_path);
    g_free (dest_path);
    g_free (vg_obj_path);
    return ((*error) == NULL);
}

/**
 * bd_lvm_pvscan:
 * @device: (allow-none): the device to scan for PVs or %NULL
 * @update_cache: whether to update the lvmetad cache or not
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the system or @device was successfully scanned for PVs or not
 *
 * The @device argument is used only if @update_cache is %TRUE. Otherwise the
 * whole system is scanned for PVs.
 */
gboolean bd_lvm_pvscan (gchar *device, gboolean update_cache, GError **error) {
    GVariantBuilder builder;
    GVariantType *type = NULL;
    GVariant *params = NULL;
    GVariant *device_var = NULL;

    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    /* activate LVs if updating the cache, update the cache and specify the
       device (if any) */
    g_variant_builder_add_value (&builder, g_variant_new_boolean (update_cache));
    g_variant_builder_add_value (&builder, g_variant_new_boolean (update_cache));
    if (update_cache && device) {
        device_var = g_variant_new ("o", device);
        g_variant_builder_add_value (&builder, g_variant_new_array (NULL, &device_var, 1));
    } else {
        type = g_variant_type_new ("ao");
        g_variant_builder_add_value (&builder, g_variant_new_array (type, NULL, 0));
        g_variant_type_free (type);
    }
    /* (major, minor)`s, we never specify them */
    type = g_variant_type_new ("a(ii)");
    g_variant_builder_add_value (&builder, g_variant_new_array (type, NULL, 0));
    g_variant_type_free (type);

    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    call_lvm_method_sync (MANAGER_OBJ, MANAGER_INTF, "PvScan", params, NULL, error);
    return ((*error) == NULL);
}

/**
 * bd_lvm_pvinfo:
 * @device: a PV to get information about or %NULL
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): information about the PV on the given @device or
 * %NULL in case of error (the @error) gets populated in those cases)
 */
BDLVMPVdata* bd_lvm_pvinfo (gchar *device, GError **error) {
    GVariant *props = NULL;
    BDLVMPVdata *ret = NULL;

    props = get_pv_properties (device, error);
    if (!props)
        /* the error is already populated */
        return NULL;

    ret = get_pv_data_from_props (props, error);
    g_variant_unref (props);

    return ret;
}

/**
 * bd_lvm_pvs:
 * @error: (out): place to store error (if any)
 *
 * Returns: (array zero-terminated=1): information about PVs found in the system
 */
BDLVMPVdata** bd_lvm_pvs (GError **error) {
    gchar **objects = NULL;
    guint64 n_pvs = 0;
    GVariant *props = NULL;
    BDLVMPVdata **ret = NULL;
    guint64 i = 0;

    objects = get_existing_objects (PV_OBJ_PREFIX, error);
    if (!objects) {
        if (!(*error)) {
            /* no PVs */
            ret = g_new0 (BDLVMPVdata*, 1);
            ret[0] = NULL;
            return ret;
        } else
            /* error is already populated */
            return NULL;
    }

    n_pvs = g_strv_length (objects);

    /* now create the return value -- NULL-terminated array of BDLVMPVdata */
    ret = g_new0 (BDLVMPVdata*, n_pvs + 1);
    for (i=0; i < n_pvs; i++) {
        props = get_object_properties (objects[i], PV_INTF, error);
        if (!props) {
            g_strfreev (objects);
            g_free (ret);
            return NULL;
        }
        ret[i] = get_pv_data_from_props (props, error);
        if (!(ret[i])) {
            g_strfreev (objects);
            g_free (ret);
            return NULL;
        }
    }
    ret[i] = NULL;

    g_strfreev (objects);
    return ret;
}

/**
 * bd_lvm_vgcreate:
 * @name: name of the newly created VG
 * @pv_list: (array zero-terminated=1): list of PVs the newly created VG should use
 * @pe_size: PE size or 0 if the default value should be used
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the VG @name was successfully created or not
 */
gboolean bd_lvm_vgcreate (gchar *name, gchar **pv_list, guint64 pe_size, GError **error) {
    GVariantBuilder builder;
    gchar *path = NULL;
    gchar **pv = NULL;
    GVariant *pvs = NULL;
    GVariant *params = NULL;
    GVariant *extra = NULL;

    /* build the array of PVs (object paths) */
    g_variant_builder_init (&builder, G_VARIANT_TYPE_OBJECT_PATH_ARRAY);
    for (pv = pv_list; *pv; pv++) {
        path = get_object_path (*pv, error);
        if (!path) {
            g_variant_builder_clear (&builder);
            return FALSE;
        }
        g_variant_builder_add_value (&builder, g_variant_new ("o", path));
    }
    pvs = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    /* build the params tuple */
    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    g_variant_builder_add_value (&builder, g_variant_new ("s", name));
    g_variant_builder_add_value (&builder, pvs);
    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    /* pe_size needs to go to extra params */
    pe_size = RESOLVE_PE_SIZE (pe_size);
    g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
    g_variant_builder_add_value (&builder, g_variant_new ("{sv}", "--physicalextentsize", create_size_str_param (pe_size, "b")));
    extra = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    call_lvm_method_sync (MANAGER_OBJ, MANAGER_INTF, "VgCreate", params, extra, error);
    return ((*error) == NULL);
}

/**
 * bd_lvm_vgremove:
 * @vg_name: name of the to be removed VG
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the VG was successfully removed or not
 */
gboolean bd_lvm_vgremove (gchar *vg_name, GError **error) {
    call_lvm_obj_method_sync (vg_name, VG_INTF, "Remove", NULL, NULL, error);
    return ((*error) == NULL);
}

/**
 * bd_lvm_vgactivate:
 * @vg_name: name of the to be activated VG
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the VG was successfully activated or not
 */
gboolean bd_lvm_vgactivate (gchar *vg_name, GError **error) {
    GVariant *params = g_variant_new ("(t)", 0);
    call_lvm_obj_method_sync (vg_name, VG_INTF, "Activate", params, NULL, error);
    return ((*error) == NULL);
}

/**
 * bd_lvm_vgdeactivate:
 * @vg_name: name of the to be deactivated VG
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the VG was successfully deactivated or not
 */
gboolean bd_lvm_vgdeactivate (gchar *vg_name, GError **error) {
    GVariant *params = g_variant_new ("(t)", 0);
    call_lvm_obj_method_sync (vg_name, VG_INTF, "Deactivate", params, NULL, error);
    return ((*error) == NULL);
}

/**
 * bd_lvm_vgextend:
 * @vg_name: name of the to be extended VG
 * @device: PV device to extend the @vg_name VG with
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the VG @vg_name was successfully extended with the given @device or not.
 */
gboolean bd_lvm_vgextend (gchar *vg_name, gchar *device, GError **error) {
    gchar *pv = NULL;
    GVariant *pv_var = NULL;
    GVariant *pvs = NULL;
    GVariant *params = NULL;

    pv = get_object_path (device, error);
    if (!pv)
        return FALSE;

    pv_var = g_variant_new ("o", pv);
    pvs = g_variant_new_array (NULL, &pv_var, 1);
    params = g_variant_new_tuple (&pvs, 1);
    call_lvm_obj_method_sync (vg_name, VG_INTF, "Extend", params, NULL, error);
    g_free (pv);
    return ((*error) == NULL);
}

/**
 * bd_lvm_vgreduce:
 * @vg_name: name of the to be reduced VG
 * @device: (allow-none): PV device the @vg_name VG should be reduced of or %NULL
 *                        if the VG should be reduced of the missing PVs
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the VG @vg_name was successfully reduced of the given @device or not
 *
 * Note: This function does not move extents off of the PV before removing
 *       it from the VG. You must do that first by calling #bd_lvm_pvmove.
 */
gboolean bd_lvm_vgreduce (gchar *vg_name, gchar *device, GError **error) {
    gchar *pv = NULL;
    GVariantBuilder builder;
    GVariantType *type = NULL;
    GVariant *pv_var = NULL;
    GVariant *params = NULL;
    GVariant *extra = NULL;

    pv = get_object_path (device, error);
    if (!pv)
        return FALSE;

    pv_var = g_variant_new ("o", pv);
    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    if (device) {
        /* do not remove missing */
        g_variant_builder_add_value (&builder, g_variant_new_boolean (FALSE));
        g_variant_builder_add_value (&builder, g_variant_new_array (NULL, &pv_var, 1));
        params = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);
    } else {
        /* remove missing */
        g_variant_builder_add_value (&builder, g_variant_new_boolean (TRUE));
        type = g_variant_type_new ("ao");
        g_variant_builder_add_value (&builder, g_variant_new_array (type, NULL, 0));
        g_variant_type_free (type);
        params = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);

        g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
        g_variant_builder_add_value (&builder, g_variant_new ("{sv}", "--force", g_variant_new ("s", "")));
        extra = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);
    }

    call_lvm_obj_method_sync (vg_name, VG_INTF, "Reduce", params, extra, error);
    g_free (pv);
    return ((*error) == NULL);
}

/**
 * bd_lvm_vginfo:
 * @vg_name: a VG to get information about
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): information about the @vg_name VG or %NULL in case
 * of error (the @error) gets populated in those cases)
 */
BDLVMVGdata* bd_lvm_vginfo (gchar *vg_name, GError **error) {
    GVariant *props = NULL;
    BDLVMVGdata *ret = NULL;

    props = get_vg_properties (vg_name, error);
    if (!props)
        /* the error is already populated */
        return NULL;

    ret = get_vg_data_from_props (props, error);
    g_variant_unref (props);

    return ret;
}

/**
 * bd_lvm_vgs:
 * @error: (out): place to store error (if any)
 *
 * Returns: (array zero-terminated=1): information about VGs found in the system
 */
BDLVMVGdata** bd_lvm_vgs (GError **error) {
    gchar **objects = NULL;
    guint64 n_vgs = 0;
    GVariant *props = NULL;
    BDLVMVGdata **ret = NULL;
    guint64 i = 0;

    objects = get_existing_objects (VG_OBJ_PREFIX, error);
    if (!objects) {
        if (!(*error)) {
            /* no VGs */
            ret = g_new0 (BDLVMVGdata*, 1);
            ret[0] = NULL;
            return ret;
        } else
            /* error is already populated */
            return NULL;
    }

    n_vgs = g_strv_length (objects);

    /* now create the return value -- NULL-terminated array of BDLVMVGdata */
    ret = g_new0 (BDLVMVGdata*, n_vgs + 1);
    for (i=0; i < n_vgs; i++) {
        props = get_object_properties (objects[i], VG_INTF, error);
        if (!props) {
            g_strfreev (objects);
            g_free (ret);
            return NULL;
        }
        ret[i] = get_vg_data_from_props (props, error);
        if (!(ret[i])) {
            g_strfreev (objects);
            g_free (ret);
            return NULL;
        }
    }
    ret[i] = NULL;

    g_strfreev (objects);
    return ret;
}

/**
 * bd_lvm_lvorigin:
 * @vg_name: name of the VG containing the queried LV
 * @lv_name: name of the queried LV
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): the origin volume for the @vg_name/@lv_name LV or
 * %NULL if failed to determine (@error) is set in those cases)
 */
gchar* bd_lvm_lvorigin (gchar *vg_name, gchar *lv_name, GError **error) {
    GVariant *prop = NULL;
    gchar *obj_path = NULL;
    gchar *ret = NULL;

    prop = get_lv_property (vg_name, lv_name, "OriginLv", error);
    if (!prop)
        return NULL;
    g_variant_get (prop, "o", &obj_path);
    g_variant_unref (prop);

    if (g_strcmp0 (obj_path, "/") == 0) {
        /* no origin LV */
        g_free (obj_path);
        return NULL;
    }
    prop = get_object_property (obj_path, LV_CMN_INTF, "Name", error);
    if (!prop) {
        g_free (obj_path);
        return NULL;
    }

    g_variant_get (prop, "s", &ret);
    g_variant_unref (prop);

    return ret;
}

/**
 * bd_lvm_lvcreate:
 * @vg_name: name of the VG to create a new LV in
 * @lv_name: name of the to-be-created LV
 * @size: requested size of the new LV
 * @type: (allow-none): type of the new LV ("striped", "raid1",..., see lvcreate (8))
 * @pv_list: (allow-none) (array zero-terminated=1): list of PVs the newly created LV should use or %NULL
 * if not specified
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the given @vg_name/@lv_name LV was successfully created or not
 */
gboolean bd_lvm_lvcreate (gchar *vg_name, gchar *lv_name, guint64 size, gchar *type, gchar **pv_list, GError **error) {
    GVariantBuilder builder;
    gchar *path = NULL;
    gchar **pv = NULL;
    GVariant *pvs = NULL;
    GVariantType *var_type = NULL;
    GVariant *params = NULL;
    GVariant *extra = NULL;

    /* build the array of PVs (object paths) */
    if (pv_list) {
        g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);
        for (pv = pv_list; *pv; pv++) {
            path = get_object_path (*pv, error);
            if (!path) {
                g_variant_builder_clear (&builder);
                return FALSE;
            }
            g_variant_builder_add_value (&builder, g_variant_new ("(ott)", path, 0, 0));
        }
        pvs = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);
    } else {
        var_type = g_variant_type_new ("a(ott)");
        pvs = g_variant_new_array (var_type, NULL, 0);
        g_variant_type_free (var_type);
    }

    /* build the params tuple */
    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    g_variant_builder_add_value (&builder, g_variant_new ("s", lv_name));
    g_variant_builder_add_value (&builder, g_variant_new ("t", size));
    g_variant_builder_add_value (&builder, pvs);
    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    if (type) {
        /* and now the extra params */
        g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
        if (pv_list && g_strcmp0 (type, "striped") == 0)
            g_variant_builder_add_value (&builder, g_variant_new ("{sv}", "stripes", g_variant_new ("i", g_strv_length (pv_list))));
        else
            g_variant_builder_add_value (&builder, g_variant_new ("{sv}", "type", g_variant_new ("s", type)));
        extra = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);
    }

    call_lvm_obj_method_sync (vg_name, VG_INTF, "LvCreate", params, extra, error);
    return ((*error) == NULL);
}

/**
 * bd_lvm_lvremove:
 * @vg_name: name of the VG containing the to-be-removed LV
 * @lv_name: name of the to-be-removed LV
 * @force: whether to force removal or not
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @vg_name/@lv_name LV was successfully removed or not
 */
gboolean bd_lvm_lvremove (gchar *vg_name, gchar *lv_name, gboolean force, GError **error) {
    GVariantBuilder builder;
    GVariant *extra = NULL;

    if (force) {
        g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
        g_variant_builder_add (&builder, "{sv}", "--force", g_variant_new ("s", ""));
        g_variant_builder_add (&builder, "{sv}", "--yes", g_variant_new ("s", ""));

        extra = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);
        extra = g_variant_new ("(v)", extra);
    }
    call_lv_method_sync (vg_name, lv_name, "Remove", NULL, extra, error);

    return (*error == NULL);
}

/**
 * bd_lvm_lvresize:
 * @vg_name: name of the VG containing the to-be-resized LV
 * @lv_name: name of the to-be-resized LV
 * @size: the requested new size of the LV
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @vg_name/@lv_name LV was successfully resized or not
 */
gboolean bd_lvm_lvresize (gchar *vg_name, gchar *lv_name, guint64 size, GError **error) {
    GVariantBuilder builder;
    GVariantType *type = NULL;
    GVariant *params = NULL;

    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    g_variant_builder_add_value (&builder, g_variant_new ("t", size));
    type = g_variant_type_new ("a(ott)");
    g_variant_builder_add_value (&builder, g_variant_new_array (type, NULL, 0));
    g_variant_type_free (type);
    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    call_lv_method_sync (vg_name, lv_name, "Resize", params, NULL, error);
    return (*error == NULL);
}

/**
 * bd_lvm_lvactivate:
 * @vg_name: name of the VG containing the to-be-activated LV
 * @lv_name: name of the to-be-activated LV
 * @ignore_skip: whether to ignore the skip flag or not
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @vg_name/@lv_name LV was successfully activated or not
 */
gboolean bd_lvm_lvactivate (gchar *vg_name, gchar *lv_name, gboolean ignore_skip, GError **error) {
    GVariant *params = g_variant_new ("(t)", 0);
    GVariantBuilder builder;
    GVariant *extra = NULL;

    if (ignore_skip) {
        g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
        g_variant_builder_add (&builder, "{sv}", "-K", g_variant_new ("s", ""));
        extra = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);
    }
    call_lv_method_sync (vg_name, lv_name, "Activate", params, extra, error);

    return (*error == NULL);
}

/**
 * bd_lvm_lvdeactivate:
 * @vg_name: name of the VG containing the to-be-deactivated LV
 * @lv_name: name of the to-be-deactivated LV
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @vg_name/@lv_name LV was successfully deactivated or not
 */
gboolean bd_lvm_lvdeactivate (gchar *vg_name, gchar *lv_name, GError **error) {
    GVariant *params = g_variant_new ("(t)", 0);
    call_lv_method_sync (vg_name, lv_name, "Deactivate", params, NULL, error);
    return (*error == NULL);
}

/**
 * bd_lvm_lvsnapshotcreate:
 * @vg_name: name of the VG containing the LV a new snapshot should be created of
 * @origin_name: name of the LV a new snapshot should be created of
 * @snapshot_name: name fo the to-be-created snapshot
 * @size: requested size for the snapshot
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @snapshot_name snapshot of the @vg_name/@origin_name LV
 * was successfully created or not.
 */
gboolean bd_lvm_lvsnapshotcreate (gchar *vg_name, gchar *origin_name, gchar *snapshot_name, guint64 size, GError **error) {
    GVariantBuilder builder;
    GVariant *params = NULL;

    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    g_variant_builder_add_value (&builder, g_variant_new ("s", snapshot_name));
    g_variant_builder_add_value (&builder, g_variant_new ("t", size));
    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    call_lv_method_sync (vg_name, origin_name, "Snapshot", params, NULL, error);

    return (*error == NULL);
}

/**
 * bd_lvm_lvsnapshotmerge:
 * @vg_name: name of the VG containing the to-be-merged LV snapshot
 * @snapshot_name: name of the to-be-merged LV snapshot
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @vg_name/@snapshot_name LV snapshot was successfully merged or not
 */
gboolean bd_lvm_lvsnapshotmerge (gchar *vg_name, gchar *snapshot_name, GError **error) {
    gchar *args[4] = {"lvconvert", "--merge", NULL, NULL};
    gboolean success = FALSE;

    args[2] = g_strdup_printf ("%s/%s", vg_name, snapshot_name);

    success = call_lvm_and_report_error (args, error);
    g_free (args[2]);

    return success;
}

/**
 * bd_lvm_lvinfo:
 * @vg_name: name of the VG that contains the LV to get information about
 * @lv_name: name of the LV to get information about
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): information about the @vg_name/@lv_name LV or %NULL in case
 * of error (the @error) gets populated in those cases)
 */
BDLVMLVdata* bd_lvm_lvinfo (gchar *vg_name, gchar *lv_name, GError **error) {
    GVariant *props = NULL;
    BDLVMLVdata *ret = NULL;

    props = get_lv_properties (vg_name, lv_name, error);
    if (!props)
        /* the error is already populated */
        return NULL;

    ret = get_lv_data_from_props (props, error);
    g_variant_unref (props);

    return ret;
}

/**
 * bd_lvm_lvs:
 * @vg_name: (allow-none): name of the VG to get information about LVs from
 * @error: (out): place to store error (if any)
 *
 * Returns: (array zero-terminated=1): information about LVs found in the given
 * @vg_name VG or in system if @vg_name is %NULL
 */
BDLVMLVdata** bd_lvm_lvs (gchar *vg_name, GError **error) {
    gchar *args[11] = {"lvs", "--noheadings", "--nosuffix", "--nameprefixes",
                       "--unquoted", "--units=b", "-a",
                       "-o", "vg_name,lv_name,lv_uuid,lv_size,lv_attr,segtype",
                       NULL, NULL};

    GHashTable *table = NULL;
    gboolean success = FALSE;
    gchar *output = NULL;
    gchar **lines = NULL;
    gchar **lines_p = NULL;
    guint num_items;
    GPtrArray *lvs = g_ptr_array_new ();
    BDLVMLVdata *lvdata = NULL;
    BDLVMLVdata **ret = NULL;
    guint64 i = 0;

    if (vg_name)
        args[9] = vg_name;

    success = call_lvm_and_capture_output (args, &output, error);

    if (!success) {
        if (g_error_matches (*error, BD_UTILS_EXEC_ERROR, BD_UTILS_EXEC_ERROR_NOOUT)) {
            /* no output => no LVs, not an error */
            g_clear_error (error);
            ret = g_new0 (BDLVMLVdata*, 1);
            ret[0] = NULL;
            return ret;
        }
        else
            /* the error is already populated from the call */
            return NULL;
    }

    lines = g_strsplit (output, "\n", 0);
    g_free (output);

    for (lines_p = lines; *lines_p; lines_p++) {
        table = parse_lvm_vars ((*lines_p), &num_items);
        if (table && (num_items == 6)) {
            /* valid line, try to parse and record it */
            lvdata = get_lv_data_from_table (table, TRUE);
            if (lvdata)
                g_ptr_array_add (lvs, lvdata);
        } else
            if (table)
                g_hash_table_destroy (table);
    }

    g_strfreev (lines);

    if (lvs->len == 0) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_PARSE,
                     "Failed to parse information about LVs");
        return NULL;
    }

    /* now create the return value -- NULL-terminated array of BDLVMLVdata */
    ret = g_new0 (BDLVMLVdata*, lvs->len + 1);
    for (i=0; i < lvs->len; i++)
        ret[i] = (BDLVMLVdata*) g_ptr_array_index (lvs, i);
    ret[i] = NULL;

    g_ptr_array_free (lvs, FALSE);

    return ret;
}

/**
 * bd_lvm_thpoolcreate:
 * @vg_name: name of the VG to create a thin pool in
 * @lv_name: name of the to-be-created pool LV
 * @size: requested size of the to-be-created pool
 * @md_size: requested metadata size or 0 to use the default
 * @chunk_size: requested chunk size or 0 to use the default
 * @profile: (allow-none): profile to use (see lvm(8) for more information) or %NULL to use
 *                         the default
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @vg_name/@lv_name thin pool was successfully created or not
 */
gboolean bd_lvm_thpoolcreate (gchar *vg_name, gchar *lv_name, guint64 size, guint64 md_size, guint64 chunk_size, gchar *profile, GError **error) {
    GVariantBuilder builder;
    GVariant *params = NULL;
    GVariant *extra = NULL;
    GVariant *param = NULL;

    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    g_variant_builder_add_value (&builder, g_variant_new ("s", lv_name));
    g_variant_builder_add_value (&builder, create_size_str_param (size, "b"));
    g_variant_builder_add_value (&builder, g_variant_new_boolean (TRUE));
    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
    if (md_size != 0) {
        param = create_size_str_param (md_size, "b");
        g_variant_builder_add (&builder, "{sv}", "poolmetadatasize", param);
    }
    if (chunk_size != 0) {
        param = create_size_str_param (chunk_size, "b");
        g_variant_builder_add (&builder, "{sv}", "chunksize", param);
    }
    if (profile) {
        g_variant_builder_add (&builder, "{sv}", "profile", g_variant_new ("s", profile));
    }
    extra = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    call_lvm_obj_method_sync (vg_name, VG_INTF, "LvCreateLinear", params, extra, error);
    return ((*error) == NULL);
}

/**
 * bd_lvm_thlvcreate:
 * @vg_name: name of the VG containing the thin pool providing extents for the to-be-created thin LV
 * @pool_name: name of the pool LV providing extents for the to-be-created thin LV
 * @lv_name: name of the to-be-created thin LV
 * @size: requested virtual size of the to-be-created thin LV
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @vg_name/@lv_name thin LV was successfully created or not
 */
gboolean bd_lvm_thlvcreate (gchar *vg_name, gchar *pool_name, gchar *lv_name, guint64 size, GError **error) {
    GVariantBuilder builder;
    GVariant *params = NULL;

    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    g_variant_builder_add_value (&builder, g_variant_new ("s", lv_name));
    g_variant_builder_add_value (&builder, g_variant_new ("t", size));
    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    call_thpool_method_sync (vg_name, pool_name, "LvCreate", params, NULL, error);

    return (*error == NULL);
}

/**
 * bd_lvm_thlvpoolname:
 * @vg_name: name of the VG containing the queried thin LV
 * @lv_name: name of the queried thin LV
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): the name of the pool volume for the @vg_name/@lv_name
 * thin LV or %NULL if failed to determine (@error) is set in those cases)
 */
gchar* bd_lvm_thlvpoolname (gchar *vg_name, gchar *lv_name, GError **error) {
    GVariant *prop = NULL;
    gboolean is_thin = FALSE;
    gchar *pool_obj_path = NULL;
    gchar *ret = NULL;

    prop = get_lv_property (vg_name, lv_name, "IsThinVolume", error);
    if (!prop)
        return NULL;
    is_thin = g_variant_get_boolean (prop);
    g_variant_unref (prop);

    if (!is_thin) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_NOEXIST,
                     "The LV '%s' is not a thin LV and thus have no thin pool", lv_name);
        return NULL;
    }
    prop = get_lv_property (vg_name, lv_name, "PoolLv", error);
    if (!prop)
        return NULL;
    g_variant_get (prop, "o", &pool_obj_path);
    prop = get_object_property (pool_obj_path, LV_CMN_INTF, "Name", error);
    g_free (pool_obj_path);
    if (!prop)
        return NULL;
    g_variant_get (prop, "s", &ret);
    g_variant_unref (prop);

    return ret;
}

/**
 * bd_lvm_thsnapshotcreate:
 * @vg_name: name of the VG containing the thin LV a new snapshot should be created of
 * @origin_name: name of the thin LV a new snapshot should be created of
 * @snapshot_name: name fo the to-be-created snapshot
 * @pool_name: (allow-none): name of the thin pool to create the snapshot in or %NULL if not specified
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @snapshot_name snapshot of the @vg_name/@origin_name
 * thin LV was successfully created or not.
 */
gboolean bd_lvm_thsnapshotcreate (gchar *vg_name, gchar *origin_name, gchar *snapshot_name, gchar *pool_name, GError **error) {
    GVariantBuilder builder;
    GVariant *params = NULL;
    GVariant *extra = NULL;

    g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
    g_variant_builder_add_value (&builder, g_variant_new ("s", snapshot_name));
    g_variant_builder_add_value (&builder, g_variant_new ("t", 0));
    params = g_variant_builder_end (&builder);
    g_variant_builder_clear (&builder);

    if (pool_name) {
        g_variant_builder_init (&builder, G_VARIANT_TYPE_DICTIONARY);
        g_variant_builder_add (&builder, "{sv}", "thinpool", g_variant_new ("s", pool_name));
        extra = g_variant_builder_end (&builder);
        g_variant_builder_clear (&builder);
    }

    call_lv_method_sync (vg_name, origin_name, "Snapshot", params, extra, error);

    return (*error == NULL);
}

/**
 * bd_lvm_set_global_config:
 * @new_config: (allow-none): string representation of the new global LVM
 *                            configuration to set or %NULL to reset to default
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the new requested global config @new_config was successfully
 *          set or not
 */
gboolean bd_lvm_set_global_config (gchar *new_config, GError **error __attribute__((unused))) {
    /* XXX: the error attribute will likely be used in the future when
       some validation comes into the game */

    g_mutex_lock (&global_config_lock);

    /* first free the old value */
    g_free (global_config_str);

    /* now store the new one */
    global_config_str = g_strdup (new_config);

    g_mutex_unlock (&global_config_lock);
    return TRUE;
}

/**
 * bd_lvm_get_global_config:
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): a copy of a string representation of the currently
 *                           set LVM global configuration
 */
gchar* bd_lvm_get_global_config (GError **error __attribute__((unused))) {
    gchar *ret = NULL;

    g_mutex_lock (&global_config_lock);
    ret = g_strdup (global_config_str ? global_config_str : "");
    g_mutex_unlock (&global_config_lock);

    return ret;
}

/**
 * bd_lvm_cache_get_default_md_size:
 * @cache_size: size of the cache to determine MD size for
 * @error: (out): place to store error (if any)
 *
 * Returns: recommended default size of the cache metadata LV or 0 in case of error
 */
guint64 bd_lvm_cache_get_default_md_size (guint64 cache_size, GError **error __attribute__((unused))) {
    return MAX ((guint64) cache_size / 1000, BD_LVM_MIN_CACHE_MD_SIZE);
}

/**
 * get_lv_type_from_flags: (skip)
 * @meta: getting type for a (future) metadata LV
 *
 * Get LV type string from flags.
 */
static gchar* get_lv_type_from_flags (BDLVMCachePoolFlags flags, gboolean meta, GError **error __attribute__((unused))) {
    if (!meta) {
        if (flags & BD_LVM_CACHE_POOL_STRIPED)
            return "striped";
        else if (flags & BD_LVM_CACHE_POOL_RAID1)
            return "raid1";
        else if (flags & BD_LVM_CACHE_POOL_RAID5)
            return "raid5";
        else if (flags & BD_LVM_CACHE_POOL_RAID6)
            return "raid6";
        else if (flags & BD_LVM_CACHE_POOL_RAID10)
            return "raid10";
        else
            return NULL;
    } else {
        if (flags & BD_LVM_CACHE_POOL_META_STRIPED)
            return "striped";
        else if (flags & BD_LVM_CACHE_POOL_META_RAID1)
            return "raid1";
        else if (flags & BD_LVM_CACHE_POOL_META_RAID5)
            return "raid5";
        else if (flags & BD_LVM_CACHE_POOL_META_RAID6)
            return "raid6";
        else if (flags & BD_LVM_CACHE_POOL_META_RAID10)
            return "raid10";
        else
            return NULL;
    }
}

/**
 * bd_lvm_cache_get_mode_str:
 * @mode: mode to get the string representation for
 * @error: (out): place to store error (if any)
 *
 * Returns: string representation of @mode or %NULL in case of error
 */
const gchar* bd_lvm_cache_get_mode_str (BDLVMCacheMode mode, GError **error) {
    if (mode == BD_LVM_CACHE_MODE_WRITETHROUGH)
        return "writethrough";
    else if (mode == BD_LVM_CACHE_MODE_WRITEBACK)
        return "writeback";
    else if (mode == BD_LVM_CACHE_MODE_UNKNOWN)
        return "unknown";
    else {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_CACHE_INVAL,
                     "Invalid mode given: %d", mode);
        return NULL;
    }
}

/**
 * bd_lvm_cache_get_mode_from_str:
 * @mode_str: string representation of a cache mode
 * @error: (out): place to store error (if any)
 *
 * Returns: cache mode for the @mode_str or %BD_LVM_CACHE_MODE_UNKNOWN if
 *          failed to determine
 */
BDLVMCacheMode bd_lvm_cache_get_mode_from_str (gchar *mode_str, GError **error) {
    if (g_strcmp0 (mode_str, "writethrough") == 0)
        return BD_LVM_CACHE_MODE_WRITETHROUGH;
    else if (g_strcmp0 (mode_str, "writeback") == 0)
        return BD_LVM_CACHE_MODE_WRITEBACK;
    else if (g_strcmp0 (mode_str, "unknown") == 0)
        return BD_LVM_CACHE_MODE_UNKNOWN;
    else {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_CACHE_INVAL,
                     "Invalid mode given: %s", mode_str);
        return BD_LVM_CACHE_MODE_UNKNOWN;
    }
}

/**
 * bd_lvm_cache_create_pool:
 * @vg_name: name of the VG to create @pool_name in
 * @pool_name: name of the cache pool LV to create
 * @pool_size: desired size of the cache pool @pool_name
 * @md_size: desired size of the @pool_name cache pool's metadata LV or 0 to
 *           use the default
 * @mode: cache mode of the @pool_name cache pool
 * @flags: a combination of (ORed) #BDLVMCachePoolFlags
 * @fast_pvs: (array zero-terminated=1): list of (fast) PVs to create the @pool_name
 *                                       cache pool (and the metadata LV)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the cache pool @vg_name/@pool_name was successfully created or not
 */
gboolean bd_lvm_cache_create_pool (gchar *vg_name, gchar *pool_name, guint64 pool_size, guint64 md_size, BDLVMCacheMode mode, BDLVMCachePoolFlags flags, gchar **fast_pvs, GError **error) {
    gboolean success = FALSE;
    gchar *type = NULL;
    gchar *name = NULL;
    gchar *args[10] = {"lvconvert", "-y", "--type", "cache-pool", "--poolmetadata", NULL, "--cachemode", NULL, NULL, NULL};

    /* create an LV for the pool */
    type = get_lv_type_from_flags (flags, FALSE, error);
    success = bd_lvm_lvcreate (vg_name, pool_name, pool_size, type, fast_pvs, error);
    if (!success) {
        g_prefix_error (error, "Failed to create the pool LV: ");
        return FALSE;
    }

    /* determine the size of the metadata LV */
    type = get_lv_type_from_flags (flags, TRUE, error);
    if (md_size == 0)
        md_size = bd_lvm_cache_get_default_md_size (pool_size, error);
    if (*error) {
        g_prefix_error (error, "Failed to determine size for the pool metadata LV: ");
        return FALSE;
    }
    name = g_strdup_printf ("%s_meta", pool_name);

    /* create the metadata LV */
    success = bd_lvm_lvcreate (vg_name, name, md_size, type, fast_pvs, error);
    if (!success) {
        g_free (name);
        g_prefix_error (error, "Failed to create the pool metadata LV: ");
        return FALSE;
    }

    /* create the cache pool from the two LVs */
    args[5] = name;
    args[7] = (gchar *) bd_lvm_cache_get_mode_str (mode, error);
    if (!args[7]) {
        g_free (args[5]);
        return FALSE;
    }
    name = g_strdup_printf ("%s/%s", vg_name, pool_name);
    args[8] = name;
    success = call_lvm_and_report_error (args, error);
    g_free (args[5]);
    g_free (args[8]);

    /* just return the result of the last step (it sets error on fail) */
    return success;
}

/**
 * bd_lvm_cache_attach:
 * @vg_name: name of the VG containing the @data_lv and the @cache_pool_lv LVs
 * @data_lv: data LV to attache the @cache_pool_lv to
 * @cache_pool_lv: cache pool LV to attach to the @data_lv
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @cache_pool_lv was successfully attached to the @data_lv or not
 */
gboolean bd_lvm_cache_attach (gchar *vg_name, gchar *data_lv, gchar *cache_pool_lv, GError **error) {
    gchar *args[7] = {"lvconvert", "--type", "cache", "--cachepool", NULL, NULL, NULL};
    gboolean success = FALSE;

    args[4] = g_strdup_printf ("%s/%s", vg_name, cache_pool_lv);
    args[5] = g_strdup_printf ("%s/%s", vg_name, data_lv);
    success = call_lvm_and_report_error (args, error);

    g_free (args[4]);
    g_free (args[5]);
    return success;
}

/**
 * bd_lvm_cache_detach:
 * @vg_name: name of the VG containing the @cached_lv
 * @cached_lv: name of the cached LV to detach its cache from
 * @destroy: whether to destroy the cache after detach or not
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the cache was successfully detached from the @cached_lv or not
 *
 * Note: synces the cache first
 */
gboolean bd_lvm_cache_detach (gchar *vg_name, gchar *cached_lv, gboolean destroy, GError **error) {
    /* need to both "assume yes" and "force" to get rid of the interactive
       questions in case of "--uncache" */
    gchar *args[6] = {"lvconvert", "-y", "-f", NULL, NULL, NULL};
    gboolean success = FALSE;

    args[3] = destroy ? "--uncache" : "--splitcache";
    args[4] = g_strdup_printf ("%s/%s", vg_name, cached_lv);
    success = call_lvm_and_report_error (args, error);

    g_free (args[4]);
    return success;
}

/**
 * bd_lvm_cache_create_cached_lv:
 * @vg_name: name of the VG to create a cached LV in
 * @lv_name: name of the cached LV to create
 * @data_size: size of the data LV
 * @cache_size: size of the cache (or cached LV more precisely)
 * @md_size: size of the cache metadata LV or 0 to use the default
 * @mode: cache mode for the cached LV
 * @flags: a combination of (ORed) #BDLVMCachePoolFlags
 * @slow_pvs: (array zero-terminated=1): list of slow PVs (used for the data LV)
 * @fast_pvs: (array zero-terminated=1): list of fast PVs (used for the cache LV)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the cached LV @lv_name was successfully created or not
 */
gboolean bd_lvm_cache_create_cached_lv (gchar *vg_name, gchar *lv_name, guint64 data_size, guint64 cache_size, guint64 md_size, BDLVMCacheMode mode, BDLVMCachePoolFlags flags,
                                        gchar **slow_pvs, gchar **fast_pvs, GError **error) {
    gboolean success = FALSE;
    gchar *name = NULL;

    success = bd_lvm_lvcreate (vg_name, lv_name, data_size, NULL, slow_pvs, error);
    if (!success) {
        g_prefix_error (error, "Failed to create the data LV: ");
        return FALSE;
    }

    name = g_strdup_printf ("%s_cache", lv_name);
    success = bd_lvm_cache_create_pool (vg_name, name, cache_size, md_size, mode, flags, fast_pvs, error);
    if (!success) {
        g_prefix_error (error, "Failed to create the cache pool '%s': ", name);
        g_free (name);
        return FALSE;
    }

    success = bd_lvm_cache_attach (vg_name, lv_name, name, error);
    if (!success) {
        g_prefix_error (error, "Failed to attach the cache pool '%s' to the data LV: ", name);
        g_free (name);
        return FALSE;
    }

    g_free (name);
    return TRUE;
}

/**
 * bd_lvm_cache_pool_name:
 * @vg_name: name of the VG containing the @cached_lv
 * @cached_lv: cached LV to get the name of the its pool LV for
 * @error: (out): place to store error (if any)
 *
 * Returns: name of the cache pool LV used by the @cached_lv or %NULL in case of error
 */
gchar* bd_lvm_cache_pool_name (gchar *vg_name, gchar *cached_lv, GError **error) {
    gchar *ret = NULL;
    gchar *name_start = NULL;
    gchar *name_end = NULL;
    gchar *pool_name = NULL;

    /* same as for a thin LV, but with square brackets */
    ret = bd_lvm_thlvpoolname (vg_name, cached_lv, error);
    if (!ret)
        return NULL;

    name_start = strchr (ret, '[');
    if (!name_start) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_CACHE_INVAL,
                     "Failed to determine cache pool name from: '%s'", ret);
        g_free (ret);
        return FALSE;
    }
    name_start++;

    name_end = strchr (ret, ']');
    if (!name_end) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_CACHE_INVAL,
                     "Failed to determine cache pool name from: '%s'", ret);
        g_free (ret);
        return FALSE;
    }

    pool_name = g_strndup (name_start, name_end - name_start);
    g_free (ret);

    return pool_name;
}

/**
 * bd_lvm_cache_stats:
 * @vg_name: name of the VG containing the @cached_lv
 * @cached_lv: cached LV to get stats for
 * @error: (out): place to store error (if any)
 *
 * Returns: stats for the @cached_lv or %NULL in case of error
 */
BDLVMCacheStats* bd_lvm_cache_stats (gchar *vg_name, gchar *cached_lv, GError **error) {
    struct dm_pool *pool = NULL;
    struct dm_task *task = NULL;
    struct dm_info info;
    struct dm_status_cache *status = NULL;
    gchar *map_name = NULL;
    guint64 start = 0;
    guint64 length = 0;
    gchar *type = NULL;
    gchar *params = NULL;
    BDLVMCacheStats *ret = NULL;

    if (geteuid () != 0) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_NOT_ROOT,
                     "Not running as root, cannot query DM maps");
        return NULL;
    }

    pool = dm_pool_create("bd-pool", 20);

    /* translate the VG+LV name into the DM map name */
    map_name = dm_build_dm_name (pool, vg_name, cached_lv, NULL);

    task = dm_task_create (DM_DEVICE_STATUS);
    if (!task) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_DM_ERROR,
                     "Failed to create DM task for the cache map '%s': ", map_name);
        dm_pool_destroy (pool);
        return NULL;
    }

    if (dm_task_set_name (task, map_name) == 0) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_DM_ERROR,
                     "Failed to create DM task for the cache map '%s': ", map_name);
        dm_task_destroy (task);
        dm_pool_destroy (pool);
        return NULL;
    }

    if (dm_task_run(task) == 0) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_DM_ERROR,
                     "Failed to run the DM task for the cache map '%s': ", map_name);
        dm_task_destroy (task);
        dm_pool_destroy (pool);
        return NULL;
    }

    if (dm_task_get_info (task, &info) == 0) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_DM_ERROR,
                     "Failed to get task info for the cache map '%s': ", map_name);
        dm_task_destroy (task);
        dm_pool_destroy (pool);
        return NULL;
    }

    if (!info.exists) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_CACHE_NOCACHE,
                     "The cache map '%s' doesn't exist: ", map_name);
        dm_task_destroy (task);
        dm_pool_destroy (pool);
        return NULL;
    }

    dm_get_next_target(task, NULL, &start, &length, &type, &params);

    if (dm_get_status_cache (pool, params, &status) == 0) {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_CACHE_INVAL,
                     "Failed to get status of the cache map '%s': ", map_name);
        dm_task_destroy (task);
        dm_pool_destroy (pool);
        return NULL;
    }

    ret = g_new0 (BDLVMCacheStats, 1);
    ret->block_size = status->block_size * SECTOR_SIZE;
    ret->cache_size = status->total_blocks * ret->block_size;
    ret->cache_used = status->used_blocks * ret->block_size;

    ret->md_block_size = status->metadata_block_size * SECTOR_SIZE;
    ret->md_size = status->metadata_total_blocks * ret->md_block_size;
    ret->md_used = status->metadata_used_blocks * ret->md_block_size;

    ret->read_hits = status->read_hits;
    ret->read_misses = status->read_misses;
    ret->write_hits = status->write_hits;
    ret->write_misses = status->write_misses;

    if (status->feature_flags & DM_CACHE_FEATURE_WRITETHROUGH)
        ret->mode = BD_LVM_CACHE_MODE_WRITETHROUGH;
    else if (status->feature_flags & DM_CACHE_FEATURE_WRITEBACK)
        ret->mode = BD_LVM_CACHE_MODE_WRITEBACK;
    else {
        g_set_error (error, BD_LVM_ERROR, BD_LVM_ERROR_CACHE_INVAL,
                      "Failed to determine status of the cache from '%"G_GUINT64_FORMAT"': ",
                      status->feature_flags);
        dm_task_destroy (task);
        dm_pool_destroy (pool);
        return NULL;
    }

    dm_task_destroy (task);
    dm_pool_destroy (pool);

    return ret;
}

/**
 * bd_lvm_data_lv_name:
 * @vg_name: name of the VG containing the queried LV
 * @lv_name: name of the queried LV
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): the name of the (internal) data LV of the
 * @vg_name/@lv_name LV
 */
gchar* bd_lvm_data_lv_name (gchar *vg_name, gchar *lv_name, GError **error) {
    gboolean success = FALSE;
    gchar *output = NULL;
    gchar *args[6] = {"lvs", "--noheadings", "-o", "data_lv", NULL, NULL};

    args[4] = g_strdup_printf ("%s/%s", vg_name, lv_name);

    success = call_lvm_and_capture_output (args, &output, error);
    g_free (args[4]);

    if (!success)
        /* the error is already populated from the call */
        return NULL;

    /* replace the '[' and ']' (marking the LV as internal) with spaces and then
       remove all the leading and trailing whitespace */
    return g_strstrip (g_strdelimit (output, "[]", ' '));
}

/**
 * bd_lvm_metadata_lv_name:
 * @vg_name: name of the VG containing the queried LV
 * @lv_name: name of the queried LV
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): the name of the (internal) metadata LV of the
 * @vg_name/@lv_name LV
 */
gchar* bd_lvm_metadata_lv_name (gchar *vg_name, gchar *lv_name, GError **error) {
    gboolean success = FALSE;
    gchar *output = NULL;
    gchar *args[6] = {"lvs", "--noheadings", "-o", "metadata_lv", NULL, NULL};

    args[4] = g_strdup_printf ("%s/%s", vg_name, lv_name);

    success = call_lvm_and_capture_output (args, &output, error);
    g_free (args[4]);

    if (!success)
        /* the error is already populated from the call */
        return NULL;

    /* replace the '[' and ']' (marking the LV as internal) with spaces and then
       remove all the leading and trailing whitespace */
    return g_strstrip (g_strdelimit (output, "[]", ' '));
}
