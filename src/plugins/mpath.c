/*
 * Copyright (C) 2014  Red Hat, Inc.
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
/* provides MAJOR, MINOR macros */
#include <linux/kdev_t.h>
#include <libdevmapper.h>
#include <unistd.h>
#include <utils.h>
#include "mpath.h"

/**
 * SECTION: mpath
 * @short_description: plugin for basic operations with multipath devices
 * @title: Mpath
 * @include: mpath.h
 *
 * A plugin for basic operations with multipath devices.
 */

/**
 * bd_mpath_error_quark: (skip)
 */
GQuark bd_mpath_error_quark (void)
{
    return g_quark_from_static_string ("g-bd-mpath-error-quark");
}

/**
 * check: (skip)
 */
gboolean check() {
    GError *error = NULL;
    gboolean ret = bd_utils_check_util_version ("multipath", MULTIPATH_MIN_VERSION, NULL, "multipath-tools v([\\d\\.]+)", &error);

    if (!ret && error) {
        g_warning("Cannot load the mpath plugin: %s" , error->message);
        g_clear_error (&error);
    }

    if (!ret)
        return FALSE;

    /* mpathconf doesn't report its version */
    ret = bd_utils_check_util_version ("mpathconf", NULL, NULL, NULL, &error);
    if (!ret && error) {
        g_warning("Cannot load the mpath plugin: %s" , error->message);
        g_clear_error (&error);
    }
    return ret;
}

/**
 * bd_mpath_flush_mpaths:
 * @error: (out): place to store error (if any)
 *
 * Returns: whether multipath device maps were successfully flushed or not
 *
 * Flushes all unused multipath device maps.
 */
gboolean bd_mpath_flush_mpaths (GError **error) {
    gchar *argv[3] = {"multipath", "-F", NULL};
    gboolean success = FALSE;
    gchar *output = NULL;

    /* try to flush the device maps */
    success = bd_utils_exec_and_report_error (argv, NULL, error);
    if (!success)
        return FALSE;

    /* list devices (there should be none) */
    argv[1] = "-ll";
    success = bd_utils_exec_and_capture_output (argv, NULL, &output, error);
    if (success && output && (g_strcmp0 (output, "") != 0)) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_FLUSH,
                     "Some device cannot be flushed: %s", output);
        g_free (output);
        return FALSE;
    }

    g_free (output);
    return TRUE;
}

static gchar* get_device_name (gchar *major_minor, GError **error) {
    gchar *path = NULL;
    gchar *link = NULL;
    gchar *ret = NULL;

    path = g_strdup_printf ("/dev/block/%s", major_minor);
    link = g_file_read_link (path, error);
    g_free (path);
    if (!link) {
        g_prefix_error (error, "Failed to determine device name for '%s'",
                        major_minor);
        return NULL;
    }

    /* 'link' should be something like "../sda" */
    /* get the last '/' */
    ret = strrchr (link, '/');
    if (!ret) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_INVAL,
                     "Failed to determine device name for '%s'",
                     major_minor);
        g_free (link);
        return NULL;
    }
    /* move right after the last '/' */
    ret++;

    /* create a new copy and free the whole link path */
    ret = g_strdup (ret);
    g_free (link);

    return ret;
}

static gboolean map_is_multipath (gchar *map_name, GError **error) {
    struct dm_task *task = NULL;
    struct dm_info info;
    guint64 start = 0;
    guint64 length = 0;
    gchar *type = NULL;
    gchar *params = NULL;
    gboolean ret = FALSE;

    if (geteuid () != 0) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_NOT_ROOT,
                     "Not running as root, cannot query DM maps");
        return FALSE;
    }

    task = dm_task_create (DM_DEVICE_STATUS);
    if (!task) {
        g_warning ("Failed to create DM task");
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to create DM task");
        return FALSE;
    }

    if (dm_task_set_name (task, map_name) == 0) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to create DM task");
        dm_task_destroy (task);
        return FALSE;
    }

    if (dm_task_run (task) == 0) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to run DM task");
        dm_task_destroy (task);
        return FALSE;
    }

    if (dm_task_get_info (task, &info) == 0) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to get task info");
        dm_task_destroy (task);
        return FALSE;
    }

    dm_get_next_target(task, NULL, &start, &length, &type, &params);
    if (g_strcmp0 (type, "multipath") == 0)
        ret = TRUE;
    else
        ret = FALSE;
    dm_task_destroy (task);

    return ret;
}

static gchar** get_map_deps (gchar *map_name, GError **error) {
    struct dm_task *task;
    struct dm_deps *deps;
    guint64 major = 0;
    guint64 minor = 0;
    guint64 i = 0;
    gchar **dep_devs = NULL;
    gchar *major_minor = NULL;

    if (geteuid () != 0) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_NOT_ROOT,
                     "Not running as root, cannot query DM maps");
        return NULL;
    }

    task = dm_task_create (DM_DEVICE_DEPS);
    if (!task) {
        g_warning ("Failed to create DM task");
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to create DM task");
        return NULL;
    }

    if (dm_task_set_name (task, map_name) == 0) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to create DM task");
        dm_task_destroy (task);
        return NULL;
    }

    if (dm_task_run (task) == 0) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to run DM task");
        dm_task_destroy (task);
        return NULL;
    }

    deps = dm_task_get_deps (task);
    if (!deps) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to device dependencies");
        dm_task_destroy (task);
        return NULL;
    }

    /* allocate space for the dependencies */
    dep_devs = g_new0 (gchar*, deps->count + 1);

    for (i = 0; i < deps->count; i++) {
        major = (guint64) MAJOR(deps->device[i]);
        minor = (guint64) MINOR(deps->device[i]);
        major_minor = g_strdup_printf ("%"G_GUINT64_FORMAT":%"G_GUINT64_FORMAT, major, minor);
        dep_devs[i] = get_device_name (major_minor, error);
        if (*error) {
            g_prefix_error (error, "Failed to resolve '%s' to device name",
                            major_minor);
            g_free (dep_devs);
            g_free (major_minor);
            return NULL;
        }
        g_free (major_minor);
    }
    dep_devs[deps->count] = NULL;

    dm_task_destroy (task);
    return dep_devs;
}

/**
 * bd_mpath_is_mpath_member:
 * @device: device to test
 * @error: (out): place to store error (if any)
 *
 * Returns: %TRUE if the device is a multipath member, %FALSE if not or an error
 * appeared when queried (@error is set in those cases)
 */
gboolean bd_mpath_is_mpath_member (gchar *device, GError **error) {
    struct dm_task *task_names = NULL;
	struct dm_names *names = NULL;
    gchar *symlink = NULL;
    guint64 next = 0;
    gchar **deps = NULL;
    gchar **dev_name = NULL;
    gboolean ret = FALSE;

    if (geteuid () != 0) {
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_NOT_ROOT,
                     "Not running as root, cannot query DM maps");
        return FALSE;
    }

    /* we check if the 'device' is a dependency of any multipath map  */
    /* get maps */
    task_names = dm_task_create(DM_DEVICE_LIST);
	if (!task_names) {
        g_warning ("Failed to create DM task");
        g_set_error (error, BD_MPATH_ERROR, BD_MPATH_ERROR_DM_ERROR,
                     "Failed to create DM task");
        return FALSE;
    }

    dm_task_run(task_names);
	names = dm_task_get_names(task_names);

    if (!names || !names->dev)
        return FALSE;

    /* in case the device is symlink, we need to resolve it because maps's deps
       are devices and not their symlinks */
    if (g_str_has_prefix (device, "/dev/mapper/") || g_str_has_prefix (device, "/dev/md/")) {
        symlink = g_file_read_link (device, error);
        if (!symlink) {
            /* the device doesn't exist and thus is not an mpath member */
            g_clear_error (error);
            dm_task_destroy (task_names);
            return FALSE;
        }

        /* the symlink starts with "../" */
        device = symlink + 3;
    }

    if (g_str_has_prefix (device, "/dev/"))
        device += 5;

    /* check all maps */
    do {
        names = (void *)names + next;
        next = names->next;

        /* we are only interested in multipath maps */
        if (map_is_multipath (names->name, error)) {
            deps = get_map_deps (names->name, error);
            if (*error) {
                g_prefix_error (error, "Failed to determine deps for '%s'", names->name);
                g_free (symlink);
                dm_task_destroy (task_names);
                return FALSE;
            }
            for (dev_name = deps; !ret && *dev_name; dev_name++)
                ret = (g_strcmp0 (*dev_name, device) == 0);
            g_strfreev (deps);
        } else if (*error) {
            g_prefix_error (error, "Failed to determine map's target for '%s'", names->name);
            g_free (symlink);
            dm_task_destroy (task_names);
            return FALSE;
        }
    } while (!ret && next);

    g_free (symlink);
    dm_task_destroy (task_names);
    return ret;
}

/**
 * bd_mpath_set_friendly_names:
 * @enabled: whether friendly names should be enabled or not
 * @error: (out): place to store error (if any)
 *
 * Returns: if successfully set or not
 */
gboolean bd_mpath_set_friendly_names (gboolean enabled, GError **error) {
    gchar *argv[8] = {"mpathconf", "--find_multipaths", "y", "--user_friendly_names", NULL, "--with_multipathd", "y", NULL};
    argv[4] = enabled ? "y" : "n";

    return bd_utils_exec_and_report_error (argv, NULL, error);
}
